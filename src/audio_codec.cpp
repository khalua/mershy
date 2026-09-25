#include "audio_codec.h"

#include <esp_log.h>

static const char *TAG = "AudioCodec";

AudioCodec::AudioCodec() {}
AudioCodec::~AudioCodec() {}

void AudioCodec::OutputData(std::vector<int16_t> &data) {
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t> &data) {
    int samples = Read(data.data(), data.size());
    return samples > 0;
}

void AudioCodec::Start() {
    // Deliberately does NOT call i2s_channel_enable() here: esp_codec_dev
    // owns the I2S channel lifecycle (its i2s data interface enables the
    // channel inside esp_codec_dev_open()), and the vendor BSP likewise
    // never touches the channels directly. The XiaoZhi original enabled
    // both manually first.
    //
    // UNVERIFIED: this was changed to match the BSP, but it did NOT fix the
    // silent-audio problem. Note also that the
    // "i2s_channel_disable(): the channel has not been enabled yet" error
    // still appears with the manual enables removed, so that message comes
    // from inside esp_codec_dev's own close-before-open and is NOT evidence
    // of a lifecycle conflict here.
    EnableInput(true);
    ESP_LOGI(TAG, "Audio codec started (input only)");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
