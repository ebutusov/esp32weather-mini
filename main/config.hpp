// Device configuration from /spiffs/config.json (see PROTOCOL.md).
#pragma once

#include <cstdint>

#include "error_info.hpp"
#include "esp_err.h"

namespace ws {

struct Config {
    char wifi_ssid[33];       // 802.11 limit: 32 bytes
    char wifi_password[65];   // WPA2 limit: 64 bytes
    char server_url[256];
    uint32_t sleep_seconds;
};

// Used when the config cannot be read.
inline constexpr uint32_t kFallbackSleepSeconds = 1800;

// Mounts the "storage" SPIFFS partition at /spiffs, reads and validates
// config.json, then unmounts. On failure fills `err` and returns an error.
esp_err_t loadConfig(Config& out, ErrorInfo& err);

}  // namespace ws
