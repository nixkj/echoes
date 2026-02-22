/**
 * @file startup.h
 * @brief Startup reporting with MAC address, node type, and light sensor data
 */

#ifndef STARTUP_H
#define STARTUP_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "echoes.h"

/* ========================================================================
 * CONFIGURATION
 * ======================================================================== */

/* STARTUP_REPORT_URL is built from CONFIG_SERVER_IP and
 * CONFIG_SERVER_PORT, set via 'idf.py menuconfig' under
 * "Server Configuration".  Do not edit this directly.            */
#ifndef STRINGIFY
#  define STRINGIFY_INNER(x) #x
#  define STRINGIFY(x)       STRINGIFY_INNER(x)
#endif
#define STARTUP_REPORT_URL      "http://" CONFIG_SERVER_IP ":" \
                                    STRINGIFY(CONFIG_SERVER_PORT) \
                                    "/startup"
#define STARTUP_HTTP_TIMEOUT_MS     10000   // 10 s per attempt — gives congested AP time to respond

/* Retry settings for startup_send_report().
 * Exponential backoff: delay before attempt N = STARTUP_RETRY_BASE_MS * 2^(N-1)
 *   Attempt 1 → immediate
 *   Attempt 2 → 2 000 ms
 *   Attempt 3 → 4 000 ms
 *   Attempt 4 → 8 000 ms
 */
#define STARTUP_MAX_RETRIES         4
#define STARTUP_RETRY_BASE_MS       2000

/* ========================================================================
 * TYPE DEFINITIONS
 * ======================================================================== */

typedef struct {
    char mac_address[18];           // MAC address as string (XX:XX:XX:XX:XX:XX)
    char node_type[32];             // Node type identifier
    bool has_errors;                // Whether any startup errors occurred
    char error_message[128];        // Error message if any
} startup_report_t;

/* ========================================================================
 * FUNCTION DECLARATIONS
 * ======================================================================== */

/**
 * @brief Capture device identity (MAC address, node type, firmware version).
 *
 * Non-blocking — call immediately on boot, then pass the populated report
 * straight to startup_send_report().
 *
 * @param report     Pointer to startup_report_t structure to fill.
 * @param hw_config  Hardware configuration, used to set node_type string.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t startup_capture_identity(startup_report_t *report, hardware_config_t hw_config);

/**
 * @brief Send startup report to server via HTTP POST
 * 
 * @param report Pointer to startup_report_t structure to send
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t startup_send_report(const startup_report_t *report);

/**
 * @brief Get MAC address as formatted string
 * 
 * @param mac_str Buffer to store MAC address string (min 18 bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t startup_get_mac_address(char *mac_str);

#endif /* STARTUP_REPORT_H */
