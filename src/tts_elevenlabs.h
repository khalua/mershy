// Text-to-speech via ElevenLabs, streamed as raw 24kHz PCM (the codec's
// output rate -- no decoding/resampling) into the audio player's buffer.
#ifndef TTS_ELEVENLABS_H
#define TTS_ELEVENLABS_H

#include <functional>
#include <string>

// Opens the connection ahead of time (see HttpConn::warm).
void tts_warm();

// Downloads `text` as speech into the player (audio_player.h). Blocks until
// the download finishes -- not playback; use player_drain() for that -- or
// should_stop() returns true. Returns false on a network/API error.
// previous_text: what was already said in this reply, so a reply spoken in
// several pieces keeps natural intonation across them.
// on_first_audio (optional) fires once, when the first PCM chunk arrives.
bool tts_speak(const std::string &text,
               const std::string &previous_text,
               const std::function<bool()> &should_stop,
               const std::function<void()> &on_first_audio = nullptr);

#endif  // TTS_ELEVENLABS_H
