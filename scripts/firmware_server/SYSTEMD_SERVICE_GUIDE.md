# Systemd Service Setup Guide
## Running Firmware Server as a System Service

This guide shows you how to run the firmware update server as a Linux system service that starts automatically on boot.

---

## Quick Install

### Important: Firmware Directory Location

The firmware server uses `$HOME/firmware_server/firmware/` as the firmware directory. This means:
- Firmware files are stored in `~/firmware_server/firmware/`
- This matches what the `build.sh deploy` command expects
- The directory is created automatically on first run

### Automatic Installation (Recommended)

```bash
cd ~/2026/echoes/20260216/echoes
chmod +x install-service.sh
./install-service.sh
```

This script will:
- Create the systemd service file
- Install it to `/etc/systemd/system/`
- Enable the service to start on boot
- Start the service immediately

### Manual Installation

If you prefer to install manually:

1. **Copy files to a permanent location** (optional but recommended):
   ```bash
   sudo mkdir -p /opt/echoes-firmware
   sudo cp firmware_server.py /opt/echoes-firmware/
   sudo mkdir -p /opt/echoes-firmware/firmware
   sudo chown -R $USER:$USER /opt/echoes-firmware
   ```

2. **Create service file**:
   ```bash
   sudo nano /etc/systemd/system/echoes-firmware.service
   ```

3. **Add this content** (update paths if needed):
   ```ini
   [Unit]
   Description=Echoes Firmware Update Server
   After=network.target

   [Service]
   Type=simple
   User=nixon
   WorkingDirectory=/Users/nixon/2026/echoes/20260216/echoes
   ExecStart=/usr/bin/python3 /Users/nixon/2026/echoes/20260216/echoes/firmware_server.py
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
   ```

4. **Enable and start**:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable echoes-firmware.service
   sudo systemctl start echoes-firmware.service
   ```

---

## Service Management

### Basic Commands

```bash
# Check if service is running
sudo systemctl status echoes-firmware

# Start the service
sudo systemctl start echoes-firmware

# Stop the service
sudo systemctl stop echoes-firmware

# Restart the service
sudo systemctl restart echoes-firmware

# Enable service (start on boot)
sudo systemctl enable echoes-firmware

# Disable service (don't start on boot)
sudo systemctl disable echoes-firmware
```

### Viewing Logs

```bash
# View recent logs
sudo journalctl -u echoes-firmware

# View logs in real-time (live tail)
sudo journalctl -u echoes-firmware -f

# View logs since last boot
sudo journalctl -u echoes-firmware -b

# View last 50 lines
sudo journalctl -u echoes-firmware -n 50

# View logs from specific date
sudo journalctl -u echoes-firmware --since "2026-02-16"
sudo journalctl -u echoes-firmware --since "1 hour ago"
```

---

## Verification

### Check Service Status

```bash
sudo systemctl status echoes-firmware
```

**Expected output:**
```
● echoes-firmware.service - Echoes Firmware Update Server
     Loaded: loaded (/etc/systemd/system/echoes-firmware.service; enabled)
     Active: active (running) since Sun 2026-02-16 10:00:00 SAST; 1min ago
   Main PID: 12345 (python3)
      Tasks: 1 (limit: 4915)
     Memory: 15.2M
        CPU: 123ms
     CGroup: /system.slice/echoes-firmware.service
             └─12345 /usr/bin/python3 /path/to/firmware_server.py

Feb 16 10:00:00 hostname systemd[1]: Started Echoes Firmware Update Server.
Feb 16 10:00:00 hostname echoes-firmware[12345]: Server running on port 8000
```

### Check If Server Is Accessible

```bash
# From local machine
curl http://localhost:8000/firmware/version.txt

# From network
curl http://YOUR_IP:8000/firmware/version.txt
```

### Check If Auto-Start Is Enabled

```bash
sudo systemctl is-enabled echoes-firmware
```

Should return: `enabled`

---

## Troubleshooting

### Service Won't Start

**Check status:**
```bash
sudo systemctl status echoes-firmware
```

**Check logs:**
```bash
sudo journalctl -u echoes-firmware -n 50
```

**Common issues:**

1. **Python not found**
   ```
   Error: /usr/bin/python3: No such file or directory
   ```
   Fix: Update `ExecStart` in service file with correct Python path:
   ```bash
   which python3  # Find Python location
   ```

2. **Permission denied**
   ```
   Error: Permission denied: 'firmware_server.py'
   ```
   Fix: Check file permissions:
   ```bash
   chmod +x firmware_server.py
   ```

3. **Port already in use**
   ```
   Error: Address already in use
   ```
   Fix: Check what's using port 8000:
   ```bash
   sudo lsof -i :8000
   sudo netstat -tulpn | grep 8000
   ```

4. **Working directory doesn't exist**
   ```
   Error: No such file or directory
   ```
   Fix: Update `WorkingDirectory` in service file to correct path

### Service Keeps Restarting

**Check logs for errors:**
```bash
sudo journalctl -u echoes-firmware -f
```

**Common causes:**
- Script has errors
- Missing dependencies
- Incorrect file paths

**Disable auto-restart temporarily:**
```bash
sudo nano /etc/systemd/system/echoes-firmware.service
```

Comment out `Restart=always`:
```ini
# Restart=always
```

Then reload:
```bash
sudo systemctl daemon-reload
sudo systemctl restart echoes-firmware
```

### Can't Access Server from Network

**Check if service is listening:**
```bash
sudo netstat -tulpn | grep 8000
# or
sudo ss -tulpn | grep 8000
```

**Check firewall:**
```bash
# Ubuntu/Debian
sudo ufw status
sudo ufw allow 8000/tcp

# CentOS/RHEL
sudo firewall-cmd --list-all
sudo firewall-cmd --add-port=8000/tcp --permanent
sudo firewall-cmd --reload
```

---

## Updating the Server

### Update Python Script

1. **Stop the service:**
   ```bash
   sudo systemctl stop echoes-firmware
   ```

2. **Update the script:**
   ```bash
   nano firmware_server.py
   # Make your changes
   ```

3. **Start the service:**
   ```bash
   sudo systemctl start echoes-firmware
   ```

### Update Service Configuration

1. **Edit service file:**
   ```bash
   sudo nano /etc/systemd/system/echoes-firmware.service
   ```

2. **Reload systemd:**
   ```bash
   sudo systemctl daemon-reload
   ```

3. **Restart service:**
   ```bash
   sudo systemctl restart echoes-firmware
   ```

---

## Uninstalling

### Automatic Uninstall

```bash
chmod +x uninstall-service.sh
./uninstall-service.sh
```

### Manual Uninstall

```bash
# Stop service
sudo systemctl stop echoes-firmware

# Disable service
sudo systemctl disable echoes-firmware

# Remove service file
sudo rm /etc/systemd/system/echoes-firmware.service

# Reload systemd
sudo systemctl daemon-reload
```

---

## Advanced Configuration

### Change Port

Edit `firmware_server.py` to change the port:

```python
PORT = 8080  # Change from 8000 to 8080
```

Then restart:
```bash
sudo systemctl restart echoes-firmware
```

### Run on Different IP

By default, the server listens on all interfaces (0.0.0.0). To restrict to specific IP:

Edit `firmware_server.py`:
```python
httpd = socketserver.TCPServer(("192.168.1.100", PORT), Handler)
```

### Add Environment Variables

Edit service file:
```bash
sudo nano /etc/systemd/system/echoes-firmware.service
```

Add under `[Service]`:
```ini
Environment="FIRMWARE_DIR=/custom/path"
Environment="PORT=8080"
```

### Resource Limits

Limit memory and CPU usage:

```ini
[Service]
MemoryLimit=100M
CPUQuota=50%
```

### Security Hardening

Additional security settings:

```ini
[Service]
# Restrict network access
RestrictAddressFamilies=AF_INET AF_INET6

# Read-only root filesystem
ProtectSystem=strict
ReadWritePaths=/path/to/firmware

# Prevent privilege escalation
NoNewPrivileges=true

# Restrict system calls
SystemCallFilter=@system-service
```

---

## Monitoring

### System Resource Usage

```bash
# Memory usage
sudo systemctl status echoes-firmware | grep Memory

# CPU usage
sudo systemctl status echoes-firmware | grep CPU

# Detailed process info
ps aux | grep firmware_server
```

### Log Rotation

Logs are managed by systemd journal. Configure retention:

```bash
sudo nano /etc/systemd/journald.conf
```

Set:
```ini
[Journal]
SystemMaxUse=100M
MaxRetentionSec=1week
```

Restart journald:
```bash
sudo systemctl restart systemd-journald
```

### Alert on Failures

Create a script to notify on failure:

```bash
sudo nano /usr/local/bin/firmware-alert.sh
```

```bash
#!/bin/bash
echo "Firmware server failed!" | mail -s "Service Alert" admin@example.com
```

Make executable:
```bash
sudo chmod +x /usr/local/bin/firmware-alert.sh
```

Add to service:
```ini
[Service]
OnFailure=firmware-alert.service
```

---

## Multiple Instances

To run multiple firmware servers (e.g., for different projects):

1. **Create template service:**
   ```bash
   sudo nano /etc/systemd/system/firmware-server@.service
   ```

2. **Use template syntax:**
   ```ini
   [Unit]
   Description=Firmware Server for %i
   After=network.target

   [Service]
   Type=simple
   User=nixon
   WorkingDirectory=/opt/firmware-servers/%i
   ExecStart=/usr/bin/python3 firmware_server.py
   Restart=always

   [Install]
   WantedBy=multi-user.target
   ```

3. **Start instances:**
   ```bash
   sudo systemctl start firmware-server@echoes
   sudo systemctl start firmware-server@project2
   ```

---

## Quick Reference

```bash
# Status
sudo systemctl status echoes-firmware

# Start/Stop/Restart
sudo systemctl start echoes-firmware
sudo systemctl stop echoes-firmware
sudo systemctl restart echoes-firmware

# Enable/Disable autostart
sudo systemctl enable echoes-firmware
sudo systemctl disable echoes-firmware

# Logs
sudo journalctl -u echoes-firmware -f

# Test connectivity
curl http://localhost:8000/firmware/version.txt

# Check if running
sudo systemctl is-active echoes-firmware

# Check if enabled
sudo systemctl is-enabled echoes-firmware
```

---

## Benefits of Running as Service

✅ **Auto-start on boot** - No need to manually start the server
✅ **Auto-restart on failure** - Service recovers automatically
✅ **Centralized logging** - Logs managed by systemd journal
✅ **Better resource management** - System manages the process
✅ **Security** - Runs with limited privileges
✅ **Easy management** - Standard systemctl commands
✅ **Integration** - Works with system monitoring tools

---

## Notes

- The service runs as your user account (not root)
- Logs are stored in systemd journal
- Server starts automatically after system reboot
- Service restarts automatically if it crashes
- **Firmware directory:** `$HOME/firmware_server/firmware/`
- Working directory is set to your project directory
- Python 3 must be installed at `/usr/bin/python3`
- The `build.sh deploy` command copies files to the correct location

For more information on systemd services:
```bash
man systemd.service
man journalctl
```
