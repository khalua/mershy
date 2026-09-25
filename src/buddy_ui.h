// Mershy: a cartoon portrait with swappable brows/eyes/mouth layers (art from
// art/build_assets.py), animated from an LVGL timer -- blinking, glances,
// mood expressions and volume-driven lip-sync.
//
// Setters are safe to call from any task (they take the LVGL port lock);
// buddy_ui_set_level() is lock-free so it can be called every audio frame.
#ifndef BUDDY_UI_H
#define BUDDY_UI_H

#include <string>

#include "lvgl.h"

enum class BuddyState {
    kBoot,       // waking up / connecting
    kIdle,       // blinking, glancing around
    kListening,  // attentive face, status pill pulses with the mic level
    kThinking,   // thinking face, animated "thinking..." pill
    kSpeaking,   // mouth follows playback level, reply caption shown
    kConfused,   // something went wrong
};

enum class Mood { kNeutral, kHappy, kExcited, kCurious, kSad, kSleepy };

Mood buddy_mood_from_string(const std::string &mood);

// Touch input, delivered from the LVGL task:
//   on_tap         -- a touch released without dragging
//   on_volume_step -- vertical drag, +1 per 20px up / -1 per 20px down
void buddy_ui_init(lv_display_t *display, void (*on_tap)(), void (*on_volume_step)(int steps));

void buddy_ui_set_state(BuddyState state, Mood mood);
void buddy_ui_set_level(float level);        // 0..1, mic or playback loudness
void buddy_ui_set_text(const char *text);    // reply text; "" hides it
void buddy_ui_set_status(const char *text);  // small line at the top; "" hides it

// Pops up the volume bar for ~1.5s.
void buddy_ui_show_volume(int percent);

// Sleeping face (eyes closed), overlays hidden, no blinking. Brightness is
// the caller's job (board_set_brightness).
void buddy_ui_set_sleeping(bool sleeping);

#endif  // BUDDY_UI_H
