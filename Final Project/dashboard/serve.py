#!/usr/bin/env python3
"""
serve.py
========
Trivial static file server for the dashboard (no dependencies).

Run:
    python3 dashboard/serve.py          # http://localhost:8080

The dashboard also works by double-clicking index.html directly — MQTT.js and
DEMO FEED mode do not require a server. This server is only needed if you want
to add CORS-friendly serving or run it on another machine.
"""
from __future__ import annotations

import http.server
import functools
import socketserver
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PORT = 8080


def main() -> None:
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(ROOT))
    with socketserver.TCPServer(("", PORT), handler) as httpd:
        print(f"Dashboard -> http://localhost:{PORT}/")
        httpd.serve_forever()


if __name__ == "__main__":
    main()