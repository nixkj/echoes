#!/usr/bin/env python3
"""
Echoes of the Machine - Startup Report Server

Simple HTTP server that receives startup reports from ESP32 devices
and logs them to a rotating log file.

Usage:
    python3 startup_server.py [--port PORT] [--logdir LOGDIR] [--verbose]

Changes from v1:
  - Threaded server (ThreadingMixIn) — handles concurrent POSTs without queuing
  - Connection-level logging: every TCP connection attempt is recorded, so
    nodes that connect but never send a valid POST are visible in the log
  - Raw-body logging on JSON decode errors — helps diagnose truncated payloads
  - Explicit 500-error logging to startup_reports.log (was only in http_server.log)
  - Connection summary stats logged every 60 s: total connections, reports
    received, errors — useful for spotting nodes that connect but fail silently
  - --known-macs option: supply a file of expected MACs (one per line) and the
    server will periodically log which ones have NOT reported in yet
"""

import json
import logging
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
from datetime import datetime
from pathlib import Path
import argparse
import sys
from logging.handlers import RotatingFileHandler
from collections import defaultdict


# ---------------------------------------------------------------------------
# Global connection statistics (thread-safe via lock)
# ---------------------------------------------------------------------------

_stats_lock = threading.Lock()
_stats = {
    "connections_accepted": 0,   # Every TCP connection that arrived
    "posts_received":       0,   # Every POST to /startup (valid or not)
    "reports_ok":           0,   # Successfully parsed and logged
    "errors_json":          0,   # JSON decode failures
    "errors_server":        0,   # Unexpected exceptions
    "errors_http":          0,   # Wrong path / wrong method / bad content-length
}

# Set of MACs that have successfully reported (populated at runtime)
_reported_macs: set = set()
_reported_macs_lock = threading.Lock()


# ---------------------------------------------------------------------------
# Threaded server
# ---------------------------------------------------------------------------

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    """Handle each request in a separate thread."""
    daemon_threads = True   # Die cleanly on Ctrl-C


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class StartupReportHandler(BaseHTTPRequestHandler):
    """HTTP request handler for startup reports."""

    # ------------------------------------------------------------------ #
    # Connection lifecycle                                                 #
    # ------------------------------------------------------------------ #

    def handle(self):
        """Override to log every TCP connection regardless of what happens next."""
        client_ip = self.client_address[0]
        client_port = self.client_address[1]

        with _stats_lock:
            _stats["connections_accepted"] += 1

        conn_logger = logging.getLogger("connections")
        conn_logger.debug(
            f"TCP connection from {client_ip}:{client_port}"
        )

        try:
            super().handle()
        except Exception as exc:
            conn_logger = logging.getLogger("connections")
            conn_logger.warning(
                f"Connection from {client_ip}:{client_port} dropped with exception: {exc}"
            )

    # ------------------------------------------------------------------ #
    # POST /startup                                                        #
    # ------------------------------------------------------------------ #

    def do_POST(self):
        client_ip   = self.client_address[0]
        client_port = self.client_address[1]
        reports_logger = logging.getLogger("startup_reports")

        with _stats_lock:
            _stats["posts_received"] += 1

        # Only accept POST to /startup
        if self.path != "/startup":
            with _stats_lock:
                _stats["errors_http"] += 1
            reports_logger.warning(
                f"Unexpected path '{self.path}' from {client_ip}:{client_port} — ignored"
            )
            self.send_error(404, "Not Found")
            return

        # Content-Length check
        try:
            content_length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            content_length = 0

        if content_length == 0:
            with _stats_lock:
                _stats["errors_http"] += 1
            reports_logger.warning(
                f"Empty POST body from {client_ip}:{client_port} — "
                "node connected but sent nothing (possible WiFi/TCP instability)"
            )
            self.send_error(400, "Empty request body")
            return

        # Read body
        post_data = self.rfile.read(content_length)

        # ---- Parse JSON ------------------------------------------------
        try:
            data = json.loads(post_data.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            with _stats_lock:
                _stats["errors_json"] += 1
            reports_logger.error(
                f"Invalid JSON from {client_ip}:{client_port} | "
                f"Error: {exc} | "
                f"Raw ({len(post_data)} bytes): {post_data[:200]!r}"
            )
            self.send_error(400, f"Invalid JSON: {exc}")
            return

        # ---- Extract fields --------------------------------------------
        try:
            mac            = data.get("mac",              "UNKNOWN")
            firmware       = data.get("firmware",         "UNKNOWN")
            node_type      = data.get("node_type",        "UNKNOWN")
            avg_light      = float(data.get("avg_light",  -1.0))
            light_samples  = int(data.get("light_samples", 0))
            sleep_duration = int(data.get("sleep_duration_ms", 0))
            has_errors     = bool(data.get("has_errors",  False))
            error_message  = data.get("error_message",    "")

            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

            log_message = (
                f"[{timestamp}] Startup Report | "
                f"MAC: {mac} | "
                f"Firmware: {firmware} | "
                f"Type: {node_type} | "
                f"IP: {client_ip} | "
                f"Light: {avg_light:.2f} lux ({light_samples} samples) | "
                f"Sleep: {sleep_duration} ms | "
                f"Errors: {'YES' if has_errors else 'NO'}"
            )

            if has_errors and error_message:
                log_message += f" | Error: {error_message}"

            if has_errors:
                reports_logger.warning(log_message)
            else:
                reports_logger.info(log_message)

            # Track which MACs have reported
            if mac != "UNKNOWN":
                with _reported_macs_lock:
                    _reported_macs.add(mac)

            with _stats_lock:
                _stats["reports_ok"] += 1

            # ---- Send 200 response -------------------------------------
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            response = {
                "status":    "ok",
                "message":   "Startup report received",
                "timestamp": timestamp,
            }
            self.wfile.write(json.dumps(response).encode("utf-8"))

        except Exception as exc:
            with _stats_lock:
                _stats["errors_server"] += 1
            reports_logger.error(
                f"Exception processing report from {client_ip}:{client_port} | "
                f"{exc} | Raw data: {post_data[:200]!r}",
                exc_info=True,
            )
            self.send_error(500, f"Internal server error: {exc}")

    # ------------------------------------------------------------------ #
    # Silence default BaseHTTPServer stderr chatter                        #
    # ------------------------------------------------------------------ #

    def log_message(self, fmt, *args):
        """Route BaseHTTPServer access log to our http_server logger."""
        logging.getLogger("http_server").debug(fmt % args)

    def log_error(self, fmt, *args):
        """Route BaseHTTPServer errors to our http_server logger."""
        logging.getLogger("http_server").warning(fmt % args)


# ---------------------------------------------------------------------------
# Background stats reporter
# ---------------------------------------------------------------------------

def _stats_reporter(interval_s: int, known_macs: set, logger: logging.Logger):
    """Log connection stats and missing MACs every *interval_s* seconds."""
    while True:
        time.sleep(interval_s)

        with _stats_lock:
            snap = dict(_stats)

        with _reported_macs_lock:
            reported = set(_reported_macs)

        logger.info(
            f"[Stats] Connections: {snap['connections_accepted']} | "
            f"POSTs: {snap['posts_received']} | "
            f"OK: {snap['reports_ok']} | "
            f"JSON errors: {snap['errors_json']} | "
            f"Server errors: {snap['errors_server']} | "
            f"HTTP errors: {snap['errors_http']}"
        )

        if known_macs:
            missing = known_macs - reported
            if missing:
                logger.warning(
                    f"[Missing nodes] {len(missing)} of {len(known_macs)} expected MACs "
                    f"have NOT reported yet: {', '.join(sorted(missing))}"
                )
            else:
                logger.info(
                    f"[Missing nodes] All {len(known_macs)} expected MACs have reported ✓"
                )


# ---------------------------------------------------------------------------
# Logging setup
# ---------------------------------------------------------------------------

def setup_logging(log_dir: Path, verbose: bool = False):
    log_dir.mkdir(parents=True, exist_ok=True)

    fmt_file    = logging.Formatter("%(message)s")
    fmt_console = logging.Formatter("%(asctime)s | %(levelname)-7s | %(message)s")

    console_handler = logging.StreamHandler(sys.stdout)
    console_handler.setLevel(logging.DEBUG if verbose else logging.INFO)
    console_handler.setFormatter(fmt_console)

    def _make_rotating(filename, max_bytes, backup_count):
        h = RotatingFileHandler(
            log_dir / filename,
            maxBytes=max_bytes,
            backupCount=backup_count,
        )
        h.setLevel(logging.DEBUG)
        h.setFormatter(fmt_file)
        return h

    # startup_reports — the main business log
    reports_logger = logging.getLogger("startup_reports")
    reports_logger.setLevel(logging.DEBUG)
    reports_logger.addHandler(_make_rotating("startup_reports.log", 10 * 1024 * 1024, 10))
    reports_logger.addHandler(console_handler)

    # connections — one line per TCP connection (DEBUG level so only in file
    # unless --verbose is passed)
    conn_logger = logging.getLogger("connections")
    conn_logger.setLevel(logging.DEBUG)
    conn_handler = RotatingFileHandler(
        log_dir / "connections.log",
        maxBytes=5 * 1024 * 1024,
        backupCount=5,
    )
    conn_handler.setLevel(logging.DEBUG)
    conn_handler.setFormatter(fmt_file)
    conn_logger.addHandler(conn_handler)
    if verbose:
        conn_logger.addHandler(console_handler)

    # http_server — low-level HTTP chatter
    http_logger = logging.getLogger("http_server")
    http_logger.setLevel(logging.DEBUG)
    http_logger.addHandler(_make_rotating("http_server.log", 5 * 1024 * 1024, 3))
    if verbose:
        http_logger.addHandler(console_handler)

    return reports_logger


# ---------------------------------------------------------------------------
# Server entry point
# ---------------------------------------------------------------------------

def run_server(
    port: int,
    log_dir: Path,
    verbose: bool = False,
    known_macs: set = None,
    stats_interval_s: int = 60,
):
    reports_logger = setup_logging(log_dir, verbose)

    known_macs = known_macs or set()

    reports_logger.info("=" * 70)
    reports_logger.info("Echoes of the Machine - Startup Report Server")
    reports_logger.info(f"Port: {port}  |  Log dir: {log_dir.absolute()}")
    if known_macs:
        reports_logger.info(f"Tracking {len(known_macs)} expected MACs")
    reports_logger.info("=" * 70)

    # Start background stats thread
    stats_thread = threading.Thread(
        target=_stats_reporter,
        args=(stats_interval_s, known_macs, reports_logger),
        daemon=True,
        name="stats-reporter",
    )
    stats_thread.start()

    httpd = ThreadedHTTPServer(("", port), StartupReportHandler)

    reports_logger.info(f"Threaded server listening on http://0.0.0.0:{port}")
    reports_logger.info("Press Ctrl+C to stop")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        reports_logger.info("\nShutting down...")
        httpd.shutdown()

        # Final summary
        with _stats_lock:
            snap = dict(_stats)
        with _reported_macs_lock:
            reported = set(_reported_macs)

        reports_logger.info(
            f"[Final stats] Connections: {snap['connections_accepted']} | "
            f"Reports OK: {snap['reports_ok']} | "
            f"Errors: {snap['errors_json'] + snap['errors_server'] + snap['errors_http']}"
        )
        if known_macs:
            missing = known_macs - reported
            if missing:
                reports_logger.warning(
                    f"[Final] Never reported: {', '.join(sorted(missing))}"
                )
            else:
                reports_logger.info("[Final] All expected MACs reported ✓")

        reports_logger.info("Server stopped.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Startup Report Server for Echoes of the Machine",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Default port 8001, logs in ./logs
  python3 startup_server.py

  # Custom port, custom log dir
  python3 startup_server.py --port 8080 --logdir /var/log/echoes

  # Track specific MACs and report which haven't checked in
  python3 startup_server.py --known-macs macs.txt

  # Verbose (prints connection-level debug to console)
  python3 startup_server.py --verbose
""",
    )

    parser.add_argument("--port",     type=int,  default=8001,         help="Port (default: 8001)")
    parser.add_argument("--logdir",   type=Path, default=Path("./logs"), help="Log directory (default: ./logs)")
    parser.add_argument("--verbose",  action="store_true",              help="Verbose console output")
    parser.add_argument(
        "--known-macs",
        type=Path,
        default=None,
        metavar="FILE",
        help="File of expected MAC addresses (one per line, XX:XX:XX:XX:XX:XX). "
             "Server will log which MACs have not yet reported.",
    )
    parser.add_argument(
        "--stats-interval",
        type=int,
        default=60,
        metavar="SECONDS",
        help="How often to print connection stats (default: 60 s)",
    )

    args = parser.parse_args()

    known_macs = set()
    if args.known_macs:
        try:
            lines = args.known_macs.read_text().splitlines()
            known_macs = {l.strip().upper() for l in lines if l.strip()}
            print(f"Loaded {len(known_macs)} known MACs from {args.known_macs}")
        except Exception as exc:
            print(f"Warning: could not read --known-macs file: {exc}", file=sys.stderr)

    try:
        run_server(args.port, args.logdir, args.verbose, known_macs, args.stats_interval)
    except Exception as exc:
        print(f"Fatal error: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
