// Captures one spoken utterance from the mic into a PSRAM buffer, using a
// simple loudness gate to decide when the speaker has finished.
#ifndef RECORDER_H
#define RECORDER_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "box_audio_codec.h"

enum class RecordResult {
    kSpeech,     // got an utterance; samples are in the buffer
    kNoSpeech,   // nothing above the speech threshold before the timeout
    kCancelled,  // should_stop() fired before any speech
};

struct Recording {
    int16_t *samples = nullptr;  // mono, AUDIO_INPUT_SAMPLE_RATE, 16-bit
    size_t count = 0;
};

void recorder_init(BoxAudioCodec *codec);

// Blocks until the utterance ends: LISTEN_SILENCE_STOP_MS of quiet after
// speech, LISTEN_MAX_MS total, or should_stop() returning true. on_level gets
// a 0..1 loudness value every frame for the UI. The returned Recording points
// at an internal buffer that stays valid until the next call.
RecordResult recorder_capture(Recording *out,
                              const std::function<bool()> &should_stop,
                              const std::function<void(float)> &on_level);

#endif  // RECORDER_H
