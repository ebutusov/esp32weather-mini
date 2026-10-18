// WiFi station bring-up for a single connect per wake.
#pragma once

#include "error_info.hpp"
#include "esp_err.h"

namespace ws {

// Requires NVS to be initialised. Connects and waits for an IP address
// (about 15 s overall, with a few reconnect attempts).
esp_err_t wifiConnect(const char* ssid, const char* password, ErrorInfo& err);

// Stops WiFi if wifiConnect() started it; safe to call in any state.
void wifiStop();

}  // namespace ws
