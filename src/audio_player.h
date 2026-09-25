// Buffered speaker output. A player task plays mono PCM from a large PSRAM
// ring buffer, so audio can be downloaded faster than real time: the next
// TTS piece downloads while the current one is still playing, and network
// hiccups don't reach the speaker.
#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "box_audio_codec.h"

// on_level gets a 0..1 loudness for the audio actually playing (for the
// mouth), and 0 when playback runs dry.
void player_init(BoxAudioCodec *codec, void (*on_level)(float));

// Queues mono samples at AUDIO_OUTPUT_SAMPLE_RATE, blocking while the buffer
// is full. Returns false (dropping the rest) if should_stop() fires.
bool player_write(const int16_t *samples, size_t count, const std::function<bool()> &should_stop);

// Blocks until everything queued has played. Returns false if should_stop()
// fired first (the queue is flushed in that case).
bool player_drain(const std::function<bool()> &should_stop);

// Drops everything queued, silencing the speaker right away.
void player_stop();

#endif  // AUDIO_PLAYER_H
