#include "http_util.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>

static const char *TAG = "http";

static constexpr size_t kMaxErrorBody = 2048;

HttpConn::HttpConn(int timeout_ms) {
    esp_http_client_config_t cfg = {};
    // Placeholder; every post() sets the real URL. The connection is kept
    // across posts as long as the host stays the same.
    cfg.url = "https://localhost";
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = timeout_ms;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;  // Anthropic/ElevenLabs request headers are long
    cfg.event_handler = &HttpConn::on_event;
    cfg.user_data = this;
    client_ = esp_http_client_init(&cfg);
    if (client_ == nullptr) {
        ESP_LOGE(TAG, "client init failed");
    }
}

// esp_http_client_perform() delivers the (de-chunked) body through
// HTTP_EVENT_ON_DATA as it is read, which is what makes streaming work.
esp_err_t HttpConn::on_event(esp_http_client_event_t *evt) {
    auto *self = static_cast<HttpConn *>(evt->user_data);
    if (evt->event_id != HTTP_EVENT_ON_DATA || self->req_ == nullptr) {
        return ESP_OK;
    }
    Request &r = *self->req_;
    int status = esp_http_client_get_status_code(evt->client);
    if (status / 100 == 2) {
        if (!r.ignoring) {
            r.delivered += evt->data_len;
            if (!(*r.on_data)(static_cast<const uint8_t *>(evt->data), evt->data_len)) {
                r.ignoring = true;  // perform() still drains the rest
            }
        }
    } else if (r.error_body != nullptr && r.error_body->size() < kMaxErrorBody) {
        r.error_body->append(static_cast<const char *>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

int HttpConn::post(const char *url, const HttpHeaders &headers, const void *body, size_t body_len,
                   const HttpDataFn &on_data, std::string *error_body) {
    if (client_ == nullptr) return 0;

    // Two attempts: the first may hit a connection the server has since
    // closed; the retry opens a fresh one.
    for (int attempt = 0; attempt < 2; attempt++) {
        esp_http_client_set_url(client_, url);
        esp_http_client_set_method(client_, HTTP_METHOD_POST);
        for (const auto &h : headers) {
            esp_http_client_set_header(client_, h.first, h.second.c_str());
        }
        esp_http_client_set_post_field(client_, static_cast<const char *>(body), (int)body_len);

        Request r = {&on_data, error_body, false, 0};
        req_ = &r;
        esp_err_t err = esp_http_client_perform(client_);
        req_ = nullptr;

        if (err == ESP_OK) {
            return esp_http_client_get_status_code(client_);
        }
        ESP_LOGW(TAG, "%s: %s (attempt %d)", url, esp_err_to_name(err), attempt + 1);
        esp_http_client_close(client_);
        if (r.delivered > 0) {
            return 0;  // never replay a half-delivered stream
        }
    }
    return 0;
}

bool HttpConn::warm(const char *url, const HttpHeaders &headers) {
    if (client_ == nullptr) return false;
    // Two attempts, like post(): the kept-alive connection may have been
    // closed by the server since the last request.
    for (int attempt = 0; attempt < 2; attempt++) {
        esp_http_client_set_url(client_, url);
        esp_http_client_set_method(client_, HTTP_METHOD_GET);
        for (const auto &h : headers) {
            esp_http_client_set_header(client_, h.first, h.second.c_str());
        }
        esp_http_client_set_post_field(client_, nullptr, 0);

        HttpDataFn discard = [](const uint8_t *, size_t) { return true; };
        Request r = {&discard, nullptr, false, 0};
        req_ = &r;
        esp_err_t err = esp_http_client_perform(client_);
        req_ = nullptr;
        if (err == ESP_OK) return true;
        esp_http_client_close(client_);
        ESP_LOGW(TAG, "warm %s: %s (attempt %d)", url, esp_err_to_name(err), attempt + 1);
    }
    return false;
}

int HttpConn::post_buffered(const char *url, const HttpHeaders &headers, const void *body,
                            size_t body_len, std::string *out) {
    out->clear();
    HttpDataFn collect = [out](const uint8_t *data, size_t len) {
        out->append(reinterpret_cast<const char *>(data), len);
        return true;
    };
    return post(url, headers, body, body_len, collect, out);
}
