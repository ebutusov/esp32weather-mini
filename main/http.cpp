#include "http.hpp"

#include "esp_http_client.h"
#include "esp_log.h"

namespace ws {

static const char* TAG = "http";

static constexpr int kTimeoutMs = 10000;

namespace {
// Closes and frees the client on every return path.
struct ClientGuard {
    esp_http_client_handle_t h;
    bool opened = false;
    ~ClientGuard() {
        if (opened) esp_http_client_close(h);
        esp_http_client_cleanup(h);
    }
};
}  // namespace

esp_err_t httpFetchExact(const char* url, uint8_t* buf, size_t len, ErrorInfo& err) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = kTimeoutMs;
    cfg.disable_auto_redirect = true;

    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        err.set("HTTP request failed", "cannot create client (bad URL?): %s", url);
        return ESP_ERR_INVALID_ARG;
    }
    ClientGuard guard{h};

    ESP_LOGI(TAG, "GET %s", url);
    esp_err_t ret = esp_http_client_open(h, 0);
    if (ret != ESP_OK) {
        err.set("Server unreachable", "%s: %s", url, esp_err_to_name(ret));
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    guard.opened = true;

    const int64_t content_length = esp_http_client_fetch_headers(h);
    if (content_length < 0) {
        err.set("HTTP request failed", "%s: no response headers (%lld)", url,
                static_cast<long long>(content_length));
        return ESP_FAIL;
    }

    const int status = esp_http_client_get_status_code(h);
    if (status != 200) {
        err.set("HTTP error", "HTTP %d from %s", status, url);
        // Make the status visible in the headline too, e.g. "HTTP 503".
        snprintf(err.cause, sizeof(err.cause), "HTTP %d", status);
        ESP_LOGE(TAG, "status %d", status);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // content_length == 0 means no Content-Length header (or chunked):
    // fall back to reading until the stream ends.
    if (content_length > 0 && content_length != static_cast<int64_t>(len)) {
        err.set("Bad image size", "Content-Length %lld, expected %u bytes",
                static_cast<long long>(content_length), static_cast<unsigned>(len));
        ESP_LOGE(TAG, "%s", err.detail);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total = 0;
    while (total < len) {
        const int n = esp_http_client_read(h, reinterpret_cast<char*>(buf) + total,
                                           static_cast<int>(len - total));
        if (n < 0) {
            err.set("Download failed", "read error %d after %u of %u bytes", n,
                    static_cast<unsigned>(total), static_cast<unsigned>(len));
            ESP_LOGE(TAG, "%s", err.detail);
            return ESP_FAIL;
        }
        if (n == 0) {
            break;  // end of body
        }
        total += static_cast<size_t>(n);
    }

    if (total < len) {
        err.set("Bad image size", "got %u bytes, expected %u",
                static_cast<unsigned>(total), static_cast<unsigned>(len));
        ESP_LOGE(TAG, "%s", err.detail);
        return ESP_ERR_INVALID_SIZE;
    }

    // Body must end here. Matters when there was no Content-Length header.
    char extra;
    const int n = esp_http_client_read(h, &extra, 1);
    if (n > 0) {
        err.set("Bad image size", "body is longer than %u bytes",
                static_cast<unsigned>(len));
        ESP_LOGE(TAG, "%s", err.detail);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "received %u bytes", static_cast<unsigned>(total));
    return ESP_OK;
}

}  // namespace ws
