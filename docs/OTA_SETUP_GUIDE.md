# OTA Update Setup Guide
## Echoes of the Machine

Complete guide for setting up Over-The-Air firmware updates.

---

## Quick Setup (5 Minutes)

### 1. Configure WiFi and Server

Edit `main/ota.h`:
```c
#define WIFI_SSID        "Echoes"
#define WIFI_PASSWORD    "REMOVED_SECRET"
#define FIRMWARE_VERSION "5.1.3"
#define OTA_URL          "http://192.168.101.2:8000/firmware/echoes.bin"
#define VERSION_URL      "http://192.168.101.2:8000/firmware/version.txt"
```
Replace `192.168.101.2` with your server's IP address. Note port `:8000` is required.

### 2. Flash Firmware

```bash
# Source ESP-IDF
. ~/esp/esp-idf-v5.4/export.sh

# Build and flash
cd ~/echoes
idf.py build
idf.py -p /dev/tty.usbserial-110 erase-flash
idf.py -p /dev/tty.usbserial-110 flash monitor
```

### 3. Start OTA Server

```bash
python3 scripts/firmware_server/firmware_server.py
```

Or via `build.sh`:
```bash
./build.sh server
```

Done! Your ESP32 will now check for updates on boot.

---

## Detailed Setup

### Prerequisites

**ESP-IDF v5.4** must be installed:

```bash
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.4
cd esp-idf-v5.4
git checkout v5.4
./install.sh
. ./export.sh  # Run this in every new terminal
```

**Python 3** for the OTA server (built-in on macOS/Linux)

### Project Structure

```
echoes/
├── CMakeLists.txt
├── partitions.csv
├── build.sh
├── scripts/
│   ├── config_server/
│   │   └── server.py                # Remote config UI (port 8002)
│   ├── firmware_server/
│   │   └── firmware_server.py       # OTA firmware server (port 8000)
│   └── startup_server/
│       └── startup_server.py        # Startup report receiver (port 8001)
└── main/
    ├── CMakeLists.txt
    ├── main.c
    ├── ota.h / ota.c
    ├── echoes.h / echoes.c
    └── ...
```

### Configuration Files

#### `main/ota.h` - WiFi and OTA Settings

```c
/* WiFi Configuration */
#define WIFI_SSID           "Echoes"
#define WIFI_PASSWORD       "REMOVED_SECRET"
#define WIFI_MAXIMUM_RETRY  5
#define WIFI_TIMEOUT_MS     20000

/* OTA Configuration */
#define FIRMWARE_VERSION    "5.1.3"            // Current version
#define OTA_URL             "http://192.168.101.2:8000/firmware/echoes.bin"
#define VERSION_URL         "http://192.168.101.2:8000/firmware/version.txt"
```

**Important:**
- Use 2.4 GHz WiFi only (ESP32 doesn't support 5 GHz)
- Replace IP address with your server's IP
- Port `:8000` is required in both URLs
- Keep `echoes.bin` as the filename (server compatibility)

#### `partitions.csv` - Partition Table

This file defines the flash memory layout:

```csv
# Name,     Type, SubType, Offset,   Size
nvs,        data, nvs,     0x9000,   0x4000
otadata,    data, ota,     0xd000,   0x2000
phy_init,   data, phy,     0xf000,   0x1000
ota_0,      app,  ota_0,   0x10000,  0x100000
ota_1,      app,  ota_1,   0x110000, 0x100000
```

There is no factory partition — the device boots from `ota_0` or `ota_1` exclusively. Each OTA slot is 1 MB; keep your firmware binary under this limit.

### Building and Flashing

#### First Time Flash

```bash
# 1. Source ESP-IDF
. ~/esp/esp-idf-v5.4/export.sh

# 2. Navigate to project
cd ~/echoes

# 3. Build
idf.py build

# 4. Erase flash (clears old partition table)
idf.py -p /dev/tty.usbserial-110 erase-flash

# 5. Flash and monitor
idf.py -p /dev/tty.usbserial-110 flash monitor
```

#### Expected Output

After flashing, you should see:

```
I (xxx) boot: Partition Table:
I (xxx) boot:  0 nvs       WiFi data
I (xxx) boot:  1 otadata   OTA data
I (xxx) boot:  2 phy_init  RF data
I (xxx) boot:  3 ota_0     OTA app    ← Should see this
I (xxx) boot:  4 ota_1     OTA app    ← And this

I (xxx) MAIN: ========================================
I (xxx) MAIN: Echoes of the Machine
I (xxx) MAIN: Firmware Version: 5.1.3
I (xxx) MAIN: ========================================
I (xxx) MAIN: Connecting to WiFi...
I (xxx) OTA:  Connected to WiFi SSID: Echoes
I (xxx) MAIN: Checking for firmware updates...
I (xxx) OTA:  Firmware is up to date (5.1.3)
```

If you see both OTA partitions (`ota_0`, `ota_1`), **success!**

---

## Setting Up OTA Server

### Option 1: Using Python Directly

```bash
python3 scripts/firmware_server/firmware_server.py
```

The server listens on port 8000, serves files from `~/firmware_server/firmware/`, and handles concurrent connections with threaded streaming.

### Option 2: Using build.sh

```bash
./build.sh server
```

### Option 3: As a systemd Service

Install scripts and service unit files are provided in `scripts/firmware_server/`:

```bash
cd scripts/firmware_server
./install-service.sh
```

See `scripts/firmware_server/SYSTEMD_SERVICE_GUIDE.md` for details.

### Find Your Server IP

```bash
# macOS
ifconfig | grep "inet " | grep -v 127.0.0.1

# Linux
hostname -I
```

After starting the server, copy your binary and set the version:

```bash
cp build/echoes.bin ~/firmware_server/firmware/echoes.bin
echo "5.1.3" > ~/firmware_server/firmware/version.txt
```

---

## Creating and Deploying Updates

### Method 1: Using build.sh (Recommended)

```bash
# 1. Make code changes
nano main/echoes.c

# 2. Increment version
./build.sh version patch      # 5.1.3 → 5.1.4

# 3. Build
./build.sh build

# 4. Deploy to server
./build.sh deploy

# 5. Start server (if not running)
./build.sh server
```

### Method 2: Manual Process

1. **Update version** in `main/ota.h`:
   ```c
   #define FIRMWARE_VERSION    "5.1.4"
   ```

2. **Build**:
   ```bash
   idf.py build
   ```

3. **Copy binary**:
   ```bash
   cp build/echoes.bin ~/firmware_server/firmware/echoes.bin
   ```

4. **Update version file**:
   ```bash
   echo "5.1.4" > ~/firmware_server/firmware/version.txt
   ```

### Testing the Update

1. **Deploy new firmware** (version 5.1.4) to server
2. **Power on ESP32** with version 5.1.3
3. **Watch the update**:

```
I (xxx) OTA: Checking for firmware updates (current: 5.1.3)
I (xxx) OTA: Fetched version: 5.1.4
I (xxx) OTA: New firmware available: 5.1.3 → 5.1.4
I (xxx) OTA: Starting OTA update from: http://192.168.101.2:8000/firmware/echoes.bin
I (xxx) OTA: Background tasks suspended for OTA download
I (xxx) OTA: Starting download...
I (xxx) OTA: Download complete (XXXXXX bytes)
I (xxx) OTA: OTA update successful! Restarting...
```

4. **Device reboots** with version 5.1.4

---

## How OTA Works

### Boot Process

```
Power On
   ↓
Bootloader reads partition table
   ↓
Boots from ota_0 or ota_1 (whichever is marked active)
   ↓
App starts — connects to WiFi
   ↓
If new OTA image (PENDING_VERIFY):
  waits 2 minutes to confirm stability,
  then marks firmware valid (cancels rollback timer)
   ↓
Fetches remote config from server (port 8002)
   ↓
Checks version.txt on OTA server (port 8000)
   ↓
Compares using semantic versioning (major.minor.patch)
   ↓
If newer: Download → flash inactive slot → restart
If same or older: Continue to normal operation
```

### Update Process

1. **WiFi connects** (20s timeout)
2. **Downloads version.txt** from server
3. **Compares** with `FIRMWARE_VERSION`
4. **If newer**:
   - Downloads firmware binary
   - Blue LED on during download
   - Writes to inactive OTA partition
   - Marks new partition as bootable
   - White LED flash (success)
   - Reboots
5. **If same/older**: Continues to normal operation

### Partition Switching

The device alternates between `ota_0` and `ota_1` on each successful update:

```
(initial flash) ota_0 (5.1.3)
→ update →      ota_1 (5.1.4)
→ update →      ota_0 (5.1.5)
→ update →      ota_1 (5.1.6)
```

---

## Troubleshooting

### WiFi Won't Connect

**Symptom:** Blue LED blinks 3 times, no update check

**Solutions:**
1. Verify SSID and password in `main/ota.h`
2. Check WiFi is 2.4GHz (not 5GHz)
3. Ensure ESP32 is in range
4. Check router logs

**Test:**
```bash
idf.py monitor
# Look for: "Connected to WiFi SSID: YourNetwork"
```

### Can't Fetch Version

**Symptom:** "Failed to fetch version information"

**Solutions:**
1. Verify server is running and accessible:
   ```bash
   curl http://192.168.101.2:8000/firmware/version.txt
   ```
2. Confirm port `:8000` is included in `VERSION_URL` in `main/ota.h`
3. Check firewall settings — port 8000 must be reachable from the ESP32's subnet
4. Ensure ESP32 and server are on the same network

### Update Fails

**Symptom:** "OTA update failed", blue LED rapid blink

**Solutions:**
1. Check binary exists:
   ```bash
   ls -lh ~/firmware_server/firmware/echoes.bin
   ```
2. Verify file size < 1MB:
   ```bash
   ./build.sh build  # Shows size
   ```
3. Check server logs for errors
4. Try re-deploying: `./build.sh deploy`

### Partition Table Not Found

**Symptom:** Boot shows old partition table (no ota_0/ota_1)

**Solution:**
```bash
# Full erase and reflash
idf.py -p /dev/tty.usbserial-110 erase-flash
idf.py -p /dev/tty.usbserial-110 flash
```

### Firmware Too Large

**Symptom:** Build warning "Firmware size exceeds 1MB"

**Solutions:**
1. Optimize code (remove debug logs)
2. Increase partition size in `partitions.csv`:
   ```csv
   ota_0,      app,  ota_0,   ,        1536K,
   ota_1,      app,  ota_1,   ,        1536K,
   ```

---

## Power Optimization

### WiFi Power Modes

WiFi modem sleep (`WIFI_PS_MIN_MODEM`) is enabled automatically after OTA completes, reducing idle current while keeping the connection alive for remote config polling every 60 seconds.

### Periodic Update Checks

`ota_task()` supports periodic background checks every 24 hours but is not started by default — OTA runs once at boot only. To enable periodic checks, add this to `app_main()` in `main/main.c` after the initial OTA call:

```c
xTaskCreate(ota_task, "ota_task", 8192, NULL, 3, NULL);
```

The interval is configurable in `main/ota.h`:

```c
#define OTA_CHECK_INTERVAL_MS    (24 * 60 * 60 * 1000)   // 24 hours
```

---

## Advanced Topics

### Secure OTA (HTTPS)

For production, use HTTPS:

1. Generate server certificate
2. Update `main/ota.c`:

```c
extern const uint8_t server_cert_pem_start[] asm("_binary_server_cert_pem_start");

esp_http_client_config_t config = {
    .url = "https://yourserver.com/firmware/echoes.bin",
    .cert_pem = (char *)server_cert_pem_start,
};
```

### Firmware Signing

Prevent unauthorized updates:

1. Enable in `sdkconfig`:
   ```
   CONFIG_SECURE_BOOT_ENABLED=y
   CONFIG_SECURE_SIGNED_APPS_ECDSA_SCHEME=y
   ```

2. Generate signing keys
3. Sign firmware during build

### Rollback Protection

If update fails repeatedly, automatically rollback:

Enable in `sdkconfig.defaults`:
```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

---

## Quick Reference

```bash
# Build commands
./build.sh build              # Build
./build.sh build clean        # Clean build
./build.sh flash              # Flash via USB
./build.sh monitor            # Serial monitor

# Version management
./build.sh version patch      # 5.1.3 → 5.1.4
./build.sh version minor      # 5.1.3 → 5.2.0
./build.sh version major      # 5.1.3 → 6.0.0

# OTA deployment
./build.sh deploy             # Deploy to server
./build.sh server             # Start server
./build.sh all                # Build + deploy

# Debugging
idf.py monitor                # View logs
idf.py erase-flash            # Erase everything
./build.sh erase              # Erase (interactive)
```

---

## Summary

OTA updates on your ESP32 are now working! Here's the workflow:

1. **Development**: Make changes, build, flash via USB
2. **Release**: Increment version, build, deploy to server
3. **Update**: ESP32 checks on boot, downloads if newer, reboots
4. **Rollback**: If update fails, previous version still available

The system is designed to be robust and automatic, with clear LED feedback at each stage.

For more help, see:
- `README.md` — Project overview, full feature list, and LED indicators
- `scripts/firmware_server/SYSTEMD_SERVICE_GUIDE.md` — Running the OTA server as a persistent background service
