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
#define WIFI_MAXIMUM_RETRY  5
#define WIFI_TIMEOUT_MS     20000  // 20 second timeout

/* OTA Configuration
 *
 * OTA_URL and VERSION_URL are built from CONFIG_SERVER_IP and
 * CONFIG_OTA_SERVER_PORT, which are set via 'idf.py menuconfig'
 * under "Server Configuration".  You should not need to edit this
 * file directly — set the IP and port in menuconfig instead.
 */
#define FIRMWARE_VERSION    "6.5.4"

/* STRINGIFY is needed to turn an integer Kconfig value into a string
 * literal so it can be concatenated with other string literals.       */
#ifndef STRINGIFY
#  define STRINGIFY_INNER(x) #x
#  define STRINGIFY(x)       STRINGIFY_INNER(x)
#endif

#define OTA_URL             "http://" CONFIG_SERVER_IP ":" \
                                STRINGIFY(CONFIG_SERVER_PORT) \
                                "/firmware/echoes.bin"
#define VERSION_URL         "http://" CONFIG_SERVER_IP ":" \
                                STRINGIFY(CONFIG_SERVER_PORT) \
                                "/firmware/version.txt"

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
 * @brief Returns true if the WiFi reconnect backoff timer was created
 *        successfully.  False means the node cannot auto-recover from
 *        mid-session WiFi drops — include this in startup diagnostics.
 */
bool wifi_reconnect_timer_ok(void);

/**
 * @brief Check WiFi connection status
 * @return true if connected, false otherwise
 */
bool wifi_is_connected(void);

/**
 * @brief Register task handles that will be suspended during OTA download.
 *
 * Suspending the flock, lux, and audio detection tasks during a firmware
 * download reduces ESP-NOW and I2S radio traffic competing with the TCP
 * stream.  Pass NULL for any handle that does not exist on this node.
 *
 * Call this from app_main after xTaskCreate() returns the handles, but
 * before ota_check_and_update() is invoked.
 *
 * @param flock  Handle of flock_task  (or NULL)
 * @param lux    Handle of lux_based_birds_task (or NULL)
 * @param audio  Handle of audio_detection_task (or NULL)
 */
void ota_register_tasks(TaskHandle_t flock, TaskHandle_t lux, TaskHandle_t audio);

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
 * @param param Task parameter (optional)
 */
void ota_task(void *param);

/**
 * @brief Perform OTA update from specified URL
 * @param url URL of the firmware binary
 * @return true if update successful, false otherwise
 */
bool ota_perform_update(const char *url);

/**
 * @brief Resume the registered application tasks after OTA completes or fails.
 *
 * Called exclusively from app_main, AFTER espnow_mesh_init() and
 * i2s_microphone_init() have been called.  Keeping resume here (instead of
 * inside ota_perform_update) guarantees tasks never run before the hardware
 * they depend on is ready.
 *
 * Safe to call even if ota_register_tasks() was never called (handles are
 * NULL in that case and the function is a no-op).
 */
void ota_resume_tasks(void);

#endif /* OTA_H */
