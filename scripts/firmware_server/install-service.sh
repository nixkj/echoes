#!/bin/bash

# Echoes Firmware Server - Systemd Service Installer
# This script installs the firmware server as a system service

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

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

# Check if running with sudo
if [ "$EUID" -eq 0 ]; then 
    print_error "Don't run this script as root/sudo"
    print_info "The script will ask for sudo when needed"
    exit 1
fi

print_header "Echoes Firmware Server - Service Installer"

# Get current user and directory
CURRENT_USER=$(whoami)
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

print_info "Current user: $CURRENT_USER"
print_info "Script directory: $SCRIPT_DIR"

# Check if firmware_server.py exists
if [ ! -f "$SCRIPT_DIR/firmware_server.py" ]; then
    print_error "firmware_server.py not found in $SCRIPT_DIR"
    print_info "Make sure you're running this from the project directory"
    exit 1
fi

print_success "Found firmware_server.py"

# Create service file
print_info "Creating systemd service file..."

SERVICE_FILE="/tmp/echoes-firmware.service"
cat > "$SERVICE_FILE" << EOF
[Unit]
Description=Echoes Firmware Update Server
After=network.target

[Service]
Type=simple
User=$CURRENT_USER
WorkingDirectory=$SCRIPT_DIR
ExecStart=/usr/bin/python3 $SCRIPT_DIR/firmware_server.py
Restart=always
RestartSec=10

# Security settings
NoNewPrivileges=true
PrivateTmp=true

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=echoes-firmware

[Install]
WantedBy=multi-user.target
EOF

print_success "Service file created"

# Install service
print_info "Installing service (requires sudo)..."
sudo cp "$SERVICE_FILE" /etc/systemd/system/echoes-firmware.service
sudo chmod 644 /etc/systemd/system/echoes-firmware.service

print_success "Service installed to /etc/systemd/system/"

# Reload systemd
print_info "Reloading systemd..."
sudo systemctl daemon-reload

print_success "Systemd reloaded"

# Enable service
print_info "Enabling service to start on boot..."
sudo systemctl enable echoes-firmware.service

print_success "Service enabled"

# Start service
print_info "Starting service..."
sudo systemctl start echoes-firmware.service

# Wait a moment for service to start
sleep 2

# Check status
if sudo systemctl is-active --quiet echoes-firmware.service; then
    print_success "Service is running!"
else
    print_error "Service failed to start"
    print_info "Check status with: sudo systemctl status echoes-firmware"
    exit 1
fi

print_header "Installation Complete!"

echo ""
echo "Firmware server is now running as a system service."
echo ""
echo "Useful commands:"
echo "  sudo systemctl status echoes-firmware    # Check status"
echo "  sudo systemctl stop echoes-firmware      # Stop service"
echo "  sudo systemctl start echoes-firmware     # Start service"
echo "  sudo systemctl restart echoes-firmware   # Restart service"
echo "  sudo systemctl disable echoes-firmware   # Disable autostart"
echo "  sudo journalctl -u echoes-firmware -f    # View logs (live)"
echo ""

# Show current status
print_header "Current Status"
sudo systemctl status echoes-firmware --no-pager

echo ""
print_info "Server is running on port 8000"
print_info "Access at: http://$(hostname -I | awk '{print $1}'):8000/firmware/"
echo ""
