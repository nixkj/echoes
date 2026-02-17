#!/bin/bash
# Installation script for Echoes Startup Report Server

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Echoes Startup Report Server - Setup${NC}"
echo -e "${GREEN}========================================${NC}"
echo

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Error: Please run as root (use sudo)${NC}"
    exit 1
fi

# Configuration
INSTALL_DIR="/opt/echoes"
LOG_DIR="/var/log/echoes"
SERVICE_NAME="echoes-startup-server"
USER="echoes"
GROUP="echoes"

echo -e "${YELLOW}Installation Configuration:${NC}"
echo "  Install directory: $INSTALL_DIR"
echo "  Log directory: $LOG_DIR"
echo "  Service user: $USER"
echo "  Service name: $SERVICE_NAME"
echo

# Create user and group if they don't exist
echo -e "${YELLOW}Creating service user...${NC}"
if ! id "$USER" &>/dev/null; then
    useradd -r -s /bin/false -d "$INSTALL_DIR" "$USER"
    echo -e "${GREEN}✓ User '$USER' created${NC}"
else
    echo -e "${GREEN}✓ User '$USER' already exists${NC}"
fi

# Create directories
echo -e "${YELLOW}Creating directories...${NC}"
mkdir -p "$INSTALL_DIR"
mkdir -p "$LOG_DIR"
echo -e "${GREEN}✓ Directories created${NC}"

# Copy files
echo -e "${YELLOW}Installing server files...${NC}"
if [ -f "startup_server.py" ]; then
    cp startup_server.py "$INSTALL_DIR/"
    chmod +x "$INSTALL_DIR/startup_server.py"
    echo -e "${GREEN}✓ Server script installed${NC}"
else
    echo -e "${RED}Error: startup_server.py not found in current directory${NC}"
    exit 1
fi

if [ -f "echoes-startup-server.service" ]; then
    cp echoes-startup-server.service /etc/systemd/system/
    echo -e "${GREEN}✓ Systemd service file installed${NC}"
else
    echo -e "${RED}Error: echoes-startup-server.service not found in current directory${NC}"
    exit 1
fi

# Set permissions
echo -e "${YELLOW}Setting permissions...${NC}"
chown -R "$USER:$GROUP" "$INSTALL_DIR"
chown -R "$USER:$GROUP" "$LOG_DIR"
chmod 755 "$INSTALL_DIR"
chmod 755 "$LOG_DIR"
echo -e "${GREEN}✓ Permissions set${NC}"

# Reload systemd
echo -e "${YELLOW}Reloading systemd...${NC}"
systemctl daemon-reload
echo -e "${GREEN}✓ Systemd reloaded${NC}"

# Enable and start service
echo -e "${YELLOW}Enabling and starting service...${NC}"
systemctl enable "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"

# Wait a moment for service to start
sleep 2

# Check service status
if systemctl is-active --quiet "$SERVICE_NAME"; then
    echo -e "${GREEN}✓ Service started successfully${NC}"
else
    echo -e "${RED}✗ Service failed to start${NC}"
    echo -e "${YELLOW}Checking service status...${NC}"
    systemctl status "$SERVICE_NAME" --no-pager
    exit 1
fi

echo
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Installation Complete!${NC}"
echo -e "${GREEN}========================================${NC}"
echo
echo "Service Status:"
systemctl status "$SERVICE_NAME" --no-pager | head -n 10
echo
echo "Useful Commands:"
echo "  View logs:           sudo journalctl -u $SERVICE_NAME -f"
echo "  View startup reports: sudo tail -f $LOG_DIR/startup_reports.log"
echo "  Restart service:      sudo systemctl restart $SERVICE_NAME"
echo "  Stop service:         sudo systemctl stop $SERVICE_NAME"
echo "  Service status:       sudo systemctl status $SERVICE_NAME"
echo
echo "The server is now listening on port 8001"
echo "ESP32 devices should send reports to: http://YOUR_SERVER_IP:8001/startup"
echo
