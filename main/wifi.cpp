#include "wifi.hpp"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace ws {

static const char* TAG = "wifi";

static constexpr uint32_t kConnectTimeoutMs = 15000;
static constexpr int kMaxRetries = 3;  // reconnects after the first attempt

static constexpr EventBits_t kConnectedBit = BIT0;
static constexpr EventBits_t kFailedBit = BIT1;

static EventGroupHandle_t s_events = nullptr;
static int s_retries = 0;
static volatile int s_last_reason = 0;
static bool s_started = false;

static void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* ev = static_cast<const wifi_event_sta_disconnected_t*>(data);
        s_last_reason = ev->reason;
        if (s_retries < kMaxRetries) {
            ++s_retries;
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d/%d", ev->reason,
                     s_retries, kMaxRetries);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "disconnected (reason %d), giving up", ev->reason);
            xEventGroupSetBits(s_events, kFailedBit);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* ev = static_cast<const ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, kConnectedBit);
    }
}

static const char* reasonText(int reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND: return "network not found";
        case WIFI_REASON_AUTH_FAIL: return "authentication failed";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake timeout (wrong password?)";
        case WIFI_REASON_ASSOC_FAIL: return "association failed";
        case WIFI_REASON_CONNECTION_FAIL: return "connection failed";
        default: return "disconnected";
    }
}

esp_err_t wifiConnect(const char* ssid, const char* password, ErrorInfo& err) {
    esp_err_t ret = esp_netif_init();
    if (ret == ESP_OK) ret = esp_event_loop_create_default();
    if (ret != ESP_OK) {
        err.set("WiFi init failed", "netif/event loop: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_netif_create_default_wifi_sta();

    s_events = xEventGroupCreate();
    if (!s_events) {
        err.set("Out of memory", "WiFi event group");
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init_cfg);
    if (ret != ESP_OK) {
        err.set("WiFi init failed", "esp_wifi_init: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr);

    // Credentials come from config.json on every boot; don't persist them in NVS.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    // ssid/password are fixed-size fields without a required terminator.
    // The auth threshold stays at its permissive default (open) so older
    // WPA networks still connect.
    wifi_config_t cfg = {};
    memcpy(cfg.sta.ssid, ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, password, strnlen(password, sizeof(cfg.sta.password)));

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret == ESP_OK) ret = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));
    if (ret == ESP_OK) ret = esp_wifi_start();
    if (ret != ESP_OK) {
        err.set("WiFi init failed", "start: %s", esp_err_to_name(ret));
        return ret;
    }
    s_started = true;

    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    const EventBits_t bits =
        xEventGroupWaitBits(s_events, kConnectedBit | kFailedBit, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(kConnectTimeoutMs));
    if (bits & kConnectedBit) {
        return ESP_OK;
    }

    const int reason = s_last_reason;
    if (bits & kFailedBit) {
        err.set("WiFi connect failed", "SSID \"%s\": %s (reason %d)", ssid,
                reasonText(reason), reason);
        return ESP_FAIL;
    }
    if (reason != 0) {
        err.set("WiFi connect failed", "SSID \"%s\": timeout after %u s, last: %s "
                "(reason %d)", ssid, static_cast<unsigned>(kConnectTimeoutMs / 1000),
                reasonText(reason), reason);
    } else {
        err.set("WiFi connect failed", "SSID \"%s\": no IP address after %u s", ssid,
                static_cast<unsigned>(kConnectTimeoutMs / 1000));
    }
    ESP_LOGE(TAG, "%s", err.detail);
    return ESP_ERR_TIMEOUT;
}

void wifiStop() {
    if (s_started) {
        esp_wifi_stop();
        s_started = false;
    }
}

}  // namespace ws
