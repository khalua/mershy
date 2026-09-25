// Persistent HTTPS connections over esp_http_client (IDF cert bundle).
//
// One HttpConn per API host, reused across requests: after the first
// request, later ones skip the TCP+TLS handshake (~0.3-1s on this chip)
// as long as the server keeps the connection alive. A stale connection is
// detected on use and re-opened transparently.
#ifndef HTTP_UTIL_H
#define HTTP_UTIL_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <esp_http_client.h>

using HttpHeaders = std::vector<std::pair<const char *, std::string>>;

// Receives response body bytes as they arrive (2xx responses only). Return
// false to ignore the rest of the body.
using HttpDataFn = std::function<bool(const uint8_t *data, size_t len)>;

class HttpConn {
public:
    explicit HttpConn(int timeout_ms);

    // POSTs `body` and streams the response to on_data. Returns the HTTP
    // status, or 0 if the request failed at the transport level. For a
    // non-2xx status the body goes to `error_body` (truncated) instead.
    // Must not be called from two tasks at once.
    int post(const char *url, const HttpHeaders &headers, const void *body, size_t body_len,
             const HttpDataFn &on_data, std::string *error_body = nullptr);

    // Opens (or re-validates) the connection ahead of time with a cheap GET,
    // so the next post() skips the TCP+TLS handshake. Body is discarded.
    // Returns true if the connection is up.
    bool warm(const char *url, const HttpHeaders &headers);

    // Convenience: POST and buffer the whole response into `out`.
    int post_buffered(const char *url, const HttpHeaders &headers, const void *body,
                      size_t body_len, std::string *out);

private:
    struct Request {
        const HttpDataFn *on_data;
        std::string *error_body;
        bool ignoring;
        size_t delivered;
    };

    static esp_err_t on_event(esp_http_client_event_t *evt);

    esp_http_client_handle_t client_ = nullptr;
    Request *req_ = nullptr;
};

#endif  // HTTP_UTIL_H
