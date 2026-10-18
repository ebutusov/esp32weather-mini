// Weather station work cycle (PROTOCOL.md, "Device work cycle"):
// config -> WiFi -> HTTP GET image -> full refresh (or error screen) -> deep sleep.
#include <cinttypes>
#include <cstdint>

#include "config.hpp"
#include "epd213.hpp"
#include "error_info.hpp"
#include "error_screen.hpp"
#include "esp_log.h"
#include "esp_sleep.h"
#include "http.hpp"
#include "nvs_flash.h"
#include "wifi.hpp"

static const char* TAG = "main";

// Image payload buffer (PROTOCOL.md: exactly 4000 bytes). Static, not on the stack.
static uint8_t s_image[epd::Epd213::kBufSize];

static esp_err_t initNvs(ws::ErrorInfo& err) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: %s, erasing", esp_err_to_name(ret));
        ret = nvs_flash_erase();
        if (ret == ESP_OK) ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        err.set("NVS init failed", "%s", esp_err_to_name(ret));
    }
    return ret;
}

// Steps 1-4. On failure `err` describes the problem; `sleep_seconds` is
// updated as soon as the config has been read.
static esp_err_t runCycle(epd::Epd213& display, uint32_t& sleep_seconds,
                          ws::ErrorInfo& err) {
    static ws::Config cfg;  // keeps the strings off the main task stack
    esp_err_t ret = ws::loadConfig(cfg, err);
    if (ret != ESP_OK) return ret;
    sleep_seconds = cfg.sleep_seconds;

    ret = initNvs(err);
    if (ret != ESP_OK) return ret;

    ret = ws::wifiConnect(cfg.wifi_ssid, cfg.wifi_password, err);
    if (ret != ESP_OK) return ret;

    ret = ws::httpFetchExact(cfg.server_url, s_image, sizeof(s_image), err);
    if (ret != ESP_OK) return ret;

    ret = display.loadFramebuffer(s_image, sizeof(s_image));
    if (ret != ESP_OK) {
        err.set("Display error", "loadFramebuffer: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = display.refreshFull();
    if (ret != ESP_OK) {
        err.set("Display error", "refresh: %s", esp_err_to_name(ret));
    }
    return ret;
}

extern "C" void app_main(void) {
    static epd::Epd213 display;  // T5 V2.3.x pins by default
    const esp_err_t display_ret = display.begin();
    if (display_ret != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(display_ret));
    }

    uint32_t sleep_seconds = ws::kFallbackSleepSeconds;
    static ws::ErrorInfo err;
    esp_err_t ret = ESP_FAIL;
    if (display_ret == ESP_OK) {
        ret = runCycle(display, sleep_seconds, err);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "image displayed");
    } else if (display_ret == ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", err.cause, err.detail);
        ws::drawErrorScreen(display, err, sleep_seconds);
        const esp_err_t r = display.refreshFull();
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "error screen refresh failed: %s", esp_err_to_name(r));
        }
    }

    // Step 6: panel off, WiFi off, deep sleep.
    if (display_ret == ESP_OK) {
        display.sleep();
        const esp_err_t r = display.holdPinsForDeepSleep();
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "panel pin hold failed: %s", esp_err_to_name(r));
        }
    }
    ws::wifiStop();
    ESP_LOGI(TAG, "deep sleep for %" PRIu32 " s", sleep_seconds);
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleep_seconds) * 1000000ULL);
    esp_deep_sleep_start();
}
