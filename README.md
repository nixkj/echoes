# Echoes of the Machine - Bird Call System with OTA Updates

A sophisticated bird call detection and response system with wireless firmware update capability.

## Features

- 🎵 Detects whistles, voice commands, and claps
- 🐦 Responds with authentic South African bird calls
- 💡 Light-adaptive bird selection (day/night species)
- 📡 WiFi connectivity with OTA firmware updates
- 🔄 Automatic update checking at boot
- 🔋 Power-efficient operation with WiFi sleep modes
- 🎛️ LED indicators for status feedback

## Hardware Requirements

- ESP32-D0WD-V3 development board
- Adafruit ICS-43434 I2S MEMS Microphone
- Adafruit MAX98357A I2S Amplifier
- Speaker (4-8Ω)
- White LED (GPIO13)
- Blue LED (GPIO2)
- Optional: BH1750 light sensor or ALS-PT19 analog sensor
- Power supply (5V USB or battery)

## Quick Start

### 1. Prerequisites

```bash
# Install ESP-IDF v5.4
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.4
cd esp-idf-v5.4 && git checkout v5.4
./install.sh
. ./export.sh
```

### 2. Build and Flash

```bash
# Navigate to your project directory
cd ~/echoes

# Configure WiFi credentials
nano main/ota.h
# Edit: WIFI_SSID and WIFI_PASSWORD
# Edit: OTA_URL and VERSION_URL (your server IP)

# Build
idf.py build

# Flash (first time requires erase)
idf.py -p /dev/tty.usbserial-110 erase-flash
idf.py -p /dev/tty.usbserial-110 flash monitor
```

### 3. Set Up OTA Server

```bash
# Start firmware server
./build.sh server

# Or manually:
python3 firmware_server.py
```

## File Structure

```
echoes/
├── CMakeLists.txt              # Project build config
├── partitions.csv              # OTA partition table
├── sdkconfig                   # SDK configuration
├── scripts
    └── firmware_server
        └── firmware_server.py  # OTA update server
├── build.sh                    # Build/deploy script
└── main/
    ├── CMakeLists.txt          # Component config
    ├── main.c                  # Entry point with OTA
    ├── echoes.h/.c             # Core system
    ├── synthesis.h/.c          # Bird call synthesis
    └── ota.h/.c                # OTA functionality
```

## Configuration

### WiFi Settings (`main/ota.h`)

```c
#define WIFI_SSID           "YourNetworkName"
#define WIFI_PASSWORD       "YourPassword"
```

### OTA Server (`main/ota.h`)

```c
#define FIRMWARE_VERSION    "1.0.0"
#define OTA_URL             "http://192.168.101.2/firmware/echoes.bin"
#define VERSION_URL         "http://192.168.101.2/firmware/version.txt"
```

## Creating Firmware Updates

### Using build.sh (Recommended)

```bash
./build.sh version patch    # Increment version
./build.sh build            # Build firmware
./build.sh deploy           # Deploy to server
./build.sh server           # Start server
```

### Manual Process

1. Edit version in `main/ota.h`
2. Build: `idf.py build`
3. Copy: `cp build/echoes.bin ~/firmware_server/firmware/echoes.bin`
4. Update: `echo "1.0.1" > ~/firmware_server/firmware/version.txt`

## Build Script Commands

```bash
./build.sh build              # Build firmware
./build.sh flash              # Flash via USB
./build.sh erase              # Erase flash
./build.sh monitor            # Serial monitor
./build.sh version patch      # Bump version
./build.sh deploy             # Deploy OTA
./build.sh server             # Start server
./build.sh all                # Build + bump + deploy
```

## LED Indicators

### Normal Operation
- **White LED**: Bird call activity
- **Blue LED**: Light level

### OTA Status
- **Blue 3× blink**: WiFi failed
- **Blue steady**: Downloading
- **White 1s**: Update success
- **Blue rapid blink**: Update failed

## Power Management

Edit `main/main.c` line 66:

```c
esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // Light sleep (20-30mA)
wifi_disconnect();                    // No WiFi (0mA)
// (comment out both)                 // Full power (160-260mA)
```

## Partition Table

```
nvs         24KB    WiFi credentials
phy_init     4KB    RF calibration
factory      1MB    Original firmware
ota_0        1MB    Update slot 1
ota_1        1MB    Update slot 2
ota_data     8KB    OTA state
```

## Troubleshooting

### WiFi Connection Fails
- Check credentials in `main/ota.h`
- Use 2.4GHz WiFi only
- Blue LED blinks 3× on failure

### OTA Update Fails  
- Verify server: `curl http://YOUR_IP/firmware/version.txt`
- Check file exists: `ls ~/firmware_server/firmware/`
- Ensure firmware < 1MB

### Partition Table Not Found
```bash
idf.py -p /dev/tty.usbserial-110 erase-flash
idf.py -p /dev/tty.usbserial-110 flash
```

## Expected Boot Output

```
I (50) boot: Partition Table:
I (79) boot:  3 ota_0            OTA app          ← Should see this
I (86) boot:  4 ota_1            OTA app          ← And this
I (93) boot:  5 ota_data         OTA data         ← And this

I (294) MAIN: Echoes of the Machine
I (305) MAIN: Firmware Version: 1.0.0
I (315) MAIN: Connecting to WiFi...
I (324) OTA: WiFi connected successfully
I (335) OTA: Checking for firmware updates...
```

## Documentation

- `README.md` - This file
- `OTA_SETUP_GUIDE.md` - Detailed OTA setup
- `BUILD_ERROR_FIXES.md` - Common build fixes
- `FLASHING_INSTRUCTIONS.md` - Flash guide

## Version History

**1.0.0** - Initial release
- WiFi & OTA updates
- 11 bird calls
- Sound detection
- Light adaptation

## License

[Your license here]

---

**Quick Reference:**
- Source ESP-IDF: `. ~/esp/esp-idf-v5.4/export.sh`
- Build: `./build.sh build`
- Flash: `./build.sh flash`
- Deploy: `./build.sh deploy`
- Server: `./build.sh server`
