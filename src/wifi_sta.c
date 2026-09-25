// WiFi station with multiple known networks (WIFI_NETWORKS in secrets.h).
//
// Scans, joins the strongest known network, and keeps a background task
// that rescans and reconnects whenever the link is lost -- so the device
// moves between e.g. home WiFi and a phone hotspot on its own.
// Originally from weather_g/src/wifi_sta.c.
#include <stdlib.h>
#include <string.h>
#include "wifi_sta.h"
#include "secrets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "wifi";

#define CONNECTED_BIT BIT0
#define LOST_BIT      BIT1   // retries on the current AP exhausted
#define MAX_RETRIES   3      // per AP, before rescanning for another one
#define NO_RETRY      1000

typedef struct {
    const char *ssid;
    const char *password;
} wifi_network_t;

#ifndef WIFI_NETWORKS
// Older secrets.h with a single network.
#define WIFI_NETWORKS { {WIFI_SSID, WIFI_PASSWORD} }
#endif

static const wifi_network_t kNetworks[] = WIFI_NETWORKS;
#define NUM_NETWORKS ((int)(sizeof(kNetworks) / sizeof(kNetworks[0])))

static EventGroupHandle_t s_wifi_events;
static volatile int s_retries;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_wifi_events, CONNECTED_BIT);
        if (s_retries < MAX_RETRIES) {
            s_retries++;
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d/%d", event->reason, s_retries, MAX_RETRIES);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, LOST_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retries = 0;
        xEventGroupClearBits(s_wifi_events, LOST_BIT);
        xEventGroupSetBits(s_wifi_events, CONNECTED_BIT);
    }
}

// Scans and returns the index of the strongest visible known network, or -1.
static int pick_network(void)
{
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed");
        return -1;
    }
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 32) count = 32;
    wifi_ap_record_t *records = calloc(count ? count : 1, sizeof(wifi_ap_record_t));
    if (records == NULL) return -1;
    esp_wifi_scan_get_ap_records(&count, records);

    int best = -1;
    int best_rssi = -1000;
    for (int n = 0; n < NUM_NETWORKS; n++) {
        for (int i = 0; i < count; i++) {
            if (strcmp((const char *)records[i].ssid, kNetworks[n].ssid) == 0) {
                ESP_LOGI(TAG, "\"%s\" seen at %d dBm (channel %d)",
                         kNetworks[n].ssid, records[i].rssi, records[i].primary);
                if (records[i].rssi > best_rssi) {
                    best_rssi = records[i].rssi;
                    best = n;
                }
            }
        }
    }
    free(records);
    if (best < 0) {
        ESP_LOGW(TAG, "no known network in range (%d APs seen; ESP32 is 2.4GHz only)", count);
    }
    return best;
}

static bool connect_to(int n, int timeout_ms)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, kNetworks[n].ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, kNetworks[n].password, sizeof(cfg.sta.password) - 1);
    // Force WPA2-PSK with PMF optional (not required): some APs' default
    // WPA2/WPA3-transition mode causes repeated 4-way-handshake timeouts
    // (802.11 reason 15) against this esp_wifi version's WPA3/SAE
    // negotiation. Pinning to plain WPA2 sidesteps SAE entirely.
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    ESP_LOGI(TAG, "connecting to \"%s\"", kNetworks[n].ssid);
    s_retries = 0;
    xEventGroupClearBits(s_wifi_events, CONNECTED_BIT | LOST_BIT);
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, CONNECTED_BIT | LOST_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & CONNECTED_BIT) {
        return true;
    }
    // Stop the driver's retries so the next scan isn't refused ("STA is
    // connecting, scan are not allowed").
    s_retries = NO_RETRY;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "could not join \"%s\"", kNetworks[n].ssid);
    return false;
}

static bool connect_best(int timeout_ms)
{
    int n = pick_network();
    return n >= 0 && connect_to(n, timeout_ms);
}

// Waits for the link to drop, then rescans and joins the best known
// network, retrying every 10s until one works.
static void reconnect_task(void *arg)
{
    while (true) {
        if (xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT) {
            xEventGroupWaitBits(s_wifi_events, LOST_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
            continue;
        }
        if (!connect_best(15000)) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }
}

bool wifi_sta_connect(int timeout_ms)
{
    // NVS is initialized by settings_init() beforehand (WiFi keeps its
    // calibration data there).
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Default modem-sleep power save lets the radio sleep between beacons,
    // which can cause missed ACKs/handshake frames against some APs --
    // symptoms are exactly "4-way handshake timeout" (reason 15) and
    // "disassociated, low ack" (reason 34) during the connect attempt.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    bool ok = connect_best(timeout_ms);
    xTaskCreate(reconnect_task, "wifi_reconnect", 6144, NULL, 4, NULL);
    return ok;
}

bool wifi_sta_is_connected(void)
{
    return s_wifi_events != NULL && (xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT);
}
