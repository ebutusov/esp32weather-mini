# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Mini weather station with two halves:
- **Firmware** (repo root, `main/`, `components/epd/`): pure ESP-IDF (C++, no Arduino) for the LILYGO T5 V2.3.x board: classic ESP32, 4 MB flash, no PSRAM, 2.13" 250x122 SSD1680-family e-paper. It downloads a ready-made 4000-byte framebuffer image from the server, shows it and deep-sleeps.
- **Server** (`server/`, Python 3.10+, Pillow): `generator.py` renders OpenWeatherMap data into that framebuffer format, `server.py` serves the newest image over HTTP.

`specs.md` is the original product spec. **`PROTOCOL.md` is the binding contract** between firmware and server (payload layout, HTTP endpoint, `config.json` keys, work cycle). Change it first if either side must deviate. The whole system (driver, weather-station `main/`, server images) works on real hardware (T5 V2.3.x, DEPG0213BN panel, ESP-IDF 6.1), including the landscape orientation of server-rendered images.

## Commands

Firmware (run from the repo root, or pass `-C <root>`): needs ESP-IDF 6.1 with the environment loaded (`$IDF_PATH`). `spiffs/config.json` must exist (copy `spiffs/config.example.json`), or CMake configure fails on purpose.

    idf.py set-target esp32      # once; sdkconfig.defaults is applied on first configure
    idf.py build
    idf.py flash monitor         # if flashing fails, hold BOOT (GPIO0) while pressing RESET

Server (Windows paths; the venv lives in `server/.venv`, created from `server/requirements.txt`):

    server\.venv\Scripts\python server\generator.py --from-json server\fixtures\sample.json --config <scratch config> --preview out.png
    server\.venv\Scripts\python server\generator.py              # real OWM fetch, uses server/config.json
    server\.venv\Scripts\python server\server.py --image-dir <dir> --port 8080

`server/config.json` holds the user's real OWM API key: never print, copy or commit it. For tests use `--from-json` with a scratch `--config` whose `image_dir` points at a scratch directory (otherwise `--from-json` writes into the real `server/images/`).

There are no tests and no lint config beyond `.clangd`. `build/`, `sdkconfig`, `sdkconfig.old`, `managed_components/` and `dependencies.lock` are generated; edit `sdkconfig.defaults` for config changes. It only takes effect on a fresh configure, so delete `sdkconfig` or run `idf.py fullclean` after changing it.

## Architecture

- `components/epd/` is the whole driver (`include/epd213.hpp`, `epd213.cpp`) in namespace `epd`. It is a local IDF component, picked up automatically by the top-level `CMakeLists.txt`. It requires `esp_driver_gpio`, `esp_driver_spi` and `esp_timer`. In ESP-IDF 6.x the monolithic `driver` component no longer provides `driver/gpio.h` or `driver/spi_master.h`, so depend on the split `esp_driver_*` components.
- `main/` is the weather-station app. `main.cpp` runs the PROTOCOL.md work cycle once per wake and then deep-sleeps. `config.*` mounts SPIFFS (label `storage`, `/spiffs`) and validates `config.json` with cJSON (registry component `espressif/cjson`; IDF 6 has no built-in `json`). `wifi.*` handles the STA connect (15 s, 3 retries). `http.*` runs `esp_http_client` open/fetch_headers/read and requires status 200 and exactly 4000 bytes. `error_screen.*` draws the landscape error screen. Steps report failures as `esp_err_t` plus a `ws::ErrorInfo` (cause and detail) that the error screen shows. Never log or display the WiFi password.
- `server/generator.py`: fetch (OWM 2.5 `weather` + `forecast?cnt=4`) or `--from-json` fixture -> parse (tolerates missing fields, shows `--`; local time from `city.timezone`) -> render landscape 250x122 with Pillow, using the bundled `server/fonts/` DejaVu Sans (no system font lookup; Pillow is pinned exactly in `requirements.txt` so Windows and Linux renders match) -> `encode_framebuffer()` -> atomic write (`.bin.tmp` + `os.replace`) as `weather_YYYYMMDD_HHMMSS.bin` (UTC) -> prune to `--keep-last`. Errors raise `GeneratorError` (exit 1) and leave the previous image in place.
- `server/server.py`: `ThreadingHTTPServer`, HTTP/1.0 with explicit Content-Length (never chunked). Only `GET /latest.bin` exists (newest `weather_*.bin` by name, 503 if none or not 4000 bytes); everything else is 404, so no file outside the image dir can be served.
- **The landscape -> native mapping lives in two places**: `Epd213::drawPixel` (Landscape branch) and `encode_framebuffer()` in `server/generator.py`. Change both together.
- `partitions.csv`: nvs, phy_init, factory (0x2F0000), `storage` SPIFFS (256 KB). `main/CMakeLists.txt` packs `spiffs/` into `storage` with `FLASH_IN_PROJECT`.

Key design points of `Epd213`:
- **Draw/refresh split.** All drawing (`drawPixel`, lines, rects, bitmap, built-in 5x7 font) only writes the in-RAM framebuffer `fb_`. Nothing reaches the panel until `refreshFull()` or `refreshPartial()`.
- **Framebuffer is in native portrait layout** (122x250, 16 bytes per row, bit 1 = white). Rotation is applied in `drawPixel` (`nx`/`ny` mapping), so mirroring fixes belong there (and in the server's generator, since `loadFramebuffer()` images bypass `drawPixel`). `Color::Black` is enum value 1, which is the opposite of the buffer's bit polarity.
- **`loadFramebuffer(data, len)`** copies a complete native-layout image into `fb_`. It returns `ESP_ERR_INVALID_SIZE` unless `len == kBufSize` (4000) and `ESP_ERR_INVALID_STATE` before `begin()`.
- **Partial refresh depends on a baseline and a custom LUT.** `baseline_valid_` is set after a full refresh. `refreshPartial()` falls back to `refreshFull()` when it is false. The "previous" image goes in RAM 0x26 and the "new" image in 0x24 (`prev_` keeps a copy that is re-sent before each partial update). The DEPG0213BN's built-in mode-2 waveform is not a usable partial update, so `refreshPartial()` uploads `kLutPartial` (from GxEPD2's `GxEPD2_213_BN`) with 0x32 and updates with 0xCC. Reference for init/update sequences: GxEPD2 `GxEPD2_213_BN.cpp`.
- **Power.** `sleep()` puts the panel in deep sleep with RAM retained. The panel should not sit powered with a static image, so call `sleep()` after the last update. Before ESP32 deep sleep, `holdPinsForDeepSleep()` frees the SPI bus and holds RST/CS high and DC/SCLK/MOSI low (`gpio_hold_en` + `gpio_deep_sleep_hold_en`, required for digital pads on classic ESP32) so the lines don't float and RST can't wake the panel. `begin()` drives the same levels and then releases the holds, so it must stay the first driver call after boot.
- Pins are the `kT5V23Pins` default (BUSY 4, RST 16, DC 17, CS 5, SCLK 18, MOSI 23). There is no power-enable GPIO on this revision. SPI uses SPI2_HOST.
- `sdkconfig.defaults` sets `CONFIG_FREERTOS_HZ=1000`, which the driver's short `vTaskDelay` BUSY polling relies on. It also selects the custom partition table, raises the main task stack to 8192 bytes (WiFi, HTTP and cJSON run on it) and disables C++ exceptions, so don't use `throw`/`try`. Report errors through `esp_err_t`.
