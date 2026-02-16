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
FIRMWARE_DIR="${HOME}/firmware_server/firmware"
PROJECT_DIR="$(pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
BINARY_NAME="echoes.bin"

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
        print_info "Run: . \$HOME/esp/esp-idf-v5.4/export.sh"
        exit 1
    fi
    print_success "ESP-IDF environment ready"
}

# Extract version from header file
get_current_version() {
    if [ -f "main/ota.h" ]; then
        VERSION=$(grep "#define FIRMWARE_VERSION" main/ota.h | cut -d'"' -f2)
        echo "$VERSION"
    else
        echo "unknown"
    fi
}

# Build firmware
build_firmware() {
    print_header "Building Firmware"
    
    local VERSION=$(get_current_version)
    print_info "Version: $VERSION"
    
    # Clean build (optional)
    if [ "$1" == "clean" ]; then
        print_info "Cleaning previous build..."
        idf.py fullclean
    fi
    
    # Build
    print_info "Building project..."
    idf.py build
    
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
    
    # Create firmware directory if it doesn't exist
    if [ ! -d "$FIRMWARE_DIR" ]; then
        print_info "Creating firmware directory..."
        mkdir -p "$FIRMWARE_DIR"
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
    
    # Update header file
    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS
        sed -i '' "s/#define FIRMWARE_VERSION.*/#define FIRMWARE_VERSION    \"${NEW_VERSION}\"/" main/ota.h
    else
        # Linux
        sed -i "s/#define FIRMWARE_VERSION.*/#define FIRMWARE_VERSION    \"${NEW_VERSION}\"/" main/ota.h
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

# Start firmware server
start_server() {
    print_header "Starting Firmware Server"
    
    if [ ! -f "firmware_server.py" ]; then
        print_error "firmware_server.py not found!"
        return 1
    fi
    
    print_info "Starting server on port 8000..."
    python3 firmware_server.py
}

# Show help
show_help() {
    cat << EOF
Echoes of the Machine - Build & Deploy Script

Usage: ./build.sh [command] [options]

Commands:
  build [clean]     Build firmware (optional: clean build)
  clean             Remove edit backups and build directory
  flash             Flash firmware via USB
  erase             Erase flash completely
  monitor           Open serial monitor
  deploy            Deploy firmware to OTA server
  version [type]    Increment version (major|minor|patch)
  server            Start firmware update server
  all               Build, bump patch version, and deploy
  help              Show this help message

Examples:
  ./build.sh build              # Build firmware
  ./build.sh clean              # Tidy up edit files and build dir
  ./build.sh build clean        # Clean build
  ./build.sh flash              # Flash via USB
  ./build.sh erase              # Erase flash
  ./build.sh deploy             # Deploy to OTA server
  ./build.sh version minor      # Increment minor version
  ./build.sh all                # Build and deploy with patch bump

Workflow:
  1. Make code changes
  2. ./build.sh build           # Test build
  3. ./build.sh flash           # Flash to device
  4. ./build.sh version patch   # Increment version
  5. ./build.sh deploy          # Deploy OTA update
  6. ./build.sh server          # Start update server

OTA Update Workflow:
  1. ./build.sh version patch   # Update version number
  2. ./build.sh build           # Build new firmware
  3. ./build.sh deploy          # Deploy to server
  4. Power on ESP32 → It will auto-update

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
        clean)
            find . -type f \( -name '*~' -o -name '.*.un~' \) -delete
	    rm -rf build sdkconfig
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
        server)
            start_server
            ;;
        all)
            check_idf
            build_firmware
            bump_version patch
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
