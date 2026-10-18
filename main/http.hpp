// Image download over plain HTTP (see PROTOCOL.md, "HTTP").
#pragma once

#include <cstddef>
#include <cstdint>

#include "error_info.hpp"
#include "esp_err.h"

namespace ws {

// GETs `url` and requires status 200 and a body of exactly `len` bytes,
// which are stored in `buf`. A longer or shorter body is an error.
esp_err_t httpFetchExact(const char* url, uint8_t* buf, size_t len, ErrorInfo& err);

}  // namespace ws
