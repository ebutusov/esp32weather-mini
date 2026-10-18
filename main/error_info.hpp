// Human-readable failure description, filled by the work-cycle steps and
// shown on the e-paper error screen.
#pragma once

#include <cstdarg>
#include <cstdio>

namespace ws {

struct ErrorInfo {
    char cause[48] = "";    // short headline, e.g. "WiFi connect failed"
    char detail[192] = "";  // longer explanation, word-wrapped on screen

    void set(const char* c, const char* fmt, ...) __attribute__((format(printf, 3, 4))) {
        snprintf(cause, sizeof(cause), "%s", c);
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
    }
};

}  // namespace ws
