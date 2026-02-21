# Echoes Startup Reporting - Quick Reference

## ESP32 Configuration

**File: `main/startup.h`**
```c
#define STARTUP_REPORT_URL      "http://192.168.101.2:8001/startup"  // ← Change IP if needed
#define STARTUP_HTTP_TIMEOUT_MS 10000   // 10 s per attempt
#define STARTUP_MAX_RETRIES     4
#define STARTUP_RETRY_BASE_MS   2000
```

Node type is set automatically from hardware config (`"echoes-full"` or `"echoes-minimal"`).

**File: `main/ota.h`** (WiFi settings)
```c
#define WIFI_SSID           "Echoes"
#define WIFI_PASSWORD       "REMOVED_SECRET"
```

## Server Installation (One Command)

```bash
cd scripts/startup_server
sudo ./install_server.sh
```

## Common Commands

### Server Management
```bash
sudo systemctl start echoes-startup-server    # Start
sudo systemctl stop echoes-startup-server     # Stop
sudo systemctl restart echoes-startup-server  # Restart
sudo systemctl status echoes-startup-server   # Status
```

### View Logs
```bash
sudo tail -f /var/log/echoes/startup_reports.log  # Startup reports
sudo journalctl -u echoes-startup-server -f       # All logs
```

### Test Server
```bash
# Send 1 test report
python3 scripts/startup_server/test_server.py

# Send 10 reports with 2s interval
python3 scripts/startup_server/test_server.py --count 10 --interval 2

# Send reports with 20% error rate
python3 scripts/startup_server/test_server.py --count 5 --error-rate 0.2

# Test remote server
python3 scripts/startup_server/test_server.py --url http://192.168.101.2:8001/startup
```

## Firewall Setup

```bash
# Ubuntu/Debian with UFW
sudo ufw allow 8001/tcp

# CentOS/RHEL with firewalld
sudo firewall-cmd --permanent --add-port=8001/tcp
sudo firewall-cmd --reload
```

## Typical Log Entry

**Success:**
```
[2026-02-17 10:23:45] Startup Report | MAC: A4:CF:12:34:56:78 | Type: echoes-full | Firmware: 5.1.3 | Errors: NO
```

**With Error:**
```
[2026-02-17 10:24:12] Startup Report | MAC: A4:CF:12:34:56:79 | Type: echoes-minimal | Firmware: 5.1.3 | Errors: YES | Error: MAC read failed: ESP_ERR_INVALID_ARG
```

## Troubleshooting Quick Checks

1. **ESP32 not connecting:**
   - Check serial output: `idf.py monitor`
   - Verify WiFi credentials in `main/ota.h`
   - Verify server URL in `main/startup.h`

2. **Server not receiving:**
   - Check server status: `sudo systemctl status echoes-startup-server`
   - Check firewall: `sudo ufw status`
   - Test locally: `python3 scripts/startup_server/test_server.py`

3. **Check network:**
   - Server listening: `sudo netstat -tlnp | grep 8001`
   - Ping ESP32: `ping 192.168.101.42`

## Files Location

- **Server**: `/opt/echoes/startup_server.py`
- **Logs**: `/var/log/echoes/startup_reports.log`
- **Service**: `/etc/systemd/system/echoes-startup-server.service`

## Port Configuration

Default: **8001**

To change:
1. Edit `/etc/systemd/system/echoes-startup-server.service`
2. Change `--port 8001` to your desired port
3. Run: `sudo systemctl daemon-reload`
4. Run: `sudo systemctl restart echoes-startup-server`
5. Update firewall rules
6. Update `STARTUP_REPORT_URL` in `main/startup.h`
