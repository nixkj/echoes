#!/usr/bin/env bash
# install.sh — Deploy Echoes configuration server
# Run as root or with sudo: sudo bash install.sh
set -e

INSTALL_DIR="/opt/echoes-config"
SERVICE_NAME="echoes-config"
# Change USER to the non-root account that will own the process
RUN_USER="${SUDO_USER:-pi}"

echo "=== Echoes of the Machine — Config Server Installer ==="
echo "Install dir : $INSTALL_DIR"
echo "Run as user : $RUN_USER"
echo ""

# 1. Create install directory
mkdir -p "$INSTALL_DIR"
cp server.py "$INSTALL_DIR/server.py"
chown -R "$RUN_USER":"$RUN_USER" "$INSTALL_DIR"

# 2. Python virtual environment
if [ ! -d "$INSTALL_DIR/venv" ]; then
    echo "Creating Python venv…"
    python3 -m venv "$INSTALL_DIR/venv"
fi
"$INSTALL_DIR/venv/bin/pip" install --quiet --upgrade pip
"$INSTALL_DIR/venv/bin/pip" install --quiet flask

# 3. Fix service file user
sed "s/User=pi/User=$RUN_USER/" echoes-config.service > /etc/systemd/system/${SERVICE_NAME}.service

# 4. Enable and start
systemctl daemon-reload
systemctl enable "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"

echo ""
echo "=== Done ==="
echo "Service status : sudo systemctl status $SERVICE_NAME"
echo "View logs      : sudo journalctl -u $SERVICE_NAME -f"
echo "Web UI         : http://$(hostname -I | awk '{print $1}'):8002"
echo "Config API     : http://$(hostname -I | awk '{print $1}'):8002/config"
