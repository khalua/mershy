#include "tts_elevenlabs.h"

#include <cstdio>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>

#include "api_keys.h"
#include "app_config.h"
#include "audio_player.h"
#include "board_config.h"
#include "http_util.h"

static const char *TAG = "tts";

static HttpConn &conn() {
    static HttpConn c(30000);
    return c;
}

void tts_warm() {
    conn().warm("https://api.elevenlabs.io/v1/models", {{"xi-api-key", ELEVENLABS_API_KEY}});
}

bool tts_speak(const std::string &text,
               const std::string &previous_text,
               const std::function<bool()> &should_stop,
               const std::function<void()> &on_first_audio) {
    char url[192];
    snprintf(url, sizeof(url),
             "https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=pcm_%d",
             TTS_VOICE_ID, AUDIO_OUTPUT_SAMPLE_RATE);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "text", text.c_str());
    cJSON_AddStringToObject(req, "model_id", TTS_MODEL_ID);
    if (!previous_text.empty()) {
        cJSON_AddStringToObject(req, "previous_text", previous_text.c_str());
    }
    char *json = cJSON_PrintUnformatted(req);
    std::string body(json);
    cJSON_free(json);
    cJSON_Delete(req);

    HttpHeaders headers = {
        {"xi-api-key", ELEVENLABS_API_KEY},
        {"Content-Type", "application/json"},
    };

    std::vector<int16_t> samples;
    uint8_t carry = 0;
    bool has_carry = false;
    bool stopped = false;
    bool got_audio = false;

    // Audio goes into the player's buffer as fast as it downloads (not paced
    // by playback), so the next piece can start downloading while this one
    // is still playing.
    auto on_chunk = [&](const uint8_t *data, size_t len) -> bool {
        if (!got_audio) {
            got_audio = true;
            if (on_first_audio) on_first_audio();
        }
        // PCM is little-endian 16-bit; a chunk can end mid-sample.
        samples.clear();
        size_t i = 0;
        if (has_carry && len > 0) {
            samples.push_back((int16_t)(carry | (data[0] << 8)));
            i = 1;
            has_carry = false;
        }
        for (; i + 1 < len; i += 2) {
            samples.push_back((int16_t)(data[i] | (data[i + 1] << 8)));
        }
        if (i < len) {
            carry = data[i];
            has_carry = true;
        }
        if (!player_write(samples.data(), samples.size(), should_stop)) {
            stopped = true;
            return false;
        }
        return true;
    };

    std::string error_body;
    int status = conn().post(url, headers, body.data(), body.size(), on_chunk, &error_body);

    if (stopped) {
        ESP_LOGI(TAG, "playback stopped by tap");
        return true;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.300s", status, error_body.c_str());
        return false;
    }
    return true;
}
