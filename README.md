# Echoes of the Machine

**Ken Nixon & Renzo Filinich Orozco, University of the Witwatersrand**

A cross-disciplinary interactive art exhibition exploring cybernetics and feedback, as well as machine-assisted code generation.

## Features

- 🎵 Detects whistles, voice, claps, and birdsong
- 🐦 Responds with synthesised South African bird calls (11 species)
- 💡 Light-adaptive bird selection and lux-scaled playback volume
- 📡 ESP-NOW broadcast mesh — nodes influence each other's bird selection
- 🐦‍⬛ Flock mode — triggered by network activity bursts across the fleet
- 🧠 Markov chain — learns event sequences and fires autonomous calls during silence
- 🌐 WiFi connectivity with OTA firmware updates
- ⚙️ Remote configuration — all parameters tunable live from a server UI without reflashing
- 🔋 Power-efficient operation with WiFi modem sleep
- 🎛️ LED indicators for status and audio VU metering

## Hardware Requirements (refer to docs for more details)

- ESP32 development board
- Adafruit ICS-43434 I2S MEMS Microphone
- Adafruit MAX98357A I2S Amplifier
- Speaker (4–8 Ω)
- White LED (GPIO 13)
- Blue LED (GPIO 2) (on ESP32-DEV board)
- BH1750 I2C light sensor **or** ALS-PT19 analog light sensor
- Prototype board, Capacitor, Power supply connector, Header pin female 3 way, 5 way, 6 way, 8 way, 15 way
- Power supply or 5V USB
- Wi-Fi Access Point
- Raspberry Pi for OTA, report logging and configuration servers

### Hardware Configurations

Two different nodes are possible - one with audio output (speaker), and another
without (basic).  The firmware auto-detects which hardware is present at boot:

| Configuration | Light Sensor | Audio Output |
|---|---|---|
| **Full** | BH1750 found on I2C (GPIO 21/22) | Bird calls via MAX98357A |
| **Minimal** | ALS-PT19 analog (GPIO 34) | LED VU meter only |

### Pin Assignments

| Signal | GPIO |
|---|---|
| Mic SCK | 32 |
| Mic WS | 33 |
| Mic DOUT | 35 |
| Speaker SCK | 18 |
| Speaker WS | 19 |
| Speaker DIN | 17 |
| I2C SDA (BH1750) | 21 |
| I2C SCL (BH1750) | 22 |
| Analog light sensor | 34 |
| White LED | 13 |
| Blue LED | 2 |

## Quick Start

### 1. Prerequisites

- Python 3.9 or newer (required by ESP-IDF v5.5.x — run `python3 --version` to check)

```bash
# Install ESP-IDF v5.5.2
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.5.2
cd esp-idf-v5.5.2 && git checkout v5.5.2
./install.sh
. ./export.sh
```

### 2. Configure

Set your WiFi credentials and server IP using `idf.py menuconfig`:

```bash
idf.py menuconfig
# Navigate to: WiFi Configuration
#   → WiFi SSID       (your network name)
#   → WiFi Password   (your network password)
#
# Navigate to: Server Configuration
#   → Server IP Address           (IP of your Raspberry Pi or host running the servers)
#   → OTA Firmware Server Port    (default: 8000)
#   → Startup Report Server Port  (default: 8001)
#   → Remote Config Server Port   (default: 8002)
```

Or add the following to `sdkconfig` directly:

```
CONFIG_WIFI_SSID=""
CONFIG_WIFI_PASSWORD=""
CONFIG_SERVER_IP="192.168.101.2"
CONFIG_OTA_SERVER_PORT=8000
CONFIG_STARTUP_SERVER_PORT=8001
CONFIG_CONFIG_SERVER_PORT=8002
```

All credentials and server addresses entered via `menuconfig` are stored in `sdkconfig`, which is excluded from version control by `.gitignore` and will never be committed to the repository.

### 3. Build and Flash

```bash
cd ~/echoes
idf.py build
idf.py -p /dev/tty.usbserial-110 erase-flash
idf.py -p /dev/tty.usbserial-110 flash monitor
```

### 4. Install the Servers

Three servers need to run on the host machine — a Raspberry Pi on the same network works well. Install all three as systemd services with:

```bash
./build.sh services
```

Or individually:

```bash
sudo bash scripts/firmware_server/install.sh
sudo bash scripts/startup_server/install.sh
sudo bash scripts/config_server/install.sh
```

For testing without systemd, start each manually in a separate terminal:

```bash
# Firmware OTA server — port 8000
python3 scripts/firmware_server/firmware_server.py

# Startup report receiver — port 8001
python3 scripts/startup_server/startup_server.py

# Remote configuration UI — port 8002
python3 scripts/config_server/server.py
```

After the OTA server is running, copy your firmware binary and set the version:

```bash
cp build/echoes.bin ~/firmware_server/firmware/echoes.bin
echo "5.4.1" > ~/firmware_server/firmware/version.txt
```

Or use `./build.sh deploy` after a successful build to do both steps at once.

## File Structure

```
echoes/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── build.sh                          # Build, flash, deploy, and server helper script
├── docs/
│   ├── 00-Notes.md                   # Developer notes and internal references
│   ├── README_STARTUP_REPORTING.md
│   ├── STARTUP_REFERENCE.md
│   ├── VU_METER_CONFIG.md
│   └── ESP32-DEV-032.jpg             # (+ other hardware reference images)
├── scripts/
│   ├── config_server/
│   │   ├── server.py                 # Remote configuration UI (port 8002)
│   │   ├── echoes-config.service     # systemd unit file
│   │   └── install.sh
│   ├── firmware_server/
│   │   ├── firmware_server.py        # OTA firmware server (port 8000)
│   │   ├── echoes-firmware.service   # systemd unit file
│   │   ├── SYSTEMD_SERVICE_GUIDE.md  # Guide for installing servers as services
│   │   ├── install.sh
│   │   └── uninstall-service.sh
│   └── startup_server/
│       ├── startup_server.py         # Startup report receiver (port 8001)
│       ├── echoes-startup-server.service  # systemd unit file
│       ├── install.sh
│       └── test_server.py
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild             # WiFi credentials and server addresses via menuconfig
    ├── main.c                        # Entry point — WiFi, OTA, task startup
    ├── echoes.h / echoes.c           # Core system, detection, LED, audio tasks
    ├── synthesis.h / synthesis.c     # Bird call synthesis (11 species)
    ├── ota.h / ota.c                 # WiFi connection and OTA update
    ├── startup.h / startup.c         # Boot identity report
    ├── espnow_mesh.h / espnow_mesh.c # ESP-NOW broadcast mesh
    ├── markov.h / markov.c           # Markov chain (learning + autonomous calls)
    └── remote_config.h / remote_config.c  # Live parameter updates from server
```

## Server Summary

| Server | File | Port | Purpose |
|---|---|---|---|
| OTA firmware | `firmware_server.py` | 8000 | Serves `echoes.bin` and `version.txt` for OTA |
| Startup reports | `startup_server.py` | 8001 | Receives boot identity POST from each node |
| Remote config | `server.py` | 8002 | Web UI + JSON API for live parameter tuning |

## Deploying a Firmware Update

### Using build.sh (recommended)

```bash
./build.sh version patch     # 5.4.1 → 5.4.2 (updates FIRMWARE_VERSION in main/ota.h)
./build.sh build             # Build firmware
./build.sh deploy            # Copy binary + write version.txt to ~/firmware_server/firmware/
```

Or in one step:

```bash
./build.sh all               # patch version bump + build + deploy
```

### Manually

```bash
# Edit FIRMWARE_VERSION in main/ota.h, then:
idf.py build
cp build/echoes.bin ~/firmware_server/firmware/echoes.bin
echo "5.4.1" > ~/firmware_server/firmware/version.txt
```

Each device checks for updates once at boot. It compares the running version string against `version.txt`; if they differ it downloads and flashes the new binary, then restarts. The attempt retries up to 3 times with linear backoff (15 s, 30 s, 45 s).

## build.sh Reference

`build.sh` wraps common `idf.py` commands, auto-detects the serial port, and handles the deploy workflow. Commands that invoke the compiler (`build`, `flash`, `erase`, `monitor`, `all`) check that ESP-IDF is sourced before proceeding — if `IDF_PATH` is not set the script exits immediately with a clear message showing the exact `export.sh` command to run. Commands that don't need the compiler (`tidyup`, `deploy`, `version`, `services`) work without ESP-IDF sourced.

| Command | Description |
|---|---|
| `./build.sh build` | Build firmware |
| `./build.sh build clean` | Full clean then build |
| `./build.sh tidyup` | Remove editor backups, `build/`, and `sdkconfig` (re-run `menuconfig` afterwards to restore credentials) |
| `./build.sh flash` | Flash via USB (auto-detects port) |
| `./build.sh erase` | Erase flash completely (prompts for confirmation) |
| `./build.sh monitor` | Open serial monitor (auto-detects port) |
| `./build.sh version patch` | Increment patch version in `main/ota.h` (e.g. 5.4.1 → 5.4.2) |
| `./build.sh version minor` | Increment minor version (e.g. 5.4.1 → 5.5.0) |
| `./build.sh version major` | Increment major version (e.g. 5.4.1 → 6.0.0) |
| `./build.sh deploy` | Copy binary and write `version.txt` to `~/firmware_server/firmware/` |
| `./build.sh services` | Install all three servers as systemd services (run on host/Pi) |
| `./build.sh all` | Patch version bump + build + deploy |
| `./build.sh help` | Show usage summary |

## Remote Configuration

All audio detection parameters, playback volume, LED behaviour, ESP-NOW thresholds, flock mode settings, Markov chain timing, and quiet hours can be adjusted live from the web UI at `http://<server-ip>:8002` without reflashing firmware. Devices poll for updates every 60 seconds.

The server also supports:
- **Remote restart** — reboots all nodes within a 90-second broadcast window
- **Silent mode** — suppresses all sound and LED output fleet-wide
- **Sound off** — silences audio while keeping LED activity
- **Quiet hours** — scheduled daily silence window (default 17:00–08:00)

## LED Indicators

### Normal Operation

| LED | Behaviour | Meaning |
|---|---|---|
| White | Pulses during playback (VU meter) | Bird call playing |
| White | Strobes rapidly | Flock mode active |
| White | Dim ambient glow | Idle (brightness scales with ambient light) |

### Boot Sequence

| LED | Behaviour | Meaning |
|---|---|---|
| Blue | Steady on | Booting |
| White | Single mid blink | Startup report sent successfully |
| Blue | Single mid blink | Startup report failed |
| White | 1 s pulse | System ready |

### WiFi / OTA Status

| LED | Behaviour | Meaning |
|---|---|---|
| Blue | 3× blink | WiFi connection failed |
| Blue | Steady | OTA download in progress |
| White | 1 s pulse | OTA update successful (device restarting) |
| Blue | 5× rapid blink | OTA update failed |

## Bird Calls (11 species)

All calls are synthesised in firmware — no audio files are required:

| Species | Responds to |
|---|---|
| Red-chested Cuckoo *(Piet-my-vrou)* | Whistle, lux |
| Cape Robin-Chat | Whistle, birdsong, lux |
| Southern Boubou | Voice, lux |
| Red-eyed Dove | Voice, lux |
| Glossy Starling | Whistle, clap, lux |
| Spotted Eagle-Owl | Voice, lux |
| Fork-tailed Drongo | Clap, lux |
| Cape Canary | Whistle, birdsong, lux |
| Southern Masked Weaver | Clap, lux |
| Red-billed Quelea | Clap, flock mode (60 % of flock calls) |
| Paradise Flycatcher | Birdsong, lux |

Bird selection varies by ambient light band (night / dawn / overcast / sunny) and is further biased by the Markov chain state and incoming ESP-NOW events from neighbouring nodes.

## ESP-NOW Mesh

Nodes communicate over ESP-NOW (broadcast to FF:FF:FF:FF:FF:FF — no pairing required). Two message types are exchanged:

- **Sound events** — when a node detects a whistle, voice, clap, or birdsong it broadcasts the detection type. Receiving nodes shift their bird selection toward the implied mood (e.g. clap → energetic birds).
- **Light events** — when ambient lux changes by more than the configured threshold a node broadcasts its reading. Receiving nodes blend it 50/50 with their own lux to create a shared network-wide mood.

Remote event influence expires after 30 seconds (configurable), after which each node reverts to its own sensor reading.

### Flock Mode

When 12 or more ESP-NOW messages arrive within a 6-second window (both thresholds remotely configurable), all nodes enter flock mode: rapid LED strobing and continuous bird calls with a 60 % Red-billed Quelea / 40 % random split. Flock mode holds for 10 seconds after the last qualifying burst then decays automatically.

## Markov Chain

Each node independently maintains a 17-state transition matrix over (detection type × light band) compound states, seeded with ecologically plausible priors and updated in real time from both local detections and received ESP-NOW events. The matrix is persisted to NVS flash and survives reboots.

Two outputs:
1. **Lux bias** — the current chain state applies a signed offset to the raw sensor reading before bird selection, nudging the mood based on recent network history.
2. **Autonomous calls** — after 45 seconds of network silence the chain samples the most probable next state and fires a bird call, keeping the installation alive when no one is interacting. A minimum 15-second gap is enforced between consecutive autonomous calls.

## Power Management

WiFi modem sleep (`WIFI_PS_MIN_MODEM`) is enabled after OTA completes, reducing idle current while keeping the connection alive for remote config polling every 60 seconds.

## Partition Table

```
# Name    Type   SubType  Offset    Size
nvs       data   nvs      0x9000    16 KB   NVS storage (WiFi credentials, Markov chain state)
otadata   data   ota      0xd000     8 KB   OTA state (active partition tracking)
phy_init  data   phy      0xf000     4 KB   RF calibration data
ota_0     app    ota_0    0x10000    1 MB   OTA firmware slot 0
ota_1     app    ota_1    0x110000   1 MB   OTA firmware slot 1
```

Note: there is no separate factory partition, for two reasons. First, the practical constraint: on a 4 MB chip with 1 MB firmware slots, adding a factory partition would require shrinking ota_0 and ota_1 below the size of a typical build. Second, and more importantly, a factory partition interferes with the OTA boot sequence during testing: when a factory partition is present the bootloader's default boot order is factory → ota_0 → ota_1, and if the factory image remains valid the bootloader can revert to it after an OTA update rather than staying on the new slot — causing an apparent OTA loop. Removing the factory partition ensures the bootloader always boots the most recently confirmed OTA slot.

The safety mechanism for an unattended installation is instead **OTA rollback**, which is enabled in `sdkconfig.defaults`:

```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

How this protects the installation: when a new firmware image is flashed via OTA, it boots in `PENDING_VERIFY` state. The firmware waits two minutes to confirm stability (see `main.c`), then calls `esp_ota_mark_app_valid_cancel_rollback()`. If the device crashes or reboots before that call, the bootloader automatically reverts to the previous OTA slot on the next boot. The previous slot always contains the last known-good image.

The only unrecoverable scenario is if both OTA slots are simultaneously corrupted (e.g. power loss mid-flash on both partitions), which requires physical reflashing via USB. For a gallery installation this is an acceptable risk given the impracticality of the factory partition alternative.

## Troubleshooting

### WiFi Connection Fails
- Check SSID and password via `idf.py menuconfig` → WiFi Configuration
- ESP32 supports 2.4 GHz only
- Blue LED blinks 3× on failure; device continues without WiFi or OTA

### OTA Update Fails
- Verify server: `curl http://192.168.101.2:8000/firmware/version.txt`
- Confirm binary exists: `ls ~/firmware_server/firmware/`
- Each OTA slot is 1 MB — ensure your binary fits
- Device retries up to 3 times with linear backoff before falling back
to normal operation without updating

### Remote Config Not Applying
- Verify `server.py` is running on port 8002
- Check device logs for `RCFG` tag — fetch errors are logged as warnings
- Devices fall back to built-in defaults if the server is unreachable

### No Sound / Only LED VU Meter

**If the device booted into Minimal mode (no BH1750 detected):**
- No BH1750 found on I2C at boot → firmware runs in Minimal mode; audio output is intentionally disabled regardless of MAX98357A wiring
- Confirm BH1750 is connected to SDA (GPIO 21) and SCL (GPIO 22) and powered
- Check device logs at boot for `Hardware detected: MINIMAL` to confirm

**If the device booted into Full mode but there is no sound:**
- Confirm MAX98357A wiring matches the pin assignments above (SCK=18, WS=19, DIN=17)
- Check `SILENT_MODE` and `SOUND_OFF` in the remote config UI at port 8002
- Verify quiet hours are not active (default window: 17:00–08:00)

### NVS / Flash Errors at Boot
```bash
idf.py -p /dev/tty.usbserial-110 erase-flash
idf.py -p /dev/tty.usbserial-110 flash
```

## Expected Boot Output
Indicative - expect variations based on hardware configuration and network
availability.

```
I (567) ECHOES: LEDs initialized
I (569) MAIN: ========================================
I (574) MAIN: Echoes of the Machine
I (577) MAIN: Firmware Version: 5.4.1
I (580) MAIN: ========================================
I (602) RCFG: Remote config initialised with defaults
I (603) MAIN: Initializing system...
I (606) ECHOES: BH1750 light sensor detected at 0x23
I (606) ECHOES: Hardware detected: FULL (BH1750 + Audio)
I (612) MARKOV: Loaded transition counts from NVS (578 bytes)
I (615) MARKOV: Markov chain ready — 17 states, current: STARTUP
I (621) MARKOV: Top transitions from STARTUP:
I (625) MARKOV:   #1  VOICE+NIGHT           32.5%
I (630) MARKOV:   #2  IDLE+DAWN             13.0%
I (634) MARKOV:   #3  VOICE+DAWN            13.0%
I (638) ECHOES: System initialized
I (642) MAIN: Connecting to WiFi...
I (645) OTA: Initializing WiFi...
I (15798) MAIN: Capturing device identity...
I (15798) STARTUP: Device MAC: xx:xx:xx:xx:xx:xx  Node type: echoes-full
I (15802) MAIN: Initializing audio hardware...
I (15808) ECHOES: Microphone (I2S RX) initialized @ 16000 Hz
I (15812) MAIN: Full hardware detected - initializing speaker
I (15818) ECHOES: Speaker (I2S TX) initialized @ 16000 Hz
I (17031) ESPNOW: espnow [version: 2.0] init
I (17032) ESPNOW: ESP-NOW mesh initialised (broadcast-only)
I (17033) ESPNOW: Initial local lux seeded: 0.8
I (17034) MAIN: Starting Echoes of the Machine (no-WiFi path)...
I (17040) ECHOES: Audio detection task started
```

## Version History

**5.4.1** — Current
- Server IP and all three server ports (OTA :8000, startup :8001, config :8002) moved to `idf.py menuconfig` (Kconfig) — no longer hardcoded in headers
- Race condition fix: `remote_config` struct now updated via mutex-protected atomic swap; `remote_config_is_quiet_hours()` snapshots fields under lock
- Documentation and deploy script improvements: `mkdir -p` in install scripts, `requirements.txt` for all three servers, stale version references cleaned up

**5.3.2**
- General tidy up of documentation and code with repo going public
- WiFi credentials moved to `idf.py menuconfig` (Kconfig) — no longer stored in source
- Status dashboard to see the state of all nodes
- Upgrade to ESP-IDF v5.5.2 (various subtle changes)

**5.1.3**
- ESP-NOW broadcast mesh with flock mode
- Markov chain with NVS persistence and autonomous calls
- Remote configuration server with live parameter updates and web UI
- Startup reporting server
- Quiet hours scheduling
- Birdsong detection (4th detection type)
- Dual hardware configurations (Full / Minimal)
- Lux-scaled playback volume

*Versions 2.x–5.x: development iterations; details available in the repository.*

**1.0.0** — Initial release
- WiFi and OTA updates
- 11 synthesised bird calls
- Whistle, voice, and clap detection
- Light-adaptive bird selection

---

## Authorship and Conceptual Contribution

* **Ken Nixon** – Hardware, software, and technical implementation.
* **Renzo Filinich Orozco** – Conceptual development, artistic vision, and curatorial framework.

All contributors should be credited in any reuse, citation, or derivative work.

## License

### Code

All software components in this repository are licensed under the **MIT License**.

You are free to use, modify, distribute, and build upon this code, including for commercial purposes, provided the original copyright notice and license are included.

### Documentation, Prompts, and Media

Textual materials, prompt designs, exhibition documentation, and accompanying media are licensed under the **Creative Commons Attribution 4.0 International (CC BY 4.0)**.

## LLM-Assisted Development Statement

This project incorporates Large Language Model (LLM) assistance in the development of selected code components and textual materials.

* LLM outputs were critically reviewed, edited, and integrated by the author (Ken Nixon).
* Conceptual framing, system design, and curatorial decisions were developed collaboratively with Renzo Filinich Orozco.
* Generated code and text function as material within an artistic and research-driven process.

This statement supports transparency, reproducibility, and academic integrity.

## Institutional Context

Developed within the academic context of the **University of the Witwatersrand, Johannesburg**.
Unless otherwise stated, the views expressed are those of the authors and do not necessarily reflect those of the institution.

## Citation / Attribution

If you reference or build upon this work, please cite or attribute:

> Nixon, K., & Filinich Orozco, R. (2026). *Echoes of the Machine*. University of the Witwatersrand. https://github.com/nixkj/echoes

---
