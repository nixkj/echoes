/**
 * @file remote_config.h
 * @brief Remote parameter configuration for Echoes of the Machine
 *
 * At boot and every REMOTE_CONFIG_POLL_INTERVAL_MS milliseconds, the device
 * fetches a JSON object from CONFIG_SERVER_URL and applies the values to a
 * live config struct.  All audio detection, playback, LED, and ESP-NOW
 * parameters can be adjusted without reflashing firmware.
 *
 * The server returns a flat JSON object, e.g.:
 *   {"VOLUME": 0.20, "WHISTLE_FREQ": 2000, ...}
 *
 * Usage
 * -----
 *   // In app_main, after WiFi is up:
 *   remote_config_init();
 *   remote_config_fetch();              // blocking first fetch
 *   xTaskCreate(remote_config_task, "rcfg", 4096, NULL, 3, NULL);
 *
 *   // Anywhere in the application:
 *   float vol = remote_config_get()->volume;
 */

#ifndef REMOTE_CONFIG_H
#define REMOTE_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* =========================================================================
 * SERVER SETTINGS
 *
 * REMOTE_CONFIG_URL is built from CONFIG_SERVER_IP and
 * CONFIG_CONFIG_SERVER_PORT, which are set via 'idf.py menuconfig'
 * under "Server Configuration".  You should not need to edit this
 * file directly — set the IP and port in menuconfig instead.
 * ========================================================================= */

/* STRINGIFY helper — also defined in ota.h; guard prevents double definition */
#ifndef STRINGIFY
#  define STRINGIFY_INNER(x) #x
#  define STRINGIFY(x)       STRINGIFY_INNER(x)
#endif

/** URL of the config endpoint on the Python server. */
#define REMOTE_CONFIG_URL   "http://" CONFIG_SERVER_IP ":" \
                                STRINGIFY(CONFIG_CONFIG_SERVER_PORT) \
                                "/config"

/** How often to poll for updates (ms). */
#define REMOTE_CONFIG_POLL_INTERVAL_MS   60000   /* 60 seconds */

/** HTTP timeout for config fetch (ms). */
#define REMOTE_CONFIG_HTTP_TIMEOUT_MS    8000

/** Maximum size of the JSON response body (bytes). */
#define REMOTE_CONFIG_MAX_BODY_SIZE      4096

/* =========================================================================
 * LIVE CONFIGURATION STRUCT
 * All fields are initialised to the same defaults as the #defines in
 * echoes.h / espnow_mesh.h / markov.h so the system works correctly even
 * if the server is unreachable.
 * ========================================================================= */

typedef struct {
    /* ── Audio detection ─────────────────────────────────────────── */
    float    gain;                  /**< Microphone pre-amp gain              */
    uint32_t whistle_freq;          /**< Goertzel whistle centre freq (Hz)    */
    uint32_t voice_freq;            /**< Goertzel voice centre freq (Hz)      */
    float    whistle_multiplier;    /**< Adaptive threshold × for whistle     */
    float    voice_multiplier;      /**< Adaptive threshold × for voice       */
    float    clap_multiplier;       /**< Adaptive threshold × for clap        */
    uint8_t  whistle_confirm;       /**< Confirmation frames — whistle        */
    uint8_t  voice_confirm;         /**< Confirmation frames — voice          */
    uint8_t  clap_confirm;          /**< Confirmation frames — clap           */
    uint8_t  debounce_buffers;      /**< Debounce buffer count                */
    uint32_t birdsong_freq;         /**< Goertzel birdsong centre freq (Hz)   */
    float    birdsong_multiplier;   /**< Adaptive threshold × for birdsong    */
    float    birdsong_hf_ratio;     /**< High-band must exceed mid by this ×  */
    float    birdsong_mf_min;       /**< Mid-freq min fraction of threshold   */
    uint8_t  birdsong_confirm;      /**< Confirmation frames — birdsong       */
    float    noise_floor_whistle;   /**< Absolute minimum threshold — whistle */
    float    noise_floor_voice;     /**< Absolute minimum threshold — voice   */
    float    noise_floor_birdsong;  /**< Absolute minimum threshold — birdsong*/

    /* ── Playback volume ─────────────────────────────────────────── */
    float    volume;                /**< Master playback amplitude            */
    float    volume_lux_min;        /**< Lux level for minimum volume         */
    float    volume_lux_max;        /**< Lux level for maximum volume         */
    float    volume_scale_min;      /**< Volume multiplier in darkness        */
    float    volume_scale_max;      /**< Volume multiplier in bright light    */
    float    quelea_gain;           /**< Post-process gain for Quelea call    */

    /* ── Light sensor ────────────────────────────────────────────── */
    uint32_t lux_poll_interval_ms;  /**< Sensor poll period (ms)              */
    float    lux_change_threshold;  /**< Min lux delta to act on              */
    float    lux_flash_threshold;   /**< Absolute lux jump → flash event      */
    float    lux_flash_percent;     /**< Relative lux change → flash event    */
    float    lux_flash_min_abs;     /**< Min absolute change for % trigger    */

    /* ── LED behaviour ───────────────────────────────────────────── */
    float    vu_max_brightness;     /**< Peak LED brightness during VU meter  */

    /* ── ESP-NOW mesh ────────────────────────────────────────────── */
    float    espnow_lux_threshold;      /**< Min lux change to broadcast      */
    uint32_t espnow_event_ttl_ms;       /**< Remote event TTL (ms)            */
    uint32_t espnow_sound_throttle_ms;  /**< Min gap between sound broadcasts */

    /* ── Flock mode (unified activity threshold) ─────────────────── */
    uint32_t flock_msg_count;   /**< Messages in window to trigger flock mode;
                                     runtime override of FLOCK_MSG_COUNT.
                                     Must be <= FLOCK_RING_MAX (50).         */
    uint32_t flock_window_ms;   /**< Sliding window for flock detection (ms) */
    uint32_t flock_hold_ms;     /**< How long flock mode persists after trigger */
    uint32_t flock_call_gap_ms; /**< Pause between consecutive flock calls (ms) */

    /* ── Markov chain ────────────────────────────────────────────── */
    uint32_t markov_idle_trigger_ms;        /**< Silence before autonomous call */
    uint32_t markov_autonomous_cooldown_ms; /**< Min gap between auto calls     */

    /* ── Output switches ────────────────────────────────────────── */
    bool     silent_mode;       /**< true → disable ALL output (LEDs + sound) */
    bool     sound_off;         /**< true → disable sound only; LEDs still run*/

    /* ── Meta ────────────────────────────────────────────────────── */
    /* ── Remote restart ─────────────────────────────────────────── */
    bool     restart_requested; /**< true → reboot at next config poll        */
    uint32_t restart_token;     /**< Unique token per restart command          */

    /* ── Quiet hours ─────────────────────────────────────────────
     * When enabled the entire flock goes silent (no sound, no LEDs)
     * between QUIET_HOUR_START and QUIET_HOUR_END (wall-clock hours,
     * 0–23, local server time as supplied via _server_time in the
     * config response).  Default: 17:00 → 08:00.                   */
    bool     quiet_hours_enabled; /**< Master switch for scheduled quiet       */
    uint8_t  quiet_hour_start;    /**< Hour at which silence begins (0-23)     */
    uint8_t  quiet_hour_end;      /**< Hour at which sound resumes (0-23)      */

    /* ── Meta ──────────────────────────────────────────────────── */
    bool     loaded;            /**< true after at least one successful fetch */
    uint32_t last_fetch_ms;     /**< Tick count of last successful fetch      */

    /* Wall-clock offset derived from _server_time.
     * Stored as UTC epoch seconds of the most recent config fetch.
     * Combined with esp_timer_get_time() to infer current hour.    */
    int64_t  server_epoch_s;    /**< UTC epoch seconds at last fetch           */
    uint32_t fetch_tick_ms;     /**< xTaskGetTickCount()*period at last fetch  */
} remote_config_t;

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

/**
 * @brief Initialise the remote config module with hardcoded defaults.
 *
 * Must be called before remote_config_fetch() or remote_config_task().
 * Safe to call even without WiFi — defaults are applied immediately.
 */
void remote_config_init(void);

/**
 * @brief Fetch config from server right now (blocking).
 *
 * Returns ESP_OK on success (HTTP 200 with valid JSON).
 * On failure the existing config (defaults or last successful fetch) is kept.
 *
 * @return ESP_OK, ESP_ERR_TIMEOUT, ESP_FAIL, etc.
 */
esp_err_t remote_config_fetch(void);

/**
 * @brief FreeRTOS task that polls the server every REMOTE_CONFIG_POLL_INTERVAL_MS.
 *
 * Start with xTaskCreate(remote_config_task, "rcfg", 4096, NULL, 3, NULL).
 */
void remote_config_task(void *param);

/**
 * @brief Get a read-only pointer to the current live configuration.
 *
 * Returns a pointer to the module-level struct.  Suitable for reading a
 * single field (e.g. `remote_config_get()->volume`) because the fetch
 * task updates the struct via a mutex-protected memcpy that is effectively
 * atomic on Xtensa for any individual aligned field.
 *
 * **If you need several fields to be mutually consistent** (e.g. multiple
 * thresholds used together in a calculation), use remote_config_snapshot()
 * instead — a config update could land between two separate dereferences
 * of the pointer returned here.
 *
 * @return Const pointer to the live remote_config_t.
 */
const remote_config_t *remote_config_get(void);

/**
 * @brief Copy the current configuration into a caller-provided buffer
 *        under the protection of the config mutex.
 *
 * All fields in @p out are guaranteed to come from the same config fetch
 * cycle, so they are mutually consistent.  Use this whenever you need
 * multiple fields together (detection thresholds, quiet-hours window, etc.).
 *
 * @param out  Destination struct — filled on success, untouched on failure.
 * @return true on success, false if the mutex could not be acquired.
 */
bool remote_config_snapshot(remote_config_t *out);

/**
 * @brief Return true if the current wall-clock time falls within the
 *        configured quiet hours window.
 *
 * Uses the server epoch captured at the last successful config fetch
 * combined with the local tick counter to estimate the current hour.
 * Returns false (i.e. not quiet) when quiet_hours_enabled is false or
 * when no server time has been received yet.
 */
bool remote_config_is_quiet_hours(void);

#endif /* REMOTE_CONFIG_H */
