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
 * SERVER SETTINGS — change to match your deployment
 * ========================================================================= */

/** URL of the config endpoint on the Python server. */
#define REMOTE_CONFIG_URL           "http://192.168.101.2:8002/config"

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
    float    lux_led_min;           /**< Lux at LED floor brightness          */
    float    lux_led_max;           /**< Lux at LED ceiling brightness        */
    float    lux_led_brightness_floor; /**< Minimum LED brightness            */
    float    lux_led_brightness_ceil;  /**< Maximum LED brightness            */
    float    vu_max_brightness;     /**< Peak LED brightness during VU meter  */

    /* ── ESP-NOW mesh ────────────────────────────────────────────── */
    float    espnow_lux_threshold;      /**< Min lux change to broadcast      */
    uint32_t espnow_event_ttl_ms;       /**< Remote event TTL (ms)            */
    uint32_t espnow_sound_throttle_ms;  /**< Min gap between sound broadcasts */
    uint8_t  espnow_flood_count;        /**< Messages to trigger flood state  */
    uint32_t espnow_flood_window_ms;    /**< Flood detection window (ms)      */

    /* ── Markov chain ────────────────────────────────────────────── */
    uint32_t markov_idle_trigger_ms;        /**< Silence before autonomous call */
    uint32_t markov_autonomous_cooldown_ms; /**< Min gap between auto calls     */

    /* ── Meta ────────────────────────────────────────────────────── */
    bool     loaded;            /**< true after at least one successful fetch */
    uint32_t last_fetch_ms;     /**< Tick count of last successful fetch      */
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
 * The pointer is stable for the lifetime of the application.  Fields may
 * be updated atomically by the polling task; read values once into locals
 * if you need a consistent snapshot across multiple uses.
 *
 * @return Const pointer to the live remote_config_t.
 */
const remote_config_t *remote_config_get(void);

#endif /* REMOTE_CONFIG_H */
