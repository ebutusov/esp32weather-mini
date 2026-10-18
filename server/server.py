#!/usr/bin/env python3
"""Serve the newest weather framebuffer image to the T5 e-paper device.

GET /latest.bin -> 200, application/octet-stream, Content-Length: 4000
                   503 if no image exists or the newest one is not 4000 bytes
anything else   -> 404
(see ../PROTOCOL.md)

Usage:
    python server.py [--image-dir images] [--host 0.0.0.0] [--port 8080] [--config config.json]
"""

from __future__ import annotations

import argparse
import json
import logging
import re
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
FB_SIZE = 4000
FILE_RE = re.compile(r"^weather_\d{8}_\d{6}\.bin$")

log = logging.getLogger("weather-server")


def newest_image(image_dir: Path) -> Path | None:
    try:
        names = [p.name for p in image_dir.iterdir() if FILE_RE.match(p.name) and p.is_file()]
    except OSError:
        return None
    return image_dir / max(names) if names else None


def read_latest(image_dir: Path) -> tuple[bytes | None, str]:
    """Return (payload, detail). payload is None when there is nothing valid to serve."""
    for _ in range(3):  # the generator may prune the file between listing and reading
        path = newest_image(image_dir)
        if path is None:
            return None, "no image available yet"
        try:
            data = path.read_bytes()
        except FileNotFoundError:
            continue
        except OSError as e:
            return None, f"cannot read {path.name}: {e}"
        if len(data) != FB_SIZE:
            return None, f"{path.name} has {len(data)} bytes, expected {FB_SIZE}"
        return data, path.name
    return None, "image disappeared while reading"


class Handler(BaseHTTPRequestHandler):
    # HTTP/1.0: one request per connection, never chunked.
    protocol_version = "HTTP/1.0"
    server_version = "T5WeatherServer/1.0"
    image_dir: Path = SCRIPT_DIR / "images"

    def _send(self, status: int, body: bytes, ctype: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        path = urllib.parse.urlsplit(self.path).path
        if path != "/latest.bin":
            self._send(404, b"not found\n", "text/plain; charset=utf-8")
            return
        data, detail = read_latest(self.image_dir)
        if data is None:
            log.warning("503: %s", detail)
            self._send(503, (detail + "\n").encode("utf-8"), "text/plain; charset=utf-8")
            return
        log.info("serving %s", detail)
        self._send(200, data, "application/octet-stream")

    def log_message(self, fmt: str, *args) -> None:
        log.info("%s %s", self.address_string(), fmt % args)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Serve the newest weather image over HTTP.")
    ap.add_argument("--image-dir", help="image directory (default: image_dir from config, else ./images next to this script)")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--config", default=str(SCRIPT_DIR / "config.json"), help="generator config (only image_dir is used)")
    args = ap.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    if args.image_dir:
        image_dir = Path(args.image_dir)
    else:
        image_dir = SCRIPT_DIR / "images"
        cfg_path = Path(args.config).resolve()
        if cfg_path.exists():
            try:
                cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
                if isinstance(cfg, dict) and cfg.get("image_dir"):
                    image_dir = Path(cfg["image_dir"])
                    if not image_dir.is_absolute():
                        image_dir = cfg_path.parent / image_dir
            except (OSError, json.JSONDecodeError) as e:
                log.warning("ignoring config %s: %s", cfg_path, e)
    Handler.image_dir = image_dir.resolve()

    try:
        httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    except OSError as e:
        log.error("cannot listen on %s:%d: %s", args.host, args.port, e)
        return 1
    httpd.daemon_threads = True
    log.info("serving %s on http://%s:%d/latest.bin", Handler.image_dir, args.host, args.port)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
