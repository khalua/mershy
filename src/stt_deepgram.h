// Speech-to-text via Deepgram's pre-recorded /v1/listen endpoint.
#ifndef STT_DEEPGRAM_H
#define STT_DEEPGRAM_H

#include <cstddef>
#include <cstdint>
#include <string>

// Sends mono 16-bit PCM at AUDIO_INPUT_SAMPLE_RATE and writes the transcript
// (possibly empty) to `out`. Returns false on a network/API error.
bool stt_transcribe(const int16_t *samples, size_t count, std::string *out);

// Opens the connection ahead of time (see HttpConn::warm).
void stt_warm();

#endif  // STT_DEEPGRAM_H
