#!/usr/bin/env python3
"""
sniffer_monitor.py — Raspberry Pi companion for the Echoes sniffer node
========================================================================

Reads the JSON Lines stream from the sniffer ESP32 over USB serial,
timestamps every record with wall-clock time, and writes it to a rotating
log file.  Also prints a live summary to stdout.

Usage
-----
    python3 sniffer_monitor.py [options]

Options
-------
    --port   /dev/ttyUSB0   Serial port (default: auto-detect first /dev/ttyUSB*)
    --baud   115200         Baud rate (must match CONFIG_SNIFFER_BAUD_RATE)
    --log    sniffer.jsonl  Output file (JSON Lines, one record per line)
    --filter SOUND          Only print/log frames of this type (default: all)
                            Comma-separated: SOUND,LIGHT — events always shown
    --quiet                 Suppress stdout pretty-print (log file unaffected)

Output file format
------------------
One JSON object per line, with a "wall" field added by this script:

    {"wall":"2025-06-01T14:23:01.123","ts":12345,"mac":"AA:BB:CC:DD:EE:FF",
     "rssi":-65,"ch":6,"type":"SOUND","detection":"WHISTLE","lux":0.00,
     "node":"FULL","wifi_assoc":true,"http_recent":true,"lux_alive":true,"flock":false}

The file is flushed after every record so a Ctrl+C leaves a complete log.

Requirements
------------
    pip3 install pyserial

Auto-detect note
----------------
The script scans /dev/ttyUSB* then /dev/ttyACM* and uses the first match.
Run with --port if you have multiple USB serial devices.
"""

import argparse
import glob
import json
import sys
import time
from datetime import datetime, timezone

try:
    import serial
except ImportError:
    sys.exit("ERROR: pyserial not installed.  Run: pip3 install pyserial")


# ── ANSI colours for terminal output ─────────────────────────────────────────
RESET   = "\033[0m"
BOLD    = "\033[1m"
RED     = "\033[91m"
YELLOW  = "\033[93m"
GREEN   = "\033[92m"
CYAN    = "\033[96m"
MAGENTA = "\033[95m"
DIM     = "\033[2m"

TYPE_COLOUR = {
    "SOUND":     GREEN,
    "LIGHT":     CYAN,
    "STATUS":    MAGENTA,
    "UNKNOWN":   RED,
}

DETECTION_COLOUR = {
    "WHISTLE":  YELLOW,
    "VOICE":    MAGENTA,
    "CLAP":     RED,
    "BIRDSONG": GREEN,
}


def auto_detect_port() -> str:
    for pattern in ("/dev/ttyUSB*", "/dev/ttyACM*"):
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    sys.exit(
        "ERROR: No serial port found.  Plug in the sniffer or use --port /dev/ttyXXX"
    )



def format_flags(record: dict) -> str:
    """
    Return a compact coloured flags summary from status_flags fields.

    Present on all SOUND, LIGHT, and STATUS frames from firmware >= 7.3.0.
    Old firmware transmits reserved=0x00 so all flags will be False/MINIMAL —
    in that case we return an empty string rather than printing misleading data.
    """
    # If none of the flag fields are present the frame is from old firmware
    if "wifi_assoc" not in record:
        return ""

    node        = record.get("node", "?")
    wifi        = record.get("wifi_assoc", False)
    http        = record.get("http_recent", False)
    lux         = record.get("lux_alive", False)
    flock       = record.get("flock", False)

    node_col  = GREEN  if node == "FULL"  else CYAN
    wifi_col  = GREEN  if wifi            else RED
    http_col  = GREEN  if http            else RED
    lux_col   = GREEN  if lux             else RED
    flock_col = YELLOW if flock           else DIM

    parts = [
        f"  {node_col}{node}{RESET}",
        f"  wifi={wifi_col}{'Y' if wifi else 'N'}{RESET}",
        f"  http={http_col}{'Y' if http else 'N'}{RESET}",
        f"  lux={'N/A' if node == 'FULL' else f'{lux_col}{chr(89) if lux else chr(78)}{RESET}'}",
    ]
    if flock:
        parts.append(f"  {flock_col}FLOCK{RESET}")
    return "".join(parts)


def format_status_line(record: dict) -> str:
    """
    Render a STATUS heartbeat as a single diagnostic line.

    STATUS frames are emitted every 30 s from espnow_rx_task regardless of
    whether lux_task or remote_config_task are alive, so they are visible on
    the monitor even when a node has gone quiet.

    Highlights:
      http_recent=N  → node has lost end-to-end HTTP connectivity
      lux_alive=N    → lux_task has stalled (minimal nodes only)
      http_stale_m   → minutes since last good fetch; rising = degrading
      rssi_node      → node's own AP RSSI; compare to rssi_sniffer for asymmetry
      seq            → TX counter; gaps = air loss, NOT a silent node
    """
    ts_str   = format_ts(record)
    is_epoch = record.get("ts_epoch", False)
    epoch_tag = f"{GREEN}●{RESET}" if is_epoch else f"{YELLOW}○{RESET}"

    mac       = record.get("mac", "??:??:??:??:??:??")
    mac_short = mac[-8:]
    rssi_s    = record.get("rssi_sniffer", record.get("rssi", 0))
    ch        = record.get("ch", "?")
    seq       = record.get("seq", "?")
    rssi_node = record.get("rssi_node", 0)
    stale_m   = record.get("http_stale_m", 0)
    uptime_m  = record.get("uptime_m", 0)

    # Colour http_stale_m by severity
    if stale_m == 0:
        stale_str = f"{GREEN}0m{RESET}"
    elif stale_m < 3:
        stale_str = f"{YELLOW}{stale_m}m{RESET}"
    elif stale_m < 255:
        stale_str = f"{RED}{BOLD}{stale_m}m{RESET}"
    else:
        stale_str = f"{RED}{BOLD}≥255m{RESET}"

    # Colour RSSI comparison
    rssi_diff = rssi_node - rssi_s
    diff_str  = (f"{RED}Δ{rssi_diff:+d}{RESET}" if abs(rssi_diff) > 6
                 else f"{DIM}Δ{rssi_diff:+d}{RESET}")

    flags = format_flags(record)

    uptime_h, uptime_min = divmod(uptime_m, 60)
    uptime_str = (f"{uptime_h}h{uptime_min:02d}m" if uptime_h
                  else f"{uptime_m}m")

    return (
        f"{DIM}{ts_str}{RESET} {epoch_tag}"
        f"  {DIM}…{mac_short}{RESET}"
        f"  ch={ch}"
        f"  {MAGENTA}{BOLD}STATUS{RESET}"
        f"  seq={seq}"
        f"  rssi_s={rssi_s:>4} rssi_n={rssi_node:>4} {diff_str}"
        f"  http_stale={stale_str}"
        f"  up={uptime_str}"
        f"{flags}"
    )


def format_stats(record: dict) -> str:
    """Render a STATS record as a compact human-readable block."""
    ts_str    = format_ts(record)
    is_epoch  = record.get("ts_epoch", False)
    epoch_tag = f"{GREEN}\u25cf{RESET}" if is_epoch else f"{YELLOW}\u25cf{RESET}"

    frames   = record.get("frames", 0)
    dropped  = record.get("dropped", 0)
    fps      = record.get("rate_fps", 0.0)
    gap_min  = record.get("gap_min_us")
    gap_p5   = record.get("gap_p5_us")
    gap_med  = record.get("gap_median_us")
    rssi_min = record.get("rssi_min")
    rssi_avg = record.get("rssi_avg")
    nf_avg   = record.get("noise_floor_avg")
    tc       = record.get("type_counts", {})
    sat      = record.get("saturated", False)

    sat_tag  = f" {RED}{BOLD}\u26a0 SATURATED{RESET}" if sat else ""
    drop_tag = f" {RED}dropped={dropped}{RESET}" if dropped else f" {DIM}dropped=0{RESET}"

    # gap_p5 bar: log scale 100 µs (saturated) .. 1 s (idle), 20 chars wide
    gap_bar = ""
    if gap_p5 is not None:
        import math
        lo_log = math.log10(100)
        hi_log = math.log10(1_000_000)
        frac   = min(1.0, max(0.0, (math.log10(max(gap_p5, 100)) - lo_log) /
                                    (hi_log - lo_log)))
        filled  = int(frac * 20)
        bar     = "\u2588" * filled + "\u2591" * (20 - filled)
        colour  = RED if frac < 0.25 else (YELLOW if frac < 0.55 else GREEN)
        gap_bar = f"\n     gap_p5  {colour}[{bar}]{RESET} {gap_p5:>8,} \u00b5s"

    status_n = tc.get('STATUS', 0)
    counts = (f"SOUND={tc.get('SOUND',0)}  LIGHT={tc.get('LIGHT',0)}  "
              f"STATUS={status_n}  "
              f"UNK={tc.get('UNKNOWN',0)}")

    lines = [
        f"{DIM}{ts_str}{RESET} {epoch_tag}  "
        f"{CYAN}{BOLD}\u2500\u2500 STATS \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500{RESET}"
        f"{sat_tag}",
        f"     frames   {BOLD}{frames:>5}{RESET}  ({fps:.1f} fps){drop_tag}",
        f"     types    {counts}",
    ]

    if gap_p5 is not None:
        lines.append(gap_bar)
        lines.append(f"     gap      min={gap_min:,} \u00b5s   median={gap_med:,} \u00b5s")

    if rssi_avg is not None:
        lines.append(f"     signal   rssi avg={rssi_avg} dBm  min={rssi_min} dBm  "
                     f"noise_floor avg={nf_avg} dBm")

    lines.append(f"     {DIM}{chr(0x2500)*48}{RESET}")
    return "\n".join(lines)


def format_ts(record: dict) -> str:
    """
    Return the best human-readable timestamp string for terminal display.

    When ts_epoch=true the ESP32 is using Unix epoch milliseconds from SNTP,
    so we convert to a local HH:MM:SS.mmm string.  When ts_epoch=false the
    value is uptime milliseconds, displayed as +HH:MM:SS.mmm since boot.
    Either way the Pi-annotated "wall" field is always available as a fallback.
    """
    ts       = record.get("ts", 0)
    is_epoch = record.get("ts_epoch", False)

    if is_epoch:
        # Convert Unix epoch ms to local time
        dt = datetime.fromtimestamp(ts / 1000.0)
        return dt.strftime("%H:%M:%S.") + f"{(ts % 1000):03d}"
    else:
        # Uptime ms → +HH:MM:SS.mmm
        total_s, ms = divmod(ts, 1000)
        h, rem = divmod(total_s, 3600)
        m, s   = divmod(rem, 60)
        return f"+{h:02d}:{m:02d}:{s:02d}.{ms:03d}"


def format_line(record: dict, quiet: bool) -> str:
    """Return a coloured one-line summary suitable for terminal output."""
    if quiet:
        return ""

    ts_str   = format_ts(record)
    is_epoch = record.get("ts_epoch", False)
    event    = record.get("event")
    rtype    = record.get("type", "")

    # Dim epoch indicator so it doesn't dominate but is visible
    epoch_tag = f"{GREEN}●{RESET}" if is_epoch else f"{YELLOW}○{RESET}"

    # ── STATS records get their own multi-line renderer ─────────────────────
    if event == "STATS":
        return format_stats(record)

    # ── Lifecycle events (WIFI_CONNECTED, ESPNOW_READY, SNTP_SYNCED, etc.) ───
    if event:
        colour = CYAN if any(k in event for k in ("CONNECTED", "READY", "SYNCED")) else YELLOW
        return (f"{DIM}{ts_str}{RESET} {epoch_tag}  "
                f"{colour}{BOLD}{event}{RESET}  {DIM}{record}{RESET}")

    # ── STATUS heartbeat — dedicated renderer ────────────────────────────────
    if rtype == "STATUS":
        return format_status_line(record)

    # ── Mesh frames (SOUND / LIGHT / UNKNOWN) ─────────────────────────────────
    mac      = record.get("mac", "??:??:??:??:??:??")
    rssi     = record.get("rssi", 0)
    ch       = record.get("ch", "?")
    detect   = record.get("detection")
    lux      = record.get("lux")
    tc       = TYPE_COLOUR.get(rtype, "")
    dc       = DETECTION_COLOUR.get(detect or "", RESET)

    # Shorten MAC for display: show only last 3 octets
    mac_short = mac[-8:]

    parts = [
        f"{DIM}{ts_str}{RESET} {epoch_tag}",
        f"  {DIM}…{mac_short}{RESET}",
        f"  rssi={rssi:>4}",
        f"  ch={ch}",
        f"  {tc}{BOLD}{rtype:<10}{RESET}",
    ]

    if detect:
        parts.append(f"  {dc}{detect}{RESET}")
    if lux is not None and rtype == "LIGHT":
        parts.append(f"  lux={lux:.1f}")
    if rtype == "UNKNOWN":
        parts.append(f"  magic={record.get('magic')} msg_type={record.get('msg_type')}")

    # Health flags (firmware >= 7.3.0 only; empty string for older nodes)
    flags = format_flags(record)
    if flags:
        parts.append(flags)

    return "".join(parts)


def main():
    parser = argparse.ArgumentParser(description="Echoes sniffer monitor")
    parser.add_argument("--port",   default=None,           help="Serial port")
    parser.add_argument("--baud",   default=115200, type=int)
    parser.add_argument("--log",    default="sniffer.jsonl", help="Output log file")
    parser.add_argument("--filter", default=None,
                        help="Comma-separated types to log/display (default: all)")
    parser.add_argument("--quiet",  action="store_true",
                        help="Suppress terminal output")
    args = parser.parse_args()

    port = args.port or auto_detect_port()
    allowed_types = None
    if args.filter:
        allowed_types = {t.strip().upper() for t in args.filter.split(",")}

    print(f"Opening {port} at {args.baud} baud …")
    print(f"Logging to {args.log}")
    if allowed_types:
        print(f"Filter: {allowed_types}")
    print("Press Ctrl+C to stop.\n")

    counts: dict[str, int] = {}
    total = 0
    start_wall = time.monotonic()

    try:
        ser = serial.Serial(port, args.baud, timeout=5)
    except serial.SerialException as exc:
        sys.exit(f"ERROR opening serial port: {exc}")

    try:
        with open(args.log, "a", encoding="utf-8") as logfile:
            for raw_line in ser:
                line = raw_line.decode("utf-8", errors="replace").strip()
                if not line:
                    continue

                # Add wall-clock timestamp
                wall = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3]

                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    # Not JSON (e.g. ESP-IDF boot log lines) — pass through
                    if not args.quiet:
                        print(f"{DIM}[raw] {line}{RESET}")
                    continue

                record["wall"] = wall
                rtype = record.get("type", "")
                event = record.get("event")

                # Apply type filter (lifecycle events always pass through)
                if allowed_types and rtype and rtype not in allowed_types and not event:
                    continue

                # Write to log file (always, filter already applied above)
                logfile.write(json.dumps(record, separators=(",", ":")) + "\n")
                logfile.flush()

                # Update counters
                key = event if event else rtype
                counts[key] = counts.get(key, 0) + 1
                total += 1

                # Terminal output
                summary = format_line(record, args.quiet)
                if summary:
                    print(summary)

                # Periodic stats line every 100 records
                if total % 100 == 0:
                    elapsed = time.monotonic() - start_wall
                    rate = total / elapsed if elapsed > 0 else 0
                    if not args.quiet:
                        print(
                            f"\n{DIM}── {total} frames in {elapsed:.0f}s "
                            f"({rate:.1f}/s)  counts={counts} ──{RESET}\n"
                        )

    except KeyboardInterrupt:
        elapsed = time.monotonic() - start_wall
        print(f"\n\nStopped.  {total} frames in {elapsed:.1f}s")
        print(f"Counts: {counts}")
        print(f"Log: {args.log}")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
