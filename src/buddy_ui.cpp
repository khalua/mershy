#include "buddy_ui.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_random.h>

#include "mershy_assets.h"

// Mershy is a full-screen cartoon portrait (the "base") with three swappable
// layers on top -- brows, eyes, mouth -- cut from variant artwork by
// art/build_assets.py. Only a layer's small rectangle is redrawn when it
// changes, which keeps blinking and lip-sync smooth over QSPI.
//
// Any variant can be missing (art is added incrementally): expressions list
// fallbacks, and with no match the layer is hidden, showing the base.

static const char *TAG = "buddy_ui";

extern const uint8_t kMershyBin[] asm("_binary_mershy_bin_start");

static constexpr int kTextHoldMs = 8000;  // reply text lingers after speaking
static constexpr int kMouthHoldMs = 90;   // min time per lip-sync frame
static constexpr int kDragStepPx = 20;    // vertical drag per volume step
static constexpr int kVolumeShowMs = 1500;

struct Part {
    const char *name;
    lv_image_dsc_t dsc;
    int x, y;
};
static Part s_parts[MERSHY_PART_COUNT];

static const Part *find_part(const char *name) {
    for (auto &p : s_parts) {
        if (strcmp(p.name, name) == 0) return &p;
    }
    return nullptr;
}

// First existing part among the names given, or nullptr (= show the base).
template <typename... Names>
static const Part *pick(Names... names) {
    for (const char *n : {names...}) {
        if (const Part *p = find_part(n)) return p;
    }
    return nullptr;
}

static void load_parts() {
    for (int i = 0; i < MERSHY_PART_COUNT; i++) {
        const mershy_part_t &m = kMershyParts[i];
        Part &p = s_parts[i];
        p.name = m.name;
        p.x = m.x;
        p.y = m.y;
        memset(&p.dsc, 0, sizeof(p.dsc));
        p.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        p.dsc.header.cf = m.has_alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
        p.dsc.header.w = m.w;
        p.dsc.header.h = m.h;
        p.dsc.header.stride = m.w * 2;
        p.dsc.data_size = m.w * m.h * (m.has_alpha ? 3 : 2);
        p.dsc.data = kMershyBin + m.offset;
    }
    ESP_LOGI(TAG, "%d art parts loaded", MERSHY_PART_COUNT);
}

// An lv_image that shows one part at a time, or hides to reveal the base.
struct Layer {
    lv_obj_t *img = nullptr;
    const Part *cur = nullptr;
    bool initialized = false;

    void set(const Part *p) {
        if (initialized && p == cur) return;
        initialized = true;
        cur = p;
        if (p == nullptr) {
            lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_image_set_src(img, &p->dsc);
        lv_obj_set_pos(img, p->x, p->y);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
};

struct Expression {
    const Part *brows, *eyes, *mouth;
};

static struct {
    void (*on_tap)() = nullptr;
    void (*on_volume_step)(int) = nullptr;

    // Touch tracking (LVGL task only).
    lv_point_t press_point = {0, 0};
    bool dragging = false;
    int drag_steps = 0;

    // Requested by setters (under the LVGL lock).
    BuddyState state = BuddyState::kBoot;
    Mood mood = Mood::kNeutral;
    bool text_visible = false;
    uint32_t text_hide_at = 0;  // 0 = no pending hide
    bool sleeping = false;

    std::atomic<float> level_in{0.0f};
    float level = 0.0f;  // smoothed

    // Animation state (LVGL task only).
    uint32_t next_blink = 0, blink_end = 0;
    uint32_t next_glance = 0, glance_end = 0;
    const Part *glance = nullptr;
    const Part *mouth_frame = nullptr;
    uint32_t mouth_frame_until = 0;
    int status_opa = -1;
    bool caption_shown = false;
    int thinking_dots = -1;

    Layer brows, eyes, mouth;
    lv_obj_t *caption = nullptr;  // pill behind the reply text
    lv_obj_t *text = nullptr;
    lv_obj_t *status = nullptr;   // pill at the top: "listening" etc.
    lv_obj_t *status_label = nullptr;
    lv_obj_t *volume = nullptr;   // pill with speaker icon + bar
    lv_obj_t *volume_bar = nullptr;
    uint32_t volume_hide_at = 0;
} s;

static uint32_t rand_between(uint32_t lo, uint32_t hi) {
    return lo + esp_random() % (hi - lo + 1);
}

static bool reached(uint32_t now, uint32_t when) {
    return (int32_t)(now - when) >= 0;
}

Mood buddy_mood_from_string(const std::string &mood) {
    if (mood == "happy") return Mood::kHappy;
    if (mood == "excited") return Mood::kExcited;
    if (mood == "curious") return Mood::kCurious;
    if (mood == "sad") return Mood::kSad;
    if (mood == "sleepy") return Mood::kSleepy;
    return Mood::kNeutral;
}

// The resting face for a mood (table in art/README.md).
static Expression mood_expression(Mood mood) {
    switch (mood) {
        case Mood::kHappy:
            return {nullptr, pick("eyes_happy"), pick("mouth_grin")};
        case Mood::kExcited:
            return {pick("brows_raised", "brows_one_up"), pick("eyes_wide"), pick("mouth_grin")};
        case Mood::kCurious:
            return {pick("brows_one_up", "brows_raised"), nullptr, pick("mouth_oo")};
        case Mood::kSad:
            return {pick("brows_sad"), pick("eyes_half"), pick("mouth_frown")};
        case Mood::kSleepy:
            return {nullptr, pick("eyes_half"), pick("mouth_flat")};
        default:
            return {nullptr, nullptr, nullptr};
    }
}

static Expression target_expression() {
    switch (s.state) {
        case BuddyState::kBoot:
            return {nullptr, pick("eyes_half", "eyes_closed"), pick("mouth_flat")};
        case BuddyState::kListening:
            return {pick("brows_raised"), pick("eyes_wide"), nullptr};
        case BuddyState::kThinking:
            return {pick("brows_furrowed"), pick("eyes_up"), pick("mouth_flat")};
        case BuddyState::kConfused:
            return {pick("brows_furrowed", "brows_one_up"), nullptr, pick("mouth_flat")};
        default:
            return mood_expression(s.mood);
    }
}

// Lip-sync: map playback loudness to a mouth shape, holding each shape for a
// minimum time so it reads as speech rather than flicker.
static const Part *speaking_mouth(uint32_t now, const Part *resting) {
    if (!reached(now, s.mouth_frame_until)) return s.mouth_frame;
    const Part *frame;
    if (s.level < 0.06f) {
        frame = resting;
    } else if (s.level < 0.25f) {
        frame = pick("mouth_talk_small");
    } else if (s.level < 0.5f) {
        frame = pick("mouth_talk_mid", "mouth_talk_small");
    } else {
        frame = pick("mouth_talk_wide", "mouth_grin", "mouth_talk_mid", "mouth_talk_small");
    }
    s.mouth_frame = frame;
    s.mouth_frame_until = now + kMouthHoldMs;
    return frame;
}

static void update_face(uint32_t now) {
    if (s.sleeping) {
        s.brows.set(nullptr);
        s.eyes.set(pick("eyes_closed", "eyes_half"));
        s.mouth.set(pick("mouth_flat"));
        return;
    }

    Expression e = target_expression();

    // Idle glances left/right, if that art exists.
    if (s.state == BuddyState::kIdle && e.eyes == nullptr) {
        if (s.glance != nullptr && reached(now, s.glance_end)) {
            s.glance = nullptr;
            s.next_glance = now + rand_between(4000, 9000);
        } else if (s.glance == nullptr && reached(now, s.next_glance)) {
            s.glance = (esp_random() & 1) ? pick("eyes_left") : pick("eyes_right");
            s.glance_end = now + rand_between(800, 1500);
            if (s.glance == nullptr) s.next_glance = now + 5000;
        }
        if (s.glance != nullptr) e.eyes = s.glance;
    } else {
        s.glance = nullptr;
    }

    // Blink.
    const Part *closed = pick("eyes_closed");
    if (closed != nullptr && e.eyes != closed) {
        if (s.blink_end == 0 && reached(now, s.next_blink)) {
            s.blink_end = now + 130;
        }
        if (s.blink_end != 0) {
            if (!reached(now, s.blink_end)) {
                e.eyes = closed;
            } else {
                s.blink_end = 0;
                s.next_blink = now + rand_between(2500, 6000);
            }
        }
    }

    if (s.state == BuddyState::kSpeaking) {
        e.mouth = speaking_mouth(now, e.mouth);
    }

    s.brows.set(e.brows);
    s.eyes.set(e.eyes);
    s.mouth.set(e.mouth);
}

static void update_overlays(uint32_t now) {
    // Reply caption.
    if (s.text_hide_at != 0 && reached(now, s.text_hide_at)) {
        s.text_visible = false;
        s.text_hide_at = 0;
    }
    bool caption = s.text_visible && !s.sleeping;
    if (caption != s.caption_shown) {
        s.caption_shown = caption;
        if (caption) {
            lv_obj_remove_flag(s.caption, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s.caption, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Status pill: pulses with the mic while listening, dots while thinking.
    int opa = 150;
    if (s.state == BuddyState::kListening) {
        opa = 90 + (int)(s.level * 6.0f) * 27;  // quantized: fewer redraws
    }
    if (opa != s.status_opa) {
        s.status_opa = opa;
        lv_obj_set_style_bg_opa(s.status, (lv_opa_t)opa, 0);
    }
    if (s.state == BuddyState::kThinking) {
        static const char *kDots[] = {"thinking", "thinking.", "thinking..", "thinking..."};
        int i = (now / 350) % 4;
        if (i != s.thinking_dots) {
            s.thinking_dots = i;
            lv_label_set_text(s.status_label, kDots[i]);
        }
    } else {
        s.thinking_dots = -1;
    }
}

static void tick(lv_timer_t *t) {
    uint32_t now = lv_tick_get();
    if (s.volume_hide_at != 0 && reached(now, s.volume_hide_at)) {
        s.volume_hide_at = 0;
        lv_obj_add_flag(s.volume, LV_OBJ_FLAG_HIDDEN);
    }
    float target = s.level_in.load(std::memory_order_relaxed);
    s.level += (target - s.level) * (target > s.level ? 0.6f : 0.25f);
    update_face(now);
    update_overlays(now);
}

// A touch is a tap unless it drags vertically past the threshold, in which
// case it becomes a volume swipe (and no tap fires on release).
static void on_screen_touch(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);

    if (code == LV_EVENT_PRESSED) {
        s.press_point = p;
        s.dragging = false;
        s.drag_steps = 0;
    } else if (code == LV_EVENT_PRESSING) {
        int dy = s.press_point.y - p.y;  // up = positive = louder
        if (!s.dragging && (dy > kDragStepPx || dy < -kDragStepPx)) {
            s.dragging = true;
        }
        if (s.dragging) {
            int steps = dy / kDragStepPx;
            if (steps != s.drag_steps && s.on_volume_step != nullptr) {
                s.on_volume_step(steps - s.drag_steps);
            }
            s.drag_steps = steps;
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (!s.dragging && s.on_tap != nullptr) s.on_tap();
    }
}

static lv_obj_t *make_pill(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, 150, 0);
    lv_obj_set_style_pad_hor(o, 16, 0);
    lv_obj_set_style_pad_ver(o, 6, 0);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *make_layer(lv_obj_t *parent) {
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

// ---- Public API ----

void buddy_ui_init(lv_display_t *display, void (*on_tap)(), void (*on_volume_step)(int steps)) {
    s.on_tap = on_tap;
    s.on_volume_step = on_volume_step;
    load_parts();

    lvgl_port_lock(0);

    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, on_screen_touch, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(screen, on_screen_touch, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(screen, on_screen_touch, LV_EVENT_RELEASED, nullptr);

    if (const Part *base = find_part("base")) {
        lv_obj_t *img = lv_image_create(screen);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_src(img, &base->dsc);
        lv_obj_set_pos(img, 0, 0);
    }
    // Brows under eyes: see the layer-order note in art/build_assets.py.
    s.brows.img = make_layer(screen);
    s.eyes.img = make_layer(screen);
    s.mouth.img = make_layer(screen);

    // Reply caption over the tank top: one scrolling line, so it stays inside
    // the round panel.
    s.caption = make_pill(screen);
    lv_obj_set_width(s.caption, 300);
    lv_obj_align(s.caption, LV_ALIGN_BOTTOM_MID, 0, -40);
    s.text = lv_label_create(s.caption);
    lv_obj_set_width(s.text, lv_pct(100));
    lv_obj_set_style_text_font(s.text, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s.text, lv_color_white(), 0);
    lv_obj_set_style_text_align(s.text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s.text, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s.text, "");
    lv_obj_add_flag(s.caption, LV_OBJ_FLAG_HIDDEN);

    s.status = make_pill(screen);
    lv_obj_set_style_bg_color(s.status, lv_color_hex(0x2B6CB0), 0);
    lv_obj_align(s.status, LV_ALIGN_TOP_MID, 0, 34);
    s.status_label = lv_label_create(s.status);
    lv_obj_set_style_text_font(s.status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s.status_label, lv_color_white(), 0);
    lv_label_set_text(s.status_label, "");
    lv_obj_add_flag(s.status, LV_OBJ_FLAG_HIDDEN);

    s.volume = make_pill(screen);
    lv_obj_set_flex_flow(s.volume, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s.volume, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s.volume, 10, 0);
    lv_obj_set_style_pad_ver(s.volume, 10, 0);
    lv_obj_set_style_bg_opa(s.volume, 200, 0);
    lv_obj_align(s.volume, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_t *icon = lv_label_create(s.volume);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_label_set_text(icon, LV_SYMBOL_VOLUME_MAX);
    s.volume_bar = lv_bar_create(s.volume);
    lv_obj_set_size(s.volume_bar, 150, 10);
    lv_bar_set_range(s.volume_bar, 0, 100);
    lv_obj_set_style_bg_color(s.volume_bar, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.volume_bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_add_flag(s.volume, LV_OBJ_FLAG_HIDDEN);

    uint32_t now = lv_tick_get();
    s.next_blink = now + 2000;
    s.next_glance = now + 5000;
    lv_timer_create(tick, 33, nullptr);

    lvgl_port_unlock();
}

void buddy_ui_set_state(BuddyState state, Mood mood) {
    lvgl_port_lock(0);
    uint32_t now = lv_tick_get();
    if (s.state == BuddyState::kSpeaking && state != BuddyState::kSpeaking && s.text_visible) {
        s.text_hide_at = now + kTextHoldMs;  // let the reply linger a bit
    }
    if (state == BuddyState::kListening || state == BuddyState::kThinking) {
        s.text_visible = false;  // new exchange: clear the old reply now
        s.text_hide_at = 0;
    }
    s.state = state;
    s.mood = mood;
    lvgl_port_unlock();
}

void buddy_ui_show_volume(int percent) {
    lvgl_port_lock(0);
    lv_bar_set_value(s.volume_bar, percent, LV_ANIM_OFF);
    lv_obj_remove_flag(s.volume, LV_OBJ_FLAG_HIDDEN);
    s.volume_hide_at = lv_tick_get() + kVolumeShowMs;
    lvgl_port_unlock();
}

void buddy_ui_set_sleeping(bool sleeping) {
    lvgl_port_lock(0);
    s.sleeping = sleeping;
    if (sleeping) {
        s.text_visible = false;
        s.text_hide_at = 0;
        lv_obj_add_flag(s.status, LV_OBJ_FLAG_HIDDEN);
    } else {
        s.next_blink = lv_tick_get() + 1500;
    }
    lvgl_port_unlock();
}

void buddy_ui_set_level(float level) {
    s.level_in.store(level, std::memory_order_relaxed);
}

void buddy_ui_set_text(const char *text) {
    lvgl_port_lock(0);
    lv_label_set_text(s.text, text);
    s.text_visible = text[0] != '\0';
    s.text_hide_at = 0;
    lvgl_port_unlock();
}

void buddy_ui_set_status(const char *text) {
    lvgl_port_lock(0);
    lv_label_set_text(s.status_label, text);
    if (text[0] != '\0') {
        lv_obj_remove_flag(s.status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s.status, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}
