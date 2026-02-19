#!/usr/bin/env python3
"""
Echoes of the Machine - Configuration Server
Serves device parameters via HTTP API and provides a web UI for editing them.
"""

from flask import Flask, jsonify, request, render_template_string
import json
import os
import time
from datetime import datetime

app = Flask(__name__)

CONFIG_FILE = os.path.join(os.path.dirname(__file__), "config.json")

# ---------------------------------------------------------------------------
# DEFAULT CONFIGURATION
# ---------------------------------------------------------------------------
# Each parameter has: value, min, max, description, unit
DEFAULT_CONFIG = {
    "VOLUME": {
        "value": 0.20,
        "min": 0.01,
        "max": 1.0,
        "step": 0.01,
        "type": "float",
        "description": "Master playback volume for synthesised bird calls. Lower values are quieter and less intrusive; higher values project further in noisy spaces.",
        "unit": "amplitude (0–1)"
    },
    "GAIN": {
        "value": 16.0,
        "min": 1.0,
        "max": 64.0,
        "step": 0.5,
        "type": "float",
        "description": "Digital microphone pre-amplifier gain applied before detection. Increase if the device misses quiet sounds; decrease if loud environments cause false triggers.",
        "unit": "linear multiplier"
    },
    "WHISTLE_FREQ": {
        "value": 2000,
        "min": 500,
        "max": 8000,
        "step": 50,
        "type": "int",
        "description": "Centre frequency (Hz) used by the Goertzel detector to recognise whistles. Should match the dominant frequency of the whistle you intend to use.",
        "unit": "Hz"
    },
    "VOICE_FREQ": {
        "value": 200,
        "min": 80,
        "max": 1000,
        "step": 10,
        "type": "int",
        "description": "Centre frequency (Hz) used to detect human voice / low-frequency sounds. Lower values respond to deeper voices and bass-heavy sounds.",
        "unit": "Hz"
    },
    "WHISTLE_MULTIPLIER": {
        "value": 2.5,
        "min": 1.2,
        "max": 10.0,
        "step": 0.1,
        "type": "float",
        "description": "Adaptive threshold multiplier for whistle detection. The detector fires when the Goertzel magnitude exceeds the running average multiplied by this value. Higher = less sensitive, fewer false positives.",
        "unit": "× running average"
    },
    "VOICE_MULTIPLIER": {
        "value": 2.5,
        "min": 1.2,
        "max": 10.0,
        "step": 0.1,
        "type": "float",
        "description": "Adaptive threshold multiplier for voice detection. Equivalent to WHISTLE_MULTIPLIER but applied to the voice frequency band.",
        "unit": "× running average"
    },
    "CLAP_MULTIPLIER": {
        "value": 4.0,
        "min": 2.0,
        "max": 15.0,
        "step": 0.5,
        "type": "float",
        "description": "Adaptive threshold multiplier for clap/impulse detection. Claps are broadband transients; a higher multiplier reduces false triggers from ambient noise bursts.",
        "unit": "× running average"
    },
    "WHISTLE_CONFIRM": {
        "value": 2,
        "min": 1,
        "max": 10,
        "step": 1,
        "type": "int",
        "description": "Number of consecutive buffer frames that must exceed the threshold before a whistle detection is confirmed. Increase to require a longer, sustained whistle.",
        "unit": "frames"
    },
    "VOICE_CONFIRM": {
        "value": 3,
        "min": 1,
        "max": 10,
        "step": 1,
        "type": "int",
        "description": "Consecutive confirmation frames required for voice detection. A higher count reduces false triggers from short percussive sounds at low frequencies.",
        "unit": "frames"
    },
    "CLAP_CONFIRM": {
        "value": 1,
        "min": 1,
        "max": 5,
        "step": 1,
        "type": "int",
        "description": "Confirmation frames for clap detection. Claps are short by nature so 1 frame is usually correct; increase only in very noisy environments.",
        "unit": "frames"
    },
    "DEBOUNCE_BUFFERS": {
        "value": 20,
        "min": 5,
        "max": 100,
        "step": 1,
        "type": "int",
        "description": "Minimum number of audio buffer reads between successive detections of the same type. Prevents a single event from triggering multiple bird calls.",
        "unit": "buffer cycles"
    },
    "LUX_POLL_INTERVAL_MS": {
        "value": 500,
        "min": 100,
        "max": 5000,
        "step": 100,
        "type": "int",
        "description": "How often the light sensor is polled. 500 ms is a good balance between responsiveness and CPU load. The BH1750 has a ~120 ms measurement time; do not go below 150 ms.",
        "unit": "ms"
    },
    "LUX_CHANGE_THRESHOLD": {
        "value": 1.0,
        "min": 0.1,
        "max": 50.0,
        "step": 0.5,
        "type": "float",
        "description": "Minimum lux change required to update the bird mapper or Markov chain. Filters out sensor noise in stable lighting conditions.",
        "unit": "lux"
    },
    "LUX_FLASH_THRESHOLD": {
        "value": 30.0,
        "min": 5.0,
        "max": 500.0,
        "step": 5.0,
        "type": "float",
        "description": "Absolute lux jump in a single poll that triggers an immediate bird-call response (light-flash event). A phone torch at 1 m produces roughly 50–200 lux.",
        "unit": "lux"
    },
    "LUX_FLASH_PERCENT": {
        "value": 0.15,
        "min": 0.05,
        "max": 0.90,
        "step": 0.05,
        "type": "float",
        "description": "Relative lux change (fraction of current reading) that also triggers a flash event. At 0.15, a 15% increase in ambient light fires a response. Works alongside LUX_FLASH_MIN_ABS.",
        "unit": "fraction (0–1)"
    },
    "LUX_FLASH_MIN_ABS": {
        "value": 15.0,
        "min": 1.0,
        "max": 100.0,
        "step": 1.0,
        "type": "float",
        "description": "Minimum absolute lux change that must accompany the percentage check for a flash trigger. Prevents micro-fluctuations in very dark rooms from firing events.",
        "unit": "lux"
    },
    "VOLUME_LUX_MIN": {
        "value": 2.0,
        "min": 0.0,
        "max": 50.0,
        "step": 1.0,
        "type": "float",
        "description": "Lux level at or below which the quietest playback volume (VOLUME_SCALE_MIN) is used. Models a 'quiet at night' behaviour.",
        "unit": "lux"
    },
    "VOLUME_LUX_MAX": {
        "value": 200.0,
        "min": 10.0,
        "max": 2000.0,
        "step": 10.0,
        "type": "float",
        "description": "Lux level at or above which the loudest playback volume (VOLUME_SCALE_MAX) is used. In a typical indoor space, 200 lux is a well-lit room.",
        "unit": "lux"
    },
    "VOLUME_SCALE_MIN": {
        "value": 0.25,
        "min": 0.01,
        "max": 1.0,
        "step": 0.05,
        "type": "float",
        "description": "Volume scale factor applied in darkness (at or below VOLUME_LUX_MIN). Final playback amplitude = VOLUME × VOLUME_SCALE_MIN.",
        "unit": "multiplier (0–1)"
    },
    "VOLUME_SCALE_MAX": {
        "value": 1.0,
        "min": 0.1,
        "max": 1.0,
        "step": 0.05,
        "type": "float",
        "description": "Volume scale factor applied in bright conditions (at or above VOLUME_LUX_MAX). Set below 1.0 to cap maximum output even in bright environments.",
        "unit": "multiplier (0–1)"
    },
    "ESPNOW_LUX_THRESHOLD": {
        "value": 12.0,
        "min": 1.0,
        "max": 100.0,
        "step": 1.0,
        "type": "float",
        "description": "Minimum lux change between consecutive ESP-NOW light broadcasts. Prevents flooding the mesh network with tiny sensor fluctuations.",
        "unit": "lux"
    },
    "ESPNOW_EVENT_TTL_MS": {
        "value": 30000,
        "min": 5000,
        "max": 300000,
        "step": 1000,
        "type": "int",
        "description": "How long (ms) a remote ESP-NOW event continues to influence local bird selection before reverting to the node's own light-level defaults.",
        "unit": "ms"
    },
    "ESPNOW_SOUND_THROTTLE_MS": {
        "value": 3000,
        "min": 500,
        "max": 30000,
        "step": 500,
        "type": "int",
        "description": "Minimum time between consecutive sound-event broadcasts from this node. Prevents flooding the mesh during rapid repeated detections.",
        "unit": "ms"
    },
    "ESPNOW_FLOOD_COUNT": {
        "value": 5,
        "min": 2,
        "max": 20,
        "step": 1,
        "type": "int",
        "description": "Number of incoming ESP-NOW messages within ESPNOW_FLOOD_WINDOW_MS that triggers 'flood' state. In flood state, bird selection switches to Red-billed Quelea to signal network-wide activity.",
        "unit": "messages"
    },
    "ESPNOW_FLOOD_WINDOW_MS": {
        "value": 8000,
        "min": 1000,
        "max": 60000,
        "step": 500,
        "type": "int",
        "description": "Sliding time window (ms) used for flood detection. If ESPNOW_FLOOD_COUNT messages arrive within this window, flood state is activated.",
        "unit": "ms"
    },
    "MARKOV_IDLE_TRIGGER_MS": {
        "value": 45000,
        "min": 5000,
        "max": 600000,
        "step": 5000,
        "type": "int",
        "description": "Duration of network silence (ms) before the Markov chain autonomously fires a bird call based on the most probable next state. Keeps the installation alive when no one is interacting.",
        "unit": "ms"
    },
    "MARKOV_AUTONOMOUS_COOLDOWN_MS": {
        "value": 15000,
        "min": 1000,
        "max": 120000,
        "step": 1000,
        "type": "int",
        "description": "Minimum gap (ms) between consecutive autonomous Markov-triggered calls. Prevents the chain from firing repeatedly during a long quiet period.",
        "unit": "ms"
    },
    "LUX_LED_MIN": {
        "value": 0.0,
        "min": 0.0,
        "max": 100.0,
        "step": 1.0,
        "type": "float",
        "description": "Lux level at which the white LED is at its floor brightness. Below this the LED stays at LUX_LED_BRIGHTNESS_FLOOR.",
        "unit": "lux"
    },
    "LUX_LED_MAX": {
        "value": 80.0,
        "min": 10.0,
        "max": 2000.0,
        "step": 10.0,
        "type": "float",
        "description": "Lux level at which the white LED reaches ceiling brightness. Tune this for your room — a phone screen at arm's length is roughly 50–100 lux.",
        "unit": "lux"
    },
    "LUX_LED_BRIGHTNESS_FLOOR": {
        "value": 0.04,
        "min": 0.0,
        "max": 0.5,
        "step": 0.01,
        "type": "float",
        "description": "Minimum white LED brightness even in a completely dark room. A small value gives the installation a gentle ambient presence at night.",
        "unit": "brightness (0–1)"
    },
    "LUX_LED_BRIGHTNESS_CEIL": {
        "value": 1.0,
        "min": 0.1,
        "max": 1.0,
        "step": 0.05,
        "type": "float",
        "description": "Maximum white LED brightness reached at LUX_LED_MAX ambient light. Reduce if the LED is uncomfortably bright in well-lit spaces.",
        "unit": "brightness (0–1)"
    },
    "VU_MAX_BRIGHTNESS": {
        "value": 0.75,
        "min": 0.1,
        "max": 1.0,
        "step": 0.05,
        "type": "float",
        "description": "Peak LED brightness during bird-call playback VU meter animation. Lower values make the visual response more subtle.",
        "unit": "brightness (0–1)"
    },
    "SILENT_MODE": {
        "value": False,
        "type": "bool",
        "description": "When ON, all output is suppressed — no sound and no LED activity. The device continues to listen and learn but produces no response. Use for maintenance or overnight quiet hours.",
        "unit": "on / off"
    },
    "SOUND_OFF": {
        "value": False,
        "type": "bool",
        "description": "When ON, bird-call audio is silenced but LEDs continue to operate normally (ambient glow and VU meter during detection). Useful in quiet settings where visual response is still wanted.",
        "unit": "on / off"
    }
}

# ---------------------------------------------------------------------------
# CONFIG PERSISTENCE
# ---------------------------------------------------------------------------

def load_config():
    """Load config from JSON file, falling back to defaults for missing keys."""
    if not os.path.exists(CONFIG_FILE):
        save_config(DEFAULT_CONFIG)
        return dict(DEFAULT_CONFIG)
    try:
        with open(CONFIG_FILE, "r") as f:
            saved = json.load(f)
        # Merge: saved values take priority, new defaults fill gaps
        merged = dict(DEFAULT_CONFIG)
        for key, saved_param in saved.items():
            if key in merged:
                merged[key] = dict(merged[key])
                merged[key]["value"] = saved_param.get("value", merged[key]["value"])
        return merged
    except Exception as e:
        print(f"[WARN] Could not load config: {e} — using defaults")
        return dict(DEFAULT_CONFIG)


def save_config(config):
    """Persist config to JSON file."""
    with open(CONFIG_FILE, "w") as f:
        json.dump(config, f, indent=2)


# In-memory config (loaded once at start)
_config = load_config()
_last_modified = datetime.now().isoformat()


# ---------------------------------------------------------------------------
# API ROUTES
# ---------------------------------------------------------------------------

@app.route("/config", methods=["GET"])
def get_config():
    """Return flat key→value map for device consumption."""
    flat = {k: v["value"] for k, v in _config.items()}
    flat["_server_time"] = time.time()
    flat["_version"] = _last_modified
    return jsonify(flat)


@app.route("/config/full", methods=["GET"])
def get_config_full():
    """Return full config with metadata (for web UI)."""
    return jsonify(_config)


@app.route("/config", methods=["POST"])
def update_config():
    """Accept JSON body with {key: new_value, ...} and persist."""
    global _config, _last_modified
    data = request.get_json(force=True)
    if not data:
        return jsonify({"error": "No JSON body"}), 400

    updated = []
    errors = []
    for key, new_val in data.items():
        if key not in _config:
            errors.append(f"Unknown key: {key}")
            continue
        param = _config[key]
        try:
            if param["type"] == "bool":
                if isinstance(new_val, bool):
                    pass  # already correct type
                elif isinstance(new_val, str):
                    new_val = new_val.lower() in ("true", "1", "yes")
                else:
                    new_val = bool(new_val)
            elif param["type"] == "int":
                new_val = int(new_val)
            else:
                new_val = float(new_val)
            if param["type"] != "bool" and (new_val < param["min"] or new_val > param["max"]):
                errors.append(f"{key}: value {new_val} out of range [{param['min']}, {param['max']}]")
                continue
            _config[key] = dict(param)
            _config[key]["value"] = new_val
            updated.append(key)
        except (ValueError, TypeError) as e:
            errors.append(f"{key}: {e}")

    if updated:
        _last_modified = datetime.now().isoformat()
        save_config(_config)

    return jsonify({"updated": updated, "errors": errors, "version": _last_modified})


@app.route("/config/reset", methods=["POST"])
def reset_config():
    """Reset all parameters to factory defaults."""
    global _config, _last_modified
    _config = dict(DEFAULT_CONFIG)
    _last_modified = datetime.now().isoformat()
    save_config(_config)
    return jsonify({"status": "reset", "version": _last_modified})


# ---------------------------------------------------------------------------
# WEB UI
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
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    background: var(--bg);
    color: var(--text);
    font-family: var(--font-mono);
    font-size: 13px;
    min-height: 100vh;
    overflow-x: hidden;
  }

  /* ── GRID NOISE OVERLAY ── */
  body::before {
    content: '';
    position: fixed; inset: 0; z-index: 0;
    background-image:
      linear-gradient(rgba(200,240,74,0.015) 1px, transparent 1px),
      linear-gradient(90deg, rgba(200,240,74,0.015) 1px, transparent 1px);
    background-size: 40px 40px;
    pointer-events: none;
  }

  .layout { position: relative; z-index: 1; max-width: 1100px; margin: 0 auto; padding: 0 24px 60px; }

  /* ── HEADER ── */
  header {
    padding: 48px 0 40px;
    border-bottom: 1px solid var(--border);
    margin-bottom: 40px;
    display: flex;
    align-items: flex-end;
    justify-content: space-between;
    flex-wrap: wrap;
    gap: 16px;
  }
  .logo-block {}
  .logo-eyebrow {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.3em;
    color: var(--accent);
    text-transform: uppercase;
    margin-bottom: 8px;
    opacity: 0.8;
  }
  h1 {
    font-family: var(--font-head);
    font-size: clamp(22px, 4vw, 38px);
    font-weight: 800;
    letter-spacing: -0.02em;
    line-height: 1;
    color: #fff;
  }
  h1 span { color: var(--accent); }
  .header-meta {
    font-size: 11px;
    color: var(--text-dim);
    line-height: 1.8;
    text-align: right;
  }
  .header-meta strong { color: var(--accent2); font-weight: 400; }

  /* ── CONTROLS BAR ── */
  .controls-bar {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-bottom: 32px;
    flex-wrap: wrap;
  }
  .filter-input {
    flex: 1; min-width: 180px;
    background: var(--surface);
    border: 1px solid var(--border);
    color: var(--text);
    font-family: var(--font-mono);
    font-size: 12px;
    padding: 8px 14px;
    border-radius: var(--radius);
    outline: none;
    transition: border-color 0.2s;
  }
  .filter-input:focus { border-color: var(--accent); }
  .filter-input::placeholder { color: var(--muted); }

  .btn {
    font-family: var(--font-mono);
    font-size: 11px;
    letter-spacing: 0.08em;
    padding: 8px 18px;
    border-radius: var(--radius);
    border: 1px solid;
    cursor: pointer;
    transition: all 0.15s;
    text-transform: uppercase;
    white-space: nowrap;
  }
  .btn-primary {
    background: var(--accent);
    border-color: var(--accent);
    color: #0a0c0f;
    font-weight: 700;
  }
  .btn-primary:hover { background: #d8ff5a; box-shadow: var(--glow); }
  .btn-ghost {
    background: transparent;
    border-color: var(--muted);
    color: var(--text-dim);
  }
  .btn-ghost:hover { border-color: var(--danger); color: var(--danger); }
  .btn-ghost2 {
    background: transparent;
    border-color: var(--border);
    color: var(--text-dim);
  }
  .btn-ghost2:hover { border-color: var(--accent2); color: var(--accent2); }

  /* ── SECTION HEADERS ── */
  .section-label {
    font-family: var(--font-head);
    font-size: 11px;
    font-weight: 600;
    letter-spacing: 0.25em;
    text-transform: uppercase;
    color: var(--text-dim);
    padding: 6px 0;
    margin: 32px 0 16px;
    border-bottom: 1px solid var(--border);
    display: flex;
    align-items: center;
    gap: 10px;
  }
  .section-label::before {
    content: '';
    display: inline-block;
    width: 8px; height: 8px;
    background: var(--accent);
    border-radius: 50%;
  }

  /* ── PARAM GRID ── */
  .param-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(460px, 1fr));
    gap: 2px;
  }

  .param-card {
    background: var(--surface);
    border: 1px solid var(--border);
    padding: 18px 20px;
    display: grid;
    grid-template-columns: 1fr auto;
    grid-template-rows: auto auto auto;
    gap: 4px 16px;
    transition: border-color 0.15s;
    position: relative;
    overflow: hidden;
  }
  .param-card::after {
    content: '';
    position: absolute;
    top: 0; left: 0;
    width: 2px; height: 100%;
    background: var(--accent);
    opacity: 0;
    transition: opacity 0.2s;
  }
  .param-card:hover { border-color: var(--muted); }
  .param-card:hover::after { opacity: 1; }
  .param-card.dirty { border-color: var(--warn); }
  .param-card.dirty::after { background: var(--warn); opacity: 1; }

  .param-key {
    grid-column: 1; grid-row: 1;
    font-family: var(--font-mono);
    font-size: 12px;
    color: var(--accent2);
    letter-spacing: 0.04em;
    align-self: center;
  }
  .param-unit {
    grid-column: 2; grid-row: 1;
    font-size: 10px;
    color: var(--text-dim);
    text-align: right;
    align-self: center;
    white-space: nowrap;
  }
  .param-desc {
    grid-column: 1 / -1; grid-row: 2;
    font-size: 11px;
    line-height: 1.6;
    color: var(--text-dim);
    padding-right: 4px;
  }
  .param-controls {
    grid-column: 1 / -1; grid-row: 3;
    display: flex;
    align-items: center;
    gap: 10px;
    margin-top: 10px;
  }

  .param-input {
    width: 90px;
    background: var(--bg);
    border: 1px solid var(--border);
    color: var(--text);
    font-family: var(--font-mono);
    font-size: 13px;
    padding: 6px 10px;
    border-radius: var(--radius);
    outline: none;
    text-align: right;
    transition: border-color 0.15s;
  }
  .param-input:focus { border-color: var(--accent2); }

  .param-range {
    flex: 1;
    -webkit-appearance: none;
    height: 2px;
    background: var(--muted);
    border-radius: 1px;
    outline: none;
    cursor: pointer;
  }
  .param-range::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 14px; height: 14px;
    border-radius: 50%;
    background: var(--accent);
    cursor: pointer;
    transition: transform 0.1s, box-shadow 0.1s;
  }
  .param-range::-webkit-slider-thumb:hover {
    transform: scale(1.3);
    box-shadow: 0 0 8px rgba(200,240,74,0.5);
  }
  .param-range::-moz-range-thumb {
    width: 14px; height: 14px;
    border-radius: 50%;
    background: var(--accent);
    border: none;
    cursor: pointer;
  }

  .param-default {
    font-size: 10px;
    color: var(--muted);
    white-space: nowrap;
  }

  /* ── TOAST ── */
  #toast {
    position: fixed;
    bottom: 32px; right: 32px;
    background: var(--surface);
    border: 1px solid var(--border);
    padding: 14px 20px;
    border-radius: var(--radius);
    font-size: 12px;
    max-width: 320px;
    transform: translateY(80px);
    opacity: 0;
    transition: all 0.3s cubic-bezier(0.34, 1.56, 0.64, 1);
    z-index: 100;
  }
  #toast.show { transform: translateY(0); opacity: 1; }
  #toast.success { border-left: 3px solid var(--accent); }
  #toast.error   { border-left: 3px solid var(--danger); }
  #toast.info    { border-left: 3px solid var(--accent2); }

  /* ── STATUS BAR ── */
  .status-bar {
    position: fixed;
    bottom: 0; left: 0; right: 0;
    background: rgba(10,12,15,0.92);
    backdrop-filter: blur(8px);
    border-top: 1px solid var(--border);
    padding: 8px 32px;
    display: flex;
    align-items: center;
    gap: 20px;
    font-size: 11px;
    color: var(--text-dim);
    z-index: 50;
  }
  .status-dot {
    width: 6px; height: 6px;
    border-radius: 50%;
    background: var(--accent);
    box-shadow: 0 0 8px var(--accent);
    animation: pulse 2s infinite;
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.4; }
  }
  .status-bar .dirty-count { color: var(--warn); }
  .status-bar .spacer { flex: 1; }

  .hidden { display: none !important; }

  /* ── TOGGLE SWITCH (bool params) ── */
  .toggle-btn {
    display: inline-flex;
    align-items: center;
    gap: 10px;
    background: none;
    border: 1px solid var(--muted);
    border-radius: var(--radius);
    padding: 7px 14px;
    cursor: pointer;
    font-family: var(--font-mono);
    font-size: 11px;
    letter-spacing: 0.1em;
    transition: border-color 0.15s, background 0.15s;
  }
  .toggle-btn:hover { border-color: var(--text-dim); }
  .toggle-track {
    display: inline-block;
    width: 32px; height: 16px;
    border-radius: 8px;
    background: var(--muted);
    position: relative;
    transition: background 0.2s;
    flex-shrink: 0;
  }
  .toggle-thumb {
    position: absolute;
    top: 2px; left: 2px;
    width: 12px; height: 12px;
    border-radius: 50%;
    background: var(--text-dim);
    transition: transform 0.2s, background 0.2s;
  }
  .toggle-label { color: var(--text-dim); transition: color 0.15s; }

  .toggle-off .toggle-track { background: var(--muted); }
  .toggle-off .toggle-thumb { transform: translateX(0); background: var(--text-dim); }
  .toggle-off .toggle-label { color: var(--text-dim); }
  .toggle-off { border-color: var(--muted); }

  .toggle-on .toggle-track { background: var(--danger); }
  .toggle-on .toggle-thumb { transform: translateX(16px); background: #fff; }
  .toggle-on .toggle-label { color: var(--danger); font-weight: 700; }
  .toggle-on { border-color: var(--danger); background: rgba(240,74,106,0.08); }
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
      <span id="param-count">0</span> parameters
    </div>
  </header>

  <div class="controls-bar">
    <input class="filter-input" type="text" id="filter-input" placeholder="Filter parameters…">
    <button class="btn btn-ghost2" onclick="expandAll()">Expand all</button>
    <button class="btn btn-ghost" onclick="confirmReset()">Reset defaults</button>
    <button class="btn btn-primary" id="save-btn" onclick="saveAll()">Save changes</button>
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
  "Output Switches": ["SILENT_MODE","SOUND_OFF"],
  "Audio Detection": ["GAIN","WHISTLE_FREQ","VOICE_FREQ","WHISTLE_MULTIPLIER","VOICE_MULTIPLIER","CLAP_MULTIPLIER","WHISTLE_CONFIRM","VOICE_CONFIRM","CLAP_CONFIRM","DEBOUNCE_BUFFERS"],
  "Playback Volume": ["VOLUME","VOLUME_LUX_MIN","VOLUME_LUX_MAX","VOLUME_SCALE_MIN","VOLUME_SCALE_MAX"],
  "Light Sensor": ["LUX_POLL_INTERVAL_MS","LUX_CHANGE_THRESHOLD","LUX_FLASH_THRESHOLD","LUX_FLASH_PERCENT","LUX_FLASH_MIN_ABS"],
  "LED Behaviour": ["LUX_LED_MIN","LUX_LED_MAX","LUX_LED_BRIGHTNESS_FLOOR","LUX_LED_BRIGHTNESS_CEIL","VU_MAX_BRIGHTNESS"],
  "ESP-NOW Mesh": ["ESPNOW_LUX_THRESHOLD","ESPNOW_EVENT_TTL_MS","ESPNOW_SOUND_THROTTLE_MS","ESPNOW_FLOOD_COUNT","ESPNOW_FLOOD_WINDOW_MS"],
  "Markov Chain": ["MARKOV_IDLE_TRIGGER_MS","MARKOV_AUTONOMOUS_COOLDOWN_MS"]
};

let fullConfig = {};
let dirty = {};

async function loadConfig() {
  const resp = await fetch("/config/full");
  fullConfig = await resp.json();
  renderAll();
  document.getElementById("last-saved").textContent = fullConfig["VOLUME"]?._last_modified || "just now";
  document.getElementById("param-count").textContent = Object.keys(fullConfig).length;
}

function renderAll() {
  const container = document.getElementById("param-sections");
  container.innerHTML = "";
  const filterVal = document.getElementById("filter-input").value.toLowerCase();

  for (const [section, keys] of Object.entries(SECTIONS)) {
    const visibleKeys = keys.filter(k => {
      if (!fullConfig[k]) return false;
      if (!filterVal) return true;
      return k.toLowerCase().includes(filterVal) ||
             fullConfig[k].description.toLowerCase().includes(filterVal);
    });
    if (visibleKeys.length === 0) continue;

    const label = document.createElement("div");
    label.className = "section-label";
    label.textContent = section;
    container.appendChild(label);

    const grid = document.createElement("div");
    grid.className = "param-grid";
    for (const key of visibleKeys) {
      grid.appendChild(makeCard(key, fullConfig[key]));
    }
    container.appendChild(grid);
  }
}

function makeCard(key, param) {
  const card = document.createElement("div");
  card.className = "param-card" + (dirty[key] !== undefined ? " dirty" : "");
  card.id = "card-" + key;

  const currentVal = dirty[key] !== undefined ? dirty[key] : param.value;

  if (param.type === "bool") {
    const isOn = currentVal === true || currentVal === "true";
    card.innerHTML = `
      <div class="param-key">${key}</div>
      <div class="param-unit">${param.unit}</div>
      <div class="param-desc">${param.description}</div>
      <div class="param-controls">
        <button class="toggle-btn ${isOn ? 'toggle-on' : 'toggle-off'}" id="toggle-${key}">
          <span class="toggle-track"><span class="toggle-thumb"></span></span>
          <span class="toggle-label">${isOn ? 'ON' : 'OFF'}</span>
        </button>
        <span class="param-default">default: OFF</span>
      </div>
    `;
    const btn = card.querySelector(`#toggle-${key}`);
    btn.addEventListener("click", () => {
      const nowOn = btn.classList.contains("toggle-off");
      btn.classList.toggle("toggle-on", nowOn);
      btn.classList.toggle("toggle-off", !nowOn);
      btn.querySelector(".toggle-label").textContent = nowOn ? "ON" : "OFF";
      markDirty(key, nowOn);
    });
    return card;
  }

  const isFloat = param.type === "float";
  const decimals = isFloat ? (param.step < 0.1 ? 3 : 2) : 0;

  card.innerHTML = `
    <div class="param-key">${key}</div>
    <div class="param-unit">${param.unit}</div>
    <div class="param-desc">${param.description}</div>
    <div class="param-controls">
      <input type="range"
        class="param-range"
        id="range-${key}"
        min="${param.min}" max="${param.max}" step="${param.step}"
        value="${currentVal}">
      <input type="number"
        class="param-input"
        id="input-${key}"
        min="${param.min}" max="${param.max}" step="${param.step}"
        value="${currentVal.toFixed ? currentVal.toFixed(decimals) : currentVal}">
      <span class="param-default">default: ${param.value.toFixed ? param.value.toFixed(decimals) : param.value}</span>
    </div>
  `;

  // Sync range ↔ number input
  const rangeEl = card.querySelector(`#range-${key}`);
  const inputEl = card.querySelector(`#input-${key}`);

  rangeEl.addEventListener("input", () => {
    const v = isFloat ? parseFloat(rangeEl.value) : parseInt(rangeEl.value);
    inputEl.value = isFloat ? v.toFixed(decimals) : v;
    markDirty(key, v);
  });
  inputEl.addEventListener("change", () => {
    const v = isFloat ? parseFloat(inputEl.value) : parseInt(inputEl.value);
    rangeEl.value = v;
    markDirty(key, v);
  });

  return card;
}

function markDirty(key, value) {
  dirty[key] = value;
  const card = document.getElementById("card-" + key);
  if (card) card.classList.add("dirty");
  updateDirtyCount();
}

function updateDirtyCount() {
  const n = Object.keys(dirty).length;
  const el = document.getElementById("dirty-indicator");
  if (n > 0) {
    el.innerHTML = `<span class="dirty-count">${n} unsaved change${n>1?"s":""}</span>`;
  } else {
    el.textContent = "";
  }
}

async function saveAll() {
  if (Object.keys(dirty).length === 0) { showToast("Nothing to save", "info"); return; }
  const btn = document.getElementById("save-btn");
  btn.textContent = "Saving…";
  btn.disabled = true;

  try {
    const resp = await fetch("/config", {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify(dirty)
    });
    const result = await resp.json();
    if (result.errors && result.errors.length > 0) {
      showToast("Errors: " + result.errors.join(", "), "error");
    } else {
      showToast(`Saved ${result.updated.length} parameter(s) ✓`, "success");
      // Commit dirty values to fullConfig
      for (const [k, v] of Object.entries(dirty)) {
        if (fullConfig[k]) fullConfig[k].value = v;
      }
      dirty = {};
      document.querySelectorAll(".param-card.dirty").forEach(c => c.classList.remove("dirty"));
      updateDirtyCount();
      document.getElementById("last-saved").textContent = new Date().toLocaleTimeString();
    }
  } catch(e) {
    showToast("Network error: " + e.message, "error");
  }
  btn.textContent = "Save changes";
  btn.disabled = false;
}

async function confirmReset() {
  if (!confirm("Reset ALL parameters to factory defaults? This cannot be undone.")) return;
  const resp = await fetch("/config/reset", { method: "POST" });
  if (resp.ok) {
    dirty = {};
    await loadConfig();
    showToast("All parameters reset to defaults", "info");
    updateDirtyCount();
  }
}

function expandAll() {
  document.getElementById("filter-input").value = "";
  renderAll();
}

function showToast(msg, type="info") {
  const t = document.getElementById("toast");
  t.textContent = msg;
  t.className = "show " + type;
  clearTimeout(t._timer);
  t._timer = setTimeout(() => { t.className = ""; }, 3500);
}

document.getElementById("filter-input").addEventListener("input", renderAll);

// Keyboard shortcut: Ctrl/Cmd + S
document.addEventListener("keydown", e => {
  if ((e.ctrlKey || e.metaKey) && e.key === "s") { e.preventDefault(); saveAll(); }
});

loadConfig();
</script>
</body>
</html>"""


@app.route("/", methods=["GET"])
def web_ui():
    return render_template_string(HTML_TEMPLATE)


if __name__ == "__main__":
    print(f"[INFO] Config file: {CONFIG_FILE}")
    print(f"[INFO] Starting Echoes configuration server on port 8002")
    app.run(host="0.0.0.0", port=8002, debug=False)
