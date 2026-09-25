#include "audio_player.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board_config.h"

static const char *TAG = "player";

static constexpr size_t kCapacity = AUDIO_OUTPUT_SAMPLE_RATE * 30;  // 30s, ~1.4MB PSRAM
static constexpr size_t kFrame = AUDIO_OUTPUT_SAMPLE_RATE / 50;     // 20ms per write

static BoxAudioCodec *s_codec = nullptr;
static void (*s_on_level)(float) = nullptr;

static std::mutex s_mutex;
static int16_t *s_ring = nullptr;
static size_t s_head = 0;   // next read
static size_t s_count = 0;  // samples queued
static std::atomic<bool> s_playing{false};  // player task mid-frame

static float frame_level(const int16_t *mono, size_t n) {
    double sum_sq = 0.0;
    for (size_t i = 0; i < n; i++) sum_sq += (double)mono[i] * mono[i];
    float v = sqrtf((float)(sum_sq / n)) / 32768.0f * 5.0f;  // speech RMS rarely exceeds ~0.2
    return v > 1.0f ? 1.0f : v;
}

static void player_task(void *arg) {
    std::vector<int16_t> mono(kFrame);
    // The codec's playback path is opened as 2-channel (see
    // BoxAudioCodec::EnableOutput), so each sample goes to both L and R.
    std::vector<int16_t> stereo(kFrame * 2);
    bool was_playing = false;

    while (true) {
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            n = s_count < kFrame ? s_count : kFrame;
            for (size_t i = 0; i < n; i++) {
                mono[i] = s_ring[(s_head + i) % kCapacity];
            }
            s_head = (s_head + n) % kCapacity;
            s_count -= n;
            s_playing = n > 0;
        }

        if (n == 0) {
            if (was_playing) {
                s_on_level(0.0f);
                was_playing = false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        was_playing = true;
        s_on_level(frame_level(mono.data(), n));
        stereo.resize(n * 2);
        for (size_t i = 0; i < n; i++) {
            stereo[2 * i] = mono[i];
            stereo[2 * i + 1] = mono[i];
        }
        s_codec->OutputData(stereo);  // blocks at the I2S rate
        s_playing = false;
    }
}

void player_init(BoxAudioCodec *codec, void (*on_level)(float)) {
    s_codec = codec;
    s_on_level = on_level;
    s_ring = static_cast<int16_t *>(heap_caps_malloc(kCapacity * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (s_ring == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for the playback buffer");
        return;
    }
    // Core 1 at high priority: it only wakes to feed the I2S DMA, and must
    // not be starved by TLS work on core 0.
    xTaskCreatePinnedToCore(player_task, "player", 4096, nullptr, 8, nullptr, 1);
}

bool player_write(const int16_t *samples, size_t count, const std::function<bool()> &should_stop) {
    size_t done = 0;
    while (done < count) {
        if (should_stop()) return false;
        size_t n;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            size_t space = kCapacity - s_count;
            n = count - done < space ? count - done : space;
            size_t tail = (s_head + s_count) % kCapacity;
            for (size_t i = 0; i < n; i++) {
                s_ring[(tail + i) % kCapacity] = samples[done + i];
            }
            s_count += n;
        }
        done += n;
        if (done < count) vTaskDelay(pdMS_TO_TICKS(20));  // buffer full: wait for playback
    }
    return true;
}

bool player_drain(const std::function<bool()> &should_stop) {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_count == 0 && !s_playing) return true;
        }
        if (should_stop()) {
            player_stop();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void player_stop() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_head = 0;
    s_count = 0;
}
