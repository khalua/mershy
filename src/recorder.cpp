#include "recorder.h"

#include <cmath>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "app_config.h"
#include "board_config.h"

static const char *TAG = "recorder";

static BoxAudioCodec *s_codec = nullptr;
static int16_t *s_buf = nullptr;
static size_t s_capacity = 0;  // in samples

static float frame_dbfs(const int16_t *interleaved, int frames, int channels) {
    double sum_sq = 0.0;
    for (int i = 0; i < frames; i++) {
        double s = interleaved[i * channels];
        sum_sq += s * s;
    }
    double rms = sqrt(sum_sq / frames);
    if (rms < 1.0) rms = 1.0;
    return 20.0f * log10f((float)rms / 32768.0f);
}

// Maps -60..-15 dBFS onto 0..1 for the UI.
static float level_from_dbfs(float dbfs) {
    float v = (dbfs + 60.0f) / 45.0f;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void recorder_init(BoxAudioCodec *codec) {
    s_codec = codec;
    s_capacity = (size_t)AUDIO_INPUT_SAMPLE_RATE * LISTEN_MAX_MS / 1000;
    s_buf = static_cast<int16_t *>(heap_caps_malloc(s_capacity * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (s_buf == nullptr) {
        ESP_LOGE(TAG, "could not allocate %u-sample PSRAM buffer", (unsigned)s_capacity);
        s_capacity = 0;
    }
}

RecordResult recorder_capture(Recording *out,
                              const std::function<bool()> &should_stop,
                              const std::function<void(float)> &on_level) {
    const int channels = s_codec->input_channels();  // ch0 = mic, ch1 = AEC reference
    const int frames = AUDIO_INPUT_SAMPLE_RATE * LISTEN_FRAME_MS / 1000;
    std::vector<int16_t> interleaved(frames * channels);

    // The codec keeps capturing while nobody reads it, so drop whatever is
    // queued in DMA (it predates the tap) before recording.
    for (int i = 0; i < 4; i++) s_codec->InputData(interleaved);

    size_t count = 0;
    int elapsed_ms = 0;
    int quiet_ms = 0;
    bool heard_speech = false;
    size_t speech_end = 0;  // sample count just after the last loud frame
    RecordResult result = RecordResult::kNoSpeech;

    while (true) {
        if (!s_codec->InputData(interleaved)) continue;
        elapsed_ms += LISTEN_FRAME_MS;

        float dbfs = frame_dbfs(interleaved.data(), frames, channels);
        on_level(level_from_dbfs(dbfs));

        for (int i = 0; i < frames && count < s_capacity; i++) {
            s_buf[count++] = interleaved[i * channels];
        }

        if (dbfs > LISTEN_SPEECH_DBFS) {
            heard_speech = true;
            quiet_ms = 0;
            speech_end = count;
        } else {
            quiet_ms += LISTEN_FRAME_MS;
        }

        if (should_stop()) {
            result = heard_speech ? RecordResult::kSpeech : RecordResult::kCancelled;
            break;
        }
        if (heard_speech && quiet_ms >= LISTEN_SILENCE_STOP_MS) {
            result = RecordResult::kSpeech;
            break;
        }
        if (!heard_speech && elapsed_ms >= LISTEN_NO_SPEECH_MS) {
            result = RecordResult::kNoSpeech;
            break;
        }
        if (count >= s_capacity) {
            result = heard_speech ? RecordResult::kSpeech : RecordResult::kNoSpeech;
            break;
        }
    }

    on_level(0.0f);

    // Drop the trailing silence we waited through: nothing to transcribe,
    // and it's upload time.
    if (heard_speech) {
        size_t keep = speech_end + (size_t)AUDIO_INPUT_SAMPLE_RATE * LISTEN_TAIL_KEEP_MS / 1000;
        if (keep < count) count = keep;
    }

    out->samples = s_buf;
    out->count = count;
    ESP_LOGI(TAG, "captured %.1fs (speech=%d)", (float)count / AUDIO_INPUT_SAMPLE_RATE, heard_speech);
    return result;
}
