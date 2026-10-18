#!/usr/bin/env python3
"""Weather image generator for the LILYGO T5 2.13" e-paper weather station.

Fetches current weather and a 12 h forecast from OpenWeatherMap (free 2.5 API),
renders a 250x122 landscape 1-bit image and stores it in the native framebuffer
format described in ../PROTOCOL.md (4000 bytes, portrait 122x250, 16 bytes/row,
MSB first, bit 1 = white).

Usage:
    python generator.py [--config config.json] [--keep-last 2]
                        [--from-json fixture.json] [--preview out.png]
"""

from __future__ import annotations

import argparse
import datetime as dt
import http.client
import json
import math
import os
import re
import socket
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
except ImportError:  # pragma: no cover
    sys.stderr.write("error: Pillow is not installed (pip install -r requirements.txt)\n")
    sys.exit(2)

SCRIPT_DIR = Path(__file__).resolve().parent

# Display geometry (landscape as seen by the user) and native framebuffer layout.
WIDTH, HEIGHT = 250, 122
NATIVE_W, NATIVE_H = 122, 250
ROW_BYTES = 16
FB_SIZE = ROW_BYTES * NATIVE_H  # 4000

OWM_BASE = "https://api.openweathermap.org/data/2.5"
HTTP_TIMEOUT = 15  # seconds
FORECAST_SLOTS = 4

FILE_RE = re.compile(r"^weather_\d{8}_\d{6}\.bin$")

BLACK, WHITE = 0, 255


class GeneratorError(Exception):
    """Fatal, user-facing error (printed without traceback, exit code 1)."""


# --------------------------------------------------------------------------- config


def load_config(path: Path, required: bool) -> dict:
    if not path.exists():
        if required:
            raise GeneratorError(
                f"config file not found: {path} (copy config.example.json to config.json)"
            )
        return {}
    try:
        with path.open("r", encoding="utf-8") as f:
            cfg = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        raise GeneratorError(f"cannot read config {path}: {e}") from e
    if not isinstance(cfg, dict):
        raise GeneratorError(f"config {path} must contain a JSON object")
    return cfg


def resolve_image_dir(cfg: dict, config_path: Path) -> Path:
    raw = cfg.get("image_dir") or "images"
    p = Path(raw)
    if not p.is_absolute():
        base = config_path.parent if config_path.exists() else SCRIPT_DIR
        p = base / p
    return p


# --------------------------------------------------------------------------- fetching


def _http_get_json(url: str, what: str) -> dict:
    req = urllib.request.Request(url, headers={"User-Agent": "t5-weather-generator/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            body = resp.read()
    except urllib.error.HTTPError as e:
        detail = ""
        try:
            detail = json.loads(e.read().decode("utf-8", "replace")).get("message", "")
        except Exception:
            pass
        raise GeneratorError(
            f"OpenWeatherMap {what} request failed: HTTP {e.code} {e.reason}"
            + (f" ({detail})" if detail else "")
        ) from e
    except urllib.error.URLError as e:
        raise GeneratorError(f"OpenWeatherMap {what} request failed: {e.reason}") from e
    except (socket.timeout, TimeoutError) as e:
        raise GeneratorError(
            f"OpenWeatherMap {what} request timed out after {HTTP_TIMEOUT} s"
        ) from e
    except (OSError, http.client.HTTPException) as e:
        raise GeneratorError(f"OpenWeatherMap {what} request failed: {e!r}") from e
    try:
        data = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise GeneratorError(f"OpenWeatherMap {what} returned invalid JSON: {e}") from e
    if not isinstance(data, dict):
        raise GeneratorError(f"OpenWeatherMap {what} returned unexpected data")
    return data


def fetch_weather(cfg: dict) -> tuple[dict, dict]:
    key = cfg.get("api_key")
    city = cfg.get("city_id")
    if not key or str(key).startswith("<"):
        raise GeneratorError("config: 'api_key' is missing")
    if city in (None, ""):
        raise GeneratorError("config: 'city_id' is missing")
    q_city = urllib.parse.quote(str(city))
    q_key = urllib.parse.quote(str(key))
    current = _http_get_json(
        f"{OWM_BASE}/weather?id={q_city}&units=metric&appid={q_key}", "current weather"
    )
    forecast = _http_get_json(
        f"{OWM_BASE}/forecast?id={q_city}&units=metric&cnt={FORECAST_SLOTS}&appid={q_key}",
        "forecast",
    )
    return current, forecast


def load_fixture(path: Path) -> tuple[dict, dict]:
    try:
        with path.open("r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        raise GeneratorError(f"cannot read fixture {path}: {e}") from e
    if not isinstance(data, dict) or "current" not in data or "forecast" not in data:
        raise GeneratorError(f"fixture {path} must be {{\"current\": ..., \"forecast\": ...}}")
    return data["current"] or {}, data["forecast"] or {}


# --------------------------------------------------------------------------- parsing


def _num(v):
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return f if math.isfinite(f) else None


def _weather0(entry: dict) -> dict:
    w = entry.get("weather") if isinstance(entry, dict) else None
    if isinstance(w, list) and w and isinstance(w[0], dict):
        return w[0]
    return {}


def _is_night(w0: dict) -> bool:
    return str(w0.get("icon", "")).endswith("n")


def parse_weather(current: dict, forecast: dict, location_override=None) -> dict:
    current = current if isinstance(current, dict) else {}
    forecast = forecast if isinstance(forecast, dict) else {}
    main = current.get("main") if isinstance(current.get("main"), dict) else {}
    w0 = _weather0(current)
    city = forecast.get("city") if isinstance(forecast.get("city"), dict) else {}

    tz = _num(city.get("timezone"))
    if tz is None:
        tz = _num(current.get("timezone"))
    tz = int(tz or 0)

    name = location_override or current.get("name") or city.get("name") or "?"

    slots = []
    for item in (forecast.get("list") or [])[:FORECAST_SLOTS]:
        if not isinstance(item, dict):
            continue
        fw0 = _weather0(item)
        fmain = item.get("main") if isinstance(item.get("main"), dict) else {}
        precip = 0.0
        have_precip = False
        for k in ("rain", "snow"):
            v = item.get(k)
            if isinstance(v, dict):
                mm = _num(v.get("3h"))
                if mm is not None:
                    precip += mm
                    have_precip = True
        ts = _num(item.get("dt"))
        slots.append(
            {
                "time": (
                    dt.datetime.fromtimestamp(int(ts) + tz, dt.timezone.utc)
                    if ts is not None
                    else None
                ),
                "temp": _num(fmain.get("temp")),
                "cond": _num(fw0.get("id")),
                "night": _is_night(fw0),
                "precip": precip if have_precip else None,
            }
        )

    ts = _num(current.get("dt"))
    return {
        "name": str(name),
        "temp": _num(main.get("temp")),
        "pressure": _num(main.get("pressure")),
        "humidity": _num(main.get("humidity")),
        "cond": _num(w0.get("id")),
        "night": _is_night(w0),
        "updated": (
            dt.datetime.fromtimestamp(int(ts) + tz, dt.timezone.utc) if ts is not None else None
        ),
        "slots": slots,
    }


def condition_kind(cond_id) -> str:
    """Map an OWM condition id to an icon kind."""
    if cond_id is None:
        return "unknown"
    c = int(cond_id)
    if 200 <= c < 300:
        return "thunder"
    if 300 <= c < 400:
        return "drizzle"
    if 500 <= c < 600:
        return "snow" if c == 511 else "rain"  # 511 = freezing rain
    if 600 <= c < 700:
        return "snow"
    if 700 <= c < 800:
        return "fog"
    if c == 800:
        return "clear"
    if c == 801:
        return "partly"
    if c == 802:
        return "cloud"
    if 803 <= c < 900:
        return "overcast"
    return "unknown"


def fmt_temp(t, unit: str = "°") -> str:
    if t is None:
        return "--" + unit
    v = int(round(t))
    if v == 0:
        v = 0  # avoid "-0"
    return f"{v}{unit}"


# --------------------------------------------------------------------------- fonts

FONT_DIR = SCRIPT_DIR / "fonts"
# Bundled so the output is identical on every host (no system font lookup).
_BUNDLED_FONTS = {False: FONT_DIR / "DejaVuSans.ttf", True: FONT_DIR / "DejaVuSans-Bold.ttf"}


class Fonts:
    def __init__(self, cfg: dict, config_dir: Path = SCRIPT_DIR):
        self._paths = {}
        for bold, key in ((False, "font_path"), (True, "font_bold_path")):
            raw = cfg.get(key)
            # Relative overrides resolve against the config file's directory.
            self._paths[bold] = config_dir / raw if raw else _BUNDLED_FONTS[bold]
        self._cache: dict[tuple[bool, int], ImageFont.FreeTypeFont] = {}

    def get(self, size: int, bold: bool = False):
        key = (bold, size)
        if key not in self._cache:
            path = self._paths[bold]
            try:
                self._cache[key] = ImageFont.truetype(str(path), size)
            except OSError as e:
                raise GeneratorError(f"cannot load font {path}: {e}") from e
        return self._cache[key]


def text_size(draw: ImageDraw.ImageDraw, text: str, font) -> tuple[int, int, int, int]:
    """Return (width, height, x_offset, y_offset) of the inked bbox."""
    l, t, r, b = draw.textbbox((0, 0), text, font=font)
    return r - l, b - t, l, t


def fit_font(draw, text: str, fonts: Fonts, sizes, bold: bool, max_w: int, truncate: bool = True):
    """Largest font from `sizes` in which `text` fits `max_w`.

    If nothing fits: with `truncate`, the smallest size with the text cut and "..."
    appended; without it, the smallest size with the text unchanged (numbers must not be cut).
    """
    for s in sizes:
        f = fonts.get(s, bold)
        if text_size(draw, text, f)[0] <= max_w:
            return f, text
    f = fonts.get(sizes[-1], bold)
    if not truncate:
        return f, text
    ell = "..."  # ASCII: the built-in fallback font has no U+2026 glyph
    t = text
    while t and text_size(draw, t + ell, f)[0] > max_w:
        t = t[:-1]
    t = t.rstrip()
    return f, (t + ell) if t else ""


def draw_text(draw, xy, text, font, anchor="l"):
    """Draw text so that its inked box starts at y and at x (left), x (center) or x (right)."""
    x, y = xy
    w, _h, ox, oy = text_size(draw, text, font)
    if anchor == "c":
        x -= w // 2
    elif anchor == "r":
        x -= w
    draw.text((x - ox, y - oy), text, font=font, fill=BLACK)


# --------------------------------------------------------------------------- icons


def _cloud_parts(x, y, w, h):
    """Ellipses/rects forming a cloud filling box (x,y,w,h). Returns list of (kind, box)."""
    base_top = y + h * 0.45
    r_big = h * 0.42
    r_small = h * 0.30
    return [
        ("rrect", (x, base_top, x + w, y + h)),
        ("ellipse", (x + w * 0.10, y + h * 0.30, x + w * 0.10 + 2 * r_small, y + h * 0.30 + 2 * r_small)),
        ("ellipse", (x + w * 0.32, y, x + w * 0.32 + 2 * r_big * 1.15, y + 2 * r_big)),
    ]


def draw_cloud(draw, x, y, w, h, stroke):
    # Build the cloud silhouette as a mask, then outline = mask minus eroded mask.
    # (Insetting each part separately leaves black wedges at the seams.)
    ox, oy = int(math.floor(x)) - 2, int(math.floor(y)) - 2
    mask = Image.new("L", (int(math.ceil(w)) + 5, int(math.ceil(h)) + 5), 0)
    md = ImageDraw.Draw(mask)
    rad = (h * 0.55) / 2
    for kind, (x0, y0, x1, y1) in _cloud_parts(x - ox, y - oy, w, h):
        if kind == "rrect":
            md.rounded_rectangle((x0, y0, x1, y1), radius=max(1, rad), fill=255)
        else:
            md.ellipse((x0, y0, x1, y1), fill=255)
    inner = mask.filter(ImageFilter.MinFilter(2 * stroke + 1))
    draw.bitmap((ox, oy), mask, fill=BLACK)
    draw.bitmap((ox, oy), inner, fill=WHITE)


def draw_sun(draw, cx, cy, r, stroke, rays=True):
    core = r * 0.55 if rays else r
    draw.ellipse((cx - core, cy - core, cx + core, cy + core), fill=BLACK)
    if rays:
        for i in range(8):
            a = i * math.pi / 4
            r0, r1 = core + max(2, r * 0.15), r
            draw.line(
                (cx + r0 * math.cos(a), cy + r0 * math.sin(a), cx + r1 * math.cos(a), cy + r1 * math.sin(a)),
                fill=BLACK,
                width=stroke,
            )


def draw_moon(draw, cx, cy, r, bg=WHITE):
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=BLACK)
    o = r * 0.55
    draw.ellipse((cx - r + o, cy - r - o * 0.35, cx + r + o, cy + r - o * 0.35), fill=bg)


def draw_icon(draw, kind: str, x: int, y: int, w: int, h: int, night: bool = False):
    """Draw a weather icon of `kind` inside box (x, y, w, h)."""
    s = min(w, h)
    stroke = max(1, round(s / 14))
    cx, cy = x + w / 2, y + h / 2

    def celestial(cx_, cy_, r_):
        if night:
            draw_moon(draw, cx_, cy_, r_ * 0.8)
        else:
            draw_sun(draw, cx_, cy_, r_, stroke)

    # Cloud box used by precipitation icons: upper ~62% of the icon.
    cw, ch = s * 0.95, s * 0.58
    cx0, cy0 = cx - cw / 2, y + (h - s) / 2

    if kind == "clear":
        celestial(cx, cy, s * 0.48)
    elif kind == "partly":
        celestial(x + (w - s) / 2 + s * 0.36, y + (h - s) / 2 + s * 0.34, s * 0.34)
        draw_cloud(draw, cx - s * 0.42, y + (h - s) / 2 + s * 0.38, s * 0.84, s * 0.5, stroke)
    elif kind == "cloud":
        draw_cloud(draw, cx - s * 0.48, cy - s * 0.30, s * 0.96, s * 0.6, stroke)
    elif kind == "overcast":
        draw_cloud(draw, cx - s * 0.30, cy - s * 0.42, s * 0.78, s * 0.48, stroke)
        draw_cloud(draw, cx - s * 0.50, cy - s * 0.16, s * 0.84, s * 0.52, stroke)
    elif kind in ("rain", "drizzle", "snow", "thunder"):
        draw_cloud(draw, cx0, cy0, cw, ch, stroke)
        top = cy0 + ch + max(1, s * 0.05)
        bot = y + (h + s) / 2 - 1
        xs = [cx - s * 0.28, cx, cx + s * 0.28]
        if kind == "rain":
            dx = s * 0.10
            for i, xx in enumerate(xs):
                off = s * 0.06 if i % 2 else 0
                draw.line((xx + dx / 2, top + off, xx - dx / 2, bot - (s * 0.06 - off)), fill=BLACK, width=stroke + (1 if s > 30 else 0))
        elif kind == "drizzle":
            dr = max(1, s * 0.045)
            for i, xx in enumerate(xs):
                for j, yy in enumerate((top + (bot - top) * 0.25, top + (bot - top) * 0.80)):
                    xo = xx + (s * 0.07 if (i + j) % 2 else -s * 0.07)
                    draw.ellipse((xo - dr, yy - dr, xo + dr, yy + dr), fill=BLACK)
        elif kind == "snow":
            fr = max(2, min((bot - top) * 0.40, s * 0.11))
            for i, xx in enumerate(xs):
                yy = (top + bot) / 2 + (-(bot - top) * 0.12 if i % 2 else (bot - top) * 0.12)
                for k in range(3):
                    a = k * math.pi / 3 + math.pi / 2
                    draw.line(
                        (xx - fr * math.cos(a), yy - fr * math.sin(a), xx + fr * math.cos(a), yy + fr * math.sin(a)),
                        fill=BLACK,
                        width=1 if s < 30 else max(1, stroke - 1),
                    )
        else:  # thunder
            bolt_top = cy0 + ch * 0.62
            pts = [
                (cx + s * 0.06, bolt_top),
                (cx - s * 0.16, bolt_top + (bot - bolt_top) * 0.58),
                (cx - s * 0.01, bolt_top + (bot - bolt_top) * 0.55),
                (cx - s * 0.09, bot),
                (cx + s * 0.17, bolt_top + (bot - bolt_top) * 0.38),
                (cx + s * 0.02, bolt_top + (bot - bolt_top) * 0.42),
                (cx + s * 0.14, bolt_top),
            ]
            draw.polygon(pts, fill=BLACK, outline=BLACK)
    elif kind == "fog":
        n = 4
        lw = max(1, stroke)
        for i in range(n):
            yy = y + (h - s) / 2 + s * (0.2 + i * 0.2)
            inset = s * (0.0 if i % 2 == 0 else 0.12)
            draw.line((cx - s * 0.45 + inset, yy, cx + s * 0.45 - (s * 0.12 - inset), yy), fill=BLACK, width=lw)
    else:  # unknown
        draw.ellipse((cx - s * 0.4, cy - s * 0.4, cx + s * 0.4, cy + s * 0.4), outline=BLACK, width=stroke)
        f = ImageFont.truetype(str(_BUNDLED_FONTS[True]), max(8, int(s * 0.5)))
        draw_text(draw, (cx, cy - text_size(draw, "?", f)[1] // 2), "?", f, anchor="c")


# --------------------------------------------------------------------------- rendering


def render(weather: dict, fonts: Fonts) -> Image.Image:
    """Render the landscape 250x122 image. Returns a mode '1' image."""
    img = Image.new("L", (WIDTH, HEIGHT), WHITE)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"  # no anti-aliasing

    # --- header: location name (left) and observation time (right)
    f_small = fonts.get(12, False)
    upd = weather.get("updated")
    upd_txt = upd.strftime("%d.%m %H:%M") if upd else ""
    upd_w = text_size(d, upd_txt, f_small)[0] if upd_txt else 0
    name_max = WIDTH - 4 - (upd_w + 8 if upd_txt else 0)
    f_name, name = fit_font(d, weather["name"], fonts, [13, 12, 11, 10], True, name_max)
    draw_text(d, (2, 1), name, f_name)
    if upd_txt:
        draw_text(d, (WIDTH - 2, 2), upd_txt, f_small, anchor="r")

    # --- current conditions: icon | BIG temperature | pressure / humidity
    top, bottom = 17, 64  # band for current conditions
    icon_s = 44
    draw_icon(d, condition_kind(weather.get("cond")), 2, top + 1, icon_s, icon_s, weather.get("night", False))

    f_info = fonts.get(13, False)
    p = weather.get("pressure")
    hmd = weather.get("humidity")
    p_txt = f"{int(round(p))} hPa" if p is not None else "-- hPa"
    h_txt = f"{int(round(hmd))} %" if hmd is not None else "-- %"
    info_w = max(text_size(d, p_txt, f_info)[0], text_size(d, h_txt, f_info)[0])
    info_x = WIDTH - 2  # right aligned
    draw_text(d, (info_x, top + 9), p_txt, f_info, anchor="r")
    draw_text(d, (info_x, top + 27), h_txt, f_info, anchor="r")

    t_txt = fmt_temp(weather.get("temp"), "°C")
    t_left = 2 + icon_s + 6
    t_max = WIDTH - 2 - info_w - 6 - t_left
    f_temp, t_txt = fit_font(d, t_txt, fonts, [42, 40, 38, 36, 34, 32, 30, 28, 26, 24], True, t_max, truncate=False)
    tw, th, _, _ = text_size(d, t_txt, f_temp)
    t_x = t_left + max(0, (t_max - tw) // 2)
    draw_text(d, (t_x, top + (bottom - top - th) // 2), t_txt, f_temp)

    # --- separator
    sep_y = bottom + 1
    d.line((0, sep_y, WIDTH - 1, sep_y), fill=BLACK, width=1)

    # --- forecast table: 4 columns
    slots = weather.get("slots") or []
    ncol = FORECAST_SLOTS
    col_w = WIDTH / ncol
    f_time = fonts.get(12, True)
    f_ftemp = fonts.get(17, True)
    f_prec = fonts.get(11, False)
    row_top = sep_y + 3
    for i in range(ncol):
        x0 = int(round(i * col_w))
        x1 = int(round((i + 1) * col_w))
        cxc = (x0 + x1) // 2
        if i > 0:
            d.line((x0, sep_y + 1, x0, HEIGHT - 1), fill=BLACK, width=1)
        if i >= len(slots):
            draw_text(d, (cxc, row_top + 18), "--", f_time, anchor="c")
            continue
        sl = slots[i]
        t = sl.get("time")
        draw_text(d, (cxc, row_top), t.strftime("%H:00") if t else "--:--", f_time, anchor="c")
        # icon (left) + temperature (right) on one line
        icon_y = row_top + 14
        isz = 23
        draw_icon(d, condition_kind(sl.get("cond")), x0 + 3, icon_y, isz, isz, sl.get("night", False))
        ft_txt = fmt_temp(sl.get("temp"))
        avail = (x1 - 3) - (x0 + 3 + isz + 3)
        f_t, fitted = fit_font(d, ft_txt, fonts, [17, 16, 15, 14, 13, 12, 11], True, avail, truncate=False)
        if text_size(d, fitted, f_t)[0] > avail and ft_txt.endswith("°"):
            ft_txt = ft_txt[:-1]  # drop the degree sign before overlapping the icon
            f_t, fitted = fit_font(d, ft_txt, fonts, [17, 16, 15, 14, 13, 12, 11], True, avail, truncate=False)
        ft_txt = fitted
        _, fth, _, _ = text_size(d, "0", f_t)
        draw_text(d, (x1 - 3, icon_y + (isz - fth) // 2), ft_txt, f_t, anchor="r")
        # precipitation
        pr = sl.get("precip")
        # OWM omits rain/snow when there is none, so a missing value means 0 mm.
        if pr is not None and pr >= 0.05:
            pr_txt = f"{pr:.1f} mm" if pr < 10 else f"{pr:.0f} mm"
        else:
            pr_txt = "0 mm"
        draw_text(d, (cxc, HEIGHT - 13), pr_txt, f_prec, anchor="c")

    # Hard threshold to 1-bit, no dithering (no grey can leak into the payload).
    return img.point(lambda v: 255 if v >= 128 else 0, mode="L").convert("1", dither=Image.Dither.NONE)


# --------------------------------------------------------------------------- encoding


def encode_framebuffer(img: Image.Image) -> bytes:
    """Encode a landscape 250x122 image into the 4000-byte native framebuffer (PROTOCOL.md).

    Pixels with value < 128 (after conversion to 'L') are black, others white.
    """
    if img.size != (WIDTH, HEIGHT):
        raise ValueError(f"image must be {WIDTH}x{HEIGHT}, got {img.size[0]}x{img.size[1]}")
    if img.mode != "1":
        img = img.convert("L").point(lambda v: 255 if v >= 128 else 0, mode="L")
        img = img.convert("1", dither=Image.Dither.NONE)
    fb = bytearray(b"\xff" * FB_SIZE)  # all white, padding bits stay 1
    px = img.load()
    for y in range(HEIGHT):
        nx = y
        byte_off = nx >> 3
        mask = 0x80 >> (nx & 7)
        inv = ~mask & 0xFF
        for x in range(WIDTH):
            if not px[x, y]:  # black
                ny = 249 - x
                fb[ny * ROW_BYTES + byte_off] &= inv
    return bytes(fb)


# --------------------------------------------------------------------------- output


def write_image(image_dir: Path, payload: bytes, now: dt.datetime | None = None) -> Path:
    if len(payload) != FB_SIZE:
        raise GeneratorError(f"internal error: payload is {len(payload)} bytes, expected {FB_SIZE}")
    image_dir.mkdir(parents=True, exist_ok=True)
    now = now or dt.datetime.now(dt.timezone.utc)
    name = now.strftime("weather_%Y%m%d_%H%M%S.bin")
    final = image_dir / name
    tmp = image_dir / (name + ".tmp")
    try:
        with tmp.open("wb") as f:
            f.write(payload)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, final)
    except OSError as e:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise GeneratorError(f"cannot write {final}: {e}") from e
    return final


def prune(image_dir: Path, keep_last: int) -> list[Path]:
    files = sorted(p for p in image_dir.iterdir() if p.is_file() and FILE_RE.match(p.name))
    removed = []
    for p in files[: max(0, len(files) - keep_last)]:
        try:
            p.unlink()
            removed.append(p)
        except OSError as e:
            print(f"warning: cannot delete {p}: {e}", file=sys.stderr)
    return removed


# --------------------------------------------------------------------------- main


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Render the OpenWeatherMap weather image for the T5 e-paper.")
    ap.add_argument("--config", default=str(SCRIPT_DIR / "config.json"), help="config file (default: config.json next to this script)")
    ap.add_argument("--keep-last", type=int, default=2, help="keep only the N newest images (default 2)")
    ap.add_argument("--from-json", metavar="FIXTURE", help='use {"current":..., "forecast":...} from this file instead of calling the API')
    ap.add_argument("--preview", metavar="PNG", help="also save the landscape image as PNG")
    args = ap.parse_args(argv)

    if args.keep_last < 1:
        ap.error("--keep-last must be >= 1")

    try:
        config_path = Path(args.config).resolve()
        cfg = load_config(config_path, required=not args.from_json)
        if args.from_json:
            current, forecast = load_fixture(Path(args.from_json))
        else:
            current, forecast = fetch_weather(cfg)
        weather = parse_weather(current, forecast, cfg.get("location_name"))
        img = render(weather, Fonts(cfg, config_path.parent))
        payload = encode_framebuffer(img)
        image_dir = resolve_image_dir(cfg, config_path)
        out = write_image(image_dir, payload)
        print(f"wrote {out} ({len(payload)} bytes)")
        if args.preview:
            try:
                img.save(args.preview)
            except (OSError, ValueError) as e:
                raise GeneratorError(f"cannot save preview {args.preview}: {e}") from e
            print(f"preview {args.preview}")
        for p in prune(image_dir, args.keep_last):
            print(f"pruned {p.name}")
    except GeneratorError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
