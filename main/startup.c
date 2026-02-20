/**
 * @file startup.c
 * @brief Startup reporting implementation
 */

#include "startup.h"
#include "echoes.h"
#include "ota.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_random.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "STARTUP";

/* ========================================================================
 * HELPER FUNCTIONS
 * ======================================================================== */

esp_err_t startup_get_mac_address(char *mac_str)
{
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    if (ret == ESP_OK) {
        snprintf(mac_str, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return ESP_OK;
    }
    
    return ret;
}

/**
 * @brief Generate random sleep duration in milliseconds
 */
static uint32_t generate_random_sleep_duration(void)
{
    uint32_t range = STARTUP_SLEEP_MAX_MS - STARTUP_SLEEP_MIN_MS;
    uint32_t random_val = esp_random();
    uint32_t duration = STARTUP_SLEEP_MIN_MS + (random_val % (range + 1));
    
    ESP_LOGI(TAG, "Generated random sleep duration: %lu ms", duration);
    return duration;
}

/* ========================================================================
 * MAIN FUNCTIONS
 * ======================================================================== */

esp_err_t startup_capture_identity(startup_report_t *report, hardware_config_t hw_config)
{
    if (!report) return ESP_ERR_INVALID_ARG;

    memset(report, 0, sizeof(startup_report_t));

    /* Node type from detected hardware */
    switch (hw_config) {
        case HW_CONFIG_FULL:
            strncpy(report->node_type, "echoes-full", sizeof(report->node_type) - 1);
            break;
        case HW_CONFIG_MINIMAL:
            strncpy(report->node_type, "echoes-minimal", sizeof(report->node_type) - 1);
            break;
        default:
            strncpy(report->node_type, "echoes-unknown", sizeof(report->node_type) - 1);
            break;
    }
    report->node_type[sizeof(report->node_type) - 1] = '\0';

    /* MAC address */
    esp_err_t ret = startup_get_mac_address(report->mac_address);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get MAC address: %s", esp_err_to_name(ret));
        report->has_errors = true;
        snprintf(report->error_message, sizeof(report->error_message),
                 "MAC read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Device MAC: %s  Node type: %s", report->mac_address, report->node_type);
    return ESP_OK;
}

esp_err_t startup_jitter_and_sample(startup_report_t *report)
{
    if (!report) return ESP_ERR_INVALID_ARG;

    /* Random jitter sleep with light sampling */
    report->sleep_duration_ms = generate_random_sleep_duration();

    const uint32_t sample_interval_ms = 100;
    uint32_t num_samples = report->sleep_duration_ms / sample_interval_ms;
    if (num_samples == 0) num_samples = 1;

    ESP_LOGI(TAG, "Jitter sleep %lu ms, taking %lu light samples",
             report->sleep_duration_ms, num_samples);

    float light_sum = 0.0f;
    uint32_t valid_samples = 0;

    for (uint32_t i = 0; i < num_samples; i++) {
        float lux = get_lux_level();
        if (lux >= 0.0f && lux == lux) {
            light_sum += lux;
            valid_samples++;
        } else {
            ESP_LOGW(TAG, "Invalid light reading at sample %lu: %f", i, lux);
        }
        if (i < num_samples - 1) {
            vTaskDelay(pdMS_TO_TICKS(sample_interval_ms));
        }
    }

    if (valid_samples > 0) {
        report->avg_light_level = light_sum / valid_samples;
        report->light_samples   = valid_samples;
        ESP_LOGI(TAG, "Average light level: %.2f lux (%lu valid samples)",
                 report->avg_light_level, valid_samples);
    } else {
        report->avg_light_level = -1.0f;
        report->light_samples   = 0;
        report->has_errors      = true;
        snprintf(report->error_message, sizeof(report->error_message),
                 "No valid light sensor readings");
        ESP_LOGW(TAG, "No valid light sensor readings obtained");
    }

    /* Sleep out any remaining jitter time */
    uint32_t elapsed = num_samples * sample_interval_ms;
    if (elapsed < report->sleep_duration_ms) {
        vTaskDelay(pdMS_TO_TICKS(report->sleep_duration_ms - elapsed));
    }

    return ESP_OK;
}

esp_err_t startup_sleep_and_sample(startup_report_t *report, hardware_config_t hw_config)
{
    /* Legacy wrapper — prefer the two-step API in new code */
    esp_err_t ret = startup_capture_identity(report, hw_config);
    if (ret != ESP_OK) return ret;
    return startup_jitter_and_sample(report);
}

/**
 * @brief HTTP event handler for startup report POST
 */
static esp_err_t startup_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        // Log server response if any
        if (evt->data_len > 0) {
            ESP_LOGI(TAG, "Server response: %.*s", evt->data_len, (char*)evt->data);
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t startup_send_report(const startup_report_t *report)
{
    if (!report) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Sending startup report to %s", STARTUP_REPORT_URL);
    
    // Build JSON payload
    char post_data[512];
    int len = snprintf(post_data, sizeof(post_data),
        "{"
        "\"mac\":\"%s\","
        "\"firmware\":\"%s\","
        "\"node_type\":\"%s\","
        "\"avg_light\":%.2f,"
        "\"light_samples\":%lu,"
        "\"sleep_duration_ms\":%lu,"
        "\"has_errors\":%s,"
        "\"error_message\":\"%s\""
        "}",
        report->mac_address,
	FIRMWARE_VERSION,
        report->node_type,
        report->avg_light_level,
        report->light_samples,
        report->sleep_duration_ms,
        report->has_errors ? "true" : "false",
        report->error_message
    );
    
    if (len >= sizeof(post_data)) {
        ESP_LOGE(TAG, "POST data buffer too small");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "POST data: %s", post_data);
    
    // Configure HTTP client with shorter timeout
    esp_http_client_config_t config = {
        .url = STARTUP_REPORT_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = startup_http_event_handler,
        .timeout_ms = STARTUP_HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,  // Don't keep connection alive
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    
    // Set headers
    esp_http_client_set_header(client, "Content-Type", "application/json");
    
    // Set POST data
    esp_http_client_set_post_field(client, post_data, len);
    
    // Perform HTTP request with exponential backoff retry.
    // Delay before attempt N (0-indexed): STARTUP_RETRY_BASE_MS * 2^N
    //   Attempt 0 → immediate
    //   Attempt 1 → 2 000 ms
    //   Attempt 2 → 4 000 ms
    //   Attempt 3 → 8 000 ms
    esp_err_t err = ESP_FAIL;
    int attempt = 0;

    while (attempt < STARTUP_MAX_RETRIES) {
        if (attempt > 0) {
            uint32_t delay_ms = (uint32_t)STARTUP_RETRY_BASE_MS << (attempt - 1);  // 2^(attempt-1) * base
            ESP_LOGW(TAG, "Startup report retry %d/%d — waiting %lu ms before next attempt",
                     attempt, STARTUP_MAX_RETRIES - 1, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        err = esp_http_client_perform(client);

        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            int content_length = esp_http_client_get_content_length(client);

            ESP_LOGI(TAG, "HTTP POST status=%d content_length=%d", status_code, content_length);

            if (status_code >= 200 && status_code < 300) {
                ESP_LOGI(TAG, "Startup report sent successfully (attempt %d)", attempt + 1);
                break;
            } else {
                // Server replied with an error status — no point retrying
                ESP_LOGW(TAG, "Server returned status %d — not retrying", status_code);
                err = ESP_FAIL;
                break;
            }
        } else if (err == ESP_ERR_HTTP_EAGAIN) {
            // Timeout — worth retrying
            ESP_LOGW(TAG, "Startup report timed out (attempt %d/%d)",
                     attempt + 1, STARTUP_MAX_RETRIES);
        } else {
            // Hard error (DNS, TCP refused, etc.) — no point retrying
            ESP_LOGE(TAG, "HTTP POST failed: %s — not retrying", esp_err_to_name(err));
            break;
        }

        attempt++;
    }

    esp_http_client_cleanup(client);

    // Timeouts are non-fatal — the system operates fine without a startup report.
    if (err == ESP_ERR_HTTP_EAGAIN) {
        ESP_LOGW(TAG, "Startup report not delivered after %d attempts — continuing",
                 STARTUP_MAX_RETRIES);
        return ESP_OK;
    }

    return err;
}
