#include "epd213.hpp"

#include <cstdlib>
#include <cstring>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace epd {

static const char* TAG = "epd213";


// SSD1680 commands used here
namespace cmd {
constexpr uint8_t DRIVER_OUTPUT = 0x01;
constexpr uint8_t DEEP_SLEEP = 0x10;
constexpr uint8_t DATA_ENTRY_MODE = 0x11;
constexpr uint8_t SW_RESET = 0x12;
constexpr uint8_t TEMP_SENSOR = 0x18;
constexpr uint8_t MASTER_ACTIVATE = 0x20;
constexpr uint8_t DISPLAY_UPDATE_CTRL1 = 0x21;
constexpr uint8_t DISPLAY_UPDATE_CTRL2 = 0x22;
constexpr uint8_t WRITE_RAM_BW = 0x24;   // "new" image
constexpr uint8_t WRITE_RAM_RED = 0x26;  // "previous" image (used by partial)
constexpr uint8_t WRITE_LUT = 0x32;
constexpr uint8_t BORDER_WAVEFORM = 0x3C;
constexpr uint8_t RAM_X_RANGE = 0x44;
constexpr uint8_t RAM_Y_RANGE = 0x45;
constexpr uint8_t RAM_X_COUNTER = 0x4E;
constexpr uint8_t RAM_Y_COUNTER = 0x4F;
}  // namespace cmd

// ---------------------------------------------------------------------------
// 5x7 font, ASCII 32..126, 5 columns per glyph, bit0 = top row.
// ---------------------------------------------------------------------------
static const uint8_t kFont5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},  // ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00},  // !
    {0x00, 0x07, 0x00, 0x07, 0x00},  // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14},  // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},  // $
    {0x23, 0x13, 0x08, 0x64, 0x62},  // %
    {0x36, 0x49, 0x56, 0x20, 0x50},  // &
    {0x00, 0x05, 0x03, 0x00, 0x00},  // '
    {0x00, 0x1C, 0x22, 0x41, 0x00},  // (
    {0x00, 0x41, 0x22, 0x1C, 0x00},  // )
    {0x14, 0x08, 0x3E, 0x08, 0x14},  // *
    {0x08, 0x08, 0x3E, 0x08, 0x08},  // +
    {0x00, 0x50, 0x30, 0x00, 0x00},  // ,
    {0x08, 0x08, 0x08, 0x08, 0x08},  // -
    {0x00, 0x60, 0x60, 0x00, 0x00},  // .
    {0x20, 0x10, 0x08, 0x04, 0x02},  // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // 1
    {0x42, 0x61, 0x51, 0x49, 0x46},  // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // 4
    {0x27, 0x45, 0x45, 0x45, 0x39},  // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // 6
    {0x01, 0x71, 0x09, 0x05, 0x03},  // 7
    {0x36, 0x49, 0x49, 0x49, 0x36},  // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // 9
    {0x00, 0x36, 0x36, 0x00, 0x00},  // :
    {0x00, 0x56, 0x36, 0x00, 0x00},  // ;
    {0x08, 0x14, 0x22, 0x41, 0x00},  // <
    {0x14, 0x14, 0x14, 0x14, 0x14},  // =
    {0x00, 0x41, 0x22, 0x14, 0x08},  // >
    {0x02, 0x01, 0x51, 0x09, 0x06},  // ?
    {0x32, 0x49, 0x79, 0x41, 0x3E},  // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // A
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // B
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // D
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // E
    {0x7F, 0x09, 0x09, 0x09, 0x01},  // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A},  // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // H
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // I
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // J
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // K
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // O
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // R
    {0x46, 0x49, 0x49, 0x49, 0x31},  // S
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F},  // W
    {0x63, 0x14, 0x08, 0x14, 0x63},  // X
    {0x07, 0x08, 0x70, 0x08, 0x07},  // Y
    {0x61, 0x51, 0x49, 0x45, 0x43},  // Z
    {0x00, 0x7F, 0x41, 0x41, 0x00},  // [
    {0x02, 0x04, 0x08, 0x10, 0x20},  // backslash
    {0x00, 0x41, 0x41, 0x7F, 0x00},  // ]
    {0x04, 0x02, 0x01, 0x02, 0x04},  // ^
    {0x40, 0x40, 0x40, 0x40, 0x40},  // _
    {0x00, 0x01, 0x02, 0x04, 0x00},  // `
    {0x20, 0x54, 0x54, 0x54, 0x78},  // a
    {0x7F, 0x48, 0x44, 0x44, 0x38},  // b
    {0x38, 0x44, 0x44, 0x44, 0x20},  // c
    {0x38, 0x44, 0x44, 0x48, 0x7F},  // d
    {0x38, 0x54, 0x54, 0x54, 0x18},  // e
    {0x08, 0x7E, 0x09, 0x01, 0x02},  // f
    {0x0C, 0x52, 0x52, 0x52, 0x3E},  // g
    {0x7F, 0x08, 0x04, 0x04, 0x78},  // h
    {0x00, 0x44, 0x7D, 0x40, 0x00},  // i
    {0x20, 0x40, 0x44, 0x3D, 0x00},  // j
    {0x7F, 0x10, 0x28, 0x44, 0x00},  // k
    {0x00, 0x41, 0x7F, 0x40, 0x00},  // l
    {0x7C, 0x04, 0x18, 0x04, 0x78},  // m
    {0x7C, 0x08, 0x04, 0x04, 0x78},  // n
    {0x38, 0x44, 0x44, 0x44, 0x38},  // o
    {0x7C, 0x14, 0x14, 0x14, 0x08},  // p
    {0x08, 0x14, 0x14, 0x18, 0x7C},  // q
    {0x7C, 0x08, 0x04, 0x04, 0x08},  // r
    {0x48, 0x54, 0x54, 0x54, 0x20},  // s
    {0x04, 0x3F, 0x44, 0x40, 0x20},  // t
    {0x3C, 0x40, 0x40, 0x20, 0x7C},  // u
    {0x1C, 0x20, 0x40, 0x20, 0x1C},  // v
    {0x3C, 0x40, 0x30, 0x40, 0x3C},  // w
    {0x44, 0x28, 0x10, 0x28, 0x44},  // x
    {0x0C, 0x50, 0x50, 0x50, 0x3C},  // y
    {0x44, 0x64, 0x54, 0x4C, 0x44},  // z
    {0x00, 0x08, 0x36, 0x41, 0x00},  // {
    {0x00, 0x00, 0x7F, 0x00, 0x00},  // |
    {0x00, 0x41, 0x36, 0x08, 0x00},  // }
    {0x10, 0x08, 0x08, 0x10, 0x08},  // ~
};

// ---------------------------------------------------------------------------

Epd213::Epd213(const Pins& pins, spi_host_device_t host)
    : pins_(pins), host_(host) {}

Epd213::~Epd213() {
    if (dev_) {
        spi_bus_remove_device(dev_);
    }
    if (bus_inited_) {
        spi_bus_free(host_);
    }
    heap_caps_free(fb_);
    heap_caps_free(prev_);
}

esp_err_t Epd213::begin() {
    fb_ = static_cast<uint8_t*>(heap_caps_malloc(kBufSize, MALLOC_CAP_DMA));
    ESP_RETURN_ON_FALSE(fb_, ESP_ERR_NO_MEM, TAG, "framebuffer alloc failed");
    memset(fb_, 0xFF, kBufSize);
    prev_ = static_cast<uint8_t*>(heap_caps_malloc(kBufSize, MALLOC_CAP_DMA));
    ESP_RETURN_ON_FALSE(prev_, ESP_ERR_NO_MEM, TAG, "prev buffer alloc failed");
    memset(prev_, 0xFF, kBufSize);

    // Release the deep-sleep holds from holdPinsForDeepSleep(). Drive the held
    // levels first so the pins don't glitch (e.g. RST low) when the hold drops.
    ESP_RETURN_ON_ERROR(driveIdleLevels(), TAG, "idle levels");
    for (gpio_num_t p : {pins_.rst, pins_.cs, pins_.dc, pins_.sclk, pins_.mosi}) {
        gpio_hold_dis(p);
    }
    gpio_deep_sleep_hold_dis();

    // Control GPIOs
    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask = (1ULL << pins_.rst) | (1ULL << pins_.dc);
    out_cfg.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio out cfg");

    gpio_config_t in_cfg = {};
    in_cfg.pin_bit_mask = 1ULL << pins_.busy;
    in_cfg.mode = GPIO_MODE_INPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&in_cfg), TAG, "gpio in cfg");

    gpio_set_level(pins_.rst, 1);
    gpio_set_level(pins_.dc, 1);

    // SPI bus: write-only, so no MISO
    spi_bus_config_t bus = {};
    bus.mosi_io_num = pins_.mosi;
    bus.miso_io_num = -1;
    bus.sclk_io_num = pins_.sclk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(host_, &bus, SPI_DMA_CH_AUTO), TAG,
                        "spi bus init");
    bus_inited_ = true;

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = 4 * 1000 * 1000;
    dev.mode = 0;
    dev.spics_io_num = pins_.cs;
    dev.queue_size = 1;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(host_, &dev, &dev_), TAG,
                        "spi add device");
    return ESP_OK;
}

// ---- low level ------------------------------------------------------------

void Epd213::hwReset() {
    gpio_set_level(pins_.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(pins_.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(pins_.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

esp_err_t Epd213::waitBusy(uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(pins_.busy) == 1) {  // BUSY is active high
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGW(TAG, "BUSY timeout");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_OK;
}

void Epd213::spiWrite(const uint8_t* data, size_t len) {
    spi_transaction_t t = {};
    t.length = len * 8;
    if (len <= 4) {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, data, len);
    } else {
        t.tx_buffer = data;
    }
    ESP_ERROR_CHECK(spi_device_polling_transmit(dev_, &t));
}

void Epd213::sendCmd(uint8_t c) {
    gpio_set_level(pins_.dc, 0);
    spiWrite(&c, 1);
}

void Epd213::sendData(uint8_t b) {
    gpio_set_level(pins_.dc, 1);
    spiWrite(&b, 1);
}

void Epd213::sendData(const uint8_t* data, size_t len) {
    gpio_set_level(pins_.dc, 1);
    while (len) {
        const size_t n = len > 4000 ? 4000 : len;
        spiWrite(data, n);
        data += n;
        len -= n;
    }
}

void Epd213::sendCmdData(uint8_t c, std::initializer_list<uint8_t> args) {
    sendCmd(c);
    for (uint8_t a : args) {
        sendData(a);
    }
}

// ---- controller setup -------------------------------------------------------

void Epd213::setRamArea() {
    // X is addressed in bytes (8 px), Y in lines.
    sendCmdData(cmd::RAM_X_RANGE, {0x00, static_cast<uint8_t>(kRowBytes - 1)});
    sendCmdData(cmd::RAM_Y_RANGE,
                {0x00, 0x00, static_cast<uint8_t>((kNativeH - 1) & 0xFF),
                 static_cast<uint8_t>((kNativeH - 1) >> 8)});
}

void Epd213::setRamCursor() {
    sendCmdData(cmd::RAM_X_COUNTER, {0x00});
    sendCmdData(cmd::RAM_Y_COUNTER, {0x00, 0x00});
}

esp_err_t Epd213::initController(uint8_t border_waveform, bool hw_reset) {
    if (hw_reset) {
        hwReset();
    }
    sendCmd(cmd::SW_RESET);
    ESP_RETURN_ON_ERROR(waitBusy(), TAG, "busy after sw reset");

    sendCmdData(cmd::DRIVER_OUTPUT,
                {static_cast<uint8_t>((kNativeH - 1) & 0xFF),
                 static_cast<uint8_t>((kNativeH - 1) >> 8), 0x00});
    sendCmdData(cmd::DATA_ENTRY_MODE, {0x03});  // X inc, Y inc, X first
    setRamArea();
    sendCmdData(cmd::BORDER_WAVEFORM, {border_waveform});
    sendCmdData(cmd::DISPLAY_UPDATE_CTRL1, {0x00, 0x80});
    sendCmdData(cmd::TEMP_SENSOR, {0x80});  // internal sensor
    setRamCursor();
    return waitBusy();
}

void Epd213::writeRam(uint8_t ram_cmd, const uint8_t* buf) {
    setRamCursor();
    sendCmd(ram_cmd);
    sendData(buf, kBufSize);
}

esp_err_t Epd213::activate(uint8_t update_ctrl2) {
    sendCmdData(cmd::DISPLAY_UPDATE_CTRL2, {update_ctrl2});
    sendCmd(cmd::MASTER_ACTIVATE);
    const TickType_t start = xTaskGetTickCount();
    const esp_err_t err = waitBusy(10000);
    ESP_LOGI(TAG, "update 0x%02X: BUSY for %u ms", update_ctrl2,
             static_cast<unsigned>((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
    return err;
}

// Custom partial-update waveform for the DEPG0213BN (SSD1680), uploaded with 0x32.
// The controller's built-in mode-2 waveform is not a usable partial update on this
// panel. Taken from the GxEPD2 driver (GxEPD2_213_BN::lut_partial).
static const uint8_t kLutPartial[153] = {
    0x0,  0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x80, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x40, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0A, 0x0,  0x0, 0x0, 0x0, 0x0, 0x2,
    0x1,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0,
    0x1,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,
};

// ---- public panel ops ---------------------------------------------------------

esp_err_t Epd213::refreshFull() {
    ESP_RETURN_ON_ERROR(initController(0x05), TAG, "init");
    writeRam(cmd::WRITE_RAM_BW, fb_);
    writeRam(cmd::WRITE_RAM_RED, fb_);  // baseline for later partial updates
    // 0xF7: clock+analog on, load temp, load LUT (full mode), display, power off
    ESP_RETURN_ON_ERROR(activate(0xF7), TAG, "full update");
    memcpy(prev_, fb_, kBufSize);
    baseline_valid_ = true;
    return ESP_OK;
}

esp_err_t Epd213::refreshPartial() {
    if (!baseline_valid_) {
        return refreshFull();
    }
    ESP_RETURN_ON_ERROR(initController(0x05, false), TAG, "init");
    // New image in 0x24, previous image in 0x26 (re-sent: a reset may drop it)
    writeRam(cmd::WRITE_RAM_RED, prev_);
    writeRam(cmd::WRITE_RAM_BW, fb_);
    sendCmd(cmd::WRITE_LUT);
    sendData(kLutPartial, sizeof(kLutPartial));
    // 0xCC: clock+analog on, display mode 2 with the LUT uploaded above (no temp/LUT load)
    ESP_RETURN_ON_ERROR(activate(0xCC), TAG, "partial update");
    // Power the analog supply down again
    sendCmdData(cmd::DISPLAY_UPDATE_CTRL2, {0x83});
    sendCmd(cmd::MASTER_ACTIVATE);
    ESP_RETURN_ON_ERROR(waitBusy(), TAG, "partial power off");
    writeRam(cmd::WRITE_RAM_RED, fb_);  // new baseline = what is now on the glass
    memcpy(prev_, fb_, kBufSize);
    return ESP_OK;
}

esp_err_t Epd213::sleep() {
    sendCmdData(cmd::DEEP_SLEEP, {0x01});  // mode 1: keep RAM
    return ESP_OK;
}

// Pin levels while the ESP32 sleeps: RST high keeps the panel out of reset (a
// reset would wake it from deep sleep), CS high deselects it, the rest low.
esp_err_t Epd213::driveIdleLevels() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << pins_.rst) | (1ULL << pins_.cs) |
                       (1ULL << pins_.dc) | (1ULL << pins_.sclk) |
                       (1ULL << pins_.mosi);
    cfg.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio idle cfg");
    gpio_set_level(pins_.rst, 1);
    gpio_set_level(pins_.cs, 1);
    gpio_set_level(pins_.dc, 0);
    gpio_set_level(pins_.sclk, 0);
    gpio_set_level(pins_.mosi, 0);
    return ESP_OK;
}

esp_err_t Epd213::holdPinsForDeepSleep() {
    // Give SCLK/MOSI/CS back to plain GPIO so their levels can be set and held.
    if (dev_) {
        ESP_RETURN_ON_ERROR(spi_bus_remove_device(dev_), TAG, "spi remove device");
        dev_ = nullptr;
    }
    if (bus_inited_) {
        ESP_RETURN_ON_ERROR(spi_bus_free(host_), TAG, "spi bus free");
        bus_inited_ = false;
    }
    ESP_RETURN_ON_ERROR(driveIdleLevels(), TAG, "idle levels");
    for (gpio_num_t p : {pins_.rst, pins_.cs, pins_.dc, pins_.sclk, pins_.mosi}) {
        ESP_RETURN_ON_ERROR(gpio_hold_en(p), TAG, "gpio hold");
    }
    // Classic ESP32: digital pads keep their hold in deep sleep only with this.
    gpio_deep_sleep_hold_en();
    return ESP_OK;
}

// ---- drawing --------------------------------------------------------------

void Epd213::clear(Color c) {
    memset(fb_, c == Color::White ? 0xFF : 0x00, kBufSize);
}

void Epd213::drawPixel(int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= width() || y >= height()) {
        return;
    }
    int nx, ny;
    if (rotation_ == Rotation::Portrait) {
        nx = x;
        ny = y;
    } else {
        // Device turned 90 degrees clockwise. If the output is mirrored,
        // swap these two lines for the opposite rotation.
        nx = y;
        ny = kNativeH - 1 - x;
    }
    const uint8_t mask = 0x80 >> (nx & 7);
    uint8_t& byte = fb_[ny * kRowBytes + (nx >> 3)];
    if (c == Color::Black) {
        byte &= ~mask;
    } else {
        byte |= mask;
    }
}

void Epd213::drawLine(int x0, int y0, int x1, int y1, Color c) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        drawPixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Epd213::drawRect(int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    drawLine(x, y, x + w - 1, y, c);
    drawLine(x, y + h - 1, x + w - 1, y + h - 1, c);
    drawLine(x, y, x, y + h - 1, c);
    drawLine(x + w - 1, y, x + w - 1, y + h - 1, c);
}

void Epd213::fillRect(int x, int y, int w, int h, Color c) {
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            drawPixel(x + i, y + j, c);
        }
    }
}

void Epd213::drawBitmap(int x, int y, const uint8_t* bits, int w, int h, Color c) {
    const int stride = (w + 7) / 8;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            if (bits[j * stride + (i >> 3)] & (0x80 >> (i & 7))) {
                drawPixel(x + i, y + j, c);
            }
        }
    }
}

void Epd213::drawChar(int x, int y, char ch, int scale, Color c) {
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t* glyph = kFont5x7[ch - 32];
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if (glyph[col] & (1 << row)) {
                fillRect(x + col * scale, y + row * scale, scale, scale, c);
            }
        }
    }
}

int Epd213::drawText(int x, int y, const char* text, int scale, Color c) {
    if (scale < 1) scale = 1;
    int cx = x;
    for (; *text; ++text) {
        if (*text == '\n') {
            cx = x;
            y += 8 * scale;
            continue;
        }
        drawChar(cx, y, *text, scale, c);
        cx += 6 * scale;  // 5 px glyph + 1 px spacing
    }
    return cx;
}

int Epd213::textWidth(const char* text, int scale) {
    return static_cast<int>(strlen(text)) * 6 * scale;
}

esp_err_t Epd213::loadFramebuffer(const uint8_t* data, size_t len) {
    ESP_RETURN_ON_FALSE(fb_, ESP_ERR_INVALID_STATE, TAG, "begin() not called");
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "null image");
    ESP_RETURN_ON_FALSE(len == kBufSize, ESP_ERR_INVALID_SIZE, TAG,
                        "image is %u bytes, expected %u",
                        static_cast<unsigned>(len), static_cast<unsigned>(kBufSize));
    memcpy(fb_, data, kBufSize);
    return ESP_OK;
}

}  // namespace epd
