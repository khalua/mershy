// AI Buddy -- ESP32-S3-Touch-AMOLED-1.75C.
//
// Mershy, a Tamagotchi-style character you talk to: tap the screen, speak,
// and he transcribes you (Deepgram), asks Claude, then shows and speaks the
// reply (ElevenLabs). BOOT puts him to sleep; he also dims and sleeps on his
// own when idle. See CLAUDE.md for the architecture.
//
// Before building: copy src/secrets.example.h to src/secrets.h (WiFi), and
// have ANTHROPIC_API_KEY / DEEPGRAM_API_KEY / ELEVENLABS_API_KEY in your env
// (scripts/gen_api_keys.py bakes them into src/api_keys.h).

#include <atomic>
#include <cstdio>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_lvgl_port.h"

#include "app_config.h"
#include "audio_player.h"
#include "board_bringup.h"
#include "board_config.h"
#include "box_audio_codec.h"
#include "buddy_ui.h"
#include "claude_client.h"
#include "reply_stream.h"
#include "recorder.h"
#include "settings.h"
#include "stt_deepgram.h"
#include "time_sync.h"
#include "tts_elevenlabs.h"
#include "wifi_sta.h"

static const char *TAG = "main";

#define WIFI_CONNECT_TIMEOUT_MS 15000

static BoxAudioCodec *s_codec = nullptr;
static TaskHandle_t s_conversation_task = nullptr;
static Mood s_mood = Mood::kNeutral;  // last mood Claude gave us

static uint32_t now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Input events. Flags carry what happened; the task notification just wakes
// the conversation task so it can look at them.
static std::atomic<bool> s_tap_pending{false};
static std::atomic<bool> s_boot_pending{false};
static std::atomic<bool> s_asleep{false};

// Speaker volume, 0-100. Changed from the LVGL task (swipes) and the
// conversation task (voice requests); saved to NVS by the conversation task
// when idle, since a flash write stalls both cores (and so audio/display).
static std::atomic<int> s_volume{TTS_VOLUME};
static std::atomic<bool> s_volume_dirty{false};

static void wake_conversation_task() {
    if (s_conversation_task != nullptr) {
        xTaskNotifyGive(s_conversation_task);
    }
}

// Called from the LVGL task for a touch released without dragging.
static void on_tap() {
    s_tap_pending = true;
    wake_conversation_task();
}

// Reads the PMIC fuel gauge into the swipe panel, at most every 5s.
// Called from both the LVGL task (swipes) and the conversation task; the
// I2C driver serializes the reads.
static void refresh_battery(bool force = false) {
    static std::atomic<uint32_t> last_ms{0};
    uint32_t now = now_ms();
    if (!force && last_ms != 0 && now - last_ms < 5000) return;
    last_ms = now;
    BatteryStatus b = board_battery();
    buddy_ui_set_battery(b.present, b.percent, b.usb);
}

static void set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    if (s_codec == nullptr) return;  // swipe during boot, before audio is up
    s_codec->SetOutputVolume(volume);
    s_volume_dirty = true;
    refresh_battery();
    buddy_ui_show_volume(volume);
}

// Called from the LVGL task while the screen is dragged vertically.
static void on_volume_step(int steps) {
    if (s_asleep) {
        on_tap();  // any touch just wakes him
        return;
    }
    set_volume(s_volume + steps * VOLUME_STEP);
}

// Polls the BOOT button (GPIO0, active low) and reports each press.
static void boot_button_task(void *arg) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&cfg);

    bool was_down = false;
    while (true) {
        bool down = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
        if (down && !was_down) {
            s_boot_pending = true;
            wake_conversation_task();
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(30));  // also debounces
    }
}

// Stop listening / talking: a tap, or BOOT (which then puts Mershy to sleep
// once the exchange unwinds). Consumes taps but not the BOOT press.
static bool stop_requested() {
    return s_tap_pending.exchange(false) || s_boot_pending.load();
}

// Opens the Deepgram, Claude and ElevenLabs connections while the user is
// still talking, so their TCP+TLS handshakes (~1.5s total) are done by the
// time the requests go out. Runs below the conversation task's priority so
// it never delays mic reads.
static SemaphoreHandle_t s_warm_done = nullptr;

static void warm_task(void *arg) {
    uint32_t t0 = now_ms();
    stt_warm();
    claude_warm();
    tts_warm();
    ESP_LOGI(TAG, "connections warm in %ums", (unsigned)(now_ms() - t0));
    xSemaphoreGive(s_warm_done);
    vTaskDelete(nullptr);
}

static void start_warming() {
    if (s_warm_done == nullptr) s_warm_done = xSemaphoreCreateBinary();
    xSemaphoreTake(s_warm_done, 0);
    xTaskCreatePinnedToCore(warm_task, "warm", 8 * 1024, nullptr, 4, nullptr, 0);
}

// The connections aren't thread-safe: wait for warm_task before using them.
static void wait_warmed() {
    if (xSemaphoreTake(s_warm_done, pdMS_TO_TICKS(8000)) != pdTRUE) {
        ESP_LOGW(TAG, "warm-up still running after 8s");
    }
}

// Called from the Claude stream task around web searches.
static void on_searching(bool searching) {
    buddy_ui_set_status(searching ? "searching the web..." : "");
}

static void show_confused(const char *message) {
    buddy_ui_set_status("");
    buddy_ui_set_state(BuddyState::kConfused, s_mood);
    buddy_ui_set_text(message);
    vTaskDelay(pdMS_TO_TICKS(3000));
    buddy_ui_set_text("");
}

// One tap -> listen -> think -> speak exchange.
static void run_exchange() {
    if (!wifi_sta_is_connected()) {
        // Right after waking, WiFi takes a few seconds to rejoin.
        buddy_ui_set_status("connecting...");
        if (!wifi_sta_wait_connected(8000)) {
            show_confused("No WiFi. Is the hotspot on?");
            return;
        }
        buddy_ui_set_status("");
    }

    buddy_ui_set_state(BuddyState::kListening, s_mood);
    buddy_ui_set_status("listening...");
    start_warming();

    Recording rec;
    RecordResult result = recorder_capture(&rec, stop_requested, buddy_ui_set_level);
    wait_warmed();  // before any early return, so no request overlaps it
    if (result == RecordResult::kCancelled || s_boot_pending) {
        buddy_ui_set_status("");
        return;
    }
    if (result == RecordResult::kNoSpeech) {
        show_confused("I didn't hear anything.");
        return;
    }

    buddy_ui_set_state(BuddyState::kThinking, s_mood);
    buddy_ui_set_status("thinking...");
    uint32_t t_end_of_speech = now_ms();

    std::string transcript;
    if (!stt_transcribe(rec.samples, rec.count, &transcript)) {
        show_confused("My ears aren't working. Check WiFi?");
        return;
    }
    if (transcript.empty()) {
        show_confused("Sorry, I didn't catch that.");
        return;
    }

    uint32_t t_stt = now_ms();

    // Prefix a device-status note so Mershy can answer "what's your volume?"
    // or "how's your battery?" (the system prompt explains the note).
    BatteryStatus battery = board_battery();
    char clock[64];
    char time_part[80] = "";
    if (time_sync_now(clock, sizeof(clock))) {
        snprintf(time_part, sizeof(time_part), "local time %s, ", clock);
    }
    char status[192];
    if (battery.present) {
        snprintf(status, sizeof(status), "(device status: %svolume %d%%, battery %d%%%s)\n",
                 time_part, s_volume.load(), battery.percent, battery.usb ? ", plugged in" : "");
    } else {
        snprintf(status, sizeof(status),
                 "(device status: %svolume %d%%, running on USB power, no battery)\n",
                 time_part, s_volume.load());
    }

    // Claude streams in the background; speech starts with the first
    // sentence while the rest is still being generated.
    reply_stream_start(status + transcript, on_searching);

    ReplyHeader header;
    if (!reply_stream_header(&header)) {
        ClaudeResult r = reply_stream_finish();
        show_confused(r == ClaudeResult::kRefusal ? "Hmm, let's talk about something else!"
                                                  : "My brain is fuzzy. Try again?");
        return;
    }
    uint32_t t_first_text = now_ms();
    ESP_LOGI(TAG, "latency: stt %ums, claude first text %ums",
             (unsigned)(t_stt - t_end_of_speech), (unsigned)(t_first_text - t_stt));

    if (!header.mood.empty()) {
        s_mood = buddy_mood_from_string(header.mood);
    }
    if (header.has_volume) {
        set_volume(header.volume_relative ? s_volume + header.volume : header.volume);
    }

    // Taps during thinking don't count as "stop talking".
    s_tap_pending = false;

    buddy_ui_set_status("");
    buddy_ui_set_state(BuddyState::kSpeaking, s_mood);

    bool stop = false;
    auto should_stop = [&stop]() {
        if (!stop) stop = stop_requested();
        return stop;
    };
    bool first_audio = true;
    auto on_first_audio = [&]() {
        if (!first_audio) return;
        first_audio = false;
        ESP_LOGI(TAG, "latency: end of speech -> voice %ums",
                 (unsigned)(now_ms() - t_end_of_speech));
    };

    std::string spoken;
    std::string chunk;
    bool tts_ok = true;
    while (reply_stream_next(&chunk)) {
        std::string previous = spoken;
        spoken += spoken.empty() ? chunk : " " + chunk;
        buddy_ui_set_text(spoken.c_str());
        if (!tts_ok) continue;  // voice is broken: keep collecting the text
        if (!tts_speak(chunk, previous, should_stop, on_first_audio)) {
            ESP_LOGW(TAG, "TTS failed; showing the reply as text only");
            tts_ok = false;
        }
        if (stop) {
            reply_stream_cancel();
            break;
        }
    }
    // tts_speak returns once a piece is downloaded; wait for the speaker.
    if (!stop) player_drain(should_stop);
    if (stop) player_stop();
    reply_stream_finish();

    if (!tts_ok) {
        vTaskDelay(pdMS_TO_TICKS(2500));  // time to read the caption
    }
}

enum class Power { kAwake, kDim };

static void fade_brightness(int from, int to) {
    const int steps = 12;
    for (int i = 1; i <= steps; i++) {
        board_set_brightness((uint8_t)(from + (to - from) * i / steps));
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

static bool boot_held() {
    return gpio_get_level(BOOT_BUTTON_GPIO) == 0;
}

// Light-sleeps the whole chip in 100ms slices until BOOT or a touch. Between
// slices it polls the touch controller over I2C (its interrupt line isn't
// wired up), which costs a few ms per wake.
static void sleep_until_woken() {
    while (boot_held()) vTaskDelay(pdMS_TO_TICKS(20));  // the press that slept us
    while (true) {
        esp_sleep_enable_timer_wakeup(100 * 1000);
        esp_light_sleep_start();
        if (boot_held() || board_touch_is_pressed()) break;
    }
    // Wait for release, so the waking touch isn't also taken as a tap.
    while (boot_held() || board_touch_is_pressed()) vTaskDelay(pdMS_TO_TICKS(30));
}

// Low-power sleep: close his eyes, fade out, then panel sleep, LVGL stopped,
// codecs and speaker amp off, radio off, CPU in light sleep. Returns once
// woken, with everything back on (WiFi reconnects in the background).
static void sleep_cycle(Power from) {
    ESP_LOGI(TAG, "sleeping");
    s_asleep = true;
    buddy_ui_set_status("");
    buddy_ui_set_sleeping(true);
    vTaskDelay(pdMS_TO_TICKS(400));  // let him close his eyes on screen first
    fade_brightness(from == Power::kDim ? BRIGHTNESS_DIM : BRIGHTNESS_AWAKE, 0);

    board_display_power(false);
    lvgl_port_stop();
    s_codec->EnableInput(false);
    s_codec->EnableOutput(false);
    wifi_sta_pause();

    sleep_until_woken();

    ESP_LOGI(TAG, "waking");
    wifi_sta_resume();
    s_codec->EnableOutput(true);
    s_codec->EnableInput(true);
    buddy_ui_set_sleeping(false);
    lvgl_port_resume();
    vTaskDelay(pdMS_TO_TICKS(100));  // redraw the awake face before the panel lights up
    board_display_power(true);
    fade_brightness(0, BRIGHTNESS_AWAKE);

    vTaskDelay(pdMS_TO_TICKS(100));  // let boot_button_task see the release
    s_boot_pending = false;
    s_tap_pending = false;
    s_asleep = false;
}

// Idle loop: waits for taps and BOOT presses, runs conversations, and dims /
// sleeps after IDLE_DIM_MS / IDLE_SLEEP_MS without one.
//   tap  -> conversation (dim brightens first)
//   BOOT -> low-power sleep until a touch or BOOT (which only wakes; tap
//           again to talk)
static void conversation_task(void *arg) {
    Power power = Power::kAwake;
    uint32_t last_activity = now_ms();
    int wifi_shown = -1;  // last WiFi state reflected in the status pill

    while (true) {
        buddy_ui_set_state(BuddyState::kIdle, s_mood);
        int wifi = wifi_sta_is_connected() ? 1 : 0;
        if (wifi != wifi_shown) {
            wifi_shown = wifi;
            buddy_ui_set_status(wifi ? "" : "no wifi");
        }
        if (s_volume_dirty.exchange(false)) {
            settings_set_volume(s_volume);
        }
        static uint32_t last_battery_ms = 0;
        if (now_ms() - last_battery_ms >= 30000) {
            last_battery_ms = now_ms();
            refresh_battery(true);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        bool boot = s_boot_pending.exchange(false);
        bool tap = s_tap_pending.exchange(false);

        if (boot) {
            sleep_cycle(power);
            power = Power::kAwake;
            last_activity = now_ms();
            wifi_shown = -1;
            continue;
        }

        if (tap) {
            if (power == Power::kDim) {
                board_set_brightness(BRIGHTNESS_AWAKE);
                power = Power::kAwake;
            }
            run_exchange();
            wifi_shown = -1;        // exchange may have changed the status pill
            s_tap_pending = false;  // drop taps that landed mid-exchange
            last_activity = now_ms();
            continue;  // a BOOT press during the exchange is handled next pass
        }

        uint32_t idle = now_ms() - last_activity;
        if (idle >= IDLE_SLEEP_MS) {
            sleep_cycle(power);
            power = Power::kAwake;
            last_activity = now_ms();
            wifi_shown = -1;
        } else if (idle >= IDLE_DIM_MS && power == Power::kAwake) {
            fade_brightness(BRIGHTNESS_AWAKE, BRIGHTNESS_DIM);
            power = Power::kDim;
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ai buddy starting");
    settings_init();

    BoardHandles board = board_bringup_init();
    buddy_ui_init(board.display, on_tap, on_volume_step);
    buddy_ui_set_status(board.touch_ok ? "waking up..." : "touch not found");

    s_codec = new BoxAudioCodec(
        board.i2c_bus, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
        AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
        AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
    s_codec->Start();
    s_volume = settings_get_volume();
    s_codec->SetOutputVolume(s_volume);
    s_codec->EnableOutput(true);

    recorder_init(s_codec);
    player_init(s_codec, buddy_ui_set_level);
    board_prepare_light_sleep();

    BatteryStatus battery = board_battery();
    ESP_LOGI(TAG, "battery: present=%d usb=%d charging=%d %d%%",
             battery.present, battery.usb, battery.charging, battery.percent);

    if (!wifi_sta_connect(WIFI_CONNECT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "WiFi not connected yet; retrying in the background");
    }
    time_sync_start();  // SNTP; syncs whenever WiFi is up

    // 12KB: TLS handshakes plus cJSON parsing. Core 0; LVGL runs on core 1.
    xTaskCreatePinnedToCore(conversation_task, "conversation", 12 * 1024, nullptr, 5,
                            &s_conversation_task, 0);
    // 4KB: gpio_config() logs, and ESP-IDF log formatting overflows 2KB.
    xTaskCreate(boot_button_task, "boot_button", 4096, nullptr, 3, nullptr);

    ESP_LOGI(TAG, "init complete");
}
