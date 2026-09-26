#include "reply_stream.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char *TAG = "reply";

static constexpr size_t kMinFirstChunk = 12;  // don't open with just "Oh!"
static constexpr size_t kMinCommaChunk = 30;  // first piece may end at a comma past this
static constexpr size_t kMaxTagLen = 24;

static struct {
    std::mutex mutex;
    std::string user_text;
    std::string pending;      // untagged text not yet handed out
    bool tags_done = false;
    bool first_handed_out = false;
    bool flush = false;       // hand out pending text as-is (a search started)
    void (*on_searching)(bool) = nullptr;
    bool done = false;
    ClaudeResult result = ClaudeResult::kOk;
    ReplyHeader header;
    std::atomic<bool> cancel{false};
    SemaphoreHandle_t changed = nullptr;   // signalled on new text / end
    SemaphoreHandle_t finished = nullptr;  // given when the task exits
} s;

// Parses leading "[...]" tags off s.pending. Caller holds the mutex.
static void parse_tags() {
    while (!s.tags_done) {
        size_t start = s.pending.find_first_not_of(" \n");
        if (start == std::string::npos) return;  // wait for more
        if (s.pending[start] != '[') {
            s.tags_done = true;
            s.pending.erase(0, start);
            return;
        }
        size_t end = s.pending.find(']', start);
        if (end == std::string::npos) {
            if (s.pending.size() - start > kMaxTagLen) s.tags_done = true;  // not a tag
            return;
        }
        std::string tag = s.pending.substr(start + 1, end - start - 1);
        if (tag.compare(0, 6, "volume") == 0) {
            std::string arg = tag.substr(6);
            size_t a = arg.find_first_not_of(' ');
            arg = a == std::string::npos ? "" : arg.substr(a);
            if (arg == "up" || arg == "down") {
                s.header.has_volume = true;
                s.header.volume_relative = true;
                s.header.volume = arg == "up" ? 15 : -15;
            } else if (!arg.empty() && isdigit((unsigned char)arg[0])) {
                s.header.has_volume = true;
                s.header.volume = atoi(arg.c_str());
            }
        } else {
            s.header.mood = tag;
        }
        s.pending.erase(0, end + 1);
    }
}

static void stream_task(void *arg) {
    ClaudeResult result = claude_chat_stream(
        s.user_text,
        [](const std::string &fragment) {
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.pending += fragment;
                parse_tags();
            }
            xSemaphoreGive(s.changed);
        },
        [] { return s.cancel.load(); },
        [](bool searching) {
            if (searching) {
                // Whatever came before the search ("Let me look that up!")
                // is complete: speak it now instead of waiting out the search.
                std::lock_guard<std::mutex> lock(s.mutex);
                s.flush = true;
            }
            if (s.on_searching != nullptr) s.on_searching(searching);
            xSemaphoreGive(s.changed);
        });

    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.result = result;
        s.done = true;
        s.tags_done = true;
    }
    xSemaphoreGive(s.changed);
    xSemaphoreGive(s.finished);
    vTaskDelete(nullptr);
}

void reply_stream_start(const std::string &user_text, void (*on_searching)(bool)) {
    if (s.changed == nullptr) {
        s.changed = xSemaphoreCreateBinary();
        s.finished = xSemaphoreCreateBinary();
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.user_text = user_text;
        s.pending.clear();
        s.tags_done = false;
        s.first_handed_out = false;
        s.flush = false;
        s.on_searching = on_searching;
        s.done = false;
        s.result = ClaudeResult::kOk;
        s.header = ReplyHeader();
        s.cancel = false;
    }
    xSemaphoreTake(s.changed, 0);
    xSemaphoreTake(s.finished, 0);
    // 10KB: TLS handshake on a cold connection plus cJSON per SSE event.
    xTaskCreatePinnedToCore(stream_task, "claude_stream", 10 * 1024, nullptr, 5, nullptr, 0);
}

bool reply_stream_header(ReplyHeader *out) {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            bool has_text = s.pending.find_first_not_of(" \n") != std::string::npos;
            if ((s.tags_done && has_text) || s.done) {
                *out = s.header;
                return has_text || s.result == ClaudeResult::kOk;
            }
        }
        xSemaphoreTake(s.changed, pdMS_TO_TICKS(100));
    }
}

// First piece: the end of the first sentence at least kMinFirstChunk long,
// or of the first clause (", ") at least kMinCommaChunk long, whichever
// comes first -- so speech starts before a long opening sentence finishes.
// Later pieces: the end of the last complete sentence. npos if none yet.
static size_t chunk_end(const std::string &text, bool first) {
    size_t found = std::string::npos;
    for (size_t i = 0; i + 1 < text.size(); i++) {
        char c = text[i];
        bool space_after = text[i + 1] == ' ' || text[i + 1] == '\n';
        if (first && c == ',' && space_after && i + 1 >= kMinCommaChunk) {
            return i + 1;
        }
        if ((c == '.' || c == '!' || c == '?') && space_after) {
            if (first && i + 1 < kMinFirstChunk) continue;
            found = i + 1;
            if (first) break;
        }
    }
    return found;
}

bool reply_stream_next(std::string *out) {
    out->clear();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.tags_done) {
                size_t end = chunk_end(s.pending, !s.first_handed_out);
                if (end == std::string::npos && (s.done || s.flush)) end = s.pending.size();
                s.flush = false;
                if (end != std::string::npos) {
                    *out = s.pending.substr(0, end);
                    s.pending.erase(0, end);
                    size_t a = out->find_first_not_of(" \n");
                    *out = a == std::string::npos ? "" : out->substr(a);
                    if (!out->empty()) {
                        s.first_handed_out = true;
                        return true;
                    }
                }
                if (s.done && s.pending.find_first_not_of(" \n") == std::string::npos) {
                    return false;
                }
            }
        }
        xSemaphoreTake(s.changed, pdMS_TO_TICKS(100));
    }
}

void reply_stream_cancel() {
    s.cancel = true;
}

ClaudeResult reply_stream_finish() {
    xSemaphoreTake(s.finished, portMAX_DELAY);
    std::lock_guard<std::mutex> lock(s.mutex);
    ESP_LOGI(TAG, "stream finished (result %d)", (int)s.result);
    return s.result;
}
