#include "error_screen.hpp"

#include <cstdio>
#include <cstring>

namespace ws {

static constexpr int kMargin = 4;
static constexpr int kGlyphW = 6;  // 5 px glyph + 1 px spacing, at scale 1
static constexpr int kGlyphH = 7;

// Draws `text` word-wrapped to the usable width, starting at y. Lines that
// would end below y_limit are dropped. Returns the y below the last line.
static int drawWrapped(epd::Epd213& d, int y, const char* text, int scale, int y_limit) {
    const int max_chars = (d.width() - 2 * kMargin) / (kGlyphW * scale);
    const int line_h = (kGlyphH + 3) * scale;
    char line[64];

    while (*text && y + kGlyphH * scale <= y_limit) {
        while (*text == ' ') ++text;
        int n = static_cast<int>(strlen(text));
        if (n == 0) break;
        if (n > max_chars) {
            // Break at the last space that fits, or hard-break a long word.
            int cut = max_chars;
            while (cut > 0 && text[cut] != ' ') --cut;
            n = cut > 0 ? cut : max_chars;
        }
        if (n > static_cast<int>(sizeof(line)) - 1) n = sizeof(line) - 1;
        memcpy(line, text, n);
        line[n] = '\0';
        d.drawText(kMargin, y, line, scale);
        text += n;
        y += line_h;
    }
    return y;
}

void drawErrorScreen(epd::Epd213& d, const ErrorInfo& err, uint32_t sleep_seconds) {
    d.setRotation(epd::Rotation::Landscape);  // 250 x 122
    d.clear();

    d.drawText(kMargin, kMargin, "Weather: error", 2);
    d.drawLine(kMargin, 21, d.width() - 1 - kMargin, 21);

    // Footer: when the next attempt happens
    char footer[48];
    if (sleep_seconds >= 120) {
        snprintf(footer, sizeof(footer), "Next try in %u min",
                 static_cast<unsigned>(sleep_seconds / 60));
    } else {
        snprintf(footer, sizeof(footer), "Next try in %u s",
                 static_cast<unsigned>(sleep_seconds));
    }
    const int footer_y = d.height() - kGlyphH - 2;
    d.drawText(kMargin, footer_y, footer, 1);

    // Cause: big if it fits on one line, otherwise small and wrapped.
    const int cause_scale =
        epd::Epd213::textWidth(err.cause, 2) <= d.width() - 2 * kMargin ? 2 : 1;
    int y = drawWrapped(d, 26, err.cause, cause_scale, footer_y - 2);

    // Detail
    drawWrapped(d, y + 2, err.detail, 1, footer_y - 2);
}

}  // namespace ws
