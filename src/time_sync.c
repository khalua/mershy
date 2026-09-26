#include "time_sync.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "app_config.h"

static const char *TAG = "time";

void time_sync_start(void)
{
    setenv("TZ", LOCAL_TIMEZONE, 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed");
    }
}

bool time_sync_now(char *out, size_t len)
{
    out[0] = '\0';
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2025) {
        return false;  // not synced yet (clock starts at 1970)
    }
    // %l is the hour without a leading zero.
    strftime(out, len, "%A, %B %e %Y, %l:%M %p", &local);
    return true;
}
