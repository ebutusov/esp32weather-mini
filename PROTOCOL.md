# Server / device contract

This is the single source of truth shared by the Python server (`server/`) and the
ESP32 firmware (`main/`, `components/epd/`). If an implementation needs to deviate,
change this file first.

## Image payload

- Exactly **4000 bytes**, no header, no compression.
- Layout = the driver's native framebuffer (`Epd213::fb_`): portrait 122 x 250,
  **16 bytes per row** (122 px + 6 padding bits), 250 rows, row-major,
  **MSB first** within a byte, **bit 1 = white, bit 0 = black**.
- Padding bits (native x 122..127) are **1** (white).
- The generator draws a **landscape 250 x 122** image (what the user sees with the
  device in `Rotation::Landscape`) and maps each landscape pixel `(x, y)` exactly like
  `Epd213::drawPixel` does for Landscape:

      nx = y
      ny = 249 - x
      byte index = ny * 16 + (nx >> 3)
      bit mask   = 0x80 >> (nx & 7)
      black pixel -> clear the bit, white pixel -> set the bit

- The device copies the payload verbatim into the framebuffer
  (`Epd213::loadFramebuffer(const uint8_t*, size_t)`, returns `ESP_ERR_INVALID_SIZE`
  unless the size is 4000) and calls `refreshFull()`.

## Image directory (generator -> server)

- Default directory: `server/images/` (configurable).
- File name: `weather_YYYYMMDD_HHMMSS.bin` (UTC timestamp of generation).
- The generator writes `<name>.bin.tmp` and then atomically renames it to `<name>.bin`,
  so the server never sees a half-written file.
- After writing, `--keep-last N` (default 2) deletes all but the N newest
  `weather_*.bin` files.
- The server picks the newest `weather_*.bin` by file name and ignores `*.tmp`
  and everything else.

## HTTP (server -> device)

- `GET /latest.bin`
  - `200`, `Content-Type: application/octet-stream`, `Content-Length: 4000`,
    body = the newest image. **No chunked transfer encoding.**
  - `503` (small text body) when no image exists yet, or the newest file is not 4000 bytes.
- Any other path returns `404`.
- Default listen address `0.0.0.0:8080` (configurable via CLI).

## Device configuration: `spiffs/config.json` (flashed to the `storage` SPIFFS partition)

```json
{
  "wifi_ssid": "MyNetwork",
  "wifi_password": "secret",
  "server_url": "http://192.168.1.10:8080/latest.bin",
  "sleep_seconds": 1800
}
```

All four keys are required; `sleep_seconds` must be > 0. The repo holds
`spiffs/config.example.json`; the real `spiffs/config.json` is git-ignored.

## Device work cycle

1. Mount SPIFFS, read and parse `/spiffs/config.json`.
2. Init NVS, connect to WiFi (timeout ~15 s).
3. HTTP GET `server_url`; require status 200 and exactly 4000 body bytes.
4. `loadFramebuffer()` + `refreshFull()` (every wake is a cold boot; no partial refresh).
5. On **any** failure in steps 1-4: clear the framebuffer, draw a short error message
   (cause + detail) with the built-in font in Landscape, `refreshFull()`.
   Then continue with step 6 using the normal sleep period
   (fallback 1800 s if the config could not be read).
6. `display.sleep()`, WiFi off, `esp_sleep_enable_timer_wakeup(sleep_seconds)`,
   `esp_deep_sleep_start()`.

## Generator configuration: `server/config.json`

```json
{
  "api_key": "<OpenWeatherMap API key>",
  "city_id": 756135,
  "location_name": null,
  "image_dir": "images",
  "font_path": null,
  "font_bold_path": null
}
```

- `location_name`: optional display override; default is the `name` from the API.
- `image_dir`: relative paths resolve against the config file's directory.
- `font_path` / `font_bold_path`: optional TTF overrides (relative paths resolve against
  the config file's directory). Default: the bundled `server/fonts/DejaVuSans.ttf` /
  `DejaVuSans-Bold.ttf`. No system font lookup; a font that cannot be loaded is an error.
- The repo holds `server/config.example.json`; the real `server/config.json` is git-ignored.

## OpenWeatherMap

Free 2.5 API, metric units:

- Current: `https://api.openweathermap.org/data/2.5/weather?id={city_id}&units=metric&appid={key}`
- Forecast: `https://api.openweathermap.org/data/2.5/forecast?id={city_id}&units=metric&cnt=4&appid={key}`
  (3-hour steps, so 4 entries = the next 12 h)

Forecast times are shown in the location's local time (`city.timezone` offset, in seconds).
