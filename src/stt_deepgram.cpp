#include "stt_deepgram.h"

#include <cstdio>
#include <cstring>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "api_keys.h"
#include "app_config.h"
#include "board_config.h"
#include "http_util.h"

static const char *TAG = "stt";

static HttpConn &conn() {
    static HttpConn c(30000);
    return c;
}

static HttpHeaders auth_headers() {
    return {{"Authorization", std::string("Token ") + DEEPGRAM_API_KEY}};
}

void stt_warm() {
    conn().warm("https://api.deepgram.com/v1/projects", auth_headers());
}

static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }  // ESP32 is little-endian
static void put_u16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

// Standard 44-byte PCM WAV header: mono, 16-bit.
static void write_wav_header(uint8_t *h, uint32_t data_bytes, uint32_t rate) {
    memcpy(h, "RIFF", 4);
    put_u32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_u32(h + 16, 16);        // fmt chunk size
    put_u16(h + 20, 1);         // PCM
    put_u16(h + 22, 1);         // channels
    put_u32(h + 24, rate);
    put_u32(h + 28, rate * 2);  // byte rate
    put_u16(h + 32, 2);         // block align
    put_u16(h + 34, 16);        // bits per sample
    memcpy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);
}

bool stt_transcribe(const int16_t *samples, size_t count, std::string *out) {
    out->clear();

    const size_t data_bytes = count * sizeof(int16_t);
    uint8_t *wav = static_cast<uint8_t *>(heap_caps_malloc(44 + data_bytes, MALLOC_CAP_SPIRAM));
    if (wav == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for %u-byte upload", (unsigned)(44 + data_bytes));
        return false;
    }
    write_wav_header(wav, data_bytes, AUDIO_INPUT_SAMPLE_RATE);
    memcpy(wav + 44, samples, data_bytes);

    char url[128];
    snprintf(url, sizeof(url),
             "https://api.deepgram.com/v1/listen?model=%s&smart_format=true", STT_MODEL);

    HttpHeaders headers = {
        {"Authorization", std::string("Token ") + DEEPGRAM_API_KEY},
        {"Content-Type", "audio/wav"},
    };
    std::string body;
    int status = conn().post_buffered(url, headers, wav, 44 + data_bytes, &body);
    heap_caps_free(wav);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.300s", status, body.c_str());
        return false;
    }

    cJSON *root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "bad JSON");
        return false;
    }
    // results.channels[0].alternatives[0].transcript
    cJSON *results = cJSON_GetObjectItem(root, "results");
    cJSON *channel = cJSON_GetArrayItem(cJSON_GetObjectItem(results, "channels"), 0);
    cJSON *alt = cJSON_GetArrayItem(cJSON_GetObjectItem(channel, "alternatives"), 0);
    cJSON *transcript = cJSON_GetObjectItem(alt, "transcript");
    if (cJSON_IsString(transcript)) {
        *out = transcript->valuestring;
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "transcript: \"%s\"", out->c_str());
    return true;
}
