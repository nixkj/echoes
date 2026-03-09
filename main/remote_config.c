/**
 * @file remote_config.c
 * @brief Remote parameter configuration — ESP-IDF v5.x implementation
 *
 * Fetches a JSON config blob from the Python server via HTTP GET and
 * applies each known key to the live remote_config_t struct.
 *
 * JSON parsing uses cJSON (bundled with ESP-IDF since v4.0).
 */

#include "remote_config.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "esp_system.h"
#include "esp_attr.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "soc/rtc_cntl_reg.h"
#include "startup.h"
#include "ota.h"
#include "echoes.h"
#include <string.h>
#include <stdlib.h>

/* =========================================================================
 * CONNECTIVITY WATCHDOG
 *
 * The HTTP config fetch is the ONLY operation that verifies end-to-end
 * bidirectional connectivity.  Every local check (wifi_is_connected,
 * esp_wifi_sta_get_ap_info, sendto) only verifies driver state — they
 * all return success when the AP has silently dropped the node.
 *
 * Recovery for minimal nodes:
 *   RCFG_FAILURES_HARD_REBOOT consecutive failures → RTC hard reboot.
 *
 * There is intentionally NO intermediate "WiFi disconnect and reconnect"
 * escalation level for minimal nodes.  Calling esp_wifi_disconnect() while
 * the node is receiving ~50 ESP-NOW frames/second (from 25 full nodes
 * broadcasting lux every 500 ms) corrupts the WiFi driver's internal
 * reconnect state machine.  Symptoms: the radio stops ACKing the AP's
 * null-frame keepalive probes, the AP logs "connection lost", and all
 * subsequent reconnect attempts silently fail — even though audio_detection,
 * flock, and keepalive tasks keep running and feeding their watchdogs.
 * The node appears permanently dead without ever triggering a reset.
 *
 * Full nodes call esp_wifi_disconnect() safely because their 500 ms lux
 * broadcasts keep the radio continuously active, preventing the driver
 * from entering the stuck-reconnect state.
 *
 * For minimal nodes: let the WiFi driver manage its own reconnection via
 * the event handler.  The ISR WDT in main.c is the backstop for genuine
 * driver deadlocks.  Only force a hard reboot after sustained failure.
 * ========================================================================= */
#define RCFG_MAX_CONSECUTIVE_FAILURES   3
#define RCFG_FAILURES_HARD_REBOOT       10  /* minimal: hard reboot after ~10 min */

/* =========================================================================
 * RTC SLOW MEMORY
 * RTC_NOINIT_ATTR variables survive esp_restart() (software reset) but are
 * cleared on power-on reset / deep sleep wakeup.  We use this to remember
 * which restart command we have already acted on so we cannot loop even if
 * the server window is still open when we come back up.
 * ========================================================================= */
RTC_NOINIT_ATTR static uint32_t s_last_restart_token;

static const char *TAG = "RCFG";

/* =========================================================================
 * DEFAULT VALUES  (mirrors echoes.h / espnow_mesh.h / markov.h #defines)
 * ========================================================================= */

/* =========================================================================
 * NVS PERSISTENCE
 * =========================================================================
 *
 * The last successfully fetched config is saved to NVS as a raw blob so that
 * a reboot without WiFi (no AP, server down) resumes with tuned values rather
 * than compiled-in defaults.
 *
 * Only the tunable fields are persisted — runtime-only state (last_fetch_ms,
 * server_epoch_s, fetch_tick_ms, restart_requested, restart_token, loaded)
 * is excluded.  A schema version key guards against loading stale blobs after
 * a firmware update that changes the struct layout.
 *
 * NVS namespace: "rcfg"  key: "cfg_blob"  version key: "cfg_ver"
 *
 * On load:
 *   1. Read cfg_ver.  If absent or != RCFG_NVS_VERSION → skip, use defaults.
 *   2. Read cfg_blob.  If size != sizeof(rcfg_nvs_t) → skip, use defaults.
 *   3. Copy fields into s_cfg.  s_cfg.loaded remains false until a live fetch
 *      succeeds, but all tunable values are now from the last known-good state.
 *
 * On save (called after every successful HTTP fetch):
 *   Write cfg_ver then cfg_blob atomically via nvs_commit().              */

#define RCFG_NVS_NAMESPACE  "rcfg"
#define RCFG_NVS_BLOB_KEY   "cfg_blob"
#define RCFG_NVS_VER_KEY    "cfg_ver"
#define RCFG_NVS_VERSION    2   /* increment when rcfg_nvs_t layout changes */

/* Persisted subset of remote_config_t — only tunable fields, no runtime state.
 * Laid out explicitly (no padding holes between float/uint32 fields) to keep
 * the blob stable across compilers.  Add new fields at the END only, then
 * bump RCFG_NVS_VERSION so old blobs are discarded rather than misread.    */
typedef struct __attribute__((packed)) {
    /* Audio detection */
    float    gain;
    uint32_t whistle_freq;
    uint32_t voice_freq;
    float    whistle_multiplier;
    float    voice_multiplier;
    float    clap_multiplier;
    uint32_t whistle_confirm;
    uint32_t voice_confirm;
    uint32_t clap_confirm;
    uint32_t debounce_buffers;
    uint32_t birdsong_freq;
    float    birdsong_multiplier;
    float    birdsong_hf_ratio;
    float    birdsong_mf_min;
    uint32_t birdsong_confirm;
    float    noise_floor_whistle;
    float    noise_floor_voice;
    float    noise_floor_birdsong;
    /* Playback volume */
    float    volume;
    float    volume_lux_min;
    float    volume_lux_max;
    float    volume_scale_min;
    float    volume_scale_max;
    float    quelea_gain;
    /* Light sensor */
    uint32_t lux_poll_interval_ms;
    float    lux_change_threshold;
    float    lux_flash_threshold;
    float    lux_flash_percent;
    float    lux_flash_min_abs;
    /* LED */
    float    vu_max_brightness;
    /* ESP-NOW */
    float    espnow_lux_threshold;
    uint32_t espnow_event_ttl_ms;
    uint32_t espnow_sound_throttle_ms;
    /* Flock mode */
    uint32_t flock_grace_ms;
    uint32_t flock_msg_count;
    uint32_t flock_window_ms;
    uint32_t flock_hold_ms;
    uint32_t flock_call_gap_ms;
    /* Markov chain */
    uint32_t markov_idle_trigger_ms;
    uint32_t markov_autonomous_cooldown_ms;
    /* Demo mode */
    uint8_t  demo_mode;
    uint32_t demo_interval_ms;
    /* Quiet hours */
    uint8_t  quiet_hours_enabled;
    uint8_t  quiet_hour_start;
    uint8_t  quiet_hour_end;
} rcfg_nvs_t;

static void rcfg_nvs_save(const remote_config_t *src)
{
    nvs_handle_t h;
    if (nvs_open(RCFG_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS save: could not open namespace");
        return;
    }

    rcfg_nvs_t blob = {
        .gain                          = src->gain,
        .whistle_freq                  = src->whistle_freq,
        .voice_freq                    = src->voice_freq,
        .whistle_multiplier            = src->whistle_multiplier,
        .voice_multiplier              = src->voice_multiplier,
        .clap_multiplier               = src->clap_multiplier,
        .whistle_confirm               = src->whistle_confirm,
        .voice_confirm                 = src->voice_confirm,
        .clap_confirm                  = src->clap_confirm,
        .debounce_buffers              = src->debounce_buffers,
        .birdsong_freq                 = src->birdsong_freq,
        .birdsong_multiplier           = src->birdsong_multiplier,
        .birdsong_hf_ratio             = src->birdsong_hf_ratio,
        .birdsong_mf_min               = src->birdsong_mf_min,
        .birdsong_confirm              = src->birdsong_confirm,
        .noise_floor_whistle           = src->noise_floor_whistle,
        .noise_floor_voice             = src->noise_floor_voice,
        .noise_floor_birdsong          = src->noise_floor_birdsong,
        .volume                        = src->volume,
        .volume_lux_min                = src->volume_lux_min,
        .volume_lux_max                = src->volume_lux_max,
        .volume_scale_min              = src->volume_scale_min,
        .volume_scale_max              = src->volume_scale_max,
        .quelea_gain                   = src->quelea_gain,
        .lux_poll_interval_ms          = src->lux_poll_interval_ms,
        .lux_change_threshold          = src->lux_change_threshold,
        .lux_flash_threshold           = src->lux_flash_threshold,
        .lux_flash_percent             = src->lux_flash_percent,
        .lux_flash_min_abs             = src->lux_flash_min_abs,
        .vu_max_brightness             = src->vu_max_brightness,
        .espnow_lux_threshold          = src->espnow_lux_threshold,
        .espnow_event_ttl_ms           = src->espnow_event_ttl_ms,
        .espnow_sound_throttle_ms      = src->espnow_sound_throttle_ms,
        .flock_grace_ms                = src->flock_grace_ms,
        .flock_msg_count               = src->flock_msg_count,
        .flock_window_ms               = src->flock_window_ms,
        .flock_hold_ms                 = src->flock_hold_ms,
        .flock_call_gap_ms             = src->flock_call_gap_ms,
        .markov_idle_trigger_ms        = src->markov_idle_trigger_ms,
        .markov_autonomous_cooldown_ms = src->markov_autonomous_cooldown_ms,
        .demo_mode                     = src->demo_mode ? 1u : 0u,
        .demo_interval_ms              = src->demo_interval_ms,
        .quiet_hours_enabled           = src->quiet_hours_enabled ? 1u : 0u,
        .quiet_hour_start              = src->quiet_hour_start,
        .quiet_hour_end                = src->quiet_hour_end,
    };

    uint8_t ver = RCFG_NVS_VERSION;
    nvs_set_u8(h, RCFG_NVS_VER_KEY, ver);
    nvs_set_blob(h, RCFG_NVS_BLOB_KEY, &blob, sizeof(blob));
    esp_err_t err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Config saved to NVS (%u bytes)", (unsigned)sizeof(blob));
    } else {
        ESP_LOGW(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    }
}

static bool rcfg_nvs_load(remote_config_t *dst)
{
    nvs_handle_t h;
    if (nvs_open(RCFG_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;   /* namespace never written — first boot */
    }

    uint8_t ver = 0;
    if (nvs_get_u8(h, RCFG_NVS_VER_KEY, &ver) != ESP_OK || ver != RCFG_NVS_VERSION) {
        nvs_close(h);
        ESP_LOGI(TAG, "NVS config version mismatch (stored=%u want=%u) — using defaults",
                 ver, RCFG_NVS_VERSION);
        return false;
    }

    rcfg_nvs_t blob;
    size_t sz = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, RCFG_NVS_BLOB_KEY, &blob, &sz);
    nvs_close(h);

    if (err != ESP_OK || sz != sizeof(blob)) {
        ESP_LOGW(TAG, "NVS config blob invalid (%s, size=%u) — using defaults",
                 esp_err_to_name(err), (unsigned)sz);
        return false;
    }

    /* Copy tunable fields into dst, leaving runtime-only fields untouched */
    dst->gain                          = blob.gain;
    dst->whistle_freq                  = blob.whistle_freq;
    dst->voice_freq                    = blob.voice_freq;
    dst->whistle_multiplier            = blob.whistle_multiplier;
    dst->voice_multiplier              = blob.voice_multiplier;
    dst->clap_multiplier               = blob.clap_multiplier;
    dst->whistle_confirm               = blob.whistle_confirm;
    dst->voice_confirm                 = blob.voice_confirm;
    dst->clap_confirm                  = blob.clap_confirm;
    dst->debounce_buffers              = blob.debounce_buffers;
    dst->birdsong_freq                 = blob.birdsong_freq;
    dst->birdsong_multiplier           = blob.birdsong_multiplier;
    dst->birdsong_hf_ratio             = blob.birdsong_hf_ratio;
    dst->birdsong_mf_min               = blob.birdsong_mf_min;
    dst->birdsong_confirm              = blob.birdsong_confirm;
    dst->noise_floor_whistle           = blob.noise_floor_whistle;
    dst->noise_floor_voice             = blob.noise_floor_voice;
    dst->noise_floor_birdsong          = blob.noise_floor_birdsong;
    dst->volume                        = blob.volume;
    dst->volume_lux_min                = blob.volume_lux_min;
    dst->volume_lux_max                = blob.volume_lux_max;
    dst->volume_scale_min              = blob.volume_scale_min;
    dst->volume_scale_max              = blob.volume_scale_max;
    dst->quelea_gain                   = blob.quelea_gain;
    dst->lux_poll_interval_ms          = blob.lux_poll_interval_ms;
    dst->lux_change_threshold          = blob.lux_change_threshold;
    dst->lux_flash_threshold           = blob.lux_flash_threshold;
    dst->lux_flash_percent             = blob.lux_flash_percent;
    dst->lux_flash_min_abs             = blob.lux_flash_min_abs;
    dst->vu_max_brightness             = blob.vu_max_brightness;
    dst->espnow_lux_threshold          = blob.espnow_lux_threshold;
    dst->espnow_event_ttl_ms           = blob.espnow_event_ttl_ms;
    dst->espnow_sound_throttle_ms      = blob.espnow_sound_throttle_ms;
    dst->flock_grace_ms                = blob.flock_grace_ms;
    dst->flock_msg_count               = blob.flock_msg_count;
    dst->flock_window_ms               = blob.flock_window_ms;
    dst->flock_hold_ms                 = blob.flock_hold_ms;
    dst->flock_call_gap_ms             = blob.flock_call_gap_ms;
    dst->markov_idle_trigger_ms        = blob.markov_idle_trigger_ms;
    dst->markov_autonomous_cooldown_ms = blob.markov_autonomous_cooldown_ms;
    dst->demo_mode                     = (blob.demo_mode != 0);
    dst->demo_interval_ms              = blob.demo_interval_ms;
    dst->quiet_hours_enabled           = (blob.quiet_hours_enabled != 0);
    dst->quiet_hour_start              = blob.quiet_hour_start;
    dst->quiet_hour_end                = blob.quiet_hour_end;

    ESP_LOGI(TAG, "Config loaded from NVS (last known-good values)");
    return true;
}

static const remote_config_t CONFIG_DEFAULTS = {
    /* Audio detection */
    .gain                        = 16.0f,
    .whistle_freq                = 2000,
    .voice_freq                  = 200,
    .whistle_multiplier          = 2.5f,
    .voice_multiplier            = 2.5f,
    .clap_multiplier             = 4.0f,
    .whistle_confirm             = 2,
    .voice_confirm               = 3,
    .clap_confirm                = 1,
    .debounce_buffers            = 20,
    .birdsong_freq               = 3500,
    .birdsong_multiplier         = 2.2f,
    .birdsong_hf_ratio           = 1.4f,
    .birdsong_mf_min             = 0.35f,
    .birdsong_confirm            = 3,
    .noise_floor_whistle         = 10000.0f,
    .noise_floor_voice           = 4000.0f,
    .noise_floor_birdsong        = 10000.0f,

    /* Playback volume */
    .volume                      = 0.20f,
    .volume_lux_min              = 2.0f,
    .volume_lux_max              = 200.0f,
    .volume_scale_min            = 0.25f,
    .volume_scale_max            = 1.0f,
    .quelea_gain                 = 1.5f,

    /* Light sensor */
    .lux_poll_interval_ms        = 500,
    .lux_change_threshold        = 1.0f,
    .lux_flash_threshold         = 30.0f,
    .lux_flash_percent           = 0.15f,
    .lux_flash_min_abs           = 15.0f,

    /* LED behaviour */
    .vu_max_brightness           = 0.75f,

    /* ESP-NOW mesh */
    .espnow_lux_threshold        = 12.0f,
    .espnow_event_ttl_ms         = 30000,
    .espnow_sound_throttle_ms    = 3000,

    /* Flock mode */
    .flock_grace_ms              = 12000,
    .flock_msg_count             = 12,
    .flock_window_ms             = 6000,
    .flock_hold_ms               = 10000,
    .flock_call_gap_ms           = 200,

    /* Markov chain */
    .markov_idle_trigger_ms          = 45000,
    .markov_autonomous_cooldown_ms   = 15000,

    /* Documentary / demo mode */
    .demo_mode                   = false,
    .demo_interval_ms            = 15000,

    /* Remote restart */
    .restart_requested           = false,
    .restart_token               = 0,

    /* Quiet hours (17:00 → 08:00).
     * Enabled by default, but remote_config_is_quiet_hours() will not
     * activate silence until server_epoch_s is populated by a successful
     * config fetch (server_epoch_s == 0 at boot → function returns false). */
    .quiet_hours_enabled         = true,
    .quiet_hour_start            = 17,
    .quiet_hour_end              = 8,

    .loaded        = false,
    .last_fetch_ms = 0,
    .server_epoch_s = 0,
    .fetch_tick_ms  = 0,
};

/* =========================================================================
 * MODULE STATE
 * ========================================================================= */

static remote_config_t s_cfg;

/* Set to esp_timer_get_time()/1000 (ms) just before
 * esp_http_client_perform() and cleared to 0 on return.
 * Monitored by wifi_keepalive_task to detect a hung connect(). */
volatile uint64_t g_rcfg_http_attempt_start_ms = 0;

/* Mutex that protects s_cfg against concurrent read/write between the
 * remote_config_task (writer) and any application task calling
 * remote_config_get() (readers).  On a dual-core ESP32 multi-byte struct
 * reads are NOT guaranteed to be atomic, so without this a reader could
 * observe a partially-updated config.  The mutex is created in
 * remote_config_init() before any task accesses the struct.             */
static SemaphoreHandle_t s_cfg_mutex = NULL;

/* Accumulation buffer for HTTP response body.
 *
 * This buffer is module-level (not stack or heap per call) for two reasons:
 * it is too large for the task stack (~4 KB), and it avoids a malloc/free
 * on every 60-second poll cycle.
 *
 * SINGLE-CALLER ASSUMPTION: remote_config_fetch() must only ever be called
 * from one task at a time.  If a second concurrent caller were added (e.g.
 * a forced-fetch from another task), s_http_body and s_http_body_len would
 * be corrupted.  The current design has exactly one caller: remote_config_task
 * (plus the one blocking call in main before the task starts), so this is safe.
 * If that ever changes, either move the buffer onto the caller's stack or
 * protect it with a mutex separate from s_cfg_mutex.                       */
static char   s_http_body[REMOTE_CONFIG_MAX_BODY_SIZE];
static size_t s_http_body_len = 0;

/* =========================================================================
 * HTTP EVENT HANDLER
 * ========================================================================= */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        /* Accumulate body regardless of transfer encoding.  ESP-IDF's HTTP
         * client reassembles chunked responses before delivering them here,
         * so the old chunked guard was incorrectly discarding valid data
         * whenever the server (e.g. Flask) used chunked Transfer-Encoding. */
        {
            size_t remaining = sizeof(s_http_body) - s_http_body_len - 1;
            if ((size_t)evt->data_len > remaining) {
                ESP_LOGW(TAG, "HTTP body truncated — response exceeds %u bytes; "
                              "increase REMOTE_CONFIG_MAX_BODY_SIZE",
                         (unsigned)sizeof(s_http_body));
            }
            size_t copy_len = ((size_t)evt->data_len < remaining)
                              ? (size_t)evt->data_len : remaining;
            memcpy(s_http_body + s_http_body_len, evt->data, copy_len);
            s_http_body_len += copy_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        s_http_body[s_http_body_len] = '\0';
        break;
    case HTTP_EVENT_DISCONNECTED:
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* =========================================================================
 * JSON → STRUCT MAPPING
 *
 * Helper macros reduce repetition.  Each field attempts to read from the
 * JSON object; missing keys are silently ignored (server-side defaults apply).
 * ========================================================================= */

#define CFG_FLOAT(json, field, key) \
    do { \
        cJSON *_item = cJSON_GetObjectItemCaseSensitive(json, key); \
        if (_item && cJSON_IsNumber(_item)) { \
            (field) = (float)_item->valuedouble; \
        } \
    } while(0)

#define CFG_UINT32(json, field, key) \
    do { \
        cJSON *_item = cJSON_GetObjectItemCaseSensitive(json, key); \
        if (_item && cJSON_IsNumber(_item)) { \
            (field) = (uint32_t)_item->valueint; \
        } \
    } while(0)

#define CFG_UINT8(json, field, key) \
    do { \
        cJSON *_item = cJSON_GetObjectItemCaseSensitive(json, key); \
        if (_item && cJSON_IsNumber(_item)) { \
            (field) = (uint8_t)_item->valueint; \
        } \
    } while(0)

#define CFG_BOOL(json, field, key) \
    do { \
        cJSON *_item = cJSON_GetObjectItemCaseSensitive(json, key); \
        if (_item && cJSON_IsBool(_item)) { \
            (field) = (bool)cJSON_IsTrue(_item); \
        } \
    } while(0)

/* apply_json_to() parses the JSON object into *dst.  It operates on a
 * caller-supplied struct so the result can be built in a temporary before
 * being swapped into s_cfg under the mutex (see remote_config_fetch).    */
static void apply_json_to(cJSON *root, remote_config_t *dst)
{
    /* Audio detection */
    CFG_FLOAT  (root, dst->gain,               "GAIN");
    CFG_UINT32 (root, dst->whistle_freq,        "WHISTLE_FREQ");
    CFG_UINT32 (root, dst->voice_freq,          "VOICE_FREQ");
    CFG_FLOAT  (root, dst->whistle_multiplier,  "WHISTLE_MULTIPLIER");
    CFG_FLOAT  (root, dst->voice_multiplier,    "VOICE_MULTIPLIER");
    CFG_FLOAT  (root, dst->clap_multiplier,     "CLAP_MULTIPLIER");
    CFG_UINT8  (root, dst->whistle_confirm,     "WHISTLE_CONFIRM");
    CFG_UINT8  (root, dst->voice_confirm,       "VOICE_CONFIRM");
    CFG_UINT8  (root, dst->clap_confirm,        "CLAP_CONFIRM");
    CFG_UINT8  (root, dst->debounce_buffers,    "DEBOUNCE_BUFFERS");
    CFG_UINT32 (root, dst->birdsong_freq,       "BIRDSONG_FREQ");
    CFG_FLOAT  (root, dst->birdsong_multiplier, "BIRDSONG_MULTIPLIER");
    CFG_FLOAT  (root, dst->birdsong_hf_ratio,   "BIRDSONG_HF_RATIO");
    CFG_FLOAT  (root, dst->birdsong_mf_min,     "BIRDSONG_MF_MIN");
    CFG_UINT8  (root, dst->birdsong_confirm,    "BIRDSONG_CONFIRM");
    CFG_FLOAT  (root, dst->noise_floor_whistle,  "NOISE_FLOOR_WHISTLE");
    CFG_FLOAT  (root, dst->noise_floor_voice,    "NOISE_FLOOR_VOICE");
    CFG_FLOAT  (root, dst->noise_floor_birdsong, "NOISE_FLOOR_BIRDSONG");

    /* Playback volume */
    CFG_FLOAT  (root, dst->volume,              "VOLUME");
    CFG_FLOAT  (root, dst->volume_lux_min,      "VOLUME_LUX_MIN");
    CFG_FLOAT  (root, dst->volume_lux_max,      "VOLUME_LUX_MAX");
    CFG_FLOAT  (root, dst->volume_scale_min,    "VOLUME_SCALE_MIN");
    CFG_FLOAT  (root, dst->volume_scale_max,    "VOLUME_SCALE_MAX");
    CFG_FLOAT  (root, dst->quelea_gain,         "QUELEA_GAIN");

    /* Light sensor */
    CFG_UINT32 (root, dst->lux_poll_interval_ms,  "LUX_POLL_INTERVAL_MS");
    CFG_FLOAT  (root, dst->lux_change_threshold,  "LUX_CHANGE_THRESHOLD");
    CFG_FLOAT  (root, dst->lux_flash_threshold,   "LUX_FLASH_THRESHOLD");
    CFG_FLOAT  (root, dst->lux_flash_percent,     "LUX_FLASH_PERCENT");
    CFG_FLOAT  (root, dst->lux_flash_min_abs,     "LUX_FLASH_MIN_ABS");

    /* LED behaviour */
    CFG_FLOAT  (root, dst->vu_max_brightness,         "VU_MAX_BRIGHTNESS");

    /* ESP-NOW mesh */
    CFG_FLOAT  (root, dst->espnow_lux_threshold,        "ESPNOW_LUX_THRESHOLD");
    CFG_UINT32 (root, dst->espnow_event_ttl_ms,         "ESPNOW_EVENT_TTL_MS");
    CFG_UINT32 (root, dst->espnow_sound_throttle_ms,    "ESPNOW_SOUND_THROTTLE_MS");

    /* Flock mode */
    CFG_UINT32 (root, dst->flock_grace_ms,     "FLOCK_GRACE_MS");
    CFG_UINT32 (root, dst->flock_msg_count,    "FLOCK_MSG_COUNT");
    CFG_UINT32 (root, dst->flock_window_ms,    "FLOCK_WINDOW_MS");
    CFG_UINT32 (root, dst->flock_hold_ms,      "FLOCK_HOLD_MS");
    CFG_UINT32 (root, dst->flock_call_gap_ms,  "FLOCK_CALL_GAP_MS");

    /* Markov chain */
    CFG_UINT32 (root, dst->markov_idle_trigger_ms,         "MARKOV_IDLE_TRIGGER_MS");
    CFG_UINT32 (root, dst->markov_autonomous_cooldown_ms,  "MARKOV_AUTONOMOUS_COOLDOWN_MS");

    /* Output switches */
    CFG_BOOL   (root, dst->silent_mode,  "SILENT_MODE");
    CFG_BOOL   (root, dst->sound_off,    "SOUND_OFF");

    /* Documentary / demo mode */
    CFG_BOOL   (root, dst->demo_mode,       "DEMO_MODE");
    CFG_UINT32 (root, dst->demo_interval_ms, "DEMO_INTERVAL_MS");

    /* Remote restart */
    CFG_BOOL   (root, dst->restart_requested, "RESTART_REQUESTED");
    CFG_UINT32 (root, dst->restart_token,     "RESTART_TOKEN");

    /* Quiet hours */
    CFG_BOOL  (root, dst->quiet_hours_enabled, "QUIET_HOURS_ENABLED");
    CFG_UINT8 (root, dst->quiet_hour_start,    "QUIET_HOUR_START");
    CFG_UINT8 (root, dst->quiet_hour_end,      "QUIET_HOUR_END");

    /* Server wall-clock time — used for quiet-hours calculation */
    {
        cJSON *_item = cJSON_GetObjectItemCaseSensitive(root, "_server_time");
        if (_item && cJSON_IsNumber(_item)) {
            dst->server_epoch_s = (int64_t)_item->valuedouble;
            dst->fetch_tick_ms  = (uint64_t)(esp_timer_get_time() / 1000ULL);
        }
    }
}

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

void remote_config_init(void)
{
    s_cfg = CONFIG_DEFAULTS;

    /* Create the config mutex once.  configASSERT aborts if heap is exhausted
     * (which would indicate a far more serious problem at boot time).        */
    if (s_cfg_mutex == NULL) {
        s_cfg_mutex = xSemaphoreCreateMutex();
        configASSERT(s_cfg_mutex != NULL);
    }

    /* Try to restore the last known-good config from NVS.  If no valid blob
     * is present (first boot, version mismatch after OTA) the struct stays
     * at CONFIG_DEFAULTS.  Either way s_cfg.loaded remains false — the server
     * is still the authoritative source; NVS is only the warm-start fallback. */
    if (rcfg_nvs_load(&s_cfg)) {
        ESP_LOGI(TAG, "Remote config initialised from NVS (last known-good)");
    } else {
        ESP_LOGI(TAG, "Remote config initialised with compiled-in defaults");
    }
}

esp_err_t remote_config_fetch(int failures)
{
    /* Reset body accumulator */
    s_http_body_len  = 0;
    s_http_body[0]   = '\0';

    esp_http_client_config_t http_cfg = {
        .url            = REMOTE_CONFIG_URL,
        .method         = HTTP_METHOD_GET,
        .event_handler  = http_event_handler,
        .timeout_ms     = REMOTE_CONFIG_HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialise HTTP client");
        return ESP_FAIL;
    }

    /* Send MAC address so the server can update node liveness on every poll */
    char mac_str[18] = {0};
    if (startup_get_mac_address(mac_str) == ESP_OK) {
        esp_http_client_set_header(client, "X-Device-MAC", mac_str);
    }

    /* Diagnostic telemetry headers — logged by the server on every poll.
     * Gives 60-second-resolution visibility into node health right up to
     * the moment a node stops polling, without requiring serial access.   */
    {
        char buf[32];

        snprintf(buf, sizeof(buf), "%lu",
                 (unsigned long)esp_get_free_heap_size());
        esp_http_client_set_header(client, "X-Heap-Free", buf);

        snprintf(buf, sizeof(buf), "%lu",
                 (unsigned long)(esp_timer_get_time() / 1000000ULL));
        esp_http_client_set_header(client, "X-Uptime-S", buf);

        snprintf(buf, sizeof(buf), "%d", failures);
        esp_http_client_set_header(client, "X-Poll-Failures", buf);

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(buf, sizeof(buf), "%d", ap.rssi);
        } else {
            snprintf(buf, sizeof(buf), "0");
        }
        esp_http_client_set_header(client, "X-RSSI", buf);
    }

    /* Mark the attempt start so wifi_keepalive_task can detect a hung
     * connect().  Cleared unconditionally before this function returns. */
    g_rcfg_http_attempt_start_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    esp_err_t err = esp_http_client_perform(client);

    /* Clear immediately — the hung-connect window is over either way. */
    g_rcfg_http_attempt_start_ms = 0;

    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP fetch failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status %d — skipping config update", status);
        return ESP_FAIL;
    }
    if (s_http_body_len == 0) {
        ESP_LOGW(TAG, "Empty response body");
        return ESP_FAIL;
    }

    /* Parse JSON into a temporary struct so the mutex hold time is minimal:
     * the HTTP round-trip and JSON parse happen outside the lock, then we
     * take it only for the fast memcpy that makes the new config visible.  */
    remote_config_t tmp = s_cfg;   /* start from current values — unset keys keep their values */

    cJSON *root = cJSON_Parse(s_http_body);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error near: %.40s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
        return ESP_FAIL;
    }

    /* apply_json writes into the module-level s_cfg — redirect it to tmp
     * by temporarily pointing the helper macros at tmp via a local alias.
     * We achieve this by parsing directly into tmp using a local copy of
     * apply_json's logic, then swapping atomically under the mutex.       */
    apply_json_to(root, &tmp);
    cJSON_Delete(root);

    tmp.loaded        = true;
    tmp.last_fetch_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    /* Hold the mutex only for the struct swap — a fast memcpy.            */
    if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_cfg = tmp;
        xSemaphoreGive(s_cfg_mutex);
    } else {
        ESP_LOGE(TAG, "remote_config_fetch: could not acquire mutex — update dropped");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config applied — vol=%.2f whistle=%luHz voice=%luHz poll=%lums",
             tmp.volume,
             (unsigned long)tmp.whistle_freq,
             (unsigned long)tmp.voice_freq,
             (unsigned long)tmp.lux_poll_interval_ms);

    if (tmp.quiet_hours_enabled) {
        ESP_LOGI(TAG, "Quiet hours: %02u:00 → %02u:00 (server epoch %lld)",
                 tmp.quiet_hour_start, tmp.quiet_hour_end,
                 (long long)tmp.server_epoch_s);
    }

    /* Persist to NVS so a future no-WiFi reboot resumes with these values
     * rather than compiled-in defaults.  Done outside the mutex — tmp is a
     * local copy and NVS writes can block for several milliseconds.         */
    rcfg_nvs_save(&tmp);

    return ESP_OK;
}

void remote_config_task(void *param)
{
    (void)param;
    int consecutive_failures = 0;

    vTaskDelay(pdMS_TO_TICKS(esp_random() % 45000));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(REMOTE_CONFIG_POLL_INTERVAL_MS));
        ESP_LOGI(TAG, "Polling config server…");
        esp_err_t err = remote_config_fetch(consecutive_failures);

        if (err != ESP_OK) {
            consecutive_failures++;
            ESP_LOGW(TAG, "Config poll failed (%s) — keeping previous values "
                          "[consecutive failures: %d]",
                     esp_err_to_name(err),
                     consecutive_failures);

            if (get_hardware_config() == HW_CONFIG_MINIMAL) {
                /* ── Minimal node escalation ─────────────────────────────
                 *
                 * HTTP is the ONLY proof of end-to-end connectivity.
                 * wifi_is_connected(), esp_wifi_sta_get_ap_info(), and
                 * sendto() all return success when the AP has silently
                 * dropped the node.
                 *
                 * There is NO intermediate WiFi disconnect/reconnect step.
                 * Calling esp_wifi_disconnect() under heavy ESP-NOW RX load
                 * (25 full nodes broadcasting every 500 ms) corrupts the
                 * driver's reconnect state machine — the radio stops ACKing
                 * null-frame probes, the AP logs "connection lost", and all
                 * reconnect attempts silently fail while all watchdog-feeding
                 * tasks continue running normally (no reset is ever triggered).
                 *
                 * Recovery: let the WiFi driver manage its own reconnection
                 * via the WIFI_EVENT_STA_DISCONNECTED handler in ota.c.
                 * After RCFG_FAILURES_HARD_REBOOT sustained failures (~10 min
                 * at 60 s poll interval) force a hard RTC reset as last resort.
                 * The ISR WDT in main.c is the backstop if THIS task stalls. */

                if (consecutive_failures >= RCFG_FAILURES_HARD_REBOOT) {
                    ESP_LOGE(TAG, "Watchdog: %d config failures — "
                                  "forcing hard reboot", consecutive_failures);
                    vTaskDelay(pdMS_TO_TICKS(esp_random() % 10000));
                    {
                        wifi_ap_record_t ap;
                        int32_t rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                                       ? ap.rssi : 0;
                        startup_write_rtc_diag(
                            RTC_DIAG_CAUSE_RCFG,
                            (uint32_t)consecutive_failures,
                            esp_get_free_heap_size(),
                            rssi,
                            (uint32_t)(esp_timer_get_time() / 1000000ULL));
                    }
                    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
                    while (1) { }   /* does not return — RTC reset fires */
                }

            } else {
                /* ── Full node: simple disconnect ────────────────────────
                 * Full nodes have no keepalive task to conflict with.     */
                if (consecutive_failures >= RCFG_MAX_CONSECUTIVE_FAILURES) {
                    consecutive_failures = 0;
                    if (!wifi_is_connected()) {
                        ESP_LOGI(TAG, "Watchdog: WiFi already disconnected — "
                                      "reconnect loop running");
                    } else {
                        uint32_t jitter_ms = esp_random() % 30000;
                        ESP_LOGW(TAG, "Watchdog: forcing WiFi reconnect "
                                      "(jitter %lu ms)",
                                 (unsigned long)jitter_ms);
                        vTaskDelay(pdMS_TO_TICKS(jitter_ms));
                        esp_wifi_disconnect();
                    }
                }
            }

        } else {
            if (consecutive_failures > 0) {
                ESP_LOGI(TAG, "Config poll recovered after %d failure(s)",
                         consecutive_failures);
            }
            consecutive_failures = 0;
        }

        if (err == ESP_OK && s_cfg.restart_requested) {
            /* Reading s_cfg directly here is safe: remote_config_task is the
             * sole writer (via remote_config_fetch above), so there is no
             * concurrent write to race against when we read restart_requested
             * and restart_token in this same task immediately after fetch.  */

            /*
             * Server has requested a remote restart.
             *
             * s_last_restart_token lives in RTC slow memory and survives
             * esp_restart() (software reset) but is cleared on power-on.
             * If the token matches the one we last acted on, we already
             * rebooted for this command -- ignore it so we cannot loop
             * even if the server window is still open when we come back up.
             */
            if (s_cfg.restart_token != 0 &&
                s_cfg.restart_token == s_last_restart_token) {
                ESP_LOGI(TAG, "Restart token %lu already acted on -- ignoring",
                         (unsigned long)s_cfg.restart_token);
            } else {
                /* New restart command -- store token and reboot */
                s_last_restart_token = s_cfg.restart_token;
                ESP_LOGW(TAG, "Remote restart (token %lu) -- rebooting in ~2 s",
                         (unsigned long)s_cfg.restart_token);
                extern void set_led(float white, float blue);
                for (int i = 0; i < 3; i++) {
                    set_led(1.0f, 1.0f);
                    vTaskDelay(pdMS_TO_TICKS(250));
                    set_led(0.0f, 0.0f);
                    vTaskDelay(pdMS_TO_TICKS(250));
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                {
                    wifi_ap_record_t ap;
                    int32_t rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                                   ? ap.rssi : 0;
                    startup_write_rtc_diag(
                        RTC_DIAG_CAUSE_REMOTE,
                        (uint32_t)consecutive_failures,
                        esp_get_free_heap_size(),
                        rssi,
                        (uint32_t)(esp_timer_get_time() / 1000000ULL));
                }
		// Better than esp_restart()
                SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
		while (1) { }   /* never reached */
            }
        }
    }
}

const remote_config_t *remote_config_get(void)
{
    /* Returns a direct pointer — safe for reading a SINGLE field because
     * remote_config_fetch() updates s_cfg via a mutex-protected memcpy that
     * is effectively atomic per aligned field on Xtensa.
     *
     * For multiple fields that must be mutually consistent, callers should
     * use remote_config_snapshot() instead.                                */
    return &s_cfg;
}

bool remote_config_snapshot(remote_config_t *out)
{
    if (!out) return false;
    if (s_cfg_mutex == NULL) {
        /* Pre-init fallback — copy defaults without locking */
        *out = s_cfg;
        return true;
    }
    if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_cfg;
        xSemaphoreGive(s_cfg_mutex);
        return true;
    }
    return false;
}

/* =========================================================================
 * QUIET HOURS
 *
 * We have no hardware RTC, so we derive the current UTC hour from:
 *   epoch_now ≈ server_epoch_s + (current_tick_ms - fetch_tick_ms) / 1000
 *
 * The server already sends its local time in _server_time (Unix epoch,
 * local time of the server).  For an indoor installation the server is
 * co-located and in the same timezone, so this works without any TZ
 * conversion.  If the server is in a different timezone adjust
 * QUIET_HOUR_START / QUIET_HOUR_END to compensate.
 *
 * The wrap-around case (start > end, e.g. 17→08) is handled explicitly:
 *   17:00 → 23:59  and  00:00 → 07:59  are both quiet.
 * ========================================================================= */

bool remote_config_is_quiet_hours(void)
{
    /* Snapshot the fields we need under the mutex so they are mutually
     * consistent (quiet_hours_enabled, quiet_hour_start/end, server_epoch_s,
     * and fetch_tick_ms all come from the same config fetch cycle).        */
    bool     enabled;
    int64_t  epoch_s;
    uint64_t tick_ms;
    uint8_t  qs, qe;

    /* Safety guard: mutex is created in remote_config_init().  If this
     * function is called before init (e.g. during early boot), treat as
     * not-quiet so nothing is silenced unexpectedly.                    */
    if (s_cfg_mutex == NULL) return false;

    if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        enabled  = s_cfg.quiet_hours_enabled;
        epoch_s  = s_cfg.server_epoch_s;
        tick_ms  = s_cfg.fetch_tick_ms;
        qs       = s_cfg.quiet_hour_start;
        qe       = s_cfg.quiet_hour_end;
        xSemaphoreGive(s_cfg_mutex);
    } else {
        /* Mutex not available — default to not-quiet so audio is never
         * silenced unexpectedly due to a scheduling glitch.              */
        return false;
    }

    if (!enabled)      return false;
    if (epoch_s == 0)  return false;   /* no time reference yet */

    /* Estimate elapsed seconds since last config fetch */
    uint64_t now_ms   = (uint64_t)(esp_timer_get_time() / 1000ULL);
    uint64_t delta_ms = now_ms - tick_ms;   /* uint64_t, no wrap concern */
    int64_t  epoch_now = epoch_s + (int64_t)(delta_ms / 1000);

    /* Extract hour from epoch (UTC, or local if server sends local time) */
    uint8_t hour = (uint8_t)((epoch_now % 86400) / 3600);

    bool quiet;
    if (qs < qe) {
        /* Simple contiguous window, e.g. 01:00 → 06:00 */
        quiet = (hour >= qs && hour < qe);
    } else {
        /* Overnight wrap-around, e.g. 17:00 → 08:00 */
        quiet = (hour >= qs || hour < qe);
    }

    return quiet;
}
