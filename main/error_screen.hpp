// Error screen drawn with the driver's built-in 5x7 font.
#pragma once

#include <cstdint>

#include "epd213.hpp"
#include "error_info.hpp"

namespace ws {

// Clears the framebuffer and draws the error in Landscape (250 x 122).
// Only touches the framebuffer; the caller refreshes the panel.
void drawErrorScreen(epd::Epd213& display, const ErrorInfo& err, uint32_t sleep_seconds);

}  // namespace ws
