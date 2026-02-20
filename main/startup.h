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

#define STARTUP_REPORT_URL      "http://192.168.101.2:8001/startup"
#define STARTUP_SLEEP_MIN_MS    0
#define STARTUP_SLEEP_MAX_MS    30000   // 30 s — staggers 50 devices across the boot window
#define STARTUP_HTTP_TIMEOUT_MS 5000  // 3 second timeout (was 5 seconds)

/* ========================================================================
 * TYPE DEFINITIONS
 * ======================================================================== */

typedef struct {
    char mac_address[18];           // MAC address as string (XX:XX:XX:XX:XX:XX)
    char node_type[32];             // Node type identifier
    float avg_light_level;          // Average light sensor reading during sleep
    uint32_t light_samples;         // Number of light samples taken
    bool has_errors;                // Whether any startup errors occurred
    char error_message[128];        // Error message if any
    uint32_t sleep_duration_ms;     // Actual sleep duration
} startup_report_t;

/* ========================================================================
 * FUNCTION DECLARATIONS
 * ======================================================================== */

/**
 * @brief Capture identity fields immediately on boot (non-blocking).
 *
 * Fills mac_address, node_type, and firmware version into the report struct
 * so startup_send_report() can be called right away — before the jitter sleep.
 * avg_light_level, light_samples, and sleep_duration_ms are zeroed and will
 * be populated by startup_jitter_and_sample().
 *
 * @param report     Pointer to startup_report_t structure to fill
 * @param hw_config  Hardware configuration, used to set node_type string
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t startup_capture_identity(startup_report_t *report, hardware_config_t hw_config);

/**
 * @brief Perform the random jitter sleep while sampling the light sensor.
 *
 * Call this AFTER startup_send_report() so the initial report reaches the
 * server immediately.  On return, report->avg_light_level,
 * report->light_samples, and report->sleep_duration_ms are populated.
 *
 * @param report  Pointer to the same startup_report_t passed to
 *                startup_capture_identity().
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t startup_jitter_and_sample(startup_report_t *report);

/**
 * @brief Perform startup sequence with random sleep and light sampling
 *
 * @deprecated  Prefer the two-step startup_capture_identity() +
 *              startup_jitter_and_sample() so the initial report is sent
 *              before the jitter delay.  This wrapper calls both in sequence
 *              for backwards compatibility.
 *
 * @param report     Pointer to startup_report_t structure to fill
 * @param hw_config  Hardware configuration, used to set node_type string
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t startup_sleep_and_sample(startup_report_t *report, hardware_config_t hw_config);

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
