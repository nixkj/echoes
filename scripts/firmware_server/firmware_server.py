#!/usr/bin/env python3
"""
Echoes of the Machine - Firmware Update Server

Serves firmware files for ESP32 OTA updates.

Usage:
    python3 firmware_server.py [port]

Default port: 8000

Directory structure:
    $HOME/firmware_server/
    ├── firmware/
    │   ├── version.txt
    │   └── echoes.bin

Changes from v1:
  - Threaded server (ThreadingMixIn) — handles all 50 nodes concurrently
  - Chunked file streaming — .bin is sent in 64 KB chunks rather than loaded
    entirely into RAM; 50 simultaneous downloads no longer exhaust memory
  - Proper thread-safe logging (logging module) instead of bare print()
  - Per-request logging for version.txt checks as well as binary downloads
  - Download statistics: bytes sent, duration, average throughput per transfer
  - Graceful shutdown on Ctrl-C with a final summary of total transfers
"""

import http.server
import socketserver
import os
import sys
import threading
import time
import logging
from datetime import datetime
from pathlib import Path

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

PORT      = 8000 if len(sys.argv) < 2 else int(sys.argv[1])
HOME      = str(Path.home())
BASE_DIR  = os.path.join(HOME, "firmware_server")
FIRMWARE_DIR = os.path.join(BASE_DIR, "firmware")

CHUNK_SIZE = 64 * 1024   # 64 KB per read — keeps memory flat under load

# ---------------------------------------------------------------------------
# Logging (thread-safe by default in the logging module)
# ---------------------------------------------------------------------------

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s | %(levelname)-7s | %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
    handlers=[logging.StreamHandler(sys.stdout)],
)
log = logging.getLogger("firmware")

# ---------------------------------------------------------------------------
# Transfer statistics (protected by a lock)
# ---------------------------------------------------------------------------

_stats_lock        = threading.Lock()
_stats = {
    "version_checks":   0,
    "binary_downloads": 0,
    "bytes_sent":       0,
    "errors":           0,
}

# ---------------------------------------------------------------------------
# Threaded server
# ---------------------------------------------------------------------------

class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    """Each request is handled in its own thread."""
    daemon_threads    = True   # threads die with the main process
    allow_reuse_address = True  # avoid "Address already in use" on restart


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class FirmwareHTTPRequestHandler(http.server.BaseHTTPRequestHandler):
    """Serves firmware files with chunked streaming and detailed logging."""

    # ------------------------------------------------------------------ #
    # GET                                                                  #
    # ------------------------------------------------------------------ #

    def do_GET(self):
        file_path = self._resolve_path()
        if file_path is None:
            self._send_404()
            return

        if not os.path.isfile(file_path):
            self._send_404()
            return

        is_binary  = file_path.endswith(".bin")
        is_version = file_path.endswith(".txt")

        content_type = (
            "application/octet-stream" if is_binary else
            "text/plain"               if is_version else
            "text/html"
        )

        file_size = os.path.getsize(file_path)
        client_ip = self.client_address[0]

        self.send_response(200)
        self.send_header("Content-Type",   content_type)
        self.send_header("Content-Length", str(file_size))
        self.send_header("Access-Control-Allow-Origin",  "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Cache-Control",  "no-cache")
        self.end_headers()

        # ---- Chunked streaming ----------------------------------------
        t_start    = time.monotonic()
        bytes_sent = 0
        try:
            with open(file_path, "rb") as f:
                while True:
                    chunk = f.read(CHUNK_SIZE)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    bytes_sent += len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            # Node disconnected mid-transfer (e.g. it already had this version)
            log.warning(
                f"{client_ip} disconnected after {bytes_sent:,} / {file_size:,} bytes"
                f" of {self.path}"
            )
            with _stats_lock:
                _stats["errors"] += 1
            return

        elapsed   = time.monotonic() - t_start
        kbps      = (bytes_sent / 1024) / elapsed if elapsed > 0 else 0

        # ---- Per-request log line -------------------------------------
        if is_binary:
            log.info(
                f"DOWNLOAD  {client_ip:>15}  {self.path}"
                f"  {bytes_sent/1024:.0f} KB  {elapsed:.1f}s  {kbps:.0f} KB/s"
            )
            with _stats_lock:
                _stats["binary_downloads"] += 1
                _stats["bytes_sent"]       += bytes_sent
        elif is_version:
            log.info(f"VERSION   {client_ip:>15}  {self.path}")
            with _stats_lock:
                _stats["version_checks"] += 1
        else:
            log.info(f"GET       {client_ip:>15}  {self.path}")

    # ------------------------------------------------------------------ #
    # OPTIONS (CORS preflight)                                             #
    # ------------------------------------------------------------------ #

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin",  "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    # ------------------------------------------------------------------ #
    # Helpers                                                              #
    # ------------------------------------------------------------------ #

    def _resolve_path(self):
        """Map the URL path to an absolute filesystem path under BASE_DIR."""
        # Prevent path traversal
        safe = os.path.normpath(self.path.lstrip("/"))
        if safe.startswith(".."):
            return None
        return os.path.join(BASE_DIR, safe)

    def _send_404(self):
        log.warning(f"404       {self.client_address[0]:>15}  {self.path}")
        with _stats_lock:
            _stats["errors"] += 1
        self.send_error(404, "File not found")

    def log_message(self, fmt, *args):
        """Silence the default BaseHTTPServer access log — we do our own."""
        pass

    def log_error(self, fmt, *args):
        log.warning(fmt % args)


# ---------------------------------------------------------------------------
# Filesystem helpers
# ---------------------------------------------------------------------------

def ensure_firmware_directory():
    os.makedirs(FIRMWARE_DIR, exist_ok=True)

    version_file = os.path.join(FIRMWARE_DIR, "version.txt")
    if not os.path.exists(version_file):
        with open(version_file, "w") as f:
            f.write("1.0.0\n")
        log.info(f"Created sample {version_file}")

    binary_file = os.path.join(FIRMWARE_DIR, "echoes.bin")
    if not os.path.exists(binary_file):
        log.warning(f"Firmware binary not found: {binary_file}")
        log.warning("Copy it with: cp build/echoes.bin " + binary_file)

    readme_file = os.path.join(FIRMWARE_DIR, "README.txt")
    if not os.path.exists(readme_file):
        with open(readme_file, "w") as f:
            f.write(f"""ESP32 Firmware Files
====================

Place your firmware files in this directory:

  version.txt  — current firmware version string (e.g. "5.0.3")
  echoes.bin   — compiled firmware binary

To deploy a new build:
  idf.py build
  cp build/echoes.bin {FIRMWARE_DIR}/echoes.bin
  echo "5.0.3" > {FIRMWARE_DIR}/version.txt
""")


def get_local_ip():
    import socket
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "localhost"


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    ensure_firmware_directory()
    local_ip = get_local_ip()

    # Serve from BASE_DIR so URL paths map directly to the filesystem
    os.chdir(BASE_DIR)

    log.info("=" * 60)
    log.info("Echoes of the Machine - Firmware Update Server")
    log.info(f"Port        : {PORT}")
    log.info(f"Base dir    : {BASE_DIR}")
    log.info(f"Version URL : http://{local_ip}:{PORT}/firmware/version.txt")
    log.info(f"Binary URL  : http://{local_ip}:{PORT}/firmware/echoes.bin")
    log.info(f"ota.h lines :")
    log.info(f'  #define OTA_URL     "http://{local_ip}:{PORT}/firmware/echoes.bin"')
    log.info(f'  #define VERSION_URL "http://{local_ip}:{PORT}/firmware/version.txt"')
    log.info("=" * 60)

    try:
        with ThreadedTCPServer(("", PORT), FirmwareHTTPRequestHandler) as httpd:
            log.info(f"Threaded server listening — press Ctrl-C to stop")
            httpd.serve_forever()

    except KeyboardInterrupt:
        log.info("Shutting down...")

    except OSError as e:
        if e.errno in (48, 98):
            log.error(f"Port {PORT} already in use — try: python3 firmware_server.py 8001")
        else:
            log.error(f"OS error: {e}")
        sys.exit(1)

    finally:
        with _stats_lock:
            snap = dict(_stats)
        log.info(
            f"Session summary — "
            f"version checks: {snap['version_checks']}  "
            f"downloads: {snap['binary_downloads']}  "
            f"bytes sent: {snap['bytes_sent']:,}  "
            f"errors: {snap['errors']}"
        )


if __name__ == "__main__":
    main()
