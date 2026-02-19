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
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

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
    .quelea_gain                 = 1.2f,

    /* Light sensor */
    .lux_poll_interval_ms        = 500,
    .lux_change_threshold        = 1.0f,
    .lux_flash_threshold         = 30.0f,
    .lux_flash_percent           = 0.15f,
    .lux_flash_min_abs           = 15.0f,

    /* LED behaviour */
    .lux_led_min                 = 0.0f,
    .lux_led_max                 = 80.0f,
    .lux_led_brightness_floor    = 0.04f,
    .lux_led_brightness_ceil     = 1.0f,
    .vu_max_brightness           = 0.75f,

    /* ESP-NOW mesh */
    .espnow_lux_threshold        = 12.0f,
    .espnow_event_ttl_ms         = 30000,
    .espnow_sound_throttle_ms    = 3000,
    .espnow_flood_count          = 5,
    .espnow_flood_window_ms      = 8000,

    /* Markov chain */
    .markov_idle_trigger_ms          = 45000,
    .markov_autonomous_cooldown_ms   = 15000,

    /* Meta */
    .loaded        = false,
    .last_fetch_ms = 0,
};

/* =========================================================================
 * MODULE STATE
 * ========================================================================= */

static remote_config_t s_cfg;

/* Accumulation buffer for HTTP response body */
static char   s_http_body[REMOTE_CONFIG_MAX_BODY_SIZE];
static size_t s_http_body_len = 0;

/* =========================================================================
 * HTTP EVENT HANDLER
 * ========================================================================= */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client)) {
            size_t remaining = sizeof(s_http_body) - s_http_body_len - 1;
            size_t copy_len  = (evt->data_len < remaining) ? evt->data_len : remaining;
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

static void apply_json(cJSON *root)
{
    /* Audio detection */
    CFG_FLOAT  (root, s_cfg.gain,               "GAIN");
    CFG_UINT32 (root, s_cfg.whistle_freq,        "WHISTLE_FREQ");
    CFG_UINT32 (root, s_cfg.voice_freq,          "VOICE_FREQ");
    CFG_FLOAT  (root, s_cfg.whistle_multiplier,  "WHISTLE_MULTIPLIER");
    CFG_FLOAT  (root, s_cfg.voice_multiplier,    "VOICE_MULTIPLIER");
    CFG_FLOAT  (root, s_cfg.clap_multiplier,     "CLAP_MULTIPLIER");
    CFG_UINT8  (root, s_cfg.whistle_confirm,     "WHISTLE_CONFIRM");
    CFG_UINT8  (root, s_cfg.voice_confirm,       "VOICE_CONFIRM");
    CFG_UINT8  (root, s_cfg.clap_confirm,        "CLAP_CONFIRM");
    CFG_UINT8  (root, s_cfg.debounce_buffers,    "DEBOUNCE_BUFFERS");
    CFG_UINT32 (root, s_cfg.birdsong_freq,       "BIRDSONG_FREQ");
    CFG_FLOAT  (root, s_cfg.birdsong_multiplier, "BIRDSONG_MULTIPLIER");
    CFG_FLOAT  (root, s_cfg.birdsong_hf_ratio,   "BIRDSONG_HF_RATIO");
    CFG_FLOAT  (root, s_cfg.birdsong_mf_min,     "BIRDSONG_MF_MIN");
    CFG_UINT8  (root, s_cfg.birdsong_confirm,    "BIRDSONG_CONFIRM");
    CFG_FLOAT  (root, s_cfg.noise_floor_whistle,  "NOISE_FLOOR_WHISTLE");
    CFG_FLOAT  (root, s_cfg.noise_floor_voice,    "NOISE_FLOOR_VOICE");
    CFG_FLOAT  (root, s_cfg.noise_floor_birdsong, "NOISE_FLOOR_BIRDSONG");

    /* Playback volume */
    CFG_FLOAT  (root, s_cfg.volume,              "VOLUME");
    CFG_FLOAT  (root, s_cfg.volume_lux_min,      "VOLUME_LUX_MIN");
    CFG_FLOAT  (root, s_cfg.volume_lux_max,      "VOLUME_LUX_MAX");
    CFG_FLOAT  (root, s_cfg.volume_scale_min,    "VOLUME_SCALE_MIN");
    CFG_FLOAT  (root, s_cfg.volume_scale_max,    "VOLUME_SCALE_MAX");
    CFG_FLOAT  (root, s_cfg.quelea_gain,         "QUELEA_GAIN");

    /* Light sensor */
    CFG_UINT32 (root, s_cfg.lux_poll_interval_ms,  "LUX_POLL_INTERVAL_MS");
    CFG_FLOAT  (root, s_cfg.lux_change_threshold,  "LUX_CHANGE_THRESHOLD");
    CFG_FLOAT  (root, s_cfg.lux_flash_threshold,   "LUX_FLASH_THRESHOLD");
    CFG_FLOAT  (root, s_cfg.lux_flash_percent,     "LUX_FLASH_PERCENT");
    CFG_FLOAT  (root, s_cfg.lux_flash_min_abs,     "LUX_FLASH_MIN_ABS");

    /* LED behaviour */
    CFG_FLOAT  (root, s_cfg.lux_led_min,               "LUX_LED_MIN");
    CFG_FLOAT  (root, s_cfg.lux_led_max,               "LUX_LED_MAX");
    CFG_FLOAT  (root, s_cfg.lux_led_brightness_floor,  "LUX_LED_BRIGHTNESS_FLOOR");
    CFG_FLOAT  (root, s_cfg.lux_led_brightness_ceil,   "LUX_LED_BRIGHTNESS_CEIL");
    CFG_FLOAT  (root, s_cfg.vu_max_brightness,         "VU_MAX_BRIGHTNESS");

    /* ESP-NOW mesh */
    CFG_FLOAT  (root, s_cfg.espnow_lux_threshold,        "ESPNOW_LUX_THRESHOLD");
    CFG_UINT32 (root, s_cfg.espnow_event_ttl_ms,         "ESPNOW_EVENT_TTL_MS");
    CFG_UINT32 (root, s_cfg.espnow_sound_throttle_ms,    "ESPNOW_SOUND_THROTTLE_MS");
    CFG_UINT8  (root, s_cfg.espnow_flood_count,          "ESPNOW_FLOOD_COUNT");
    CFG_UINT32 (root, s_cfg.espnow_flood_window_ms,      "ESPNOW_FLOOD_WINDOW_MS");

    /* Markov chain */
    CFG_UINT32 (root, s_cfg.markov_idle_trigger_ms,         "MARKOV_IDLE_TRIGGER_MS");
    CFG_UINT32 (root, s_cfg.markov_autonomous_cooldown_ms,  "MARKOV_AUTONOMOUS_COOLDOWN_MS");
}

/* =========================================================================
 * PUBLIC API
 * ========================================================================= */

void remote_config_init(void)
{
    s_cfg = CONFIG_DEFAULTS;
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

    /* Parse JSON */
    cJSON *root = cJSON_Parse(s_http_body);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error near: %.40s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
        return ESP_FAIL;
    }

    apply_json(root);
    cJSON_Delete(root);

    s_cfg.loaded        = true;
    s_cfg.last_fetch_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "Config applied — vol=%.2f whistle=%luHz voice=%luHz poll=%lums",
             s_cfg.volume,
             (unsigned long)s_cfg.whistle_freq,
             (unsigned long)s_cfg.voice_freq,
             (unsigned long)s_cfg.lux_poll_interval_ms);

    return ESP_OK;
}

void remote_config_task(void *param)
{
    (void)param;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(REMOTE_CONFIG_POLL_INTERVAL_MS));
        ESP_LOGI(TAG, "Polling config server…");
        esp_err_t err = remote_config_fetch();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Config poll failed (%s) — keeping previous values",
                     esp_err_to_name(err));
        }
    }
}

const remote_config_t *remote_config_get(void)
{
    return &s_cfg;
}
