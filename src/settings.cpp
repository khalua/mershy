#include "settings.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "app_config.h"

static const char *TAG = "settings";
static const char *kNamespace = "buddy";

void settings_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

int settings_get_volume() {
    nvs_handle_t h;
    int32_t volume = TTS_VOLUME;
    if (nvs_open(kNamespace, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "volume", &volume);
        nvs_close(h);
    }
    return volume;
}

void settings_set_volume(int volume) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    nvs_set_i32(h, "volume", volume);
    nvs_commit(h);
    nvs_close(h);
}
