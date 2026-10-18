# Weather station server

Python side of the T5 2.13" e-paper weather station. Two scripts:

- `generator.py` fetches the current weather and the 12 h forecast from OpenWeatherMap,
  renders a 250x122 1-bit image and stores it as a 4000-byte framebuffer file in the
  image directory.
- `server.py` serves the newest image at `GET /latest.bin`.

The payload format, file naming and HTTP behaviour are defined in `../PROTOCOL.md`.

## Setup

Python 3.10 or newer.

    cd server
    python -m venv .venv
    .venv\Scripts\activate          # Linux/macOS: source .venv/bin/activate
    pip install -r requirements.txt
    copy config.example.json config.json   # Linux/macOS: cp ...

Edit `config.json`:

| key              | meaning                                                                  |
|------------------|--------------------------------------------------------------------------|
| `api_key`        | OpenWeatherMap API key (free plan is enough; new keys can take ~2 h to activate) |
| `city_id`        | OWM city id (number or string), e.g. from the city's URL on openweathermap.org |
| `location_name`  | optional display name; `null` uses the name returned by the API          |
| `image_dir`      | output directory; relative paths resolve against the config file's directory |
| `font_path`, `font_bold_path` | optional TTF overrides (relative paths resolve against the config file's directory); `null` uses the bundled DejaVu Sans in `fonts/` |

The fonts are bundled (`fonts/`, DejaVu Sans 2.37, license in `fonts/LICENSE-DejaVu.txt`)
and Pillow is pinned to an exact version, because Pillow wheels ship their own FreeType.
Together these make the image byte-identical on Windows and Linux (verified with
Pillow 13.0.0 on Windows / Python 3.14 and Linux / Python 3.12). To check,
render the fixture on both hosts (with a scratch config whose `image_dir` points at a
scratch directory) and compare `sha256sum` of the two `.bin` files. Do this again
whenever you change the Pillow version.

`config.json` holds your API key and is git-ignored. Do not commit it.

## Generating an image

    python generator.py                          # uses config.json next to the script
    python generator.py --keep-last 2 --preview preview.png
    python generator.py --from-json fixtures/sample.json --preview preview.png   # offline, no key needed

Options: `--config PATH`, `--keep-last N` (default 2), `--from-json FIXTURE`
(`{"current": <weather response>, "forecast": <forecast response>}`), `--preview PNG`.
The exit code is non-zero on any failure (network, HTTP error, bad config), with
the reason on stderr. The previous image stays in place, so the device keeps
getting the last good image.

Screen layout (landscape 250x122): location name and observation date and time (local, `DD.MM HH:MM`) on the
top line; condition icon, large temperature, pressure and humidity in the middle;
four forecast columns (local time, icon, temperature, precipitation for that 3 h step)
at the bottom.

### Scheduling

Generate a little more often than the device wakes up (default device sleep is 30 min).

Linux cron (every 15 minutes):

    */15 * * * * cd /opt/t5weather/server && .venv/bin/python generator.py >> generator.log 2>&1

Windows Task Scheduler (every 15 minutes, run from `cmd`):

    schtasks /Create /TN "T5 weather generator" /SC MINUTE /MO 15 ^
      /TR "\"D:\path\to\server\.venv\Scripts\python.exe\" \"D:\path\to\server\generator.py\""

The generator finds `config.json` next to itself, so the working directory does not matter.

## Running the server

    python server.py                              # 0.0.0.0:8080, image dir from config.json or ./images
    python server.py --image-dir images --host 0.0.0.0 --port 8080

- `GET /latest.bin` returns 200 with exactly 4000 bytes (`Content-Length: 4000`), or
  503 when there is no image yet or the newest one is not 4000 bytes.
- Every other path returns 404.

Allow the port through the firewall and point the device's `server_url` at
`http://<this host>:8080/latest.bin`. To run it permanently, use a systemd service
on Linux or a Task Scheduler task with the trigger "At startup" on Windows.
