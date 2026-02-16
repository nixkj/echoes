#!/bin/bash

# Echoes Firmware Server - Systemd Service Uninstaller

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

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

echo "=========================================="
echo "Echoes Firmware Server - Uninstaller"
echo "=========================================="
echo ""

# Check if service exists
if [ ! -f /etc/systemd/system/echoes-firmware.service ]; then
    print_error "Service not installed"
    exit 1
fi

print_warning "This will remove the firmware server service"
read -p "Continue? (y/N) " -n 1 -r
echo

if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    print_info "Uninstall cancelled"
    exit 0
fi

# Stop service
print_info "Stopping service..."
if sudo systemctl is-active --quiet echoes-firmware.service; then
    sudo systemctl stop echoes-firmware.service
    print_success "Service stopped"
else
    print_info "Service not running"
fi

# Disable service
print_info "Disabling service..."
sudo systemctl disable echoes-firmware.service
print_success "Service disabled"

# Remove service file
print_info "Removing service file..."
sudo rm /etc/systemd/system/echoes-firmware.service
print_success "Service file removed"

# Reload systemd
print_info "Reloading systemd..."
sudo systemctl daemon-reload
print_success "Systemd reloaded"

echo ""
print_success "Service uninstalled successfully!"
echo ""
print_info "To start the server manually:"
print_info "  python3 firmware_server.py"
echo ""
