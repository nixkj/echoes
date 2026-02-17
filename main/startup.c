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

esp_err_t startup_sleep_and_sample(startup_report_t *report)
{
    if (!report) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Initialize report structure
    memset(report, 0, sizeof(startup_report_t));
    strncpy(report->node_type, NODE_TYPE, sizeof(report->node_type) - 1);
    
    // Get MAC address
    esp_err_t ret = startup_get_mac_address(report->mac_address);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get MAC address: %s", esp_err_to_name(ret));
        report->has_errors = true;
        snprintf(report->error_message, sizeof(report->error_message),
                 "MAC read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Device MAC: %s", report->mac_address);
    
    // Generate random sleep duration
    report->sleep_duration_ms = generate_random_sleep_duration();
    
    // Calculate sampling parameters
    const uint32_t sample_interval_ms = 100;  // Sample every 100ms
    uint32_t num_samples = report->sleep_duration_ms / sample_interval_ms;
    
    if (num_samples == 0) {
        num_samples = 1;  // At least one sample
    }
    
    ESP_LOGI(TAG, "Sleeping for %lu ms, taking %lu light samples",
             report->sleep_duration_ms, num_samples);
    
    // Sample light sensor during sleep period
    float light_sum = 0.0f;
    uint32_t valid_samples = 0;
    
    for (uint32_t i = 0; i < num_samples; i++) {
        // Get light level
        float lux = get_lux_level();
        
        // Check if reading is valid (not negative, not NaN)
        if (lux >= 0.0f && lux == lux) {  // lux == lux checks for NaN
            light_sum += lux;
            valid_samples++;
        } else {
            ESP_LOGW(TAG, "Invalid light reading at sample %lu: %f", i, lux);
        }
        
        // Sleep until next sample (or end of sleep period)
        if (i < num_samples - 1) {
            vTaskDelay(pdMS_TO_TICKS(sample_interval_ms));
        }
    }
    
    // Calculate average
    if (valid_samples > 0) {
        report->avg_light_level = light_sum / valid_samples;
        report->light_samples = valid_samples;
        ESP_LOGI(TAG, "Average light level: %.2f lux (%lu valid samples)",
                 report->avg_light_level, valid_samples);
    } else {
        report->avg_light_level = -1.0f;  // Indicate no valid readings
        report->light_samples = 0;
        report->has_errors = true;
        snprintf(report->error_message, sizeof(report->error_message),
                 "No valid light sensor readings");
        ESP_LOGW(TAG, "No valid light sensor readings obtained");
    }
    
    // Sleep for any remaining time
    uint32_t elapsed = num_samples * sample_interval_ms;
    if (elapsed < report->sleep_duration_ms) {
        vTaskDelay(pdMS_TO_TICKS(report->sleep_duration_ms - elapsed));
    }
    
    return ESP_OK;
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
    
    // Perform HTTP request with retry on timeout
    esp_err_t err = ESP_FAIL;
    int retry_count = 0;
    const int max_retries = 3;
    
    while (retry_count < max_retries) {
        err = esp_http_client_perform(client);
        
        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            int content_length = esp_http_client_get_content_length(client);
            
            ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d",
                     status_code, content_length);
            
            if (status_code >= 200 && status_code < 300) {
                ESP_LOGI(TAG, "Startup report sent successfully");
                break;
            } else {
                ESP_LOGW(TAG, "Server returned status code: %d", status_code);
                err = ESP_FAIL;
                break;
            }
        } else if (err == ESP_ERR_HTTP_EAGAIN) {
            retry_count++;
            if (retry_count < max_retries) {
                ESP_LOGW(TAG, "Timeout, retrying (%d/%d)...", retry_count, max_retries);
                vTaskDelay(pdMS_TO_TICKS(500));  // Wait 500ms before retry
            } else {
                ESP_LOGW(TAG, "Startup report timeout after %d retries (server may not be running)", max_retries);
                // Don't treat this as a critical error - system can still function
                err = ESP_OK;  // Return OK so system continues
            }
        } else {
            ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
            break;
        }
    }
    
    esp_http_client_cleanup(client);
    
    // If timeout, return OK anyway (non-critical failure)
    if (err == ESP_ERR_HTTP_EAGAIN) {
        ESP_LOGI(TAG, "Continuing without startup report (non-critical)");
        return ESP_OK;
    }
    
    return err;
}
