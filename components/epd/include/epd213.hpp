// Minimal ESP-IDF driver for the 2.13" 250x122 e-paper (SSD1680 family:
// DEPG0213BN / GDEY0213B74 / GDEM0213B74) on the LILYGO T5 V2.3.x board.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

namespace epd {

struct Pins {
    gpio_num_t busy;
    gpio_num_t rst;
    gpio_num_t dc;
    gpio_num_t cs;
    gpio_num_t sclk;
    gpio_num_t mosi;
};

// LILYGO T5 V2.3 / V2.3.1 / V2.3.2 (2.13")
inline constexpr Pins kT5V23Pins{GPIO_NUM_4,  GPIO_NUM_16, GPIO_NUM_17,
                                 GPIO_NUM_5,  GPIO_NUM_18, GPIO_NUM_23};

enum class Color : uint8_t { White = 0, Black = 1 };
enum class Rotation : uint8_t { Portrait = 0, Landscape = 1 };

class Epd213 {
public:
    // Native panel geometry (portrait): 122 px wide, 250 px tall.
    static constexpr int kNativeW = 122;
    static constexpr int kNativeH = 250;
    static constexpr int kRowBytes = (kNativeW + 7) / 8;  // 16
    static constexpr size_t kBufSize = static_cast<size_t>(kRowBytes) * kNativeH;

    explicit Epd213(const Pins& pins = kT5V23Pins,
                    spi_host_device_t host = SPI2_HOST);
    ~Epd213();
    Epd213(const Epd213&) = delete;
    Epd213& operator=(const Epd213&) = delete;

    // Sets up GPIO + SPI and allocates the framebuffer. Does not touch the panel.
    esp_err_t begin();

    // ---- Drawing (operates on the in-RAM framebuffer only) ----
    void setRotation(Rotation r) { rotation_ = r; }
    int width() const { return rotation_ == Rotation::Portrait ? kNativeW : kNativeH; }
    int height() const { return rotation_ == Rotation::Portrait ? kNativeH : kNativeW; }

    void clear(Color c = Color::White);
    void drawPixel(int x, int y, Color c = Color::Black);
    void drawLine(int x0, int y0, int x1, int y1, Color c = Color::Black);
    void drawRect(int x, int y, int w, int h, Color c = Color::Black);
    void fillRect(int x, int y, int w, int h, Color c = Color::Black);
    // 1bpp bitmap, rows padded to whole bytes, MSB first, bit 1 = ink.
    void drawBitmap(int x, int y, const uint8_t* bits, int w, int h,
                    Color c = Color::Black);
    // Built-in 5x7 font (ASCII 32..126), integer scale >= 1. '\n' is supported.
    // Returns the x coordinate after the last drawn character.
    int drawText(int x, int y, const char* text, int scale = 1,
                 Color c = Color::Black);
    static int textWidth(const char* text, int scale = 1);
    // Copies a complete native-layout image (kBufSize bytes, bit 1 = white)
    // into the framebuffer. ESP_ERR_INVALID_SIZE unless len == kBufSize.
    esp_err_t loadFramebuffer(const uint8_t* data, size_t len);

    // ---- Panel updates ----
    // Full refresh: slow (~2 s), flashes, clears ghosting.
    esp_err_t refreshFull();
    // Fast partial refresh (~0.3-0.5 s), no flashing, accumulates ghosting.
    // Falls back to refreshFull() if no baseline image exists yet.
    esp_err_t refreshPartial();
    // Deep sleep (RAM retained). Call after each update to protect the panel.
    esp_err_t sleep();
    // Before ESP32 deep sleep: releases the SPI bus and holds the control lines
    // at fixed levels (RST/CS high, DC/SCLK/MOSI low) instead of letting them float,
    // so the panel stays in deep sleep. Call after sleep(), as the last driver call
    // before esp_deep_sleep_start(). begin() releases the holds on the next boot.
    esp_err_t holdPinsForDeepSleep();

private:
    void drawChar(int x, int y, char ch, int scale, Color c);
    esp_err_t driveIdleLevels();

    // low level
    void hwReset();
    esp_err_t waitBusy(uint32_t timeout_ms = 5000);
    void spiWrite(const uint8_t* data, size_t len);
    void sendCmd(uint8_t cmd);
    void sendData(uint8_t b);
    void sendData(const uint8_t* data, size_t len);
    void sendCmdData(uint8_t cmd, std::initializer_list<uint8_t> args);

    esp_err_t initController(uint8_t border_waveform, bool hw_reset = true);
    void setRamArea();
    void setRamCursor();
    void writeRam(uint8_t ram_cmd, const uint8_t* buf);
    esp_err_t activate(uint8_t update_ctrl2);

    Pins pins_;
    spi_host_device_t host_;
    spi_device_handle_t dev_ = nullptr;
    bool bus_inited_ = false;
    uint8_t* prev_ = nullptr;  // copy of what the panel currently shows
    uint8_t* fb_ = nullptr;  // native layout, bit 1 = white
    Rotation rotation_ = Rotation::Portrait;
    bool baseline_valid_ = false;
};

}  // namespace epd
