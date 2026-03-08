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
#include "esp_attr.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "STARTUP";

/* ========================================================================
 * RTC DIAGNOSTIC — persists across software resets, cleared on power-on
 * ======================================================================== */

RTC_NOINIT_ATTR static echoes_rtc_diag_t s_rtc_diag;

/**
 * @brief Write pre-reset diagnostic state to RTC_NOINIT memory.
 * IRAM_ATTR: safe to call from ISR context (pure memory writes, no alloc).
 */
void IRAM_ATTR startup_write_rtc_diag(uint32_t cause, uint32_t failures,
                                       uint32_t heap, int32_t rssi,
                                       uint32_t uptime_s)
{
    s_rtc_diag.cause                = cause;
    s_rtc_diag.consecutive_failures = failures;
    s_rtc_diag.heap_free            = heap;
    s_rtc_diag.rssi                 = rssi;
    s_rtc_diag.uptime_s             = uptime_s;
    s_rtc_diag.magic                = RTC_DIAG_MAGIC;  /* write magic last */
}

/* Human-readable label for RTC_DIAG_CAUSE_* constants */
static const char *rtc_diag_cause_str(uint32_t cause)
{
    switch (cause) {
    case RTC_DIAG_CAUSE_RCFG:    return "RCFG_REBOOT";
    case RTC_DIAG_CAUSE_REMOTE:  return "REMOTE_RESTART";
    case RTC_DIAG_CAUSE_ISR_WDT: return "ISR_WDT";
    default:                     return "UNKNOWN";
    }
}

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

/* ========================================================================
 * RESET REASON
 * ======================================================================== */

/**
 * @brief Convert esp_reset_reason_t to a short log-friendly label.
 *
 * Labels are kept short (<12 chars) so they fit neatly in fixed-width log
 * lines.  "TASK_WDT" and "PANIC" are the two most actionable values —
 * they indicate firmware bugs rather than deliberate or power-related resets.
 */
const char *startup_reset_reason_str(int reason)
{
    switch ((esp_reset_reason_t)reason) {
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT_PIN";
    case ESP_RST_SW:         return "SW_RESET";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT_WDT";
    case ESP_RST_TASK_WDT:   return "TASK_WDT";
    case ESP_RST_WDT:        return "WDT";
    case ESP_RST_DEEPSLEEP:  return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_SDIO:       return "SDIO";
    default:                 return "UNKNOWN";
    }
}

esp_err_t startup_capture_identity(startup_report_t *report, hardware_config_t hw_config)
{
    if (!report) return ESP_ERR_INVALID_ARG;

    memset(report, 0, sizeof(startup_report_t));

    /* Reset reason — read before any other initialisation touches the RTC
     * registers.  Called here (in startup_capture_identity) so the value is
     * available to the server before the application tasks start.           */
    esp_reset_reason_t reason = esp_reset_reason();
    strncpy(report->reset_reason,
            startup_reset_reason_str((int)reason),
            sizeof(report->reset_reason) - 1);
    report->reset_reason[sizeof(report->reset_reason) - 1] = '\0';
    ESP_LOGI(TAG, "Reset reason: %s (%d)", report->reset_reason, (int)reason);

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

    /* Read previous-boot RTC diagnostic — valid only after a software reset
     * triggered by one of our own reset paths (ISR WDT, RCFG reboot, remote
     * restart).  On power-on the RTC_NOINIT region is random garbage so we
     * check the magic word before trusting any of the fields.              */
    report->has_prev_diag = false;
    if (s_rtc_diag.magic == RTC_DIAG_MAGIC) {
        report->has_prev_diag       = true;
        report->prev_diag_failures  = s_rtc_diag.consecutive_failures;
        report->prev_diag_heap      = s_rtc_diag.heap_free;
        report->prev_diag_rssi      = s_rtc_diag.rssi;
        report->prev_diag_uptime_s  = s_rtc_diag.uptime_s;
        strncpy(report->prev_diag_cause,
                rtc_diag_cause_str(s_rtc_diag.cause),
                sizeof(report->prev_diag_cause) - 1);
        report->prev_diag_cause[sizeof(report->prev_diag_cause) - 1] = '\0';
        /* Invalidate so we don't re-report it on a subsequent power-on reset
         * that happens to land on the same memory pattern.                  */
        s_rtc_diag.magic = 0;
        ESP_LOGI(TAG, "Prev-boot diag: cause=%s failures=%lu heap=%lu rssi=%d uptime=%lus",
                 report->prev_diag_cause,
                 (unsigned long)report->prev_diag_failures,
                 (unsigned long)report->prev_diag_heap,
                 (int)report->prev_diag_rssi,
                 (unsigned long)report->prev_diag_uptime_s);
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
    char post_data[768];
    int len;

    if (report->has_prev_diag) {
        len = snprintf(post_data, sizeof(post_data),
            "{"
            "\"mac\":\"%s\","
            "\"firmware\":\"%s\","
            "\"node_type\":\"%s\","
            "\"reset_reason\":\"%s\","
            "\"has_errors\":%s,"
            "\"error_message\":\"%s\","
            "\"prev_diag\":{"
                "\"cause\":\"%s\","
                "\"failures\":%lu,"
                "\"heap\":%lu,"
                "\"rssi\":%d,"
                "\"uptime_s\":%lu"
            "}"
            "}",
            report->mac_address,
            FIRMWARE_VERSION,
            report->node_type,
            report->reset_reason,
            report->has_errors ? "true" : "false",
            report->error_message,
            report->prev_diag_cause,
            (unsigned long)report->prev_diag_failures,
            (unsigned long)report->prev_diag_heap,
            (int)report->prev_diag_rssi,
            (unsigned long)report->prev_diag_uptime_s
        );
    } else {
        len = snprintf(post_data, sizeof(post_data),
            "{"
            "\"mac\":\"%s\","
            "\"firmware\":\"%s\","
            "\"node_type\":\"%s\","
            "\"reset_reason\":\"%s\","
            "\"has_errors\":%s,"
            "\"error_message\":\"%s\""
            "}",
            report->mac_address,
            FIRMWARE_VERSION,
            report->node_type,
            report->reset_reason,
            report->has_errors ? "true" : "false",
            report->error_message
        );
    }
    
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
    
    /* Perform HTTP request.
     *
     * Retry policy: only ESP_ERR_HTTP_EAGAIN (connection timeout) is retried
     * with exponential backoff.  Hard errors (connection refused, DNS failure,
     * etc.) are not retried — they are unlikely to resolve quickly and the
     * boot sequence should not be held up.  Server-side non-2xx responses are
     * also not retried (the server received the data; the error is its own).
     *
     * Backoff delays (0-indexed attempt N): STARTUP_RETRY_BASE_MS * 2^(N-1)
     *   Attempt 0 → immediate
     *   Attempt 1 → 2 000 ms
     *   Attempt 2 → 4 000 ms
     *   Attempt 3 → 8 000 ms
     */
    esp_err_t err = ESP_FAIL;
    int attempt = 0;

    while (attempt < STARTUP_MAX_RETRIES) {
        if (attempt > 0) {
            uint32_t delay_ms = (uint32_t)STARTUP_RETRY_BASE_MS << (attempt - 1);
            ESP_LOGW(TAG, "Startup report retry %d/%d — waiting %lu ms",
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
                /* Non-2xx: server received the request but rejected it.
                 * No point retrying — treat as a permanent failure.     */
                ESP_LOGW(TAG, "Server returned status %d — not retrying", status_code);
                err = ESP_FAIL;
                break;
            }
        } else if (err == ESP_ERR_HTTP_EAGAIN) {
            /* Timeout — worth retrying in case the server is temporarily
             * unreachable.  Increment attempt and loop.                 */
            ESP_LOGW(TAG, "Startup report timed out (attempt %d/%d)",
                     attempt + 1, STARTUP_MAX_RETRIES);
            attempt++;
            continue;
        } else {
            /* Hard transport error (refused, DNS failure, etc.) — not retried. */
            ESP_LOGE(TAG, "HTTP POST failed: %s — not retrying", esp_err_to_name(err));
            break;
        }
    }

    esp_http_client_cleanup(client);

    if (err == ESP_ERR_HTTP_EAGAIN) {
        /* All attempts timed out — report failure so the caller can signal
         * this correctly (e.g. blue LED blink rather than false white blink). */
        ESP_LOGW(TAG, "Startup report not delivered after %d attempts — continuing",
                 STARTUP_MAX_RETRIES);
        return ESP_ERR_TIMEOUT;
    }

    return err;
}
