/**
 * @file ota.c
 * @brief OTA Firmware Update Implementation
 */

#include "ota.h"
#include "echoes.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_netif.h"

#include "nvs_flash.h"

#include <string.h>

/* ========================================================================
 * CONSTANTS AND GLOBALS
 * ======================================================================== */

static const char *TAG = "OTA";

/* FreeRTOS event group for WiFi events */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static ota_state_t s_ota_state = {
    .wifi_connected = false,
    .ota_status = OTA_STATUS_IDLE,
    .current_version = FIRMWARE_VERSION,
    .available_version = "",
    .download_progress_percent = 0
};

/* ========================================================================
 * WIFI EVENT HANDLER
 * ======================================================================== */

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry connecting to WiFi (%d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        s_ota_state.wifi_connected = false;
        ESP_LOGI(TAG, "WiFi connection failed");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_ota_state.wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ========================================================================
 * WIFI INITIALIZATION
 * ======================================================================== */

bool wifi_init_and_connect(void)
{
    ESP_LOGI(TAG, "Initializing WiFi...");
    
    /* Initialize NVS (required for WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());
    
    /* Create default event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    /* Create default WiFi station */
    esp_netif_create_default_wifi_sta();

    /* Initialize WiFi with default config */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Create event group */
    s_wifi_event_group = xEventGroupCreate();

    /* Register event handlers */
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    /* Configure WiFi */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished. Connecting to SSID: %s", WIFI_SSID);

    /* Wait for connection */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    /* Check result */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi SSID: %s", WIFI_SSID);
        return true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", WIFI_SSID);
        return false;
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout");
        return false;
    }
}

bool wifi_is_connected(void)
{
    return s_ota_state.wifi_connected;
}

void wifi_disconnect(void)
{
    if (s_ota_state.wifi_connected) {
        ESP_LOGI(TAG, "Disconnecting WiFi...");
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_ota_state.wifi_connected = false;
    }
}

/* ========================================================================
 * OTA FUNCTIONS
 * ======================================================================== */

/**
 * @brief HTTP event handler for OTA progress tracking
 */
static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * @brief Fetch version string from server
 * @param version_buffer Buffer to store version string
 * @param buffer_size Size of buffer
 * @return true if successful, false otherwise
 */
static bool fetch_version_string(char *version_buffer, size_t buffer_size)
{
    esp_http_client_config_t config = {
        .url = VERSION_URL,
        .event_handler = ota_http_event_handler,
        .timeout_ms = 5000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int content_length = esp_http_client_get_content_length(client);
        int status_code = esp_http_client_get_status_code(client);
        
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, content_length);
        
        if (status_code == 200 && content_length > 0 && content_length < buffer_size) {
            int read_len = esp_http_client_read(client, version_buffer, content_length);
            if (read_len > 0) {
                version_buffer[read_len] = '\0';
                
                /* Remove trailing whitespace/newlines */
                for (int i = read_len - 1; i >= 0; i--) {
                    if (version_buffer[i] == '\n' || version_buffer[i] == '\r' || version_buffer[i] == ' ') {
                        version_buffer[i] = '\0';
                    } else {
                        break;
                    }
                }
                
                esp_http_client_cleanup(client);
                return true;
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return false;
}

/**
 * @brief Compare version strings
 * @param v1 Version string 1 (e.g., "1.2.3")
 * @param v2 Version string 2
 * @return positive if v1 > v2, 0 if equal, negative if v1 < v2
 */
static int compare_versions(const char *v1, const char *v2)
{
    int v1_major = 0, v1_minor = 0, v1_patch = 0;
    int v2_major = 0, v2_minor = 0, v2_patch = 0;
    
    sscanf(v1, "%d.%d.%d", &v1_major, &v1_minor, &v1_patch);
    sscanf(v2, "%d.%d.%d", &v2_major, &v2_minor, &v2_patch);
    
    if (v1_major != v2_major) return v1_major - v2_major;
    if (v1_minor != v2_minor) return v1_minor - v2_minor;
    return v1_patch - v2_patch;
}

bool ota_perform_update(const char *url)
{
    ESP_LOGI(TAG, "Starting OTA update from: %s", url);
    s_ota_state.ota_status = OTA_STATUS_DOWNLOADING;
    s_ota_state.download_progress_percent = 0;
    
    /* Blink blue LED during update */
    set_led(0, BRIGHT_MID);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = ota_http_event_handler,
        .timeout_ms = 30000,
        .buffer_size = OTA_BUFFER_SIZE,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA update successful! Restarting...");
        s_ota_state.ota_status = OTA_STATUS_SUCCESS;
        s_ota_state.download_progress_percent = 100;
        
        /* Indicate success with white LED */
        set_led(BRIGHT_FULL, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        /* Restart ESP32 */
        esp_restart();
        return true;
    } else {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(ret));
        s_ota_state.ota_status = OTA_STATUS_FAILED;
        
        /* Indicate failure with rapid blue blink */
        for (int i = 0; i < 5; i++) {
            set_led(0, BRIGHT_FULL);
            vTaskDelay(pdMS_TO_TICKS(100));
            set_led(0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        return false;
    }
}

bool ota_check_and_update(void)
{
    if (!s_ota_state.wifi_connected) {
        ESP_LOGW(TAG, "Cannot check for updates: WiFi not connected");
        return false;
    }

    ESP_LOGI(TAG, "Checking for firmware updates...");
    ESP_LOGI(TAG, "Current version: %s", s_ota_state.current_version);
    
    s_ota_state.ota_status = OTA_STATUS_CHECKING;
    
    /* Fetch available version from server */
    char available_version[32];
    if (!fetch_version_string(available_version, sizeof(available_version))) {
        ESP_LOGE(TAG, "Failed to fetch version information");
        s_ota_state.ota_status = OTA_STATUS_FAILED;
        return false;
    }
    
    strncpy(s_ota_state.available_version, available_version, sizeof(s_ota_state.available_version) - 1);
    ESP_LOGI(TAG, "Available version: %s", s_ota_state.available_version);
    
    /* Compare versions */
    int version_diff = compare_versions(available_version, s_ota_state.current_version);
    
    if (version_diff > 0) {
        ESP_LOGI(TAG, "New firmware available! Updating from %s to %s", 
                 s_ota_state.current_version, available_version);
        
        /* Perform OTA update */
        return ota_perform_update(OTA_URL);
    } else {
        ESP_LOGI(TAG, "Firmware is up to date");
        s_ota_state.ota_status = OTA_STATUS_NO_UPDATE;
        return false;
    }
}

const ota_state_t* ota_get_state(void)
{
    return &s_ota_state;
}

void ota_task(void *param)
{
    /* Initial check at boot */
    ESP_LOGI(TAG, "OTA task started. Firmware version: %s", FIRMWARE_VERSION);
    
    /* Wait for system to stabilize */
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    /* Check for update */
    ota_check_and_update();
    
    /* Periodic update checks */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
        
        if (s_ota_state.wifi_connected) {
            ESP_LOGI(TAG, "Performing periodic update check");
            ota_check_and_update();
        } else {
            ESP_LOGW(TAG, "Skipping update check - WiFi not connected");
        }
    }
}
