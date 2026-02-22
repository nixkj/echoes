#!/usr/bin/env bash
# install.sh — Deploy the consolidated Echoes server (echoes-server)
#
# Replaces three separate services:
#   echoes-firmware       (was port 8000)
#   echoes-startup-server (was port 8001)
#   echoes-config         (was port 8002)
#
# Run from the scripts/server directory with:  sudo bash install.sh
set -e

INSTALL_DIR="/opt/echoes"
SERVICE_NAME="echoes-server"
LOG_DIR="/var/log/echoes"
RUN_USER="${SUDO_USER:-pi}"
OLD_SERVICES="echoes-config echoes-startup-server echoes-firmware"

echo "=== Echoes of the Machine — Consolidated Server Installer ==="
echo "Install dir : $INSTALL_DIR"
echo "Run as user : $RUN_USER"
echo "Log dir     : $LOG_DIR"
echo ""

# ── 1. Stop and disable the old three services ───────────────────────────
for svc in $OLD_SERVICES; do
    if systemctl is-active --quiet "$svc" 2>/dev/null; then
        echo "Stopping  $svc"
        systemctl stop "$svc"
    fi
    if systemctl is-enabled --quiet "$svc" 2>/dev/null; then
        echo "Disabling $svc"
        systemctl disable "$svc"
    fi
done

# ── 2. Create directories ────────────────────────────────────────────────
mkdir -p "$INSTALL_DIR"
mkdir -p "$LOG_DIR"
chown "$RUN_USER":"$RUN_USER" "$INSTALL_DIR" "$LOG_DIR"

# Firmware directory — build.sh deploy writes here, server reads from here.
# Path is unchanged from the old firmware_server so no workflow changes needed.
FIRMWARE_DIR=$(eval echo "~$RUN_USER/firmware_server/firmware")
mkdir -p "$FIRMWARE_DIR"
chown -R "$RUN_USER":"$RUN_USER" "$(eval echo "~$RUN_USER/firmware_server")"
echo "Firmware dir: $FIRMWARE_DIR"

# ── 3. Install server script ─────────────────────────────────────────────
cp echoes-server.py "$INSTALL_DIR/echoes-server.py"
chown "$RUN_USER":"$RUN_USER" "$INSTALL_DIR/echoes-server.py"

# Migrate config.json from old echoes-config install if present
if [ -f "/opt/echoes-config/config.json" ] && [ ! -f "$INSTALL_DIR/config.json" ]; then
    echo "Migrating config.json from old echoes-config install"
    cp /opt/echoes-config/config.json "$INSTALL_DIR/config.json"
    chown "$RUN_USER":"$RUN_USER" "$INSTALL_DIR/config.json"
fi

# ── 4. Python venv + dependencies ────────────────────────────────────────
if [ ! -d "$INSTALL_DIR/venv" ]; then
    echo "Creating Python venv…"
    python3 -m venv "$INSTALL_DIR/venv"
fi
"$INSTALL_DIR/venv/bin/pip" install --quiet --upgrade pip
"$INSTALL_DIR/venv/bin/pip" install --quiet -r requirements.txt

# ── 5. Systemd service ───────────────────────────────────────────────────
sed "s/User=pi/User=$RUN_USER/" echoes-server.service \
    > /etc/systemd/system/${SERVICE_NAME}.service
systemctl daemon-reload
systemctl enable  "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"

sleep 2
if systemctl is-active --quiet "$SERVICE_NAME"; then
    echo "✓ Service running"
else
    echo "✗ Service failed to start"
    echo "  Check: sudo journalctl -u $SERVICE_NAME -n 50"
    exit 1
fi

# ── Summary ──────────────────────────────────────────────────────────────
LOCAL_IP=$(hostname -I | awk '{print $1}')
echo ""
echo "=== Done ==="
echo "Status    : sudo systemctl status $SERVICE_NAME"
echo "Logs      : sudo journalctl -u $SERVICE_NAME -f"
echo "File log  : tail -f $LOG_DIR/echoes-server.log"
echo ""
echo "Endpoints on http://${LOCAL_IP}:8002"
echo "  /                       Config web UI"
echo "  /fleet                  Fleet dashboard"
echo "  /config                 Device config poll (GET)"
echo "  /startup                Boot report (POST)"
echo "  /firmware/version.txt   OTA version check"
echo "  /firmware/echoes.bin    OTA binary"
