/**
 * @file startup.h
 * @brief Startup reporting with MAC address, node type, and light sensor data
 */

#ifndef STARTUP_H
#define STARTUP_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_system.h"
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

/* Separate magic for the boot-reason record — different value to avoid
 * any accidental aliasing with s_rtc_diag on first power-on.           */
#define RTC_BOOT_MAGIC          0xB007CA11u

/* reset cause codes stored in echoes_rtc_diag_t.cause */
#define RTC_DIAG_CAUSE_NONE      0u  /* no prior diagnostic (power-on or first boot)    */
#define RTC_DIAG_CAUSE_RCFG      1u  /* remote_config_task: RCFG_FAILURES_HARD_REBOOT   */
#define RTC_DIAG_CAUSE_REMOTE    2u  /* remote_config_task: server-commanded restart     */
#define RTC_DIAG_CAUSE_ISR_WDT   3u  /* ISR hardware watchdog timeout                   */
#define RTC_DIAG_CAUSE_HTTP_STUCK 4u /* wifi_keepalive: HTTP connect() hung > 30 s      */

typedef struct {
    uint32_t magic;              /* RTC_DIAG_MAGIC when struct is valid               */
    uint32_t cause;              /* RTC_DIAG_CAUSE_* — which reset path fired         */
    uint32_t consecutive_failures; /* remote_config consecutive HTTP failures at reset */
    uint32_t heap_free;          /* esp_get_free_heap_size() at reset (0 from ISR)    */
    int32_t  rssi;               /* AP signal strength at reset (0 if unavailable)    */
    uint32_t uptime_s;           /* seconds since boot at reset                       */
} echoes_rtc_diag_t;

/* Boot-reason record — written on EVERY boot before WiFi starts,
 * and read back on the following boot.
 *
 * Unlike echoes_rtc_diag_t (written only by our own deliberate reset paths),
 * this captures esp_reset_reason() unconditionally, including uncontrolled
 * crashes: ESP_RST_PANIC, ESP_RST_TASK_WDT, ESP_RST_INT_WDT, ESP_RST_BROWNOUT.
 * These all bypass startup_write_rtc_diag(), so s_rtc_diag.magic is never set
 * and PREV_DIAG is never logged — making the crash completely invisible.
 *
 * s_boot_diag survives software reset (RTC_NOINIT) but is re-written on every
 * boot, so after a crash + boot-loop the most recent entry is the loop reset,
 * not the original crash.  However, the FIRST subsequent successful boot (when
 * WiFi reconnects and a startup report reaches the server) will carry whatever
 * reset_reason ESP-IDF reports for that particular reset — and the server log
 * will show "PANIC" or "TASK_WDT" in the Reset field, giving us the crash type.
 *
 * More importantly: even if WiFi never reconnects, the magic + reason survive
 * in RTC until the next physical power-cycle.  On that power-on the magic is
 * invalid so we won't falsely report an old crash, but the reset_reason field
 * from the panic boot is still carried in the startup report for that cycle.
 *
 * Fields are minimal — this struct must be as small as possible to minimise
 * RTC_NOINIT fragmentation alongside s_rtc_diag.                             */
typedef struct {
    uint32_t magic;         /* RTC_BOOT_MAGIC when struct is valid                  */
    uint32_t reason;        /* raw esp_reset_reason_t from the PREVIOUS boot        */
    uint32_t uptime_s;      /* uptime_s at end of previous boot (0 if unknown)      */
} echoes_rtc_boot_t;

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

    /* Previous-boot raw reset reason — populated from echoes_rtc_boot_t.
     * Unlike has_prev_diag (which only fires on our own deliberate resets),
     * this fires on EVERY reset including uncontrolled crashes: PANIC,
     * TASK_WDT, INT_WDT, BROWNOUT.  Empty string when not available
     * (first power-on or RTC region corrupted).                             */
    char     prev_boot_reset_reason[20];  // e.g. "PANIC", "TASK_WDT", "POWERON"
    uint32_t prev_boot_uptime_s;          // uptime of previous boot at exit (0 if unknown)
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

/**
 * @brief Stamp the current boot's reset reason into RTC_NOINIT before it is
 *        overwritten on the next boot.
 *
 * Must be called as early as possible in app_main — before wifi_init, before
 * any task creation — so that even a very early crash on the NEXT boot is
 * preceded by a valid stamp from this boot.
 *
 * Also called immediately before every deliberate reset so the next boot can
 * see how long this boot ran.
 *
 * @param reason    esp_reset_reason() for THIS boot (already read by caller)
 * @param uptime_s  current uptime in seconds (0 when called at boot start)
 */
void startup_record_boot_reason(esp_reset_reason_t reason, uint32_t uptime_s);

#endif /* STARTUP_REPORT_H */
