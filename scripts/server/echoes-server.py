#!/usr/bin/env python3
"""
Echoes of the Machine — Server  (port 8002)

Single process serving all node endpoints:

  GET  /firmware/version.txt   OTA version check
  GET  /firmware/echoes.bin    OTA binary download
  POST /startup                Node boot report (identity + error state)
  GET  /config                 Remote config JSON (60-second heartbeat)
  POST /config                 Config write from dashboard
  GET  /nodes                  Fleet registry JSON (dashboard)
  GET  /                       Fleet dashboard web UI

The node registry is populated by startup POSTs (node_type, firmware, ip)
and kept alive by 60-second config polls (last_seen, poll_count), giving
the fleet dashboard a complete, live picture of all nodes.

Firmware files are served from /opt/echoes/firmware — written there by
build.sh deploy.
"""

from flask import Flask, jsonify, request, render_template_string, send_from_directory
import argparse
import csv
import json
import logging
import os
import threading
import time
import urllib.request
import urllib.error
import urllib.parse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from pathlib import Path
from logging.handlers import RotatingFileHandler

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

def _setup_logging(log_dir: Path, debug: bool = False) -> None:
    log_dir.mkdir(parents=True, exist_ok=True)
    fmt = logging.Formatter("%(asctime)s | %(levelname)-7s | %(message)s",
                            datefmt="%Y-%m-%d %H:%M:%S")
    root = logging.getLogger()
    root.setLevel(logging.DEBUG if debug else logging.INFO)
    # Console
    ch = logging.StreamHandler()
    ch.setFormatter(fmt)
    root.addHandler(ch)
    # Rotating file — general server log
    fh = RotatingFileHandler(log_dir / "echoes-server.log",
                             maxBytes=10 * 1024 * 1024, backupCount=5)
    fh.setFormatter(fmt)
    root.addHandler(fh)

def _setup_startup_log(log_dir: Path) -> logging.Logger:
    """Return a dedicated logger that writes startup reports to startup_reports.log."""
    startup_logger = logging.getLogger("echoes.startup_reports")
    startup_logger.setLevel(logging.INFO)
    startup_logger.propagate = False   # don't duplicate into the root/server log
    sh = RotatingFileHandler(log_dir / "startup_reports.log",
                             maxBytes=10 * 1024 * 1024, backupCount=10)
    sh.setFormatter(logging.Formatter("%(message)s"))  # pre-formatted lines
    startup_logger.addHandler(sh)
    return startup_logger

LOG_DIR = Path(os.environ.get("ECHOES_LOG_DIR", "/var/log/echoes"))
_setup_logging(LOG_DIR)
log = logging.getLogger("echoes")
startup_log = _setup_startup_log(LOG_DIR)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

_HERE         = Path(__file__).parent.resolve()
CONFIG_FILE   = _HERE / "config.json"
REGISTRY_FILE = _HERE / "node_registry.json"
NODES_FILE    = _HERE / "nodes.csv"
FIRMWARE_DIR  = Path("/opt/echoes/firmware")

# ---------------------------------------------------------------------------
# Flask app
# ---------------------------------------------------------------------------

app = Flask(__name__)

# Silence Flask's own request logger — we log manually
import logging as _logging
_logging.getLogger("werkzeug").setLevel(_logging.WARNING)

# ---------------------------------------------------------------------------
# DEFAULT CONFIGURATION
# ---------------------------------------------------------------------------

DEFAULT_CONFIG = {
    "VOLUME": {
        "value": 0.20, "min": 0.01, "max": 1.0, "step": 0.01, "type": "float",
        "description": "Master playback volume for synthesised bird calls. Lower values are quieter and less intrusive; higher values project further in noisy spaces.",
        "unit": "amplitude (0–1)"
    },
    "GAIN": {
        "value": 16.0, "min": 1.0, "max": 64.0, "step": 0.5, "type": "float",
        "description": "Digital microphone pre-amplifier gain applied before detection. Increase if the device misses quiet sounds; decrease if loud environments cause false triggers.",
        "unit": "linear multiplier"
    },
    "WHISTLE_FREQ": {
        "value": 2000, "min": 500, "max": 8000, "step": 50, "type": "int",
        "description": "Centre frequency (Hz) used by the Goertzel detector to recognise whistles. Should match the dominant frequency of the whistle you intend to use.",
        "unit": "Hz"
    },
    "VOICE_FREQ": {
        "value": 200, "min": 80, "max": 1000, "step": 10, "type": "int",
        "description": "Centre frequency (Hz) used to detect human voice / low-frequency sounds. Lower values respond to deeper voices and bass-heavy sounds.",
        "unit": "Hz"
    },
    "WHISTLE_MULTIPLIER": {
        "value": 2.5, "min": 1.2, "max": 10.0, "step": 0.1, "type": "float",
        "description": "Adaptive threshold multiplier for whistle detection. The detector fires when the Goertzel magnitude exceeds the running average multiplied by this value. Higher = less sensitive, fewer false positives.",
        "unit": "× running average"
    },
    "VOICE_MULTIPLIER": {
        "value": 2.5, "min": 1.2, "max": 10.0, "step": 0.1, "type": "float",
        "description": "Adaptive threshold multiplier for voice detection. Equivalent to WHISTLE_MULTIPLIER but applied to the voice frequency band.",
        "unit": "× running average"
    },
    "CLAP_MULTIPLIER": {
        "value": 4.0, "min": 2.0, "max": 15.0, "step": 0.5, "type": "float",
        "description": "Adaptive threshold multiplier for clap/impulse detection. Claps are broadband transients; a higher multiplier reduces false triggers from ambient noise bursts.",
        "unit": "× running average"
    },
    "WHISTLE_CONFIRM": {
        "value": 2, "min": 1, "max": 10, "step": 1, "type": "int",
        "description": "Number of consecutive buffer frames that must exceed the threshold before a whistle detection is confirmed. Increase to require a longer, sustained whistle.",
        "unit": "frames"
    },
    "VOICE_CONFIRM": {
        "value": 3, "min": 1, "max": 10, "step": 1, "type": "int",
        "description": "Consecutive confirmation frames required for voice detection. A higher count reduces false triggers from short percussive sounds at low frequencies.",
        "unit": "frames"
    },
    "CLAP_CONFIRM": {
        "value": 1, "min": 1, "max": 5, "step": 1, "type": "int",
        "description": "Confirmation frames for clap detection. Claps are short by nature so 1 frame is usually correct; increase only in very noisy environments.",
        "unit": "frames"
    },
    "DEBOUNCE_BUFFERS": {
        "value": 20, "min": 5, "max": 100, "step": 1, "type": "int",
        "description": "Minimum number of audio buffer reads between successive detections of the same type. Prevents a single event from triggering multiple bird calls.",
        "unit": "buffer cycles"
    },
    "BIRDSONG_FREQ": {
        "value": 3500, "min": 1000, "max": 8000, "step": 100, "type": "int",
        "description": "Centre frequency (Hz) used by the Goertzel detector to recognise birdsong.",
        "unit": "Hz"
    },
    "BIRDSONG_MULTIPLIER": {
        "value": 2.2, "min": 1.2, "max": 10.0, "step": 0.1, "type": "float",
        "description": "Adaptive threshold multiplier for birdsong detection. Higher = less sensitive.",
        "unit": "x running average"
    },
    "BIRDSONG_HF_RATIO": {
        "value": 1.4, "min": 1.0, "max": 5.0, "step": 0.1, "type": "float",
        "description": "Spectral ratio: high-frequency band must exceed mid-frequency band by at least this factor for birdsong to be confirmed.",
        "unit": "ratio"
    },
    "BIRDSONG_MF_MIN": {
        "value": 0.35, "min": 0.05, "max": 1.0, "step": 0.05, "type": "float",
        "description": "Mid-frequency must be at least this fraction of its own threshold for a birdsong detection to proceed.",
        "unit": "fraction (0-1)"
    },
    "BIRDSONG_CONFIRM": {
        "value": 3, "min": 1, "max": 10, "step": 1, "type": "int",
        "description": "Consecutive confirmation frames required for birdsong detection. Filters brief transients.",
        "unit": "frames"
    },
    "NOISE_FLOOR_WHISTLE": {
        "value": 10000.0, "min": 100.0, "max": 100000.0, "step": 500.0, "type": "float",
        "description": "Absolute minimum Goertzel magnitude for whistle detection, regardless of adaptive threshold. Prevents detections in near-silence.",
        "unit": "magnitude"
    },
    "NOISE_FLOOR_VOICE": {
        "value": 4000.0, "min": 100.0, "max": 100000.0, "step": 500.0, "type": "float",
        "description": "Absolute minimum Goertzel magnitude for voice detection. Lower than whistle because voice energy at 200 Hz is naturally lower amplitude.",
        "unit": "magnitude"
    },
    "NOISE_FLOOR_BIRDSONG": {
        "value": 10000.0, "min": 100.0, "max": 100000.0, "step": 500.0, "type": "float",
        "description": "Absolute minimum Goertzel magnitude for birdsong detection. Guards against false triggers in quiet environments.",
        "unit": "magnitude"
    },
    "LUX_POLL_INTERVAL_MS": {
        "value": 500, "min": 100, "max": 5000, "step": 100, "type": "int",
        "description": "How often the light sensor is polled. 500 ms is a good balance between responsiveness and CPU load. The BH1750 has a ~120 ms measurement time; do not go below 150 ms.",
        "unit": "ms"
    },
    "LUX_CHANGE_THRESHOLD": {
        "value": 1.0, "min": 0.1, "max": 50.0, "step": 0.5, "type": "float",
        "description": "Minimum lux change required to update the bird mapper or Markov chain. Filters out sensor noise in stable lighting conditions.",
        "unit": "lux"
    },
    "LUX_FLASH_THRESHOLD": {
        "value": 30.0, "min": 5.0, "max": 500.0, "step": 5.0, "type": "float",
        "description": "Absolute lux jump in a single poll that triggers an immediate bird-call response (light-flash event). A phone torch at 1 m produces roughly 50–200 lux.",
        "unit": "lux"
    },
    "LUX_FLASH_PERCENT": {
        "value": 0.15, "min": 0.05, "max": 0.90, "step": 0.05, "type": "float",
        "description": "Relative lux change (fraction of current reading) that also triggers a flash event. At 0.15, a 15% increase in ambient light fires a response. Works alongside LUX_FLASH_MIN_ABS.",
        "unit": "fraction (0–1)"
    },
    "LUX_FLASH_MIN_ABS": {
        "value": 15.0, "min": 1.0, "max": 100.0, "step": 1.0, "type": "float",
        "description": "Minimum absolute lux change that must accompany the percentage check for a flash trigger. Prevents micro-fluctuations in very dark rooms from firing events.",
        "unit": "lux"
    },
    "VOLUME_LUX_MIN": {
        "value": 2.0, "min": 0.0, "max": 50.0, "step": 1.0, "type": "float",
        "description": "Lux level at or below which the quietest playback volume (VOLUME_SCALE_MIN) is used. Models a 'quiet at night' behaviour.",
        "unit": "lux"
    },
    "VOLUME_LUX_MAX": {
        "value": 200.0, "min": 10.0, "max": 2000.0, "step": 10.0, "type": "float",
        "description": "Lux level at or above which the loudest playback volume (VOLUME_SCALE_MAX) is used. In a typical indoor space, 200 lux is a well-lit room.",
        "unit": "lux"
    },
    "VOLUME_SCALE_MIN": {
        "value": 0.25, "min": 0.01, "max": 1.0, "step": 0.05, "type": "float",
        "description": "Volume scale factor applied in darkness (at or below VOLUME_LUX_MIN). Final playback amplitude = VOLUME × VOLUME_SCALE_MIN.",
        "unit": "multiplier (0–1)"
    },
    "VOLUME_SCALE_MAX": {
        "value": 1.0, "min": 0.1, "max": 1.0, "step": 0.05, "type": "float",
        "description": "Volume scale factor applied in bright conditions (at or above VOLUME_LUX_MAX). Set below 1.0 to cap maximum output even in bright environments.",
        "unit": "multiplier (0–1)"
    },
    "QUELEA_GAIN": {
        "value": 1.2, "min": 0.1, "max": 4.0, "step": 0.05, "type": "float",
        "description": "Post-process gain applied specifically to the Red-billed Quelea synthesised call. Quelea bypasses the global VOLUME constant so this is its sole loudness control.",
        "unit": "linear multiplier"
    },
    "ESPNOW_LUX_THRESHOLD": {
        "value": 12.0, "min": 1.0, "max": 100.0, "step": 1.0, "type": "float",
        "description": "Minimum lux change between consecutive ESP-NOW light broadcasts. Prevents flooding the mesh network with tiny sensor fluctuations.",
        "unit": "lux"
    },
    "ESPNOW_EVENT_TTL_MS": {
        "value": 30000, "min": 5000, "max": 300000, "step": 1000, "type": "int",
        "description": "How long (ms) a remote ESP-NOW event continues to influence local bird selection before reverting to the node's own light-level defaults.",
        "unit": "ms"
    },
    "ESPNOW_SOUND_THROTTLE_MS": {
        "value": 3000, "min": 500, "max": 30000, "step": 500, "type": "int",
        "description": "Minimum time between consecutive sound-event broadcasts from this node. Prevents flooding the mesh during rapid repeated detections.",
        "unit": "ms"
    },
    "DEBUG_ESPNOW_STATUS": {
        "value": False, "type": "bool",
        "description": "When ON, every node broadcasts a STATUS heartbeat every 30 s carrying RSSI, HTTP staleness, uptime, and health flags. Intended for sniffer-based diagnostics during development or fault investigation. Has no effect on normal audio/light behaviour. Turn OFF during performances to avoid unnecessary air traffic.",
        "unit": "on / off"
    },
    "FLOCK_GRACE_MS": {
        "value": 12000, "min": 0, "max": 60000, "step": 1000, "type": "int",
        "description": "Boot grace period (ms) — flock mode is suppressed for this long after each node starts up. Prevents all nodes from triggering flock simultaneously during a mass restart, which can cause I2S stalls and stuck LEDs. Set to 0 to disable (not recommended).",
        "unit": "ms"
    },
    "FLOCK_MSG_COUNT": {
        "value": 12, "min": 2, "max": 50, "step": 1, "type": "int",
        "description": "Number of ESP-NOW messages that must arrive within FLOCK_WINDOW_MS to trigger flock mode. Max 50 (one per node). Lower values make the flock more reactive; higher values require a denser burst.",
        "unit": "messages"
    },
    "FLOCK_WINDOW_MS": {
        "value": 6000, "min": 1000, "max": 60000, "step": 500, "type": "int",
        "description": "Sliding time window (ms) in which FLOCK_MSG_COUNT messages must arrive to trigger flock mode.",
        "unit": "ms"
    },
    "MARKOV_IDLE_TRIGGER_MS": {
        "value": 45000, "min": 5000, "max": 600000, "step": 5000, "type": "int",
        "description": "Duration of network silence (ms) before the Markov chain autonomously fires a bird call based on the most probable next state. Keeps the installation alive when no one is interacting.",
        "unit": "ms"
    },
    "MARKOV_AUTONOMOUS_COOLDOWN_MS": {
        "value": 15000, "min": 1000, "max": 120000, "step": 1000, "type": "int",
        "description": "Minimum gap (ms) between consecutive autonomous Markov-triggered calls. Prevents the chain from firing repeatedly during a long quiet period.",
        "unit": "ms"
    },
    "FLOCK_HOLD_MS": {
        "value": 10000, "min": 1000, "max": 120000, "step": 1000, "type": "int",
        "description": "How long (ms) flock mode persists after the last qualifying burst before automatically decaying back to normal.",
        "unit": "ms"
    },
    "FLOCK_CALL_GAP_MS": {
        "value": 200, "min": 50, "max": 2000, "step": 50, "type": "int",
        "description": "Minimum silence gap (ms) between consecutive bird calls during flock mode. Keeps individual calls perceptible rather than blending into a sustained tone.",
        "unit": "ms"
    },
    "VU_MAX_BRIGHTNESS": {
        "value": 0.75, "min": 0.1, "max": 1.0, "step": 0.05, "type": "float",
        "description": "Peak LED brightness during bird-call playback VU meter animation. Lower values make the visual response more subtle.",
        "unit": "brightness (0–1)"
    },
    "DEMO_MODE": {
        "value": False, "type": "bool",
        "description": "Documentary / performance mode. When ON, full nodes fire bird calls autonomously at DEMO_INTERVAL_MS intervals with no human input. Each call is broadcast over ESP-NOW so the entire mesh responds and flock mode triggers naturally — producing the rich, multi-node audio of a live audience session. Ideal for unattended recording. Minimal nodes participate via LED and flock strobe; they never play audio regardless of this setting.",
        "unit": "on / off"
    },
    "DEMO_INTERVAL_MS": {
        "value": 15000, "min": 2000, "max": 120000, "step": 1000, "type": "int",
        "description": "Interval between autonomous bird calls on each full node when DEMO_MODE is active. With 25 full nodes at 15 s each, the installation produces roughly 1–2 calls per second across the space. Reduce for denser activity; increase for a calmer, more spaced soundscape. Includes 20% random jitter. Does not affect minimal nodes.",
        "unit": "ms"
    },
    "SILENT_MODE": {
        "value": False, "type": "bool",
        "description": "When ON, all output is suppressed — no sound and no LED activity. The device continues to listen and learn but produces no response. Use for maintenance or overnight quiet hours.",
        "unit": "on / off"
    },
    "SOUND_OFF": {
        "value": False, "type": "bool",
        "description": "When ON, bird-call audio is silenced but LEDs continue to operate normally (ambient glow and VU meter during detection). Useful in quiet settings where visual response is still wanted.",
        "unit": "on / off"
    },
    "QUIET_HOURS_ENABLED": {
        "value": True, "type": "bool",
        "description": "When ON, all nodes enter a fully silent mode (no sound, no LEDs) between QUIET_HOUR_START and QUIET_HOUR_END each day. Uses the server's local time so keep server and installation in the same timezone.",
        "unit": "on / off"
    },
    "QUIET_HOUR_START": {
        "value": 17, "min": 0, "max": 23, "step": 1, "type": "int",
        "description": "Hour (0–23, 24-hour clock) at which the daily quiet period begins. Default 17 = 17:00 (5 pm). The quiet window wraps overnight when START > END.",
        "unit": "hour (0-23)"
    },
    "QUIET_HOUR_END": {
        "value": 8, "min": 0, "max": 23, "step": 1, "type": "int",
        "description": "Hour (0–23) at which the daily quiet period ends and normal operation resumes. Default 8 = 08:00 (8 am). Set equal to START to disable the window without touching QUIET_HOURS_ENABLED.",
        "unit": "hour (0-23)"
    },
}

# ---------------------------------------------------------------------------
# Config persistence
# ---------------------------------------------------------------------------

def load_config() -> dict:
    if not CONFIG_FILE.exists():
        save_config(DEFAULT_CONFIG)
        return dict(DEFAULT_CONFIG)
    try:
        saved = json.loads(CONFIG_FILE.read_text())
        merged = dict(DEFAULT_CONFIG)
        for key, saved_param in saved.items():
            if key in merged:
                merged[key] = dict(merged[key])
                merged[key]["value"] = saved_param.get("value", merged[key]["value"])
        return merged
    except Exception as e:
        log.warning(f"Could not load config: {e} — using defaults")
        return dict(DEFAULT_CONFIG)


def save_config(config: dict) -> None:
    CONFIG_FILE.write_text(json.dumps(config, indent=2))


_config       = load_config()
_last_modified = datetime.now().isoformat()

# Restart flag
RESTART_WINDOW_S      = 90
_restart_pending      = False
_restart_expires      = 0.0
_restart_timestamp    = None
_restart_token        = 0
_restart_delivered    : set = set()   # MACs already logged for current token

# ---------------------------------------------------------------------------
# Node catalogue  (nodes.csv)
# ---------------------------------------------------------------------------
# A static, ordered list of every known node read from nodes.csv at startup.
# The catalogue defines:
#   • the canonical display order for the fleet grid and /nodes API
#   • the expected MAC → ID mapping (so nodes appear immediately, even
#     before they have ever booted and sent a startup report)
#
# CSV columns: id, mac, ip
# Node type is inferred from the id field: numeric → echoes-full,
# alphabetic → echoes-minimal.

_catalogue: list = []          # ordered list of catalogue entries
_mac_to_id: dict = {}          # MAC (upper) → node id string


def _infer_node_type(node_id: str) -> str:
    return "echoes-full" if node_id.isdigit() else "echoes-minimal"


def _load_catalogue() -> None:
    """Read nodes.csv and populate _catalogue / _mac_to_id.

    Rows with id '---' and no MAC are separator markers — they are kept in
    the catalogue (to preserve display order) but skipped for the registry.
    """
    global _catalogue, _mac_to_id
    if not NODES_FILE.exists():
        log.warning(f"Node catalogue not found at {NODES_FILE} — fleet order will be undefined")
        return
    entries = []
    mac_map = {}
    try:
        with open(NODES_FILE, newline="") as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                node_id = row.get("id",  "").strip()
                mac     = row.get("mac", "").strip().upper()
                ip      = row.get("ip",  "").strip()

                # Separator row — no MAC, id starts with '---'
                if node_id.startswith("---") and not mac:
                    entries.append({"separator": True})
                    continue

                if not node_id or not mac:
                    continue

                entries.append({
                    "id":        node_id,
                    "mac":       mac,
                    "ip":        ip,
                    "node_type": _infer_node_type(node_id),
                })
                mac_map[mac] = node_id

        _catalogue = entries
        _mac_to_id = mac_map
        node_count = sum(1 for e in _catalogue if not e.get("separator"))
        sep_count  = len(_catalogue) - node_count
        log.info(f"Node catalogue loaded: {node_count} node(s), {sep_count} separator(s) from {NODES_FILE}")
    except Exception as e:
        log.warning(f"Could not load node catalogue: {e}")


_load_catalogue()


# ---------------------------------------------------------------------------
# Node registry
# ---------------------------------------------------------------------------
# Keyed by MAC (upper-case string).  Two update paths:
#
#   POST /startup  — sets node_type, firmware, ip, increments boot_count
#   GET  /config   — updates last_seen / poll_count (liveness heartbeat)
#
# Both update paths hit the same process, so the dashboard has the
# complete picture: identity from startup + liveness from config polls.
#
# The registry is pre-seeded from the catalogue at startup so that every
# node appears in the fleet grid immediately, even before its first boot.

_nodes: dict = {}
_nodes_lock  = threading.Lock()

# ---------------------------------------------------------------------------
# Node registry persistence
# ---------------------------------------------------------------------------
# Saved to REGISTRY_FILE on every boot report (immediate) and by a background
# thread every REGISTRY_SAVE_INTERVAL_S seconds when liveness data is dirty.
# This coalesces the high-frequency poll writes (up to 50 nodes × every 60 s)
# into at most one disk write per interval — much kinder to SD cards.

REGISTRY_SAVE_INTERVAL_S = 30   # background flush interval
_registry_dirty = False         # set True by _node_poll; cleared by background saver


def _load_nodes() -> None:
    """Populate _nodes: first seed from catalogue stubs, then overlay persisted data."""
    # 1. Seed every catalogue entry as an offline stub so the full fleet is
    #    always visible in the grid, even on a fresh server start.
    #    Separator entries are skipped — they have no MAC and no registry state.
    for entry in _catalogue:
        if entry.get("separator"):
            continue
        mac = entry["mac"]
        _nodes[mac] = {
            "id":           entry["id"],
            "mac":          mac,
            "node_type":    entry["node_type"],
            "firmware":     "unknown",
            "ip":           entry["ip"],
            "first_seen":   None,
            "last_seen":    None,
            "last_seen_ts": None,
            "poll_count":   0,
            "boot_count":   0,
        }

    # 2. Overlay any richer data we already persisted from previous runs.
    if not REGISTRY_FILE.exists():
        log.info("Node registry file not found — starting from catalogue stubs only")
        return
    try:
        saved = json.loads(REGISTRY_FILE.read_text())
        if isinstance(saved, list):
            overlaid = 0
            with _nodes_lock:
                for entry in saved:
                    mac = entry.get("mac", "").strip().upper()
                    if not mac:
                        continue
                    # last_seen_ts is a runtime field — it must NOT survive a
                    # server restart.  If it were restored, the stale monitor
                    # would fire immediately for every node whose previous
                    # session ended more than _STALE_THRESHOLD_S seconds ago,
                    # producing a flood of false-positive warnings on every
                    # restart.  Clearing it here means the stale monitor
                    # ignores a node until it polls the current server instance.
                    entry["last_seen_ts"] = None
                    if mac in _nodes:
                        # Preserve catalogue id; merge everything else.
                        existing_id = _nodes[mac].get("id")
                        _nodes[mac] = entry
                        _nodes[mac]["mac"] = mac          # normalise case
                        if existing_id:
                            _nodes[mac]["id"] = existing_id
                    else:
                        # Node in registry but not in catalogue (e.g. old node).
                        _nodes[mac] = entry
                        _nodes[mac]["mac"] = mac
                    overlaid += 1
            log.info(f"Node registry loaded: {overlaid} saved record(s) merged from {REGISTRY_FILE}")
    except Exception as e:
        log.warning(f"Could not load node registry: {e} — using catalogue stubs only")


def _save_nodes() -> None:
    """Persist current registry snapshot to REGISTRY_FILE (thread-safe)."""
    try:
        with _nodes_lock:
            snapshot = list(_nodes.values())
        REGISTRY_FILE.write_text(json.dumps(snapshot, indent=2))
    except Exception as e:
        log.warning(f"Could not save node registry: {e}")


def _registry_background_saver() -> None:
    """Background thread: flush registry to disk when dirty, every REGISTRY_SAVE_INTERVAL_S."""
    global _registry_dirty
    while True:
        time.sleep(REGISTRY_SAVE_INTERVAL_S)
        if _registry_dirty:
            _save_nodes()
            _registry_dirty = False


# How long without a poll before a node is considered stale.
# 3× the 60-second poll interval gives one missed poll as tolerance.
_STALE_THRESHOLD_S = 180

# How long to suppress repeat STALE warnings for the same node once it has
# already been flagged.  Avoids filling the log with the same warning every
# 60 seconds for a node that is known to be down.
_STALE_REPEAT_S = 300   # re-warn every 5 minutes at most

# Human-readable labels for RTC_DIAG_CAUSE_* constants (must match startup.h).
_CAUSE_LABELS = {
    0: "NONE",
    1: "RCFG_REBOOT",
    2: "REMOTE_CMD",
    3: "ISR_WDT",
    4: "HTTP_STUCK",
}

def _stale_node_monitor() -> None:
    """Background thread: warn when a node stops polling.

    Checks every 60 seconds.  Emits a WARNING the first time a node's
    last_seen_ts goes stale, then suppresses repeats for _STALE_REPEAT_S
    seconds so the log doesn't flood for a node that remains down.
    Logs a recovery INFO when a previously-stale node is seen again.

    State: last_warned maps mac → float timestamp of the last WARNING, or
    None when the node is currently healthy (or has never been warned).

    Using None (not 0) as the healthy sentinel is critical.  If 0 were used,
    `now - 0` equals the current Unix timestamp (~1.7 billion), which is
    always ≥ _STALE_REPEAT_S.  That means a node that recovers and is
    immediately set to 0 would re-trigger STALE on the very next check cycle
    instead of waiting _STALE_REPEAT_S seconds — producing the observed
    pattern of STALE → RECOVERED → STALE every 5 minutes for a healthy node.
    """
    # mac → float timestamp of last STALE warning, or None if currently healthy
    last_warned: dict = {}

    # Small startup grace: don't warn about nodes that haven't polled yet
    # (e.g. server just started, nodes are still booting).
    time.sleep(_STALE_THRESHOLD_S)

    while True:
        time.sleep(60)
        now = time.time()
        with _nodes_lock:
            snapshot = list(_nodes.items())

        for mac, node in snapshot:
            ts = node.get("last_seen_ts")
            if not ts:
                continue   # node has never polled this session — not monitored yet

            silent_s = int(now - ts)
            node_id   = node.get("id", "?")
            node_type = node.get("node_type", "?")
            last_seen = node.get("last_seen", "?")

            if silent_s > _STALE_THRESHOLD_S:
                prev_warn = last_warned.get(mac)   # None if healthy/never-warned
                if prev_warn is None or now - prev_warn >= _STALE_REPEAT_S:
                    log.warning(
                        f"STALE  {mac}  id={node_id}  type={node_type}"
                        f"  last_seen={last_seen}  silent={silent_s}s"
                    )
                    last_warned[mac] = now
            else:
                # Node is healthy — log recovery if it was previously flagged
                if last_warned.get(mac) is not None:
                    log.info(
                        f"RECOVERED  {mac}  id={node_id}  type={node_type}"
                        f"  silent_was={int(now - ts)}s"
                    )
                last_warned[mac] = None   # explicit healthy sentinel


# Load persisted state immediately at import time (works with WSGI runners too)
_load_nodes()

# Start background saver daemon thread
_saver_thread = threading.Thread(target=_registry_background_saver, daemon=True, name="registry-saver")
_saver_thread.start()

# Start stale-node monitor daemon thread
_stale_thread = threading.Thread(target=_stale_node_monitor, daemon=True, name="stale-monitor")
_stale_thread.start()


def _node_startup(mac: str, node_type: str, firmware: str, ip: str) -> None:
    """Record a boot event — sets static identity fields, increments boot_count."""
    now = datetime.now().isoformat(timespec="seconds")
    with _nodes_lock:
        existing = _nodes.get(mac, {})
        # Preserve the catalogue id if already set; fall back to mac fragment.
        node_id = existing.get("id") or _mac_to_id.get(mac, mac[-5:])
        _nodes[mac] = {
            "id":           node_id,
            "mac":          mac,
            "node_type":    node_type or existing.get("node_type") or "unknown",
            "firmware":     firmware  or "unknown",
            "ip":           ip,
            "first_seen":   existing.get("first_seen", now),
            "last_seen":    now,
            "last_seen_ts": time.time(),
            "poll_count":   existing.get("poll_count", 0),
            "boot_count":   existing.get("boot_count", 0) + 1,
        }
    # Boot events are infrequent and important — save immediately.
    _save_nodes()
    log.info(f"STARTUP  {mac}  id={node_id}  type={node_type}  fw={firmware}  ip={ip}")


def _node_poll(mac: str, ip: str) -> None:
    """Record a 60-second config poll — updates liveness and IP fields."""
    global _registry_dirty
    now = datetime.now().isoformat(timespec="seconds")
    with _nodes_lock:
        if mac in _nodes:
            _nodes[mac]["last_seen"]    = now
            _nodes[mac]["last_seen_ts"] = time.time()
            _nodes[mac]["poll_count"]  += 1
            # Always refresh IP — it is available from every HTTP request and may
            # change (DHCP lease renewal) independently of a node reboot.
            if ip:
                _nodes[mac]["ip"] = ip
        else:
            # Node polling before its first startup report (e.g. server restarted).
            # Skeleton entry: node_type/firmware enriched on next node reboot.
            node_id = _mac_to_id.get(mac, mac[-5:])
            _nodes[mac] = {
                "id":           node_id,
                "mac":          mac,
                "node_type":    _infer_node_type(node_id) if node_id in _mac_to_id.values() else "unknown",
                "firmware":     "unknown",
                "ip":           ip or "unknown",
                "first_seen":   now,
                "last_seen":    now,
                "last_seen_ts": time.time(),
                "poll_count":   1,
                "boot_count":   0,
            }
    # Mark dirty — background thread will flush within REGISTRY_SAVE_INTERVAL_S.
    _registry_dirty = True


# ---------------------------------------------------------------------------
# OTA firmware routes
# ---------------------------------------------------------------------------

@app.route("/firmware/<path:filename>")
def serve_firmware(filename):
    """Serve OTA binary and version.txt from /opt/echoes/firmware/."""
    # Guard against path traversal
    safe = os.path.normpath(filename)
    if safe.startswith("..") or safe.startswith("/"):
        return "Forbidden", 403

    full = FIRMWARE_DIR / safe
    if not full.is_file():
        log.warning(f"OTA 404  {request.remote_addr}  /firmware/{filename}")
        return "Not found", 404

    is_bin = filename.endswith(".bin")
    log.info(
        f"OTA {'BIN ' if is_bin else 'VER '}  {request.remote_addr}  "
        f"/firmware/{filename}  ({full.stat().st_size:,} bytes)"
    )
    return send_from_directory(str(FIRMWARE_DIR), safe,
                               mimetype="application/octet-stream" if is_bin else "text/plain")

# ---------------------------------------------------------------------------
# Startup report route
# ---------------------------------------------------------------------------

@app.route("/startup", methods=["POST"])
def startup_report():
    """Receive boot report from a node.  Populates node_type, firmware, IP."""
    log.info("POST /startup received")
    try:
        data         = request.get_json(force=True, silent=True) or {}
        mac          = data.get("mac",           "").strip().upper()
        node_type    = data.get("node_type",     "unknown")
        firmware     = data.get("firmware",      "unknown")
        reset_reason = data.get("reset_reason",  "unknown")
        has_errors   = data.get("has_errors",    False)
        error_msg    = data.get("error_message", "")
        prev_diag    = data.get("prev_diag",     None)   # dict or None
        prev_boot_reason  = data.get("prev_boot_reset_reason", "")   # firmware 7.3.2+
        prev_boot_uptime  = data.get("prev_boot_uptime_s",     None) # firmware 7.3.2+
        ip           = request.remote_addr or "unknown"

        # Warn in the main server log for abnormal resets and hard errors
        if reset_reason in ("TASK_WDT", "INT_WDT", "WDT", "PANIC", "BROWNOUT"):
            log.warning(f"STARTUP  {mac}  type={node_type}  fw={firmware}  ip={ip}"
                        f"  RESET={reset_reason}")
        elif has_errors and error_msg:
            log.warning(f"STARTUP  {mac}  type={node_type}  fw={firmware}  ip={ip}"
                        f"  ERROR: {error_msg}")

        # Log prev_diag whenever present — this is the key diagnostic record
        # that tells us what state a node was in immediately before it reset.
        if prev_diag:
            cause_raw   = prev_diag.get("cause", "?")
            cause_label = _CAUSE_LABELS.get(cause_raw, f"UNKNOWN({cause_raw})")
            failures = prev_diag.get("failures", "?")
            heap     = prev_diag.get("heap",     "?")
            rssi     = prev_diag.get("rssi",     "?")
            uptime   = prev_diag.get("uptime_s", "?")
            log.warning(
                f"PREV_DIAG  {mac}  cause={cause_label}  failures={failures}"
                f"  heap={heap}  rssi={rssi}  uptime={uptime}s"
            )

        # Log prev_boot_reset_reason — fires on EVERY reset including
        # uncontrolled crashes (PANIC, TASK_WDT, INT_WDT) that bypass
        # startup_write_rtc_diag() and would otherwise leave no trace.
        # Only log at WARNING when it indicates a crash — POWERON / SW_RESET
        # are expected and logged at INFO to avoid noise.
        if prev_boot_reason and prev_boot_reason not in ("", "POWERON"):
            uptime_str = f"  prev_uptime={prev_boot_uptime}s" if prev_boot_uptime is not None else ""
            if prev_boot_reason in ("PANIC", "TASK_WDT", "INT_WDT", "WDT", "BROWNOUT"):
                log.warning(
                    f"PREV_BOOT  {mac}  prev_reset={prev_boot_reason}{uptime_str}"
                    f"  ← uncontrolled crash on previous boot"
                )
            else:
                log.info(
                    f"PREV_BOOT  {mac}  prev_reset={prev_boot_reason}{uptime_str}"
                )

        if mac:
            _node_startup(mac, node_type, firmware, ip)

        # Write a line to the dedicated startup report log.
        timestamp  = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        errors_str = error_msg if (has_errors and error_msg) else ("unknown error" if has_errors else "NO")
        diag_str   = ""
        if prev_boot_reason and prev_boot_reason not in ("", "POWERON"):
            uptime_str = f" uptime={prev_boot_uptime}s" if prev_boot_uptime is not None else ""
            diag_str += f" | PrevBoot: {prev_boot_reason}{uptime_str}"
        if prev_diag:
            cause_raw = prev_diag.get("cause", "?")
            diag_str += (f" | PrevDiag: cause={_CAUSE_LABELS.get(cause_raw, 'UNKNOWN(' + str(cause_raw) + ')')}"
                        f" failures={prev_diag.get('failures','?')}"
                        f" heap={prev_diag.get('heap','?')}"
                        f" rssi={prev_diag.get('rssi','?')}"
                        f" uptime={prev_diag.get('uptime_s','?')}s")
        startup_log.info(
            f"[{timestamp}] Startup Report"
            f" | MAC: {mac}"
            f" | Type: {node_type}"
            f" | Firmware: {firmware}"
            f" | IP: {ip}"
            f" | Reset: {reset_reason}"
            f" | Errors: {errors_str}"
            + diag_str
        )

    except Exception as e:
        log.exception(f"Exception in /startup endpoint: {e}")

    # Content-Length is mandatory — ESP-IDF returns ESP_ERR_HTTP_INCOMPLETE_DATA
    # when it is absent (chunked encoding, which BaseHTTPRequestHandler defaults to).
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    body = json.dumps({
        "status":    "ok",
        "message":   "Startup report received",
        "timestamp": timestamp,
    }).encode()
    return app.response_class(
        response=body,
        status=200,
        mimetype="application/json",
        headers={"Content-Length": str(len(body))},
    )

# ---------------------------------------------------------------------------
# Node registry API
# ---------------------------------------------------------------------------

@app.route("/nodes")
def get_nodes():
    """Return fleet registry in catalogue order, with any unknown nodes appended.

    Separator entries from nodes.csv are passed through as
    {"separator": true} objects so the dashboard can render dividers
    between physical groups.
    """
    with _nodes_lock:
        snapshot = dict(_nodes)   # shallow copy under lock

    # Build output in catalogue order first.
    seen_macs = set()
    result = []
    for entry in _catalogue:
        if entry.get("separator"):
            result.append({"separator": True})
            continue
        mac = entry["mac"]
        seen_macs.add(mac)
        result.append(snapshot.get(mac, {
            "id":           entry["id"],
            "mac":          mac,
            "node_type":    entry["node_type"],
            "firmware":     "unknown",
            "ip":           entry["ip"],
            "first_seen":   None,
            "last_seen":    None,
            "last_seen_ts": None,
            "poll_count":   0,
            "boot_count":   0,
        }))

    # Append any nodes that reported in but aren't in the catalogue.
    for mac, node in snapshot.items():
        if mac not in seen_macs:
            result.append(node)

    return jsonify(result)

# ---------------------------------------------------------------------------
# Config API  (unchanged from original server.py)
# ---------------------------------------------------------------------------

@app.route("/config")
def get_config():
    """Return flat key→value map for device consumption (60-second poll)."""
    global _restart_pending, _restart_expires, _restart_timestamp, _restart_delivered

    if _restart_pending and time.time() > _restart_expires:
        _restart_pending = False
        _restart_timestamp = None
        _restart_delivered = set()
        log.info("Restart window expired — flag cleared")

    # Track liveness — MAC sent in X-Device-MAC header (set in remote_config.c)
    mac = request.headers.get("X-Device-MAC", "").strip().upper()
    if mac:
        _node_poll(mac, request.remote_addr or "")

    # Log diagnostic telemetry headers when present (firmware 7.1.1+).
    # These give 60-second-resolution visibility into node health without
    # requiring serial access.  Logged at DEBUG normally, WARNING when
    # consecutive failures > 0 so abnormal nodes stand out in the log.
    heap     = request.headers.get("X-Heap-Free",     "")
    uptime   = request.headers.get("X-Uptime-S",      "")
    failures = request.headers.get("X-Poll-Failures", "")
    rssi     = request.headers.get("X-RSSI",          "")
    if mac and any([heap, uptime, failures, rssi]):
        diag = (f"POLL  {mac}"
                + (f"  heap={heap}"     if heap     else "")
                + (f"  uptime={uptime}s" if uptime   else "")
                + (f"  failures={failures}" if failures else "")
                + (f"  rssi={rssi}"     if rssi     else ""))
        try:
            if int(failures) > 0:
                log.warning(diag)
            else:
                log.debug(diag)   # Changed from debug: heap/uptime/RSSI visible in production log
        except (ValueError, TypeError):
            log.debug(diag)       # Changed from debug: heap/uptime/RSSI visible in production log

    flat = {k: v["value"] for k, v in _config.items()}
    flat["_server_time"]        = time.time()
    flat["_version"]            = _last_modified
    flat["RESTART_REQUESTED"]   = _restart_pending
    flat["RESTART_TOKEN"]       = _restart_token
    flat["QUIET_HOURS_ENABLED"] = _config["QUIET_HOURS_ENABLED"]["value"]
    flat["QUIET_HOUR_START"]    = _config["QUIET_HOUR_START"]["value"]
    flat["QUIET_HOUR_END"]      = _config["QUIET_HOUR_END"]["value"]

    if _restart_pending:
        if mac and mac not in _restart_delivered:
            _restart_delivered.add(mac)
            log.info(f"RESTART token={_restart_token} delivered to {mac}")
    return jsonify(flat)


@app.route("/config/full")
def get_config_full():
    return jsonify(_config)


@app.route("/config", methods=["POST"])
def update_config():
    global _config, _last_modified
    data = request.get_json(force=True)
    if not data:
        return jsonify({"error": "No JSON body"}), 400

    updated, errors = [], []
    for key, new_val in data.items():
        if key not in _config:
            errors.append(f"Unknown key: {key}")
            continue
        param = _config[key]
        try:
            if param["type"] == "bool":
                if isinstance(new_val, str):
                    new_val = new_val.lower() in ("true", "1", "yes")
                else:
                    new_val = bool(new_val)
            elif param["type"] == "int":
                new_val = int(new_val)
            else:
                new_val = float(new_val)
            if param["type"] != "bool" and (new_val < param["min"] or new_val > param["max"]):
                errors.append(f"{key}: {new_val} out of range [{param['min']}, {param['max']}]")
                continue
            _config[key] = dict(param)
            _config[key]["value"] = new_val
            updated.append(key)
        except (ValueError, TypeError) as e:
            errors.append(f"{key}: {e}")

    if updated:
        _last_modified = datetime.now().isoformat()
        save_config(_config)
        log.info(f"Config updated: {updated}")

    return jsonify({"updated": updated, "errors": errors, "version": _last_modified})


@app.route("/config/reset", methods=["POST"])
def reset_config():
    global _config, _last_modified
    _config = dict(DEFAULT_CONFIG)
    _last_modified = datetime.now().isoformat()
    save_config(_config)
    log.info("Config reset to defaults")
    return jsonify({"status": "reset", "version": _last_modified})


@app.route("/restart", methods=["POST"])
def request_restart():
    global _restart_pending, _restart_expires, _restart_timestamp, _restart_token, _restart_delivered
    _restart_pending   = True
    _restart_expires   = time.time() + RESTART_WINDOW_S
    _restart_timestamp = datetime.now().isoformat()
    _restart_token     = int(time.time()) & 0xFFFFFFFF
    _restart_delivered = set()   # clear for new token
    log.info(f"Restart queued token={_restart_token} expires_in={RESTART_WINDOW_S}s")
    return jsonify({"status": "restart_pending", "timestamp": _restart_timestamp,
                    "token": _restart_token, "window_s": RESTART_WINDOW_S})


@app.route("/restart/cancel", methods=["POST"])
def cancel_restart():
    global _restart_pending, _restart_expires, _restart_timestamp, _restart_token, _restart_delivered
    was = _restart_pending
    _restart_pending = False; _restart_expires = 0.0
    _restart_timestamp = None; _restart_token = 0
    _restart_delivered = set()
    return jsonify({"status": "cancelled", "was_pending": was})


@app.route("/restart/status")
def restart_status():
    remaining = max(0.0, _restart_expires - time.time()) if _restart_pending else 0.0
    return jsonify({"pending": _restart_pending, "timestamp": _restart_timestamp,
                    "remaining_s": int(remaining)})


@app.route("/quiet-hours/status")
def quiet_hours_status():
    now     = datetime.now()
    hour    = now.hour
    qs      = _config["QUIET_HOUR_START"]["value"]
    qe      = _config["QUIET_HOUR_END"]["value"]
    enabled = _config["QUIET_HOURS_ENABLED"]["value"]

    if not enabled:
        quiet_now = False
    elif qs < qe:
        quiet_now = qs <= hour < qe
    else:
        quiet_now = hour >= qs or hour < qe

    def _minutes_to(h):
        delta = (h - hour) % 24
        return 0 if delta == 0 else delta * 60 - now.minute

    return jsonify({
        "enabled": enabled, "quiet_now": quiet_now,
        "current_hour": hour, "quiet_start": qs, "quiet_end": qe,
        "next_event": "end" if quiet_now else "start",
        "minutes_to_next_event": _minutes_to(qe if quiet_now else qs),
        "server_time": now.strftime("%H:%M:%S"),
    })

# ---------------------------------------------------------------------------
# Sonoff / Tasmota power control
# ---------------------------------------------------------------------------
# Four Sonoff Basic R4 devices running Tasmota, one per node group.
# Group numbering matches the separator blocks in nodes.csv (top to bottom,
# left to right): group 1 = String 1 = .11, group 4 = String 4 = .14.
#
# The server proxies all requests to Tasmota so the browser avoids CORS
# issues when talking to devices on the same LAN.
#
# Tasmota HTTP API:
#   GET /cm?cmnd=Power         → {"POWER":"ON"} or {"POWER":"OFF"}
#   GET /cm?cmnd=Power%20ON    → turn on
#   GET /cm?cmnd=Power%20OFF   → turn off
#   GET /cm?cmnd=Power%20TOGGLE → flip state

SONOFF_DEVICES = [
    {"id": 1, "label": "String 1", "ip": "192.168.101.11"},
    {"id": 2, "label": "String 2", "ip": "192.168.101.12"},
    {"id": 3, "label": "String 3", "ip": "192.168.101.13"},
    {"id": 4, "label": "String 4", "ip": "192.168.101.14"},
]
SONOFF_TIMEOUT = 3   # seconds


def _tasmota_get(ip: str, cmnd: str) -> dict | None:
    """Send a Tasmota command and return the parsed JSON, or None on failure."""
    url = f"http://{ip}/cm?cmnd={urllib.parse.quote(cmnd)}"
    try:
        with urllib.request.urlopen(url, timeout=SONOFF_TIMEOUT) as resp:
            return json.loads(resp.read())
    except Exception as e:
        # DEBUG not WARNING — timeouts are routine when devices are unreachable
        # and would otherwise flood the log during normal offline operation.
        log.debug(f"Tasmota {ip} [{cmnd}] failed: {e}")
        return None


def _poll_device(dev: dict) -> dict:
    """Poll one Sonoff device; always returns a result dict."""
    data  = _tasmota_get(dev["ip"], "Power")
    state = data.get("POWER", "unknown").upper() if data else "unknown"
    return {"id": dev["id"], "label": dev["label"], "ip": dev["ip"], "state": state}


@app.route("/sonoff/status")
def sonoff_status():
    """Return current ON/OFF state of all four Sonoff devices (parallel, non-blocking)."""
    with ThreadPoolExecutor(max_workers=len(SONOFF_DEVICES)) as pool:
        results = list(pool.map(_poll_device, SONOFF_DEVICES))
    return jsonify(results)


@app.route("/sonoff/<int:device_id>/toggle", methods=["POST"])
def sonoff_toggle(device_id):
    """Toggle one Sonoff device and return its new state."""
    dev = next((d for d in SONOFF_DEVICES if d["id"] == device_id), None)
    if not dev:
        return jsonify({"error": f"Unknown device id {device_id}"}), 404
    data = _tasmota_get(dev["ip"], "Power TOGGLE")
    state = "unknown"
    if data:
        state = data.get("POWER", "unknown").upper()
    log.info(f"Sonoff {device_id} ({dev['ip']}) toggled → {state}")
    return jsonify({"id": device_id, "label": dev["label"], "state": state})


@app.route("/sonoff/<int:device_id>/set", methods=["POST"])
def sonoff_set(device_id):
    """Explicitly turn a device ON or OFF. Body: {"state": "ON"} or {"state": "OFF"}."""
    dev = next((d for d in SONOFF_DEVICES if d["id"] == device_id), None)
    if not dev:
        return jsonify({"error": f"Unknown device id {device_id}"}), 404
    body = request.get_json(force=True, silent=True) or {}
    target = body.get("state", "").upper()
    if target not in ("ON", "OFF"):
        return jsonify({"error": "state must be ON or OFF"}), 400
    data = _tasmota_get(dev["ip"], f"Power {target}")
    state = data.get("POWER", "unknown").upper() if data else "unknown"
    log.info(f"Sonoff {device_id} ({dev['ip']}) set {target} → {state}")
    return jsonify({"id": device_id, "label": dev["label"], "state": state})


# ---------------------------------------------------------------------------
# Web UI — Config page
# ---------------------------------------------------------------------------

HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Echoes of the Machine — Configuration</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Space+Mono:ital,wght@0,400;0,700;1,400&family=Syne:wght@400;600;800&display=swap" rel="stylesheet">
<style>
  :root {
    --bg:       #0a0c0f;
    --surface:  #111318;
    --border:   #1e2330;
    --accent:   #c8f04a;
    --accent2:  #4af0c8;
    --muted:    #3a4055;
    --text:     #dde3f0;
    --text-dim: #6b7898;
    --danger:   #f04a6a;
    --warn:     #f0b84a;
    --font-head: 'Syne', sans-serif;
    --font-mono: 'Space Mono', monospace;
    --radius:   4px;
    --glow: 0 0 20px rgba(200,240,74,0.12);
  }
  :root.light {
    --bg:       #f2f4f8;
    --surface:  #ffffff;
    --border:   #cdd4e8;
    --accent:   #5a8a00;
    --accent2:  #008a6a;
    --muted:    #aab0c8;
    --text:     #1a1f2e;
    --text-dim: #5a6480;
    --danger:   #c01838;
    --warn:     #a05800;
    --glow: 0 0 20px rgba(90,138,0,0.10);
  }
  .theme-toggle { background: none; border: 1px solid var(--border); border-radius: var(--radius); padding: 5px 11px; cursor: pointer; font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); transition: border-color 0.15s, color 0.15s; }
  .theme-toggle:hover { border-color: var(--accent2); color: var(--accent2); }
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: var(--bg); color: var(--text); font-family: var(--font-mono); font-size: 13px; min-height: 100vh; overflow-x: hidden; }
  body::before { content: ''; position: fixed; inset: 0; z-index: 0; background-image: linear-gradient(rgba(200,240,74,0.015) 1px, transparent 1px), linear-gradient(90deg, rgba(200,240,74,0.015) 1px, transparent 1px); background-size: 40px 40px; pointer-events: none; }
  .layout { position: relative; z-index: 1; max-width: 1100px; margin: 0 auto; padding: 0 24px 60px; }
  header { padding: 48px 0 40px; border-bottom: 1px solid var(--border); margin-bottom: 40px; display: flex; align-items: flex-end; justify-content: space-between; flex-wrap: wrap; gap: 16px; }
  .logo-eyebrow { font-size: 10px; letter-spacing: 0.3em; color: var(--accent); text-transform: uppercase; margin-bottom: 8px; opacity: 0.8; }
  .logo-eyebrow a { color: var(--accent2); text-decoration: none; margin-left: 18px; opacity: 0.7; transition: opacity 0.15s; }
  .logo-eyebrow a:hover { opacity: 1; }
  h1 { font-family: var(--font-head); font-size: clamp(22px, 4vw, 38px); font-weight: 800; letter-spacing: -0.02em; line-height: 1; color: #fff; }
  h1 span { color: var(--accent); }
  .header-meta { font-size: 11px; color: var(--text-dim); line-height: 1.8; text-align: right; }
  .header-meta strong { color: var(--accent2); font-weight: 400; }
  .controls-bar { display: flex; align-items: center; gap: 12px; margin-bottom: 32px; flex-wrap: wrap; }
  .filter-input { flex: 1; min-width: 180px; background: var(--surface); border: 1px solid var(--border); color: var(--text); font-family: var(--font-mono); font-size: 12px; padding: 8px 14px; border-radius: var(--radius); outline: none; transition: border-color 0.2s; }
  .filter-input:focus { border-color: var(--accent); }
  .filter-input::placeholder { color: var(--muted); }
  .btn { font-family: var(--font-mono); font-size: 11px; letter-spacing: 0.08em; padding: 8px 18px; border-radius: var(--radius); border: 1px solid; cursor: pointer; transition: all 0.15s; text-transform: uppercase; white-space: nowrap; }
  .btn-primary { background: var(--accent); border-color: var(--accent); color: #0a0c0f; font-weight: 700; }
  .btn-primary:hover { background: #d8ff5a; box-shadow: var(--glow); }
  .btn-ghost { background: transparent; border-color: var(--muted); color: var(--text-dim); }
  .btn-ghost:hover { border-color: var(--danger); color: var(--danger); }
  .btn-ghost2 { background: transparent; border-color: var(--border); color: var(--text-dim); }
  .btn-ghost2:hover { border-color: var(--accent2); color: var(--accent2); }
  .btn-danger { background: transparent; border: 1px solid var(--danger); color: var(--danger); font-family: var(--font-mono); font-size: 11px; padding: 8px 16px; cursor: pointer; border-radius: var(--radius); letter-spacing: 0.05em; transition: background 0.15s, color 0.15s; }
  .btn-danger:hover { background: var(--danger); color: var(--bg); }
  #restart-status { font-size: 11px; color: var(--warn); font-family: var(--font-mono); letter-spacing: 0.05em; }
  .section-label { font-family: var(--font-head); font-size: 11px; font-weight: 600; letter-spacing: 0.25em; text-transform: uppercase; color: var(--text-dim); padding: 6px 0; margin: 32px 0 16px; border-bottom: 1px solid var(--border); display: flex; align-items: center; gap: 10px; }
  .section-label::before { content: ''; display: inline-block; width: 8px; height: 8px; background: var(--accent); border-radius: 50%; }
  .param-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(460px, 1fr)); gap: 2px; }
  .param-card { background: var(--surface); border: 1px solid var(--border); padding: 18px 20px; display: grid; grid-template-columns: 1fr auto; grid-template-rows: auto auto auto; gap: 4px 16px; transition: border-color 0.15s; position: relative; overflow: hidden; }
  .param-card::after { content: ''; position: absolute; top: 0; left: 0; width: 2px; height: 100%; background: var(--accent); opacity: 0; transition: opacity 0.2s; }
  .param-card:hover { border-color: var(--muted); }
  .param-card:hover::after { opacity: 1; }
  .param-card.dirty { border-color: var(--warn); }
  .param-card.dirty::after { background: var(--warn); opacity: 1; }
  .param-key { grid-column: 1; grid-row: 1; font-size: 12px; color: var(--accent2); letter-spacing: 0.04em; align-self: center; }
  .param-unit { grid-column: 2; grid-row: 1; font-size: 10px; color: var(--text-dim); text-align: right; align-self: center; white-space: nowrap; }
  .param-desc { grid-column: 1 / -1; grid-row: 2; font-size: 11px; line-height: 1.6; color: var(--text-dim); }
  .param-controls { grid-column: 1 / -1; grid-row: 3; display: flex; align-items: center; gap: 10px; margin-top: 10px; }
  .param-input { width: 90px; background: var(--bg); border: 1px solid var(--border); color: var(--text); font-family: var(--font-mono); font-size: 13px; padding: 6px 10px; border-radius: var(--radius); outline: none; text-align: right; transition: border-color 0.15s; }
  .param-input:focus { border-color: var(--accent2); }
  .param-range { flex: 1; -webkit-appearance: none; height: 2px; background: var(--muted); border-radius: 1px; outline: none; cursor: pointer; }
  .param-range::-webkit-slider-thumb { -webkit-appearance: none; width: 14px; height: 14px; border-radius: 50%; background: var(--accent); cursor: pointer; transition: transform 0.1s, box-shadow 0.1s; }
  .param-range::-webkit-slider-thumb:hover { transform: scale(1.3); box-shadow: 0 0 8px rgba(200,240,74,0.5); }
  .param-range::-moz-range-thumb { width: 14px; height: 14px; border-radius: 50%; background: var(--accent); border: none; cursor: pointer; }
  .param-default { font-size: 10px; color: var(--muted); white-space: nowrap; }
  #toast { position: fixed; bottom: 32px; right: 32px; background: var(--surface); border: 1px solid var(--border); padding: 14px 20px; border-radius: var(--radius); font-size: 12px; max-width: 320px; transform: translateY(80px); opacity: 0; transition: all 0.3s cubic-bezier(0.34, 1.56, 0.64, 1); z-index: 100; }
  #toast.show { transform: translateY(0); opacity: 1; }
  #toast.success { border-left: 3px solid var(--accent); }
  #toast.error   { border-left: 3px solid var(--danger); }
  #toast.info    { border-left: 3px solid var(--accent2); }
  .status-bar { position: fixed; bottom: 0; left: 0; right: 0; background: rgba(10,12,15,0.92); backdrop-filter: blur(8px); border-top: 1px solid var(--border); padding: 8px 32px; display: flex; align-items: center; gap: 20px; font-size: 11px; color: var(--text-dim); z-index: 50; }
  .status-dot { width: 6px; height: 6px; border-radius: 50%; background: var(--accent); box-shadow: 0 0 8px var(--accent); animation: pulse 2s infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.4; } }
  .status-bar .dirty-count { color: var(--warn); }
  .status-bar .spacer { flex: 1; }
  .toggle-btn { display: inline-flex; align-items: center; gap: 10px; background: none; border: 1px solid var(--muted); border-radius: var(--radius); padding: 7px 14px; cursor: pointer; font-family: var(--font-mono); font-size: 11px; letter-spacing: 0.1em; transition: border-color 0.15s, background 0.15s; }
  .toggle-track { display: inline-block; width: 32px; height: 16px; border-radius: 8px; background: var(--muted); position: relative; transition: background 0.2s; flex-shrink: 0; }
  .toggle-thumb { position: absolute; top: 2px; left: 2px; width: 12px; height: 12px; border-radius: 50%; background: var(--text-dim); transition: transform 0.2s, background 0.2s; }
  .toggle-label { color: var(--text-dim); transition: color 0.15s; }
  .toggle-off .toggle-track { background: var(--muted); }
  .toggle-off .toggle-thumb { transform: translateX(0); background: var(--text-dim); }
  .toggle-off .toggle-label { color: var(--text-dim); }
  .toggle-off { border-color: var(--muted); }
  .toggle-on .toggle-track { background: var(--danger); }
  .toggle-on .toggle-thumb { transform: translateX(16px); background: #fff; }
  .toggle-on .toggle-label { color: var(--danger); font-weight: 700; }
  .toggle-on { border-color: var(--danger); background: rgba(240,74,106,0.08); }

  /* ── Light mode overrides for fleet grid & tooltips ── */
  :root.light .fnode.fonline  { background: #c8f0d4; border-color: #2a8c42; }
  :root.light .fnode.fstale   { background: #fdecc8; border-color: #b07010; }
  :root.light .fnode.foffline { background: #fad4d4; border-color: #c04040; }
  :root.light .fonline  .fnode-label { color: #1a6e30; }
  :root.light .fstale   .fnode-label { color: #7a4800; }
  :root.light .foffline .fnode-label { color: #8c1010; }
  :root.light .fnode-pwr.pon     { background: #c8f0d4; border-color: #1e6e38; }
  :root.light .fnode-pwr.poff    { background: #fad4d4; border-color: #c04040; }
  :root.light .fnode-pwr.punknown { background: #e8eaf0; border-color: var(--muted); }
  :root.light .fnode-pwr.pon  .fnode-pwr-icon { color: #1a6e30; text-shadow: none; }
  :root.light .fnode-pwr.poff .fnode-pwr-icon { color: #8c1010; }
  :root.light .ftooltip { background: #ffffff; box-shadow: 0 8px 24px rgba(0,0,0,0.15); }
  :root.light .fonline  .ftooltip-id .ftt-node-id { color: #1a6e30; }
  :root.light .fstale   .ftooltip-id .ftt-node-id { color: #7a4800; }
  :root.light .foffline .ftooltip-id .ftt-node-id { color: #c01010; }
  :root.light .fonline  .ftooltip-status { color: #1a6e30; background: #c8f0d4; }
  :root.light .fstale   .ftooltip-status { color: #7a4800; background: #fdecc8; }
  :root.light .foffline .ftooltip-status { color: #8c1010; background: #fad4d4; }
  :root.light body::before { background-image: linear-gradient(rgba(90,138,0,0.04) 1px, transparent 1px), linear-gradient(90deg, rgba(90,138,0,0.04) 1px, transparent 1px); }
  .fnode-pwr { position: relative; aspect-ratio: 1; border-radius: 2px; cursor: pointer; border: 1px solid; transition: all 0.15s ease; display: flex; align-items: center; justify-content: center; padding: 0; background: none; }
  .fnode-pwr:hover:not(:disabled) { transform: scale(1.25); z-index: 5; filter: brightness(1.2); }
  .fnode-pwr:active:not(:disabled) { transform: scale(0.95); }
  .fnode-pwr:disabled { cursor: wait; opacity: 0.5; }
  .fnode-pwr.pon     { background: #0d2a1a; border-color: #1e6e38; }
  .fnode-pwr.poff    { background: #1a0d0d; border-color: #5c2424; }
  .fnode-pwr.punknown { background: #111318; border-color: var(--muted); }
  .fnode-pwr-icon { font-size: 9px; line-height: 1; pointer-events: none; user-select: none; }
  .fnode-pwr.pon     .fnode-pwr-icon { color: #3ddc5e; text-shadow: 0 0 5px #3ddc5e88; }
  .fnode-pwr.poff    .fnode-pwr-icon { color: #5c2424; }
  .fnode-pwr.punknown .fnode-pwr-icon { color: var(--muted); }

  /* ── Fleet Grid ── */
  .fleet-section { margin-bottom: 36px; border: 1px solid var(--border); border-radius: var(--radius); background: var(--surface); overflow: hidden; }
  .fleet-header { display: flex; align-items: center; justify-content: space-between; padding: 12px 18px; border-bottom: 1px solid var(--border); flex-wrap: wrap; gap: 8px; }
  .fleet-title { font-family: var(--font-head); font-size: 11px; font-weight: 600; letter-spacing: 0.2em; text-transform: uppercase; color: var(--text-dim); display: flex; align-items: center; gap: 8px; }
  .fleet-title::before { content: ''; display: inline-block; width: 7px; height: 7px; background: var(--accent2); border-radius: 50%; animation: pulse 2s infinite; }
  .fleet-stats { display: flex; gap: 20px; }
  .fstat { display: flex; align-items: baseline; gap: 5px; font-size: 11px; font-family: var(--font-mono); }
  .fstat-val { font-size: 16px; }
  .fstat-val.green { color: #3ddc5e; }
  .fstat-val.amber { color: #e0a020; }
  .fstat-val.red   { color: #e03a3a; }
  .fstat-lbl { color: var(--text-dim); font-size: 10px; letter-spacing: 0.1em; text-transform: uppercase; }
  .fleet-legend { display: flex; gap: 12px; font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); }
  .fleg { display: flex; align-items: center; gap: 4px; }
  .fleg-dot { width: 7px; height: 7px; border-radius: 50%; }
  .fleg-dot.green { background: #3ddc5e; }
  .fleg-dot.amber { background: #e0a020; }
  .fleg-dot.red   { background: #e03a3a; }
  .fleet-body { padding: 14px 18px; overflow-x: auto; -webkit-overflow-scrolling: touch; }
  .fleet-poll { display: flex; align-items: center; gap: 8px; font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); margin-bottom: 10px; }
  .fleet-poll-dot { width: 6px; height: 6px; border-radius: 50%; background: var(--accent2); animation: pulse 2s infinite; }
  .fnode-grid { display: grid; grid-template-columns: repeat(29, 1fr); gap: 2px; min-width: 460px; }
  .fnode { position: relative; aspect-ratio: 1; border-radius: 2px; cursor: default; border: 1px solid transparent; transition: transform 0.12s ease; display: flex; align-items: center; justify-content: center; }
  .fnode:hover { transform: scale(1.25); z-index: 5; }
  .fnode.fonline  { background: #1a5c2a; border-color: #2a8c42; }
  .fnode.fstale   { background: #4a3408; border-color: #7a5010; }
  .fnode.foffline { background: #2a1010; border-color: #5c2424; }
  .fnode-label { font-family: var(--font-mono); font-size: 8px; font-weight: 700; line-height: 1; letter-spacing: 0; pointer-events: none; user-select: none; }
  .fonline  .fnode-label { color: #5dfc7e; }
  .fstale   .fnode-label { color: #e0a020; }
  .foffline .fnode-label { color: #8c4040; }
  /* Column separator — single cell, vertical tick mark, sits between two groups on the same row */
  .fnode-col-sep { aspect-ratio: 1; display: flex; align-items: center; justify-content: center; }
  .fnode-col-sep::after { content: ''; width: 1px; height: 55%; background: var(--border); }
  /* Row break — spans all columns, thin horizontal rule between the two pairs of groups */
  .fnode-row-break { grid-column: 1 / -1; height: 7px; display: flex; align-items: center; gap: 8px; }
  .fnode-row-break::before, .fnode-row-break::after { content: ''; flex: 1; height: 1px; background: var(--border); }
  .fnode-row-break-dot { width: 4px; height: 4px; border-radius: 50%; background: var(--muted); flex-shrink: 0; }
  /* Tooltip — fixed positioning via JS */
  .ftooltip { display: none; position: fixed; background: #090b09; border: 1px solid var(--border); border-radius: 3px; padding: 10px 13px; min-width: 220px; z-index: 9999; pointer-events: none; box-shadow: 0 8px 24px rgba(0,0,0,0.7); font-size: 12px; }
  .ftooltip-hdr { display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px; padding-bottom: 5px; border-bottom: 1px solid var(--border); }
  .ftooltip-id { font-family: var(--font-mono); font-size: 13px; color: var(--text); }
  .ftooltip-id .ftt-node-id { font-size: 16px; font-weight: 700; margin-right: 6px; }
  .fonline  .ftooltip-id .ftt-node-id { color: #5dfc7e; }
  .fstale   .ftooltip-id .ftt-node-id { color: #e0a020; }
  .foffline .ftooltip-id .ftt-node-id { color: #e03a3a; }
  .ftooltip-status { font-size: 10px; letter-spacing: 0.12em; text-transform: uppercase; padding: 1px 5px; border-radius: 2px; }
  .fonline  .ftooltip-status { color: #3ddc5e; background: #1a5c2a; }
  .fstale   .ftooltip-status { color: #e0a020; background: #4a3408; }
  .foffline .ftooltip-status { color: #e03a3a; background: #5c1a1a; }
  .ftooltip-row { display: flex; justify-content: space-between; gap: 10px; margin-top: 4px; font-size: 11px; }
  .ftooltip-key { color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.08em; }
  .ftooltip-val { font-family: var(--font-mono); color: var(--text); text-align: right; }
  .ftooltip-ts { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); margin-top: 5px; padding-top: 5px; border-top: 1px solid var(--border); word-break: break-all; }
</style>
</head>
<body>
<div class="layout">
  <header>
    <div class="logo-block">
      <div class="logo-eyebrow">ESP32 Mesh Installation</div>
      <h1>Echoes of the <span>Machine</span></h1>
    </div>
    <div class="header-meta">
      Configuration Server<br>
      Last saved: <strong id="last-saved">—</strong><br>
      <span id="param-count">0</span> parameters &nbsp;
      <button class="theme-toggle" id="theme-btn" onclick="toggleTheme()">☀ Light</button>
    </div>
  </header>

  <!-- ── Fleet Grid ── -->
  <div class="fleet-section">
    <div class="fleet-header">
      <div class="fleet-title">Fleet Monitor — 50 Devices</div>
      <div class="fleet-stats">
        <div class="fstat"><span class="fstat-val green" id="fs-online">—</span></div>
        <div class="fstat"><span class="fstat-val amber" id="fs-stale">—</span></div>
        <div class="fstat"><span class="fstat-val red" id="fs-offline">—</span></div>
      </div>
      <div class="fleet-legend">
        <span class="fleg"><span class="fleg-dot green"></span>Online</span>
        <span class="fleg"><span class="fleg-dot amber"></span>Stale</span>
        <span class="fleg"><span class="fleg-dot red"></span>Offline</span>
      </div>
    </div>
    <div class="fleet-body">
      <div class="fleet-poll"><div class="fleet-poll-dot"></div><span id="fleet-poll-label">Live · refreshing every 15 s · STALE &gt;90 s · OFFLINE &gt;180 s</span></div>
      <div class="fnode-grid" id="fleetGrid"></div>
    </div>
  </div>

  <div id="quiet-hours-banner" style="display:none;margin-bottom:24px;padding:12px 18px;border-radius:4px;border:1px solid var(--muted);font-family:var(--font-mono);font-size:12px;"></div>

  <div class="controls-bar">
    <input class="filter-input" type="text" id="filter-input" placeholder="Filter parameters…">
    <button class="btn btn-ghost" onclick="confirmReset()">Reset defaults</button>
    <button class="btn btn-primary" id="save-btn" onclick="saveAll()">Save changes</button>
    <button class="btn btn-danger" id="restart-btn" onclick="requestRestart()"
      title="Queue a reboot — nodes restart at next poll (within 60 s)">
      ⟳ Restart all nodes
    </button>
    <span id="restart-status"></span>
  </div>

  <div id="param-sections"></div>
</div>

<div id="toast"></div>
<div class="status-bar">
  <div class="status-dot"></div>
  <span>Server running</span>
  <span class="spacer"></span>
  <span id="dirty-indicator"></span>
  <span>Devices poll every 60 s</span>
</div>

<script>
const SECTIONS = {
  "Documentary Mode": ["DEMO_MODE","DEMO_INTERVAL_MS"],
  "Output Switches":  ["SILENT_MODE","SOUND_OFF"],
  "Quiet Hours":      ["QUIET_HOURS_ENABLED","QUIET_HOUR_START","QUIET_HOUR_END"],
  "Audio Detection":  ["GAIN","WHISTLE_FREQ","VOICE_FREQ","BIRDSONG_FREQ","NOISE_FLOOR_WHISTLE","NOISE_FLOOR_VOICE","NOISE_FLOOR_BIRDSONG"],
  "Playback Volume":  ["VOLUME","VOLUME_LUX_MIN","VOLUME_LUX_MAX","VOLUME_SCALE_MIN","VOLUME_SCALE_MAX","QUELEA_GAIN"],
  "Light Sensor":     ["LUX_POLL_INTERVAL_MS","LUX_CHANGE_THRESHOLD","LUX_FLASH_THRESHOLD","LUX_FLASH_PERCENT","LUX_FLASH_MIN_ABS"],
  "LED Behaviour":    ["VU_MAX_BRIGHTNESS"],
  "ESP-NOW Mesh":     ["ESPNOW_LUX_THRESHOLD","ESPNOW_EVENT_TTL_MS","ESPNOW_SOUND_THROTTLE_MS","DEBUG_ESPNOW_STATUS"],
  "Markov Chain":     ["MARKOV_IDLE_TRIGGER_MS","MARKOV_AUTONOMOUS_COOLDOWN_MS"],
  "Flock Mode":       ["FLOCK_GRACE_MS","FLOCK_MSG_COUNT","FLOCK_WINDOW_MS","FLOCK_HOLD_MS","FLOCK_CALL_GAP_MS"],
};

let fullConfig = {}, dirty = {};

async function loadConfig() {
  const resp = await fetch("/config/full");
  fullConfig = await resp.json();
  renderAll();
  document.getElementById("param-count").textContent = Object.keys(fullConfig).length;
  updateQuietHoursBanner();
}

async function updateQuietHoursBanner() {
  try {
    const d = await (await fetch("/quiet-hours/status")).json();
    const banner = document.getElementById("quiet-hours-banner");
    if (!d.enabled) { banner.style.display = "none"; return; }
    banner.style.display = "block";
    if (d.quiet_now) {
      banner.style.cssText += ";background:rgba(240,74,106,0.08);border-color:var(--danger);color:var(--danger)";
      banner.innerHTML = `🔇 <strong>QUIET HOURS ACTIVE</strong> — fleet is silent until ${String(d.quiet_end).padStart(2,"0")}:00 &nbsp;·&nbsp; ${d.minutes_to_next_event} min remaining &nbsp;·&nbsp; server time ${d.server_time}`;
    } else {
      banner.style.cssText += ";background:rgba(200,240,74,0.06);border-color:var(--accent);color:var(--accent)";
      banner.innerHTML = `🔊 Fleet active — quiet hours begin at ${String(d.quiet_start).padStart(2,"0")}:00 &nbsp;·&nbsp; ${d.minutes_to_next_event} min until silence &nbsp;·&nbsp; server time ${d.server_time}`;
    }
  } catch(e) {}
}

function renderAll() {
  const container = document.getElementById("param-sections");
  container.innerHTML = "";
  const filterVal = document.getElementById("filter-input").value.toLowerCase();
  for (const [section, keys] of Object.entries(SECTIONS)) {
    const vis = keys.filter(k => fullConfig[k] && (!filterVal || k.toLowerCase().includes(filterVal) || fullConfig[k].description.toLowerCase().includes(filterVal)));
    if (!vis.length) continue;
    const label = document.createElement("div");
    label.className = "section-label";
    label.textContent = section;
    container.appendChild(label);
    const grid = document.createElement("div");
    grid.className = "param-grid";
    vis.forEach(k => grid.appendChild(makeCard(k, fullConfig[k])));
    container.appendChild(grid);
  }
}

function makeCard(key, param) {
  const card = document.createElement("div");
  card.className = "param-card" + (dirty[key] !== undefined ? " dirty" : "");
  card.id = "card-" + key;
  const cur = dirty[key] !== undefined ? dirty[key] : param.value;
  if (param.type === "bool") {
    const on = cur === true || cur === "true";
    card.innerHTML = `<div class="param-key">${key}</div><div class="param-unit">${param.unit}</div><div class="param-desc">${param.description}</div><div class="param-controls"><button class="toggle-btn ${on?'toggle-on':'toggle-off'}" id="toggle-${key}"><span class="toggle-track"><span class="toggle-thumb"></span></span><span class="toggle-label">${on?'ON':'OFF'}</span></button><span class="param-default">default: OFF</span></div>`;
    const btn = card.querySelector(`#toggle-${key}`);
    btn.addEventListener("click", () => {
      const nowOn = btn.classList.contains("toggle-off");
      btn.classList.toggle("toggle-on", nowOn); btn.classList.toggle("toggle-off", !nowOn);
      btn.querySelector(".toggle-label").textContent = nowOn ? "ON" : "OFF";
      markDirty(key, nowOn);
    });
    return card;
  }
  const isFloat = param.type === "float";
  const dec = isFloat ? (param.step < 0.1 ? 3 : 2) : 0;
  card.innerHTML = `<div class="param-key">${key}</div><div class="param-unit">${param.unit}</div><div class="param-desc">${param.description}</div><div class="param-controls"><input type="range" class="param-range" id="range-${key}" min="${param.min}" max="${param.max}" step="${param.step}" value="${cur}"><input type="number" class="param-input" id="input-${key}" min="${param.min}" max="${param.max}" step="${param.step}" value="${cur.toFixed?cur.toFixed(dec):cur}"><span class="param-default">default: ${param.value.toFixed?param.value.toFixed(dec):param.value}</span></div>`;
  const rng = card.querySelector(`#range-${key}`), inp = card.querySelector(`#input-${key}`);
  rng.addEventListener("input",  () => { const v=isFloat?parseFloat(rng.value):parseInt(rng.value); inp.value=isFloat?v.toFixed(dec):v; markDirty(key,v); });
  inp.addEventListener("change", () => { const v=isFloat?parseFloat(inp.value):parseInt(inp.value); rng.value=v; markDirty(key,v); });
  return card;
}

function markDirty(key, value) {
  dirty[key] = value;
  document.getElementById("card-"+key)?.classList.add("dirty");
  updateDirtyCount();
}
function updateDirtyCount() {
  const n = Object.keys(dirty).length;
  document.getElementById("dirty-indicator").innerHTML = n > 0 ? `<span class="dirty-count">${n} unsaved change${n>1?"s":""}</span>` : "";
}

async function saveAll() {
  if (!Object.keys(dirty).length) { showToast("Nothing to save","info"); return; }
  const btn = document.getElementById("save-btn");
  btn.textContent = "Saving…"; btn.disabled = true;
  try {
    const res = await (await fetch("/config",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(dirty)})).json();
    if (res.errors?.length) { showToast("Errors: "+res.errors.join(", "),"error"); }
    else {
      showToast(`Saved ${res.updated.length} parameter(s) ✓`,"success");
      for (const [k,v] of Object.entries(dirty)) if (fullConfig[k]) fullConfig[k].value=v;
      dirty={}; document.querySelectorAll(".param-card.dirty").forEach(c=>c.classList.remove("dirty"));
      updateDirtyCount(); document.getElementById("last-saved").textContent=new Date().toLocaleTimeString();
    }
  } catch(e) { showToast("Network error: "+e.message,"error"); }
  btn.textContent="Save changes"; btn.disabled=false;
}

async function confirmReset() {
  if (!confirm("Reset ALL parameters to factory defaults? This cannot be undone.")) return;
  if ((await fetch("/config/reset",{method:"POST"})).ok) { dirty={}; await loadConfig(); showToast("All parameters reset to defaults","info"); updateDirtyCount(); }
}
function toggleTheme() {
  const light = document.documentElement.classList.toggle('light');
  document.getElementById('theme-btn').textContent = light ? '☾ Dark' : '☀ Light';
  try { localStorage.setItem('echoes-theme', light ? 'light' : 'dark'); } catch(e) {}
}
// Restore saved theme before first paint
(function() {
  try {
    if (localStorage.getItem('echoes-theme') === 'light') {
      document.documentElement.classList.add('light');
      document.addEventListener('DOMContentLoaded', () => {
        const b = document.getElementById('theme-btn');
        if (b) b.textContent = '☾ Dark';
      });
    }
  } catch(e) {}
})();
function showToast(msg,type="info") {
  const t=document.getElementById("toast"); t.textContent=msg; t.className="show "+type;
  clearTimeout(t._t); t._t=setTimeout(()=>t.className="",3500);
}
document.getElementById("filter-input").addEventListener("input",renderAll);
document.addEventListener("keydown",e=>{if((e.ctrlKey||e.metaKey)&&e.key==="s"){e.preventDefault();saveAll();}});

const RESTART_COOLDOWN_MS = 3 * 60 * 1000;   // 3 minutes
let _restartCooldownTimer = null;

async function requestRestart() {
  const btn=document.getElementById("restart-btn"), st=document.getElementById("restart-status");
  if (!confirm("Queue a restart for ALL nodes?\n\nEach node reboots at its next config poll (within 60 s).\nYou can cancel until the first node polls.")) return;
  btn.disabled=true; btn.textContent="Queuing…";
  try {
    const d=await(await fetch("/restart",{method:"POST"})).json();
    if (d.status==="restart_pending") {
      showToast("Restart queued — nodes reboot within 90 s","success"); st.textContent="⏳ restart pending";
      btn.textContent="✕ Cancel restart"; btn.onclick=cancelRestart; btn.disabled=false;
    } else { showToast("Unexpected response","error"); resetRestartBtn(); }
  } catch(e) { showToast("Network error: "+e.message,"error"); resetRestartBtn(); }
}
async function cancelRestart() {
  const st=document.getElementById("restart-status");
  try { const d=await(await fetch("/restart/cancel",{method:"POST"})).json(); showToast(d.was_pending?"Restart cancelled":"No restart was pending","info"); } catch(e) { showToast("Cancel failed","error"); }
  resetRestartBtn();
}
function resetRestartBtn(startCooldown=false) {
  const btn=document.getElementById("restart-btn"), st=document.getElementById("restart-status");
  if (_restartCooldownTimer) { clearInterval(_restartCooldownTimer); _restartCooldownTimer=null; }
  if (startCooldown) {
    const until = Date.now() + RESTART_COOLDOWN_MS;
    btn.disabled=true; btn.onclick=null;
    function tick() {
      const remaining = Math.max(0, Math.ceil((until - Date.now()) / 1000));
      const m = Math.floor(remaining/60), s = remaining%60;
      btn.innerHTML=`⟳ Restart all nodes (${m}:${String(s).padStart(2,'0')})`;
      st.textContent="";
      if (remaining <= 0) {
        clearInterval(_restartCooldownTimer); _restartCooldownTimer=null;
        btn.innerHTML="⟳ Restart all nodes"; btn.onclick=requestRestart; btn.disabled=false;
      }
    }
    tick();
    _restartCooldownTimer = setInterval(tick, 1000);
  } else {
    btn.innerHTML="⟳ Restart all nodes"; btn.onclick=requestRestart; btn.disabled=false; st.textContent="";
  }
}

// ── Fleet Grid ────────────────────────────────────────────────────────────
const FLOCK_STALE   = 90_000;   // ms — matches server-side thresholds
const FLOCK_OFFLINE = 180_000;

function fleetClassify(n) {
  if (!n.last_seen_ts) return 'foffline';
  const age = Date.now() - n.last_seen_ts * 1000;
  if (age < FLOCK_STALE)   return 'fonline';
  if (age < FLOCK_OFFLINE) return 'fstale';
  return 'foffline';
}
function fleetAgeStr(n) {
  if (!n.last_seen_ts) return '—';
  const s = Math.floor((Date.now() - n.last_seen_ts * 1000) / 1000);
  if (s < 60)   return `${s}s ago`;
  if (s < 3600) return `${Math.floor(s/60)}m ${s%60}s ago`;
  return `${Math.floor(s/3600)}h ${Math.floor((s%3600)/60)}m ago`;
}
function fleetFmtDate(ts) {
  if (!ts) return 'No data received';
  const d = new Date(ts * 1000), p = n => String(n).padStart(2,'0');
  return `${d.getFullYear()}-${p(d.getMonth()+1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}
function fleetStatusLabel(cls) {
  return {fonline:'ONLINE',fstale:'STALE',foffline:'OFFLINE'}[cls];
}

const fTip = document.createElement('div');
fTip.className = 'ftooltip';
document.body.appendChild(fTip);

let fleetNodes = [];

// ── Power state ───────────────────────────────────────────────────────────
const powerStates = {1:'unknown', 2:'unknown', 3:'unknown', 4:'unknown'};

function _applyPowerCell(cell, id, state) {
  powerStates[id] = state;
  const pwrCls = state === 'ON' ? 'pon' : state === 'OFF' ? 'poff' : 'punknown';
  cell.className = `fnode-pwr ${pwrCls}`;
  cell.querySelector('.fnode-pwr-icon').textContent = '⏻';
  cell.title = `Group ${id} · ${
    state==='ON'  ? 'ON — click to power off'  :
    state==='OFF' ? 'OFF — click to power on'  : 'power state unknown'}`;
}

function applyPowerState(id, state) {
  const cell = document.getElementById(`fnode-pwr-${id}`);
  if (cell) _applyPowerCell(cell, id, state);
  else powerStates[id] = state;   // store for when grid is next built
}

async function fetchPowerStatus() {
  try {
    const data = await (await fetch('/sonoff/status')).json();
    data.forEach(d => applyPowerState(d.id, d.state));
  } catch(e) { /* devices unreachable — leave cells as-is */ }
}

async function togglePower(id) {
  const cell = document.getElementById(`fnode-pwr-${id}`);
  if (!cell || cell.disabled) return;
  cell.disabled = true;
  cell.querySelector('.fnode-pwr-icon').textContent = '…';
  try {
    const d = await (await fetch(`/sonoff/${id}/toggle`, {method:'POST'})).json();
    _applyPowerCell(cell, id, d.state);
    showToast(`Group ${id} → ${d.state}`, d.state==='ON' ? 'success' : 'info');
  } catch(e) {
    showToast(`Group ${id}: no response`, 'error');
    _applyPowerCell(cell, id, 'unknown');
  } finally {
    cell.disabled = false;
  }
}

function makePowerCell(groupId) {
  const btn = document.createElement('button');
  btn.id        = `fnode-pwr-${groupId}`;
  btn.className = 'fnode-pwr punknown';
  btn.onclick   = () => togglePower(groupId);
  const icon = document.createElement('span');
  icon.className   = 'fnode-pwr-icon';
  icon.textContent = '⏻';
  btn.appendChild(icon);
  _applyPowerCell(btn, groupId, powerStates[groupId] || 'unknown');
  return btn;
}

fetchPowerStatus();
setInterval(fetchPowerStatus, 5000);

// ── Grid builder ──────────────────────────────────────────────────────────
function buildFleetGrid() {
  const g = document.getElementById('fleetGrid');
  g.innerHTML = '';
  let online=0, stale=0, offline=0;
  let sepCount  = 0;
  let groupId   = 1;   // first power cell goes at position 0 (group 1)

  // Power cell at the very start of group 1
  g.appendChild(makePowerCell(groupId++));

  fleetNodes.forEach(n => {
    if (n.separator) {
      sepCount++;
      if (sepCount % 2 === 0) {
        // Row break between the two row-pairs
        const brk = document.createElement('div');
        brk.className = 'fnode-row-break';
        brk.innerHTML = '<span class="fnode-row-break-dot"></span>';
        g.appendChild(brk);
      } else {
        // Spacer before the first col-sep so group 2 aligns with group 4
        if (sepCount === 1) {
          const spacer = document.createElement('div');
          g.appendChild(spacer);
        }
        // Column separator between groups on the same row
        const sep = document.createElement('div');
        sep.className = 'fnode-col-sep';
        g.appendChild(sep);
      }
      // Power cell at the start of each new group
      g.appendChild(makePowerCell(groupId++));
      return;
    }

    const cls    = fleetClassify(n);
    if      (cls === 'fonline')  online++;
    else if (cls === 'fstale')   stale++;
    else                         offline++;

    const nodeId = n.id || n.mac.slice(-5);
    const div    = document.createElement('div');
    div.className = `fnode ${cls}`;
    div.title     = nodeId;

    const lbl = document.createElement('span');
    lbl.className   = 'fnode-label';
    lbl.textContent = nodeId;
    div.appendChild(lbl);

    const showFleetTip = e => {
      fTip.className = `ftooltip ${cls}`;
      fTip.innerHTML =
        `<div class="ftooltip-hdr">
          <span class="ftooltip-id"><span class="ftt-node-id">${nodeId}</span>${n.mac}</span>
          <span class="ftooltip-status">${fleetStatusLabel(cls)}</span>
        </div>
        <div class="ftooltip-row"><span class="ftooltip-key">Type</span><span class="ftooltip-val">${n.node_type}</span></div>
        <div class="ftooltip-row"><span class="ftooltip-key">FW</span><span class="ftooltip-val">${n.firmware}</span></div>
        <div class="ftooltip-row"><span class="ftooltip-key">IP</span><span class="ftooltip-val">${n.ip}</span></div>
        <div class="ftooltip-row"><span class="ftooltip-key">Age</span><span class="ftooltip-val">${fleetAgeStr(n)}</span></div>
        <div class="ftooltip-row"><span class="ftooltip-key">Polls</span><span class="ftooltip-val">${n.poll_count}</span></div>
        <div class="ftooltip-row"><span class="ftooltip-key">Boots</span><span class="ftooltip-val">${n.boot_count}</span></div>
        <div class="ftooltip-ts">⏱ ${fleetFmtDate(n.last_seen_ts)}</div>`;
      fTip.style.display = 'block';
      positionFleetTip(e);
    };
    div.addEventListener('mouseenter', showFleetTip);
    div.addEventListener('mousemove', positionFleetTip);
    div.addEventListener('mouseleave', () => { fTip.style.display = 'none'; });
    div.addEventListener('touchstart', e => { e.preventDefault(); showFleetTip(e); }, { passive: false });
    div.addEventListener('touchmove',  e => { e.preventDefault(); positionFleetTip(e); }, { passive: false });
    div.addEventListener('touchend',   () => { fTip.style.display = 'none'; });
    g.appendChild(div);
  });

  const total = fleetNodes.filter(n => !n.separator).length;
  document.querySelector('.fleet-title').textContent =
    `Fleet Monitor — ${total} Device${total !== 1 ? 's' : ''}`;
  document.getElementById('fs-online').textContent  = online;
  document.getElementById('fs-stale').textContent   = stale;
  document.getElementById('fs-offline').textContent = offline;
}

function positionFleetTip(e) {
  // Support both mouse and touch events
  const src = (e.touches && e.touches.length) ? e.touches[0] : e;
  const cx = src.clientX, cy = src.clientY;
  const tw = fTip.offsetWidth || 220, th = fTip.offsetHeight || 160;
  const margin = 10, vw = window.innerWidth, vh = window.innerHeight;
  // Prefer placing tooltip above-right of the touch/cursor
  let x = cx + 14, y = cy - th - 10;
  // Clamp right edge, then left edge
  if (x + tw + margin > vw) x = cx - tw - 14;
  if (x < margin) x = margin;
  // Clamp top edge, then bottom edge
  if (y < margin) y = cy + 14;
  if (y + th + margin > vh) y = vh - th - margin;
  // Final safety clamp for very small viewports
  x = Math.max(margin, Math.min(x, vw - tw - margin));
  y = Math.max(margin, Math.min(y, vh - th - margin));
  fTip.style.left = x + 'px'; fTip.style.top = y + 'px';
}

// ── Restart status polling ────────────────────────────────────────────────
// Polls /restart/status every 10 s while a restart is pending.
// When the server clears the window (pending → false) the button enters
// the 3-minute cooldown so it cannot be accidentally re-fired immediately.
let _restartWasPending = false;
async function pollRestartStatus() {
  try {
    const d = await (await fetch('/restart/status')).json();
    const btn = document.getElementById('restart-btn');
    const isCancelMode = btn && btn.textContent.startsWith('✕');
    if (!d.pending && _restartWasPending && !isCancelMode) {
      // Window just expired naturally — start cooldown
      resetRestartBtn(true);
    }
    _restartWasPending = d.pending;
  } catch(e) { /* ignore network errors */ }
}
setInterval(pollRestartStatus, 10000);

async function fetchFleetNodes() {
  try {
    const resp = await fetch('/nodes');
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    fleetNodes = await resp.json();
    buildFleetGrid();
  } catch(e) {
    console.warn('Fleet node fetch failed:', e);
  }
}

fetchFleetNodes();
setInterval(fetchFleetNodes, 15000);


loadConfig();
setInterval(updateQuietHoursBanner, 60000);
</script>
</body>
</html>"""

# ---------------------------------------------------------------------------
# Route handlers for web UIs
# ---------------------------------------------------------------------------

@app.route("/")
def web_ui():
    return render_template_string(HTML_TEMPLATE)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Echoes of the Machine — fleet server (port 8002)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 echoes-server.py                  # normal operation
  python3 echoes-server.py --debug          # DEBUG logging (all polls visible)
  python3 echoes-server.py --port 8080      # custom port
  python3 echoes-server.py --debug --port 8080

Debug mode enables:
  - All log.debug() calls (individual polls, Tasmota failures, etc.)
  - Flask debug mode (auto-reload on code change, detailed tracebacks)
""",
    )
    parser.add_argument(
        "--debug", "-d",
        action="store_true",
        default=False,
        help="Enable DEBUG-level logging and Flask debug mode",
    )
    parser.add_argument(
        "--port", "-p",
        type=int,
        default=8002,
        metavar="PORT",
        help="TCP port to listen on (default: 8002)",
    )
    args = parser.parse_args()

    # Re-initialise logging now that we know the --debug flag.
    # _setup_logging() was already called at module load time (INFO level) so
    # background threads already have a working logger.  Clear existing handlers
    # first to avoid duplicates, then re-add with the correct level.
    root_logger = logging.getLogger()
    root_logger.handlers.clear()
    _setup_logging(LOG_DIR, debug=args.debug)

    FIRMWARE_DIR.mkdir(parents=True, exist_ok=True)
    log.info("=" * 60)
    log.info("Echoes of the Machine — Consolidated Server")
    log.info(f"Port         : {args.port}")
    log.info(f"Log level    : {'DEBUG' if args.debug else 'INFO'}")
    log.info(f"Nodes file   : {NODES_FILE}  ({len(_catalogue)} node(s))")
    log.info(f"Config file  : {CONFIG_FILE}")
    log.info(f"Firmware dir : {FIRMWARE_DIR}")
    log.info(f"Log dir      : {LOG_DIR}")
    log.info("Routes:")
    log.info("  GET  /firmware/<file>   OTA binary + version")
    log.info("  POST /startup           node boot reports")
    log.info("  GET  /config            60-second device poll")
    log.info("  GET  /nodes             fleet registry JSON")
    log.info("  GET  /                  config + fleet web UI")
    log.info("=" * 60)
    if args.debug:
        log.debug("DEBUG logging active — every poll will be visible in the log")
    app.run(host="0.0.0.0", port=args.port, debug=args.debug, threaded=True)
