/**
 * @file startup_report.h
 * @brief Startup reporting with MAC address, node type, and light sensor data
 */

#ifndef STARTUP_H
#define STARTUP_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ========================================================================
 * CONFIGURATION
 * ======================================================================== */

#define STARTUP_REPORT_URL      "http://192.168.101.2:8001/startup"
#define NODE_TYPE               "echoes-v1"
#define STARTUP_SLEEP_MIN_MS    0
#define STARTUP_SLEEP_MAX_MS    5000

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
 * @brief Perform startup sequence with random sleep and light sampling
 * 
 * This function:
 * - Generates a random sleep duration (0-5 seconds)
 * - Samples light sensor during sleep period
 * - Collects system information (MAC, node type, errors)
 * 
 * @param report Pointer to startup_report_t structure to fill
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t startup_sleep_and_sample(startup_report_t *report);

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
