#!/bin/bash

# Echoes of the Machine - Build and Deploy Script
# This script helps with building firmware and deploying OTA updates

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
# Firmware is now served from /opt/echoes/firmware (owned by the 'echoes'
# system user, group-writable for members of the 'echoes' group).
# The installer (scripts/server/install.sh) sets permissions automatically.
# To grant your build user deploy access:
#   sudo usermod -aG echoes $USER
# Log out and back in (or run 'newgrp echoes') after usermod.
FIRMWARE_DIR="/opt/echoes/firmware"
PROJECT_DIR="$(pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
BINARY_NAME="echoes.bin"
OTA_H="${PROJECT_DIR}/main/ota.h"   # Absolute path — never ambiguous

# Functions
print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}! $1${NC}"
}

print_info() {
    echo -e "${BLUE}→ $1${NC}"
}

# Check if ESP-IDF is sourced
check_idf() {
    if [ -z "$IDF_PATH" ]; then
        print_error "ESP-IDF environment not set up!"
        print_info "Run: . \$HOME/esp/esp-idf-v5.5.2/export.sh"
        exit 1
    fi
    print_success "ESP-IDF environment ready"
}

# Extract version from header file
get_current_version() {
    if [ -f "${OTA_H}" ]; then
        VERSION=$(grep "#define FIRMWARE_VERSION" "${OTA_H}" | cut -d'"' -f2)
        echo "$VERSION"
    else
        echo "unknown"
    fi
}

# Build firmware
build_firmware() {
    print_header "Building Firmware"
    
    local VERSION=$(get_current_version)
    print_info "Version: $VERSION (from ${OTA_H})"
    
    # Clean build (optional)
    if [ "$1" == "clean" ]; then
        print_info "Cleaning previous build..."
        idf.py fullclean
    fi
    
    # Check for a local config override one directory above the project.
    # sdkconfig.local lets each developer keep site-specific settings (WiFi
    # credentials, server IP, etc.) outside the repo without touching any
    # tracked files.  ESP-IDF v5 merges multiple defaults files in order;
    # later files take precedence, so local overrides win over sdkconfig.defaults.
    local LOCAL_CFG="${PROJECT_DIR}/../sdkconfig.local"
    # Build
    print_info "Building project..."
    if [ -f "$LOCAL_CFG" ]; then
        print_info "Applying local overrides from $LOCAL_CFG"
        SDKCONFIG_DEFAULTS="sdkconfig.defaults;${LOCAL_CFG}" idf.py build
    else
        idf.py build
    fi
    
    # Check build result
    if [ -f "${BUILD_DIR}/${BINARY_NAME}" ]; then
        local SIZE=$(stat -f%z "${BUILD_DIR}/${BINARY_NAME}" 2>/dev/null || stat -c%s "${BUILD_DIR}/${BINARY_NAME}")
        print_success "Build successful!"
        print_info "Firmware size: $(($SIZE / 1024)) KB"
        
        # Check if size exceeds 1MB
        if [ $SIZE -gt 1048576 ]; then
            print_warning "Firmware size exceeds 1MB! May not fit in OTA partition."
        fi
        
        return 0
    else
        print_error "Build failed!"
        return 1
    fi
}

# Flash firmware via USB
flash_firmware() {
    print_header "Flashing Firmware"
    
    # Find serial port
    local PORT=$(find_serial_port)
    
    if [ -z "$PORT" ]; then
        print_error "No serial port found!"
        print_info "Connect ESP32 and try again"
        return 1
    fi
    
    print_info "Using port: $PORT"
    print_info "Flashing firmware..."
    
    idf.py -p "$PORT" flash
    
    print_success "Flash complete!"
    print_info "Run './build.sh monitor' to view output"
}

# Find serial port automatically
find_serial_port() {
    # Try common serial port names (macOS and Linux)
    for PORT in /dev/tty.usbserial-* /dev/cu.usbserial-* /dev/ttyUSB0 /dev/ttyUSB1 /dev/cu.SLAB_USBtoUART; do
        if [ -e "$PORT" ]; then
            echo "$PORT"
            return 0
        fi
    done
    return 1
}

# Monitor serial output
monitor_serial() {
    print_header "Serial Monitor"
    
    local PORT=$(find_serial_port)
    
    if [ -z "$PORT" ]; then
        print_error "No serial port found!"
        return 1
    fi
    
    print_info "Monitoring $PORT (Ctrl+] to exit)"
    idf.py -p "$PORT" monitor
}

# Deploy to OTA server
deploy_ota() {
    print_header "Deploying OTA Update"
    
    # Check if build exists
    if [ ! -f "${BUILD_DIR}/${BINARY_NAME}" ]; then
        print_error "No firmware binary found!"
        print_info "Run './build.sh build' first"
        return 1
    fi
    
    # Get version
    local VERSION=$(get_current_version)
    print_info "Version: $VERSION"
    
    # Firmware directory must exist and be writable — created by install.sh
    if [ ! -d "$FIRMWARE_DIR" ]; then
        print_error "Firmware directory not found: $FIRMWARE_DIR"
        print_info "Run the server installer first: sudo bash scripts/server/install.sh"
        return 1
    fi
    if [ ! -w "$FIRMWARE_DIR" ]; then
        print_error "No write permission on $FIRMWARE_DIR"
        print_info "Ensure your user is in the 'echoes' group:"
        print_info "  sudo usermod -aG echoes \$USER"
        print_info "Then log out and back in (or run: newgrp echoes)"
        return 1
    fi
    
    # Copy binary
    print_info "Copying firmware binary..."
    cp "${BUILD_DIR}/${BINARY_NAME}" "${FIRMWARE_DIR}/echoes.bin"
    print_success "Binary copied to $FIRMWARE_DIR/echoes.bin"
    
    # Update version file
    print_info "Updating version file..."
    echo "$VERSION" > "${FIRMWARE_DIR}/version.txt"
    print_success "Version file updated"
    
    # Calculate MD5 for verification
    if command -v md5sum &> /dev/null; then
        MD5=$(md5sum "${FIRMWARE_DIR}/echoes.bin" | cut -d' ' -f1)
        print_info "MD5: $MD5"
    elif command -v md5 &> /dev/null; then
        MD5=$(md5 -q "${FIRMWARE_DIR}/echoes.bin")
        print_info "MD5: $MD5"
    fi

    # Archive — keep a timestamped copy under firmware/archive/<version>/
    local ARCHIVE_DIR="${FIRMWARE_DIR}/archive/${VERSION}"
    local TIMESTAMP=$(date "+%Y-%m-%d %H:%M:%S")
    local DATESTAMP=$(date "+%Y%m%d_%H%M%S")
    if [ -d "$ARCHIVE_DIR" ]; then
        # Version already archived — keep both, suffix with timestamp
        ARCHIVE_DIR="${FIRMWARE_DIR}/archive/${VERSION}_${DATESTAMP}"
        print_warning "Archive for $VERSION already exists — saving as ${VERSION}_${DATESTAMP}"
    fi
    mkdir -p "$ARCHIVE_DIR"
    cp "${FIRMWARE_DIR}/echoes.bin"  "$ARCHIVE_DIR/echoes.bin"
    echo "$VERSION"                  > "$ARCHIVE_DIR/version.txt"
    # Write a manifest with build metadata
    cat > "$ARCHIVE_DIR/manifest.txt" <<EOF
version:    $VERSION
deployed:   $TIMESTAMP
deployed_by: ${USER:-unknown}
md5:        ${MD5:-unknown}
source:     ${BUILD_DIR}/${BINARY_NAME}
size_bytes: $(stat -c%s "${FIRMWARE_DIR}/echoes.bin" 2>/dev/null || stat -f%z "${FIRMWARE_DIR}/echoes.bin")
EOF
    print_success "Archived to $ARCHIVE_DIR"
    
    print_success "OTA update deployed!"
    print_info "Firmware: ${FIRMWARE_DIR}/echoes.bin"
    print_info "Version: $VERSION"
    print_warning "Make sure firmware server is running!"
}

# Increment version number
bump_version() {
    print_header "Incrementing Version"
    
    local CURRENT_VERSION=$(get_current_version)
    print_info "Current version: $CURRENT_VERSION"
    
    # Validate we actually got a version
    if [ "$CURRENT_VERSION" == "unknown" ] || [ -z "$CURRENT_VERSION" ]; then
        print_error "Could not read current version from main/ota.h"
        return 1
    fi
    
    # Parse version
    IFS='.' read -r -a VERSION_PARTS <<< "$CURRENT_VERSION"
    MAJOR="${VERSION_PARTS[0]}"
    MINOR="${VERSION_PARTS[1]}"
    PATCH="${VERSION_PARTS[2]}"
    
    # Increment based on argument
    case "$1" in
        major)
            MAJOR=$((MAJOR + 1))
            MINOR=0
            PATCH=0
            ;;
        minor)
            MINOR=$((MINOR + 1))
            PATCH=0
            ;;
        patch|*)
            PATCH=$((PATCH + 1))
            ;;
    esac
    
    NEW_VERSION="${MAJOR}.${MINOR}.${PATCH}"
    
    # Show exactly which file we're editing so mismatches are obvious
    print_info "Editing: ${OTA_H}"

    # Use LC_ALL=C to force byte-level sed behaviour regardless of the
    # file's encoding.  Without this, a single non-ASCII character anywhere
    # in ota.h (e.g. a UTF-8 ellipsis or multiplication sign in a comment)
    # can cause sed to silently skip the substitution on macOS.
    if [[ "$OSTYPE" == "darwin"* ]]; then
        LC_ALL=C sed -i '' "s/#define FIRMWARE_VERSION.*/#define FIRMWARE_VERSION    \"${NEW_VERSION}\"/" "${OTA_H}"
    else
        LC_ALL=C sed -i "s/#define FIRMWARE_VERSION.*/#define FIRMWARE_VERSION    \"${NEW_VERSION}\"/" "${OTA_H}"
    fi
    
    # Verify the change actually took effect
    local WRITTEN_VERSION=$(get_current_version)
    if [ "$WRITTEN_VERSION" != "$NEW_VERSION" ]; then
        print_error "Version update failed! File shows '$WRITTEN_VERSION', expected '$NEW_VERSION'"
        print_info "File being edited: ${OTA_H}"
        print_info "Check for non-ASCII characters: grep -Pn '[^\\x00-\\x7F]' \"${OTA_H}\""
        return 1
    fi
    
    print_success "Version updated: $CURRENT_VERSION → $NEW_VERSION"
}

# Erase flash completely
erase_flash() {
    print_header "Erasing Flash"
    
    local PORT=$(find_serial_port)
    
    if [ -z "$PORT" ]; then
        print_error "No serial port found!"
        return 1
    fi
    
    print_warning "This will erase ALL data on the ESP32!"
    read -p "Continue? (y/N) " -n 1 -r
    echo
    
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        print_info "Erasing flash on $PORT..."
        idf.py -p "$PORT" erase-flash
        print_success "Flash erased!"
    else
        print_info "Erase cancelled"
    fi
}

# Install the consolidated server as a systemd service
install_services() {
    print_header "Installing Server Service"

    local INSTALLER="${PROJECT_DIR}/scripts/server/install.sh"

    if [ ! -f "$INSTALLER" ]; then
        print_error "Installer not found: $INSTALLER"
        return 1
    fi

    print_info "Installing consolidated echoes-server (ports 8002)..."
    sudo bash "$INSTALLER"
    print_success "echoes-server installed"

    print_success "Service installed and started"
    print_info "Check status with: sudo systemctl status echoes-server"
}

# Show help
show_help() {
    cat << EOF
Echoes of the Machine - Build & Deploy Script

Usage: ./build.sh [command] [options]

Commands:
  build [clean]     Build firmware (optional: clean build)
  tidyup            Remove editor backups, build/, and sdkconfig
  flash             Flash firmware via USB
  erase             Erase flash completely
  monitor           Open serial monitor
  deploy            Deploy firmware to OTA server
  version [type]    Increment version (major|minor|patch)
  services          Install the consolidated echoes-server service
  all               Bump patch version, build, and deploy
  help              Show this help message

Examples:
  ./build.sh build              # Build firmware
  ./build.sh tidyup             # Remove editor backups, build/, and sdkconfig
  ./build.sh build clean        # Clean build
  ./build.sh flash              # Flash via USB
  ./build.sh erase              # Erase flash
  ./build.sh deploy             # Deploy to OTA server
  ./build.sh version minor      # Increment minor version
  ./build.sh services           # Install consolidated server as systemd service
  ./build.sh all                # Bump patch version, build, and deploy

Workflow:
  1. Make code changes
  2. ./build.sh build           # Test build
  3. ./build.sh flash           # Flash to device
  4. ./build.sh version patch   # Increment version
  5. ./build.sh deploy          # Deploy OTA update
  6. ./build.sh services        # Install server (first time, run on host/Pi)

OTA Update Workflow:
  1. ./build.sh version patch   # Update version number
  2. ./build.sh build           # Build new firmware
  3. ./build.sh deploy          # Deploy to server at ${FIRMWARE_DIR}
  4. Power on ESP32 → It will auto-update

Local config overrides:
  Create ../sdkconfig.local (one level above this repo) to set site-specific
  values such as WiFi credentials and server IP without touching tracked files.
  The build automatically detects and merges it — later entries take precedence
  over sdkconfig.defaults.  Example ../sdkconfig.local:
    CONFIG_WIFI_SSID="my_network"
    CONFIG_WIFI_PASSWORD="my_password"
    CONFIG_SERVER_IP="192.168.1.100"

Firmware directory: ${FIRMWARE_DIR}
  The deploy step writes to this path. The installer sets group-write
  permissions automatically. To grant your build user deploy access:
    sudo usermod -aG echoes \$USER
  Log out and back in (or run 'newgrp echoes') after usermod.

ESP32 Port Detection:
  Automatically detects: /dev/tty.usbserial-*, /dev/ttyUSB*, etc.
  
EOF
}

# Main script
main() {
    case "$1" in
        build)
            check_idf
            build_firmware "$2"
            ;;
        tidyup)
            # Remove editor backup files (*~ and .*.un~ vim/emacs files) from
            # anywhere in the project tree, and delete the build/ directory.
            # Site-specific settings belong in ../sdkconfig.local (outside the
            # repo) so they are never lost by a tidy.
            find . -type f \( -name '*~' -o -name '.*.un~' -o -name '.DS_Store' \) -delete
            rm -rf build
            print_success "Tidy complete — editor backups and build/* removed"
            ;;
        flash)
            check_idf
            flash_firmware
            ;;
        erase)
            check_idf
            erase_flash
            ;;
        monitor)
            check_idf
            monitor_serial
            ;;
        deploy)
            deploy_ota
            ;;
        version)
            bump_version "$2"
            ;;
        services)
            install_services
            ;;
        all)
            check_idf
            bump_version patch
            build_firmware
            deploy_ota
            ;;
        help|--help|-h|"")
            show_help
            ;;
        *)
            print_error "Unknown command: $1"
            show_help
            exit 1
            ;;
    esac
}

# Run main
main "$@"
