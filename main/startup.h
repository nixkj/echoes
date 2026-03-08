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
 * RTC DIAGNOSTIC STRUCT
 *
 * Written to RTC_NOINIT memory immediately before any forced reset so the
 * next boot can report exactly what state the node was in when it died.
 * RTC_NOINIT survives software reset but is cleared on power-on.
 * ======================================================================== */

#define RTC_DIAG_MAGIC          0xD1A90001u

/* reset cause codes stored in echoes_rtc_diag_t.cause */
#define RTC_DIAG_CAUSE_NONE     0u   /* no prior diagnostic (power-on or first boot) */
#define RTC_DIAG_CAUSE_RCFG     1u   /* remote_config_task: RCFG_FAILURES_HARD_REBOOT */
#define RTC_DIAG_CAUSE_REMOTE   2u   /* remote_config_task: server-commanded restart   */
#define RTC_DIAG_CAUSE_ISR_WDT  3u   /* ISR hardware watchdog timeout                  */

typedef struct {
    uint32_t magic;              /* RTC_DIAG_MAGIC when struct is valid               */
    uint32_t cause;              /* RTC_DIAG_CAUSE_* — which reset path fired         */
    uint32_t consecutive_failures; /* remote_config consecutive HTTP failures at reset */
    uint32_t heap_free;          /* esp_get_free_heap_size() at reset (0 from ISR)    */
    int32_t  rssi;               /* AP signal strength at reset (0 if unavailable)    */
    uint32_t uptime_s;           /* seconds since boot at reset                       */
} echoes_rtc_diag_t;

/* ========================================================================
 * TYPE DEFINITIONS
 * ======================================================================== */

typedef struct {
    char mac_address[18];           // MAC address as string (XX:XX:XX:XX:XX:XX)
    char node_type[32];             // Node type identifier
    char reset_reason[20];          // Reset reason string (e.g. "TASK_WDT", "POWERON")
    bool has_errors;                // Whether any startup errors occurred
    char error_message[128];        // Error message if any

    /* Previous-boot RTC diagnostics — populated by startup_capture_identity()
     * when a valid echoes_rtc_diag_t is found in RTC_NOINIT memory.        */
    bool     has_prev_diag;         // true when fields below are valid
    char     prev_diag_cause[20];   // human-readable cause string
    uint32_t prev_diag_failures;    // consecutive HTTP failures before reset
    uint32_t prev_diag_heap;        // free heap bytes before reset
    int32_t  prev_diag_rssi;        // AP RSSI before reset (dBm)
    uint32_t prev_diag_uptime_s;    // uptime in seconds before reset
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

/**
 * @brief Return a short human-readable label for an esp_reset_reason_t value.
 *
 * @param reason  Value returned by esp_reset_reason()
 * @return        Static string, e.g. "TASK_WDT", "POWERON", "PANIC"
 */
const char *startup_reset_reason_str(int reason);

/**
 * @brief Write diagnostic state to RTC_NOINIT memory before a forced reset.
 *
 * Call this immediately before SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, ...)
 * from any reset path.  The struct survives the software reset and is read
 * by startup_capture_identity() on the next boot.
 *
 * Safe to call from ISR context (IRAM_ATTR, pure memory writes).
 * Pass 0 for heap/rssi/uptime when calling from ISR context where those
 * values cannot be safely obtained.
 *
 * @param cause      RTC_DIAG_CAUSE_* constant identifying the reset path
 * @param failures   consecutive_failures at the time of reset
 * @param heap       esp_get_free_heap_size() (0 if unavailable)
 * @param rssi       AP RSSI in dBm (0 if unavailable)
 * @param uptime_s   seconds since boot (0 if unavailable)
 */
void startup_write_rtc_diag(uint32_t cause, uint32_t failures,
                             uint32_t heap, int32_t rssi, uint32_t uptime_s);

#endif /* STARTUP_REPORT_H */
