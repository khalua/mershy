// AI Buddy tunables -- everything you might want to tweak lives here.
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

// ---- Claude ----
#define CLAUDE_MODEL          "claude-sonnet-5"
#define CLAUDE_MAX_TOKENS     1024
#define CLAUDE_HISTORY_TURNS  10      // user+assistant pairs kept for context

#define BUDDY_SYSTEM_PROMPT                                                     \
    "You are Mershy, a small, cheerful Tamagotchi-style buddy who lives inside a " \
    "round glowing screen. You talk with your owner out loud, so reply in one " \
    "or two short, natural spoken sentences, and keep the first one brief. "  \
    "Never use markdown, lists, emoji or special symbols -- your screen font " \
    "can't show them and your voice "                                         \
    "reads everything aloud. Be warm, playful and curious. Start every reply "  \
    "with exactly one mood tag from this list, then a space, then your words: " \
    "[happy] [excited] [curious] [sad] [sleepy]. "                           \
    "If your owner asks you to be louder or quieter, or to set your volume, " \
    "also put one volume tag right after the mood tag: [volume up], "         \
    "[volume down], or [volume N] with N from 0 to 100 -- and say you did it."

// ---- ElevenLabs TTS ----
// Jon ("calm & nurturing"), from the Capsule app's voice list.
#define TTS_VOICE_ID  "Cz0K1kOv9tD8l0b5Qu53"
#define TTS_MODEL_ID  "eleven_turbo_v2_5"
#define TTS_VOLUME    80  // 0-100, default speaker volume (then saved in NVS)
#define VOLUME_STEP   5   // per swipe notch (20px)

// ---- Deepgram STT ----
#define STT_MODEL "nova-3"

// ---- Listening / voice activity ----
#define LISTEN_FRAME_MS          30
#define LISTEN_SPEECH_DBFS      -45.0f  // louder than this counts as speech
#define LISTEN_SILENCE_STOP_MS   800    // stop after this much quiet post-speech
#define LISTEN_TAIL_KEEP_MS      250    // silence kept after the last speech (the rest isn't uploaded)
#define LISTEN_NO_SPEECH_MS      5000   // give up if nothing is said at all
#define LISTEN_MAX_MS            15000  // hard cap on one utterance

// ---- Sleep ----
#define BRIGHTNESS_AWAKE   100     // percent
#define BRIGHTNESS_DIM     25
#define IDLE_DIM_MS        60000   // dim after 1 min without a conversation
#define IDLE_SLEEP_MS      180000  // screen off after 3 min

#endif  // APP_CONFIG_H
