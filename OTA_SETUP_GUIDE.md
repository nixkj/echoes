# OTA Update Setup Guide
## Echoes of the Machine

Complete guide for setting up Over-The-Air firmware updates.

---

## Quick Setup (5 Minutes)

### 1. Configure WiFi

Edit `main/ota.h`:
```c
#define WIFI_SSID           "YourNetworkName"
#define WIFI_PASSWORD       "YourPassword"
```

### 2. Set Server URL

Edit `main/ota.h`:
```c
#define OTA_URL             "http://192.168.101.2/firmware/echoes.bin"
#define VERSION_URL         "http://192.168.101.2/firmware/version.txt"
```
Replace `192.168.101.2` with your computer's IP address.

### 3. Flash Firmware

```bash
# Source ESP-IDF
. ~/esp/esp-idf-v5.4/export.sh

# Build and flash
cd ~/echoes
idf.py build
idf.py -p /dev/tty.usbserial-110 erase-flash
idf.py -p /dev/tty.usbserial-110 flash monitor
```

### 4. Start OTA Server

```bash
# In your project directory
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

Your project should look like this:

```
echoes/
├── CMakeLists.txt
├── partitions.csv
├── firmware_server.py
├── build.sh
└── main/
    ├── CMakeLists.txt
    ├── main.c
    ├── echoes.h
    ├── echoes.c
    ├── synthesis.h
    ├── synthesis.c
    ├── ota.h
    └── ota.c
```

### Configuration Files

#### `main/ota.h` - WiFi and OTA Settings

```c
/* WiFi Configuration */
#define WIFI_SSID           "Echoes"           // Your WiFi name
#define WIFI_PASSWORD       "REMOVED_SECRET"         // Your WiFi password
#define WIFI_MAXIMUM_RETRY  5
#define WIFI_TIMEOUT_MS     20000

/* OTA Configuration */
#define FIRMWARE_VERSION    "1.0.0"            // Current version
#define OTA_URL             "http://192.168.101.2/firmware/echoes.bin"
#define VERSION_URL         "http://192.168.101.2/firmware/version.txt"
```

**Important:** 
- Use 2.4GHz WiFi only (ESP32 doesn't support 5GHz)
- Replace IP address with your server's IP
- Keep `echoes.bin` as the filename (server compatibility)

#### `partitions.csv` - Partition Table

This file defines the flash memory layout:

```csv
# Name,     Type, SubType, Offset,  Size,    Flags
nvs,        data, nvs,     0x9000,  0x6000,
phy_init,   data, phy,     0xf000,  0x1000,
factory,    app,  factory, 0x10000, 1M,
ota_0,      app,  ota_0,   ,        1M,
ota_1,      app,  ota_1,   ,        1M,
ota_data,   data, ota,     ,        0x2000,
```

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
I (50) boot: Partition Table:
I (59) boot:  0 nvs              WiFi data
I (66) boot:  1 phy_init         RF data
I (72) boot:  2 factory          factory app      00 00 00010000 00100000
I (79) boot:  3 ota_0            OTA app          00 10 00110000 00100000
I (86) boot:  4 ota_1            OTA app          00 11 00210000 00100000
I (93) boot:  5 ota_data         OTA data         01 00 00310000 00002000

I (294) MAIN: ========================================
I (302) MAIN: Echoes of the Machine
I (305) MAIN: Firmware Version: 1.0.0
I (315) MAIN: Connecting to WiFi...
I (324) OTA: WiFi connected successfully
I (335) OTA: Checking for firmware updates...
I (345) OTA: Firmware is up to date
```

If you see the OTA partitions (ota_0, ota_1, ota_data), **success!**

---

## Setting Up OTA Server

### Option 1: Using build.sh (Easiest)

```bash
./build.sh server
```

This starts a Python server at `http://YOUR_IP:8000`

### Option 2: Using Python Directly

```bash
python3 firmware_server.py
```

### Option 3: Manual Python Server

```bash
mkdir -p ~/firmware_server/firmware
cd ~/firmware_server
python3 -m http.server 8000
```

Then create these files:
```bash
# Create version file
echo "1.0.0" > firmware/version.txt

# Copy firmware binary
cp ~/echoes.bin firmware/echoes.bin
```

### Find Your Server IP

```bash
# macOS
ifconfig | grep "inet " | grep -v 127.0.0.1

# Linux
hostname -I
```

Use this IP in `main/ota.h` as `OTA_URL` and `VERSION_URL`.

---

## Creating and Deploying Updates

### Method 1: Using build.sh (Recommended)

```bash
# 1. Make code changes
nano main/echoes.c

# 2. Increment version
./build.sh version patch      # 1.0.0 → 1.0.1

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
   #define FIRMWARE_VERSION    "1.0.1"
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
   echo "1.0.1" > ~/firmware_server/firmware/version.txt
   ```

### Testing the Update

1. **Deploy new firmware** (version 1.0.1) to server
2. **Power on ESP32** with version 1.0.0
3. **Watch the update**:

```
I (335) OTA: Checking for firmware updates...
I (340) OTA: Current version: 1.0.0
I (345) OTA: Available version: 1.0.1
I (350) OTA: New firmware available! Updating...
I (355) OTA: Starting OTA update from: http://...
I (5000) OTA: OTA update successful! Restarting...
```

4. **Device reboots** with version 1.0.1

---

## How OTA Works

### Boot Process

```
Power On
   ↓
Bootloader reads partition table
   ↓
Boots from factory (first time)
or ota_0/ota_1 (after update)
   ↓
Your app starts
   ↓
Connects to WiFi
   ↓
Checks version.txt on server
   ↓
Compares with current version
   ↓
If newer: Download → Install → Reboot
If same: Continue normally
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

- **First boot**: Runs from `factory` partition
- **After update**: Alternates between `ota_0` and `ota_1`
- **Rollback**: If update fails, boots previous version

Example:
```
factory (1.0.0) → Update → ota_0 (1.0.1) → Update → ota_1 (1.0.2) → Update → ota_0 (1.0.3)
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
1. Verify server is running:
   ```bash
   curl http://192.168.101.2:8000/firmware/version.txt
   ```
2. Check firewall settings
3. Ensure ESP32 and server on same network
4. Verify IP address in `main/ota.h`

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

Edit `main/main.c` around line 66:

```c
// Option 1: Light sleep (default - 20-30mA)
esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

// Option 2: Disconnect (0mA, no periodic updates)
wifi_disconnect();

// Option 3: Full power (160-260mA)
// (comment out both lines above)
```

### Periodic Updates

To enable automatic daily checks, uncomment in `main/main.c`:

```c
xTaskCreate(ota_task, "ota_task", 8192, NULL, 3, NULL);
```

This checks every 24 hours (configurable in `main/ota.h`):

```c
#define OTA_CHECK_INTERVAL_MS    (24 * 60 * 60 * 1000)
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
./build.sh version patch      # 1.0.0 → 1.0.1
./build.sh version minor      # 1.0.0 → 1.1.0
./build.sh version major      # 1.0.0 → 2.0.0

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
- `README.md` - Project overview
- `BUILD_ERROR_FIXES.md` - Common build issues
- `FLASHING_INSTRUCTIONS.md` - Detailed flash guide
