# Echoes of the Machine — Startup Reporting

Each ESP32 node sends a startup report to the server immediately after
establishing a WiFi connection.  The report is used to populate the fleet
dashboard and the `startup_reports.log` file.

> **Note:** Startup reporting is handled by the consolidated `echoes-server`
> process (port 8002), introduced in v6.0.0.  Earlier versions of this document
> described a separate `echoes-startup-server` on port 8001.  That service no
> longer exists; all traffic goes to port 8002.

## What the Report Contains

```json
{
  "mac":           "A4:CF:12:34:56:78",
  "firmware":      "7.4.2",
  "node_type":     "echoes-full",
  "reset_reason":  "POWERON",
  "has_errors":    false,
  "error_message": ""
}
```

`node_type` is set automatically based on hardware detected at boot:

| Value | Meaning |
|---|---|
| `echoes-full` | BH1750 I2C sensor found → Full mode (audio + light) |
| `echoes-minimal` | BH1750 not found → Minimal mode (LED VU meter only) |
| `echoes-unknown` | Hardware detection failed |

`reset_reason` captures the ESP-IDF reset reason string (e.g. `POWERON`,
`SW`, `TASK_WDT`, `PANIC`).  If a previous boot ended in an uncontrolled
crash, the next boot's report carries the crash type via the RTC diagnostic
region even if WiFi never connected during the crashed session.

## Retry Behaviour

The startup report is sent with exponential backoff:

| Attempt | Delay before |
|---|---|
| 1 | none |
| 2 | 2 s |
| 3 | 4 s |
| 4 | 8 s |

`STARTUP_MAX_RETRIES` and `STARTUP_RETRY_BASE_MS` are defined in
`main/startup.h`.  After all retries are exhausted the node continues
operating normally; the report is simply not sent for that boot.

## Server Endpoint

```
POST http://<SERVER_IP>:8002/startup
Content-Type: application/json
```

The server IP and port are configured at build time via `idf.py menuconfig`
under **Server Configuration → Server IP Address / Server Port**.  They are
stored as Kconfig symbols (`CONFIG_SERVER_IP`, `CONFIG_SERVER_PORT`) and must
not be hardcoded in source files.

Successful response:

```json
{"status": "ok", "message": "Startup report received", "timestamp": "2026-02-27 16:58:57"}
```

## Relevant Source Files

| File | Purpose |
|---|---|
| `main/startup.h` | URL macro, retry constants, RTC diagnostic structures |
| `main/startup.c` | `startup_capture_identity()`, `startup_send_report()`, RTC helpers |
| `scripts/server/echoes-server.py` | `POST /startup` handler, node registry update |

## Server Setup

The consolidated server is installed once and handles OTA, startup reports,
and remote configuration on port 8002.  See the main README for full
installation instructions.

```bash
# Install as a systemd service (run on the host/Raspberry Pi)
sudo bash scripts/server/install.sh

# Check service status
sudo systemctl status echoes-server

# View startup reports in real time
sudo tail -f /var/log/echoes/startup_reports.log
```

## Log Format

One line per boot event in `startup_reports.log`:

```
[2026-02-27 16:58:57] MAC: A4:CF:12:34:56:78 | Type: echoes-full | FW: 7.4.2 | Reset: POWERON | Errors: NO
```

With errors:

```
[2026-02-27 16:59:10] MAC: A4:CF:12:34:56:79 | Type: echoes-minimal | FW: 7.4.2 | Reset: TASK_WDT | Errors: YES | WiFi reconnect timer alloc failed
```

## Verifying a Report Was Received

```bash
# Check the log directly
sudo tail -5 /var/log/echoes/startup_reports.log

# Test the endpoint manually
curl -s -X POST http://localhost:8002/startup \
  -H "Content-Type: application/json" \
  -d '{"mac":"AA:BB:CC:DD:EE:FF","firmware":"7.4.2","node_type":"echoes-full",
       "reset_reason":"POWERON","has_errors":false,"error_message":""}'
```

Expected response: `{"status": "ok", ...}`

## Troubleshooting

**Node not sending report:**
- Check WiFi credentials via `idf.py menuconfig` → WiFi Configuration
- Verify server IP in menuconfig → Server Configuration
- Check serial output: `idf.py monitor`

**Server not receiving reports:**
- Confirm service is running: `sudo systemctl status echoes-server`
- Check firewall: `sudo ufw allow 8002/tcp`
- Verify port: `sudo ss -tlnp | grep 8002`
