#include "claude_client.h"

#include <cstring>
#include <deque>
#include <utility>

#include <cJSON.h>
#include <esp_log.h>

#include "api_keys.h"
#include "app_config.h"
#include "http_util.h"

static const char *TAG = "claude";

// (user, assistant) pairs, oldest first. Always complete pairs, so the
// messages array we send strictly alternates user/assistant.
static std::deque<std::pair<std::string, std::string>> s_history;

static void add_message(cJSON *messages, const char *role, const std::string &text) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", role);
    cJSON_AddStringToObject(msg, "content", text.c_str());
    cJSON_AddItemToArray(messages, msg);
}

static std::string build_request(const std::string &user_text) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CLAUDE_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", CLAUDE_MAX_TOKENS);
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON_AddStringToObject(root, "system", BUDDY_SYSTEM_PROMPT);

    // Short spoken chat replies don't benefit from thinking, and it delays
    // the first token.
    cJSON *thinking = cJSON_AddObjectToObject(root, "thinking");
    cJSON_AddStringToObject(thinking, "type", "disabled");

    // Anthropic's server-side web search: Claude decides when to search, the
    // searches run on Anthropic's side, and results come back inside this
    // same streamed response -- no extra requests from the device.
    cJSON *tools = cJSON_AddArrayToObject(root, "tools");
    cJSON *search = cJSON_CreateObject();
    cJSON_AddStringToObject(search, "type", "web_search_20260209");
    cJSON_AddStringToObject(search, "name", "web_search");
    cJSON_AddNumberToObject(search, "max_uses", WEB_SEARCH_MAX_USES);
    cJSON *loc = cJSON_AddObjectToObject(search, "user_location");
    cJSON_AddStringToObject(loc, "type", "approximate");
    if (USER_CITY[0] != '\0') cJSON_AddStringToObject(loc, "city", USER_CITY);
    cJSON_AddStringToObject(loc, "region", USER_REGION);
    cJSON_AddStringToObject(loc, "country", USER_COUNTRY);
    cJSON_AddStringToObject(loc, "timezone", USER_TIMEZONE);
    cJSON_AddItemToArray(tools, search);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    for (const auto &turn : s_history) {
        add_message(messages, "user", turn.first);
        add_message(messages, "assistant", turn.second);
    }
    add_message(messages, "user", user_text);

    char *json = cJSON_PrintUnformatted(root);
    std::string body(json);
    cJSON_free(json);
    cJSON_Delete(root);
    return body;
}

static HttpConn &conn() {
    static HttpConn c(60000);
    return c;
}

static HttpHeaders headers() {
    return {
        {"x-api-key", ANTHROPIC_API_KEY},
        {"anthropic-version", "2023-06-01"},
        {"content-type", "application/json"},
    };
}

void claude_warm() {
    conn().warm("https://api.anthropic.com/v1/models?limit=1", headers());
}

namespace {

// Incremental parser for the Messages API's server-sent events. Only the
// `data:` lines matter; each holds one JSON event.
struct SseParser {
    std::string line;
    std::string raw;  // all text generated so far
    std::string stop_reason;
    bool error = false;
    bool searching = false;
    const std::function<void(const std::string &)> *on_text;
    const std::function<void(bool)> *on_searching;

    void feed(const uint8_t *data, size_t len) {
        for (size_t i = 0; i < len; i++) {
            char c = (char)data[i];
            if (c == '\n') {
                handle_line();
                line.clear();
            } else if (c != '\r') {
                line += c;
            }
        }
    }

    void handle_line() {
        if (line.compare(0, 6, "data: ") != 0) return;
        cJSON *ev = cJSON_Parse(line.c_str() + 6);
        if (ev == nullptr) return;
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "type"));
        if (type == nullptr) {
            // ignore
        } else if (strcmp(type, "content_block_start") == 0) {
            // Block types: "text", or "server_tool_use" / "web_search_tool_result"
            // around a search. Only text is spoken; the rest just drives the
            // "searching" indicator.
            const char *btype = cJSON_GetStringValue(
                cJSON_GetObjectItem(cJSON_GetObjectItem(ev, "content_block"), "type"));
            if (btype != nullptr) {
                bool is_search = strcmp(btype, "server_tool_use") == 0;
                bool is_text = strcmp(btype, "text") == 0;
                if (is_search && !searching) {
                    searching = true;
                    ESP_LOGI(TAG, "web search");
                    (*on_searching)(true);
                } else if (is_text && searching) {
                    searching = false;
                    (*on_searching)(false);
                }
                // Text resumes in a new block after a search, with no
                // leading space; keep the words apart.
                if (is_text && !raw.empty() && raw.back() != ' ' && raw.back() != '\n') {
                    raw += ' ';
                    (*on_text)(" ");
                }
            }
        } else if (strcmp(type, "content_block_delta") == 0) {
            cJSON *delta = cJSON_GetObjectItem(ev, "delta");
            const char *dtype = cJSON_GetStringValue(cJSON_GetObjectItem(delta, "type"));
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(delta, "text"));
            if (dtype != nullptr && strcmp(dtype, "text_delta") == 0 && text != nullptr) {
                std::string fragment(text);
                raw += fragment;
                (*on_text)(fragment);
            }
        } else if (strcmp(type, "message_delta") == 0) {
            const char *reason = cJSON_GetStringValue(
                cJSON_GetObjectItem(cJSON_GetObjectItem(ev, "delta"), "stop_reason"));
            if (reason != nullptr) stop_reason = reason;
        } else if (strcmp(type, "error") == 0) {
            char *dump = cJSON_PrintUnformatted(ev);
            ESP_LOGE(TAG, "stream error: %s", dump);
            cJSON_free(dump);
            error = true;
        }
        cJSON_Delete(ev);
    }
};

}  // namespace

ClaudeResult claude_chat_stream(const std::string &user_text,
                                const std::function<void(const std::string &)> &on_text,
                                const std::function<bool()> &cancelled,
                                const std::function<void(bool)> &on_searching) {
    std::string body = build_request(user_text);

    SseParser parser;
    parser.on_text = &on_text;
    parser.on_searching = &on_searching;
    bool was_cancelled = false;
    HttpDataFn on_data = [&](const uint8_t *data, size_t len) {
        if (cancelled()) {
            was_cancelled = true;
            return false;
        }
        parser.feed(data, len);
        return true;
    };

    std::string error_body;
    int status = conn().post("https://api.anthropic.com/v1/messages", headers(), body.data(),
                           body.size(), on_data, &error_body);
    if (was_cancelled) {
        return ClaudeResult::kCancelled;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.400s", status, error_body.c_str());
        return ClaudeResult::kError;
    }
    if (parser.searching) {
        on_searching(false);  // stream ended mid-search (e.g. pause_turn)
    }
    if (parser.stop_reason == "refusal") {
        ESP_LOGW(TAG, "refusal");
        return ClaudeResult::kRefusal;
    }
    if (parser.error || parser.raw.empty()) {
        return ClaudeResult::kError;
    }

    ESP_LOGI(TAG, "reply: %s", parser.raw.c_str());
    s_history.emplace_back(user_text, parser.raw);
    while (s_history.size() > CLAUDE_HISTORY_TURNS) {
        s_history.pop_front();
    }
    return ClaudeResult::kOk;
}

void claude_reset_history() {
    s_history.clear();
}
