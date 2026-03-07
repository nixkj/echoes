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
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "esp_system.h"
#include "esp_attr.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
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
 * Escalation for minimal nodes (which have no other recovery path):
 *   RCFG_FAILURES_WIFI_RESTART consecutive failures →
 *       esp_wifi_stop() + esp_wifi_start()  (reinit radio hardware)
 *   RCFG_FAILURES_HARD_REBOOT consecutive failures →
 *       esp_restart()  (last resort)
 *
 * Full nodes use a simpler disconnect (they have no keepalive task to
 * conflict with) and rarely need this because their frequent lux
 * broadcasts keep the radio active.
 *
 * Jitter is applied before any WiFi operation to prevent all 50 nodes
 * from reassociating simultaneously.
 * ========================================================================= */
#define RCFG_MAX_CONSECUTIVE_FAILURES   3
#define RCFG_FAILURES_WIFI_RESTART      3   /* minimal: stop/start WiFi */
#define RCFG_FAILURES_HARD_REBOOT       5   /* minimal: full reboot */

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
            dst->fetch_tick_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
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

    ESP_LOGI(TAG, "Remote config initialised with defaults");
}

esp_err_t remote_config_fetch(void)
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

    esp_err_t err = esp_http_client_perform(client);
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
    tmp.last_fetch_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

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
        esp_err_t err = remote_config_fetch();

        if (err != ESP_OK) {
            consecutive_failures++;
            ESP_LOGW(TAG, "Config poll failed (%s) — keeping previous values "
                          "[consecutive failures: %d]",
                     esp_err_to_name(err),
                     consecutive_failures);

            if (get_hardware_config() == HW_CONFIG_MINIMAL) {
                /* ── Minimal node escalation ─────────────────────────────
                 *
                 * This HTTP fetch is the ONLY proof of end-to-end network
                 * connectivity.  wifi_is_connected(), get_ap_info(), and
                 * sendto() all return success when the AP has silently
                 * dropped the node.  Only a bidirectional TCP exchange
                 * (HTTP GET) can detect the loss.
                 *
                 * Level 1 (RCFG_FAILURES_WIFI_RESTART): full WiFi radio
                 *   reinit.  Recovers hardware TX stalls where the
                 *   radio can receive but not transmit.
                 *
                 * Level 2 (RCFG_FAILURES_HARD_REBOOT): system reboot.
                 *   Recovers driver deadlocks, DMA corruption, and
                 *   any other state where WiFi restart is insufficient.
                 *
                 * These are the ONLY reliable recovery mechanisms.
                 * The ISR WDT in main.c is the backstop if this task
                 * itself stalls.                                          */

                if (consecutive_failures >= RCFG_FAILURES_HARD_REBOOT) {
                    ESP_LOGE(TAG, "Watchdog: %d config failures — "
                                  "forcing hard reboot", consecutive_failures);
                    vTaskDelay(pdMS_TO_TICKS(esp_random() % 10000));
		    /* Better than esp_restart() */
		    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
                    /* Does not return */

                } else if (consecutive_failures >= RCFG_FAILURES_WIFI_RESTART) {
                    if (!wifi_is_connected()) {
                        ESP_LOGI(TAG, "Watchdog: WiFi already disconnected");
			} else {
			    uint32_t jitter_ms = esp_random() % 15000;
			    ESP_LOGW(TAG, "Watchdog: %d config failures — "
					  "forcing WiFi disconnect (jitter %lu ms)",
				     consecutive_failures, (unsigned long)jitter_ms);
			    vTaskDelay(pdMS_TO_TICKS(jitter_ms));
			    esp_wifi_disconnect();
			    // WIFI_EVENT_STA_DISCONNECTED fires → event handler calls esp_wifi_connect()
			    // No esp_wifi_stop(), no ESP-NOW teardown, no deadlock.
		    }
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
		// Better than esp_restart()
                SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
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
    uint32_t tick_ms;
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
    uint32_t now_ms   = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t delta_ms = now_ms - tick_ms;   /* wraps safely at 32-bit */
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
