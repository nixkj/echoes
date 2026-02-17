# Echoes Startup Reporting - Quick Reference

## ESP32 Configuration

**File: startup_report.h**
```c
#define STARTUP_REPORT_URL      "http://192.168.101.2:8000/startup"  // ← Change this
#define NODE_TYPE               "echoes-v1"
#define STARTUP_SLEEP_MIN_MS    0
#define STARTUP_SLEEP_MAX_MS    5000
```

**File: ota.h** (WiFi settings)
```c
#define WIFI_SSID           "Echoes"      // ← Change this
#define WIFI_PASSWORD       "REMOVED_SECRET"    // ← Change this
```

## Server Installation (One Command)

```bash
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
python3 test_server.py

# Send 10 reports with 2s interval
python3 test_server.py --count 10 --interval 2

# Send reports with 20% error rate
python3 test_server.py --count 5 --error-rate 0.2

# Test remote server
python3 test_server.py --url http://192.168.101.2:8000/startup
```

## Firewall Setup

```bash
# Ubuntu/Debian with UFW
sudo ufw allow 8000/tcp

# CentOS/RHEL with firewalld
sudo firewall-cmd --permanent --add-port=8000/tcp
sudo firewall-cmd --reload
```

## Typical Log Entry

**Success:**
```
[2026-02-17 10:23:45] Startup Report | MAC: A4:CF:12:34:56:78 | Type: echoes-v1 | IP: 192.168.101.42 | Light: 124.50 lux (50 samples) | Sleep: 3247ms | Errors: NO
```

**With Error:**
```
[2026-02-17 10:24:12] Startup Report | MAC: A4:CF:12:34:56:79 | Type: echoes-v1 | IP: 192.168.101.43 | Light: -1.00 lux (0 samples) | Sleep: 1523ms | Errors: YES | Error: No valid light sensor readings
```

## Troubleshooting Quick Checks

1. **ESP32 not connecting:**
   - Check serial output: `idf.py monitor`
   - Verify WiFi credentials in `ota.h`
   - Verify server URL in `startup_report.h`

2. **Server not receiving:**
   - Check server status: `sudo systemctl status echoes-startup-server`
   - Check firewall: `sudo ufw status`
   - Test locally: `python3 test_server.py`

3. **Check network:**
   - Server listening: `sudo netstat -tlnp | grep 8000`
   - Ping ESP32: `ping 192.168.101.42`

## Files Location

- **Server**: `/opt/echoes/startup_server.py`
- **Logs**: `/var/log/echoes/startup_reports.log`
- **Service**: `/etc/systemd/system/echoes-startup-server.service`

## Port Configuration

Default: 8000

To change:
1. Edit `/etc/systemd/system/echoes-startup-server.service`
2. Change `--port 8000` to your desired port
3. Run: `sudo systemctl daemon-reload`
4. Run: `sudo systemctl restart echoes-startup-server`
5. Update firewall rules
6. Update ESP32 `STARTUP_REPORT_URL`
