# T5 V2.3.x e-paper weather station (ESP-IDF firmware + Python server)

Target: LILYGO T5 V2.3 / V2.3.1 / V2.3.2, 2.13" 250x122 e-paper, classic ESP32 (4 MB flash, no PSRAM).
The e-paper driver was tested on a LILYGO T5 V2.3.x with a DEPG0213BN (SSD1680) panel and ESP-IDF 6.1. Full and partial refresh both work. Other panel variants are untested.

This repo is a mini weather station in two parts. The Python server in `server/` renders the weather into a ready-made 4000-byte framebuffer image. The device wakes, downloads that image, shows it, and goes back to deep sleep. `PROTOCOL.md` is the contract between the two: payload format, HTTP endpoint, config keys and work cycle.

## Architecture

    OpenWeatherMap --> server/generator.py --> server/images/weather_*.bin --> server/server.py --> GET /latest.bin --> ESP32 (T5)
                       (scheduled, e.g. every 15 min)                         (always running)        (wakes every sleep_seconds)

- `server/generator.py` fetches current weather and a 12 h forecast (4 x 3 h), renders a 250x122 landscape 1-bit image with Pillow and writes it in the panel's native framebuffer layout. Old images are pruned (`--keep-last 2`).
- `server/server.py` serves the newest image as exactly 4000 bytes at `GET /latest.bin` (503 if there is none yet).
- The firmware copies the bytes verbatim into the framebuffer, does a full refresh and deep-sleeps. On any failure it shows an error screen instead.

Details: [`server/README.md`](server/README.md) for the server, [`PROTOCOL.md`](PROTOCOL.md) for the contract.

## Quick start: server

Python 3.10+ (full instructions, scheduling with cron or Task Scheduler: [`server/README.md`](server/README.md)).

    cd server
    python -m venv .venv
    .venv\Scripts\activate                 # Linux/macOS: source .venv/bin/activate
    pip install -r requirements.txt
    copy config.example.json config.json   # then set api_key and city_id (Linux/macOS: cp)
    python generator.py --from-json fixtures/sample.json --preview preview.png   # offline smoke test
    python generator.py                    # real data; schedule this every ~15 min
    python server.py                       # http://0.0.0.0:8080/latest.bin

`server/config.json` holds your OpenWeatherMap API key and is git-ignored. Then set the device's `server_url` to `http://<server host>:8080/latest.bin` (see the device setup below).

## Work cycle (every wake is a cold boot)

1. Mount SPIFFS (`storage` partition at `/spiffs`) and read `/spiffs/config.json`.
2. Init NVS and connect to WiFi (about 15 s timeout, a few reconnect attempts).
3. `GET server_url`. The response must be status 200 with exactly 4000 body bytes.
4. Copy the body into the framebuffer (`loadFramebuffer`) and do a full refresh.
5. Put the panel to sleep, stop WiFi, and deep-sleep for `sleep_seconds`.

### Error screen

If any of steps 1-4 fails, the device shows a short error screen in landscape: the title "Weather: error", the cause in large text (for example `WiFi connect failed`, `HTTP 503`, `config.json missing`), a word-wrapped detail line (SSID and WiFi disconnect reason, URL, byte counts, and so on) and the time until the next attempt. It then sleeps the normal period, or 1800 s if the config could not be read. Details are also logged on the serial console. The WiFi password is never logged or displayed.

## Quick start: device

Needs ESP-IDF 6.1 with the environment loaded (`$IDF_PATH`). cJSON is pulled from the component registry (`espressif/cjson`, see `main/idf_component.yml`) on the first configure.

1. Create the device config:

       cp spiffs/config.example.json spiffs/config.json

   Then edit it:

   ```json
   {
     "wifi_ssid": "MyNetwork",
     "wifi_password": "secret",
     "server_url": "http://192.168.1.10:8080/latest.bin",
     "sleep_seconds": 1800
   }
   ```

   All four keys are required. `sleep_seconds` must be a whole number greater than 0. An empty `wifi_password` means an open network. Only plain `http://` URLs are supported. `spiffs/config.json` is git-ignored, and the build stops with a clear message if it is missing.

2. Build, flash and monitor:

       idf.py set-target esp32      # once
       idf.py build
       idf.py flash monitor

   `idf.py flash` writes the app and the SPIFFS image (the `spiffs/` directory) to the `storage` partition. After editing `config.json`, run `idf.py flash` again. If flashing fails, hold BOOT (GPIO0) while pressing RESET.

`sdkconfig.defaults` only applies on a fresh configure. After changing it, delete `sdkconfig` or run `idf.py fullclean`.

## Partitions (`partitions.csv`, 4 MB flash)

| Name     | Type         | Offset   | Size    |
|----------|--------------|----------|---------|
| nvs      | data/nvs     | 0x9000   | 24 KB   |
| phy_init | data/phy     | 0xF000   | 4 KB    |
| factory  | app/factory  | 0x10000  | 2.9 MB  |
| storage  | data/spiffs  | 0x300000 | 256 KB  |

The app is about 1 MB, so the factory partition has plenty of headroom.

## Layout

- `components/epd/` - driver component (`epd213.hpp`, `epd213.cpp`): SPI/GPIO setup, SSD1680 init, full and partial refresh (custom partial LUT), deep sleep, framebuffer drawing, built-in 5x7 font, `loadFramebuffer()` for ready-made images.
- `main/main.cpp` - the work cycle and deep sleep.
- `main/config.*` - SPIFFS mount and `config.json` parsing and validation (cJSON).
- `main/wifi.*` - WiFi station connect and stop.
- `main/http.*` - `esp_http_client` download that requires exactly N bytes.
- `main/error_screen.*` - error screen layout.
- `spiffs/` - contents of the SPIFFS image (`config.example.json`, your `config.json`).
- `server/` - Python generator and HTTP server (`generator.py`, `server.py`, `config.example.json`, `fixtures/sample.json` for offline tests). See [`server/README.md`](server/README.md).
- `PROTOCOL.md` - the server/device contract.

## Pins (see `kT5V23Pins` in `epd213.hpp`)

BUSY 4, RST 16, DC 17, CS 5, SCLK 18, MOSI 23. No power-enable GPIO on this revision (that is V2.4 only).

## Driver notes

- Partial refresh uploads a custom waveform (`kLutPartial`, from GxEPD2's `GxEPD2_213_BN`) with command 0x32 and updates with 0xCC. The controller's built-in mode-2 waveform is not a usable partial update on this panel: it either runs the full 5 s waveform or changes nothing. A partial update takes about 0.7 s, a full one about 5 s. The weather station only uses full refreshes.
- Other panels need their own init sequence and LUT. An older GDEH0213B73 / SSD1675 type most likely needs different update-control values as well.
- Image mirrored in landscape: swap the `nx`/`ny` mapping in `Epd213::drawPixel`. The server's image generator uses the same mapping (`encode_framebuffer()` in `server/generator.py`, see PROTOCOL.md), so change both. The error screen goes through `drawPixel`, so a mirrored image and a mirrored error screen point at the same mapping.
- Always call `sleep()` after the last update.
