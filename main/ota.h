/**
 * @file ota.h
 * @brief OTA Firmware Update for Echoes of the Machine
 * 
 * Handles WiFi connection and firmware updates from a remote server
 */

#ifndef OTA_H
#define OTA_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ========================================================================
 * CONFIGURATION
 * ======================================================================== */

/* WiFi Configuration - Update these for your network */
#define WIFI_SSID           "Echoes"
#define WIFI_PASSWORD       "REMOVED_SECRET"
#define WIFI_MAXIMUM_RETRY  5
#define WIFI_TIMEOUT_MS     20000  // 20 second timeout

/* OTA Configuration */
#define FIRMWARE_VERSION    "5.1.0"
#define OTA_URL             "http://192.168.101.2:8000/firmware/echoes.bin"
#define VERSION_URL         "http://192.168.101.2:8000/firmware/version.txt"

/* Update check settings */
#define OTA_CHECK_INTERVAL_MS    (24 * 60 * 60 * 1000)  // Check once per day
#define OTA_BUFFER_SIZE          4096

/* OTA retry settings
 * ota_check_and_update() will attempt the full version-check + download cycle
 * up to OTA_MAX_ATTEMPTS times.  Each retry waits OTA_RETRY_BASE_DELAY_MS *
 * attempt number (linear backoff: 15 s, 30 s, 45 s...) so that a fleet of
 * devices that all fail together do not all retry in lockstep.
 */
#define OTA_MAX_ATTEMPTS            3
#define OTA_RETRY_BASE_DELAY_MS     15000   // 15 s x attempt index

/* ========================================================================
 * TYPE DEFINITIONS
 * ======================================================================== */

typedef enum {
    OTA_STATUS_IDLE = 0,
    OTA_STATUS_CHECKING,
    OTA_STATUS_DOWNLOADING,
    OTA_STATUS_UPDATING,
    OTA_STATUS_SUCCESS,
    OTA_STATUS_FAILED,
    OTA_STATUS_NO_UPDATE
} ota_status_t;

typedef struct {
    bool wifi_connected;
    ota_status_t ota_status;
    char current_version[32];
    char available_version[32];
    uint32_t download_progress_percent;
} ota_state_t;

/* ========================================================================
 * FUNCTION DECLARATIONS
 * ======================================================================== */

/**
 * @brief Initialize WiFi and connect to network
 * @return true if connection successful, false otherwise
 */
bool wifi_init_and_connect(void);

/**
 * @brief Check WiFi connection status
 * @return true if connected, false otherwise
 */
bool wifi_is_connected(void);

/**
 * @brief Disconnect from WiFi
 */
void wifi_disconnect(void);

/**
 * @brief Register task handles that will be suspended during OTA download.
 *
 * Suspending the chaos, lux, and audio detection tasks during a firmware
 * download reduces ESP-NOW and I2S radio traffic competing with the TCP
 * stream.  Pass NULL for any handle that does not exist on this node.
 *
 * Call this from app_main after xTaskCreate() returns the handles, but
 * before ota_check_and_update() is invoked.
 *
 * @param chaos  Handle of chaos_task  (or NULL)
 * @param lux    Handle of lux_based_birds_task (or NULL)
 * @param audio  Handle of audio_detection_task (or NULL)
 */
void ota_register_tasks(TaskHandle_t chaos, TaskHandle_t lux, TaskHandle_t audio);

/**
 * @brief Check for firmware update and install if available
 * @return true if update was performed, false otherwise
 */
bool ota_check_and_update(void);

/**
 * @brief Get current OTA state
 * @return Pointer to OTA state structure
 */
const ota_state_t* ota_get_state(void);

/**
 * @brief OTA task for periodic update checking
 * @param param Task parameter (unused)
 */
void ota_task(void *param);

/**
 * @brief Perform OTA update from specified URL
 * @param url URL of the firmware binary
 * @return true if update successful, false otherwise
 */
bool ota_perform_update(const char *url);

#endif /* SA_BIRD_OTA_H */
