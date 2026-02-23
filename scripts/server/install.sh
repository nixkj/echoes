#!/usr/bin/env bash
# install.sh — Deploy the consolidated Echoes server (echoes-server)
#
# Replaces three separate services:
#   echoes-firmware       (was port 8000)
#   echoes-startup-server (was port 8001)
#   echoes-config         (was port 8002)
#
# May be run from any directory:  sudo bash /path/to/scripts/server/install.sh
set -e

# Resolve the directory this script lives in so relative file references
# (echoes-server.py, requirements.txt, echoes-server.service) work correctly
# regardless of the working directory the caller used.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

INSTALL_DIR="/opt/echoes"
SERVICE_NAME="echoes-server"
LOG_DIR="/var/log/echoes"
RUN_USER="echoes"
RUN_GROUP="echoes"
OLD_SERVICES="echoes-config echoes-startup-server echoes-firmware"

echo "=== Echoes of the Machine — Consolidated Server Installer ==="
echo "Install dir : $INSTALL_DIR"
echo "Run as user : $RUN_USER (group: $RUN_GROUP)"
echo "Log dir     : $LOG_DIR"
echo ""

if ! getent group "$RUN_GROUP" >/dev/null 2>&1; then
    echo "Creating system group: $RUN_GROUP"
    groupadd --system "$RUN_GROUP"
    echo "✓ Group '$RUN_GROUP' created"
else
    echo "✓ Group '$RUN_GROUP' already exists"
fi

if ! id -u "$RUN_USER" >/dev/null 2>&1; then
    echo "Creating system user: $RUN_USER"
    useradd \
        --system \
        --no-create-home \
        --home-dir "$INSTALL_DIR" \
        --shell /usr/sbin/nologin \
        --gid "$RUN_GROUP" \
        --comment "Echoes of the Machine server" \
        "$RUN_USER"
    echo "✓ User '$RUN_USER' created (primary group: $RUN_GROUP)"
else
    echo "✓ User '$RUN_USER' already exists"
fi

# ── 3. Stop and disable the old three services ───────────────────────────
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

# ── 4. Create directories ────────────────────────────────────────────────
mkdir -p "$INSTALL_DIR"
mkdir -p "$LOG_DIR"
chown "$RUN_USER":"$RUN_GROUP" "$INSTALL_DIR" "$LOG_DIR"

# Firmware directory — build.sh deploy writes here, server reads from here.
# Placed under INSTALL_DIR so it is covered by the service's ReadWritePaths.
# g+ws: group-write lets any member of the 'echoes' group deploy firmware;
#       the setgid bit ensures new files inherit the 'echoes' group automatically.
# To grant a deploy user access:  sudo usermod -aG echoes <username>
# (log out and back in, or run: newgrp echoes)
FIRMWARE_DIR="$INSTALL_DIR/firmware"
mkdir -p "$FIRMWARE_DIR"
chown -R "$RUN_USER":"$RUN_GROUP" "$FIRMWARE_DIR"
chmod g+ws "$FIRMWARE_DIR"
echo "Firmware dir: $FIRMWARE_DIR  (writable by group '$RUN_GROUP')"

# ── 5. Install server script ─────────────────────────────────────────────
cp "$SCRIPT_DIR/echoes-server.py" "$INSTALL_DIR/echoes-server.py"
chown "$RUN_USER":"$RUN_GROUP" "$INSTALL_DIR/echoes-server.py"

# Migrate config.json from old echoes-config install if present
if [ -f "/opt/echoes-config/config.json" ] && [ ! -f "$INSTALL_DIR/config.json" ]; then
    echo "Migrating config.json from old echoes-config install"
    cp /opt/echoes-config/config.json "$INSTALL_DIR/config.json"
    chown "$RUN_USER":"$RUN_GROUP" "$INSTALL_DIR/config.json"
fi

# ── 6. Python venv + dependencies ────────────────────────────────────────
if [ ! -d "$INSTALL_DIR/venv" ]; then
    echo "Creating Python venv…"
    python3 -m venv "$INSTALL_DIR/venv"
fi
"$INSTALL_DIR/venv/bin/pip" install --quiet --upgrade pip
"$INSTALL_DIR/venv/bin/pip" install --quiet -r "$SCRIPT_DIR/requirements.txt"

# ── 7. Systemd service ───────────────────────────────────────────────────
sed -e "s/User=pi/User=$RUN_USER/" \
    -e "s/User=$RUN_USER/User=$RUN_USER\nGroup=$RUN_GROUP/" \
    "$SCRIPT_DIR/echoes-server.service" \
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
echo "  /config                 Device config poll (GET)"
echo "  /startup                Boot report (POST)"
echo "  /firmware/version.txt   OTA version check"
echo "  /firmware/echoes.bin    OTA binary"
