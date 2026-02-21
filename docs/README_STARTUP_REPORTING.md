# Echoes of the Machine - Startup Reporting

This extension adds startup reporting functionality to the Echoes of the Machine project. Each ESP32 device will:

1. Sleep for a random 0-5 seconds on boot
2. Sample the light sensor during the sleep period
3. Send a concise HTTP POST message with device information
4. Include any startup errors in the report

## Features

- **Device Identification**: Reports MAC address, firmware version, and node type
- **Hardware Config Reporting**: Automatically detects and reports `echoes-full` or `echoes-minimal`
- **Error Tracking**: Captures and reports any startup errors
- **Retry with Exponential Backoff**: Up to 4 attempts; delays of 2 s, 4 s, 8 s between retries
- **Reliable Server**: Python HTTP server with rotating log files
- **Systemd Integration**: Run as a system service with automatic restart

## ESP32 Integration

### Files

- **`main/startup.h`** — Configuration and function declarations
- **`main/startup.c`** — Implementation

### Configuration

Edit `main/startup.h`:

```c
#define STARTUP_REPORT_URL      "http://192.168.101.2:8001/startup"
#define STARTUP_HTTP_TIMEOUT_MS 10000   // 10 s per attempt
#define STARTUP_MAX_RETRIES     4
#define STARTUP_RETRY_BASE_MS   2000
```

Node type is set automatically based on hardware detected at boot: `"echoes-full"` (BH1750 present) or `"echoes-minimal"` (analog sensor only).

### Integration

The startup reporting module is already integrated into `main/main.c`. The CMakeLists.txt should include `startup.c`:

```cmake
idf_component_register(SRCS "main.c"
                             "echoes.c"
                             "synthesis.c"
                             "ota.c"
                             "espnow_mesh.c"
                             "markov.c"
                             "remote_config.c"
                             "startup.c"
                        INCLUDE_DIRS ".")
```

## Python Server Setup

### Quick Installation

1. **Copy server files to your server machine:**
   ```bash
   scp scripts/startup_server/startup_server.py \
       scripts/startup_server/echoes-startup-server.service \
       scripts/startup_server/install_server.sh \
       user@server:~/
   ```

2. **Run the installation script:**
   ```bash
   ssh user@server
   chmod +x install_server.sh
   sudo ./install_server.sh
   ```

   This will:
   - Create a dedicated `echoes` user
   - Install the server to `/opt/echoes/`
   - Set up log directory at `/var/log/echoes/`
   - Install and start the systemd service
   - Configure automatic startup on boot

### Manual Installation

If you prefer manual setup:

```bash
# Create directories
sudo mkdir -p /opt/echoes
sudo mkdir -p /var/log/echoes

# Create user
sudo useradd -r -s /bin/false -d /opt/echoes echoes

# Copy files
sudo cp startup_server.py /opt/echoes/
sudo chmod +x /opt/echoes/startup_server.py

# Set permissions
sudo chown -R echoes:echoes /opt/echoes
sudo chown -R echoes:echoes /var/log/echoes

# Install systemd service
sudo cp echoes-startup-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable echoes-startup-server
sudo systemctl start echoes-startup-server
```

### Verifying Installation

```bash
# Check service status
sudo systemctl status echoes-startup-server

# View real-time logs
sudo journalctl -u echoes-startup-server -f

# View startup reports
sudo tail -f /var/log/echoes/startup_reports.log
```

## Usage

### Server Commands

```bash
# Start server
sudo systemctl start echoes-startup-server

# Stop server
sudo systemctl stop echoes-startup-server

# Restart server
sudo systemctl restart echoes-startup-server

# View status
sudo systemctl status echoes-startup-server

# View logs
sudo journalctl -u echoes-startup-server -f

# View startup reports only
sudo tail -f /var/log/echoes/startup_reports.log
```

### Running Manually (for testing)

```bash
# Run directly with default settings
python3 startup_server.py

# Run on custom port
python3 startup_server.py --port 8080

# Run with custom log directory
python3 startup_server.py --logdir /tmp/echoes-logs

# Run with verbose logging
python3 startup_server.py --verbose
```

## Log Format

### Startup Report Log Entry

```
[2026-02-17 10:23:45] Startup Report | MAC: A4:CF:12:34:56:78 | Type: echoes-full | Firmware: 5.1.3 | Errors: NO
```

With errors:
```
[2026-02-17 10:24:12] Startup Report | MAC: A4:CF:12:34:56:79 | Type: echoes-minimal | Firmware: 5.1.3 | Errors: YES | Error: MAC read failed: ESP_ERR_INVALID_ARG
```

### JSON Payload

ESP32 devices send JSON data:

```json
{
  "mac": "A4:CF:12:34:56:78",
  "firmware": "5.1.3",
  "node_type": "echoes-full",
  "has_errors": false,
  "error_message": ""
}
```

`node_type` is one of `"echoes-full"` (BH1750 detected), `"echoes-minimal"` (analog sensor only), or `"echoes-unknown"` (detection failed).

## Log Rotation

The server uses rotating log files:

- **startup_reports.log**: Main log file (10MB max, keeps 10 rotated files)
- **http_server.log**: HTTP server debug log (5MB max, keeps 3 rotated files)

Old log files are automatically compressed and rotated.

## Troubleshooting

### ESP32 Not Sending Reports

1. Check WiFi credentials in `main/ota.h`
2. Verify server IP address in `main/startup.h`
3. Check ESP32 serial output: `idf.py monitor`
4. Ensure server is running: `sudo systemctl status echoes-startup-server`

### Server Not Receiving Reports

1. Check firewall rules:
   ```bash
   sudo ufw allow 8001/tcp
   ```

2. Verify server is listening:
   ```bash
   sudo netstat -tlnp | grep 8001
   ```

3. Test with curl:
   ```bash
   curl -X POST http://localhost:8001/startup \
     -H "Content-Type: application/json" \
     -d '{"mac":"AA:BB:CC:DD:EE:FF","firmware":"5.1.3","node_type":"echoes-full","has_errors":false,"error_message":""}'
   ```

### Viewing Logs

```bash
# Systemd journal
sudo journalctl -u echoes-startup-server -f

# Startup reports
sudo tail -f /var/log/echoes/startup_reports.log

# HTTP server debug log
sudo tail -f /var/log/echoes/http_server.log

# All logs with filter
sudo tail -f /var/log/echoes/*.log
```

## Security Considerations

- The server runs as a non-privileged user (`echoes`)
- Systemd service includes security hardening options
- Consider using HTTPS for production deployments
- Implement firewall rules to restrict access
- Review log files regularly for suspicious activity

## Performance

- Lightweight server using Python's built-in http.server
- Handles concurrent connections
- Minimal memory footprint (~10-20MB)
- Log rotation prevents disk space issues
- Automatic restart on failure

## Future Enhancements

Potential improvements:
- Database storage for historical analysis
- Web dashboard for viewing device status
- Alerts for devices with repeated errors
- HTTPS support with certificates
- Authentication for POST endpoints
- Grafana integration for visualization

## License

Same as the main Echoes of the Machine project.

## Support

For issues or questions, check:
1. ESP32 serial monitor output
2. Server logs: `sudo journalctl -u echoes-startup-server -f`
3. Startup reports log: `/var/log/echoes/startup_reports.log`
