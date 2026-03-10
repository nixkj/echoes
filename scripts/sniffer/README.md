# echoes-sniffer

A dedicated passive listener node for the **Echoes of the Machine** ESP-NOW
mesh.  It sits on the same WiFi channel as the fleet, receives every broadcast
frame, and streams decoded JSON Lines to a connected Raspberry Pi over USB
serial for troubleshooting and analysis.

---

## Hardware

Any spare ESP32 board with the standard echoes board layout.  No microphone,
speaker, or light sensor needs to be populated — the sniffer uses only the
radio and UART0 (USB serial).

---

## Build & Flash

```bash
cd echoes-sniffer
idf.py set-target esp32
idf.py menuconfig          # Set WiFi SSID / Password under "WiFi Configuration"
idf.py build flash
```

> **Important:** use the same SSID and password as the fleet nodes.  The
> sniffer connects to the AP purely to lock onto the correct WiFi channel —
> it does not use TCP/IP for anything else.

---

## Output Format

One JSON object per line on UART0 (115 200 baud, same as the IDF monitor):

```
{"ts":0,"event":"SNIFFER_BOOT","firmware":"echoes-sniffer"}
{"ts":312,"event":"WIFI_CONNECTED","ip":"192.168.1.55","ch":6}
{"ts":314,"event":"ESPNOW_READY","ch":6}

# SOUND frame
{"ts":5210,"mac":"AA:BB:CC:DD:EE:FF","rssi":-65,"ch":6,"type":"SOUND","detection":"WHISTLE","lux":0.00}

# LIGHT frame
{"ts":6100,"mac":"AA:BB:CC:DD:EE:FF","rssi":-72,"ch":6,"type":"LIGHT","detection":null,"lux":125.50}

# HEARTBEAT from a minimal node
{"ts":7000,"mac":"11:22:33:44:55:66","rssi":-70,"ch":6,"type":"HEARTBEAT","detection":null,"lux":0.00}

# Non-Echoes frame (alien traffic on the same channel)
{"ts":9000,"mac":"DE:AD:BE:EF:CA:FE","rssi":-80,"ch":6,"type":"UNKNOWN","magic":"0xAB","msg_type":"0x07","len":14}

# Keepalive (every 5 s of silence)
{"ts":15000,"event":"SNIFFER_ALIVE"}
```

### Fields

| Field       | Type    | Description                                              |
|-------------|---------|----------------------------------------------------------|
| `ts`        | uint32  | Milliseconds since sniffer boot (wraps ~49 days)         |
| `mac`       | string  | Sender's WiFi STA MAC address                            |
| `rssi`      | int8    | Received signal strength in dBm                          |
| `ch`        | uint8   | WiFi channel the frame was received on                   |
| `type`      | string  | `SOUND` \| `LIGHT` \| `HEARTBEAT` \| `UNKNOWN`           |
| `detection` | string  | `WHISTLE` \| `VOICE` \| `CLAP` \| `BIRDSONG` \| `null`  |
| `lux`       | float   | Lux reading (valid on `LIGHT`; 0.0 on others)            |
| `magic`     | string  | Hex magic byte — `UNKNOWN` frames only                   |
| `msg_type`  | string  | Hex msg type byte — `UNKNOWN` frames only                |
| `len`       | int     | Raw payload byte count — `UNKNOWN` frames only           |
| `event`     | string  | Lifecycle events only (no `mac`/`type` on these)         |

---

## Raspberry Pi Setup

Install the one dependency:

```bash
pip3 install pyserial
```

Plug the sniffer ESP32 into the Pi via USB, then run:

```bash
python3 scripts/sniffer_monitor.py
```

Useful options:

```bash
# Specify port explicitly
python3 scripts/sniffer_monitor.py --port /dev/ttyUSB0

# Watch only sound events
python3 scripts/sniffer_monitor.py --filter SOUND

# Watch sound and light, suppress heartbeats
python3 scripts/sniffer_monitor.py --filter SOUND,LIGHT

# Log to a custom file, suppress terminal noise
python3 scripts/sniffer_monitor.py --log capture-2025-06-01.jsonl --quiet
```

The log file is in JSON Lines format — every line is a valid JSON object with
a `"wall"` field (ISO 8601 UTC) added by the script.  You can analyse it
after the session with standard tools:

```bash
# Count events by type
cat sniffer.jsonl | python3 -c "
import sys, json, collections
c = collections.Counter()
for l in sys.stdin:
    r = json.loads(l)
    c[r.get('type', r.get('event','?'))] += 1
print(c)
"

# Show all SOUND events from a specific node
grep '"type":"SOUND"' sniffer.jsonl | grep 'AA:BB:CC'
```

---

## What It Does NOT Do

- **Does not transmit ESP-NOW frames** — purely passive after WiFi association.
- **Does not affect flock mode** — HEARTBEAT frames are received and logged but
  not injected into the mesh; the sniffer is invisible to the fleet.
- **Does not implement OTA, remote config, or startup reporting** — flash it
  manually when you need it.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| Only `UNKNOWN` frames, no `SOUND`/`LIGHT` | Wrong WiFi channel | Check SSID/password in menuconfig; ensure AP matches fleet |
| No frames at all | AP not reachable | Check `WIFI_CONNECT_FAILED` in log; fix credentials |
| `SNIFFER_ALIVE` but no mesh frames | Sniffer out of range | Move closer to the fleet |
| Garbled JSON | Baud mismatch | Ensure both sides use 115200 |
