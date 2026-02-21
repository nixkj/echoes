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

#include <string.h>

/* ========================================================================
 * CONSTANTS AND GLOBALS
 * ======================================================================== */

static const char *TAG = "OTA";

/* Task handles so we can suspend noisy tasks during the OTA download.
 * Populated by ota_register_tasks() called from main.c after task creation. */
static TaskHandle_t s_flock_task_handle   = NULL;
static TaskHandle_t s_lux_task_handle     = NULL;
static TaskHandle_t s_audio_task_handle   = NULL;

/* Structure to hold received HTTP data */
typedef struct {
    char *buffer;
    int buffer_size;
    int data_len;
} http_receive_data_t;

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
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished. Connecting to SSID: %s", CONFIG_WIFI_SSID);

    /* Wait for connection */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    /* Check result */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi SSID: %s", CONFIG_WIFI_SSID);
        return true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", CONFIG_WIFI_SSID);
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
 * @brief HTTP event handler that captures response data
 */
static esp_err_t version_http_event_handler(esp_http_client_event_t *evt)
{
    http_receive_data_t *recv_data = (http_receive_data_t *)evt->user_data;
    
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", 
                 evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        // Copy data to buffer
        if (recv_data && evt->data_len > 0 && 
            (recv_data->data_len + evt->data_len) < recv_data->buffer_size) {
            memcpy(recv_data->buffer + recv_data->data_len, evt->data, evt->data_len);
            recv_data->data_len += evt->data_len;
            recv_data->buffer[recv_data->data_len] = '\0';
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

/**
 * @brief Fetch version string from server
 * @param version_buffer Buffer to store version string
 * @param buffer_size Size of buffer
 * @return true if successful, false otherwise
 */
static bool fetch_version_string(char *version_buffer, size_t buffer_size)
{
    // Prepare receive data structure
    http_receive_data_t recv_data = {
        .buffer = version_buffer,
        .buffer_size = buffer_size,
        .data_len = 0
    };
    
    esp_http_client_config_t config = {
        .url = VERSION_URL,
        .event_handler = version_http_event_handler,
        .user_data = &recv_data,
        .timeout_ms = 5000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        
        ESP_LOGI(TAG, "HTTP GET Status = %d, received %d bytes", 
                 status_code, recv_data.data_len);
        
        if (status_code == 200 && recv_data.data_len > 0) {
            // Remove trailing whitespace/newlines
            for (int i = recv_data.data_len - 1; i >= 0; i--) {
                if (version_buffer[i] == '\n' || version_buffer[i] == '\r' || 
                    version_buffer[i] == ' ' || version_buffer[i] == '\t') {
                    version_buffer[i] = '\0';
                } else {
                    break;
                }
            }
            
            ESP_LOGI(TAG, "Fetched version: %s", version_buffer);
            esp_http_client_cleanup(client);
            return true;
        } else {
            ESP_LOGE(TAG, "Invalid response: status=%d, data_len=%d", 
                     status_code, recv_data.data_len);
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

/**
 * Alternative OTA implementation using basic esp_http_client
 * Replace the ota_perform_update() function (lines 332-391) with this version
 */

bool ota_perform_update(const char *url)
{
    ESP_LOGI(TAG, "Starting OTA update from: %s", url);
    s_ota_state.ota_status = OTA_STATUS_DOWNLOADING;
    s_ota_state.download_progress_percent = 0;

    /* Suspend background tasks that generate ESP-NOW and I2S traffic.
     * Keeping the radio quiet during download reduces TCP retransmissions
     * on congested channels.  Tasks are resumed on failure or after restart
     * is called (success path calls esp_restart() so resume is not needed). */
    if (s_flock_task_handle)   vTaskSuspend(s_flock_task_handle);
    if (s_lux_task_handle)     vTaskSuspend(s_lux_task_handle);
    if (s_audio_task_handle)   vTaskSuspend(s_audio_task_handle);
    ESP_LOGI(TAG, "Background tasks suspended for OTA download");

    /* Disable WiFi power saving for the duration of the download.
     * WIFI_PS_MIN_MODEM lets the radio sleep between DTIM beacons; during
     * a sustained TCP stream that introduces latency spikes which can look
     * like packet loss and trigger unnecessary retransmissions.             */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Blink blue LED during update */
    set_led(0, BRIGHT_MID);

    esp_err_t err;

    /* Initialize OTA */
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%lx",
             update_partition->subtype, update_partition->address);

    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Configure HTTP client - simplified for plain HTTP */
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        esp_ota_abort(ota_handle);
        return false;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed, status = %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        return false;
    }

    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, content_length);
    ESP_LOGI(TAG, "Starting download...");

    /* Download and write firmware */
    int binary_file_length = 0;
    int consecutive_empty  = 0;       /* guard against infinite zero-read loops */
    char *buffer = malloc(4096);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        return false;
    }

    while (1) {
        int data_read = esp_http_client_read(client, buffer, 4096);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Error reading HTTP data (data_read=%d)", data_read);
            err = ESP_FAIL;
            break;
        } else if (data_read > 0) {
            consecutive_empty = 0;
            err = esp_ota_write(ota_handle, (const void *)buffer, data_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                break;
            }
            binary_file_length += data_read;

            /* Update progress */
            if (content_length > 0) {
                s_ota_state.download_progress_percent = (binary_file_length * 100) / content_length;
            }

            /* Blink LED to show activity */
            if ((binary_file_length % 40960) == 0) {
                set_led(0, BRIGHT_FULL);
                vTaskDelay(pdMS_TO_TICKS(10));
                set_led(0, BRIGHT_MID);
            }
        } else {
            /* data_read == 0: server hasn't sent more data yet OR connection closed. */
            if (esp_http_client_is_complete_data_received(client)) {
                ESP_LOGI(TAG, "Download complete (%d bytes)", binary_file_length);
                break;
            }
            /* Not complete — guard against spinning forever on a stalled connection */
            consecutive_empty++;
            if (consecutive_empty > 100) {
                ESP_LOGE(TAG, "Stalled download after %d bytes — aborting", binary_file_length);
                err = ESP_FAIL;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));  /* brief yield before re-polling */
        }
    }

    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Total binary data length: %d", binary_file_length);

    if (binary_file_length == 0 || err != ESP_OK) {
        ESP_LOGE(TAG, "Download failed");
        esp_ota_abort(ota_handle);

        /* Restore power save and background tasks on failure */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (s_flock_task_handle)   vTaskResume(s_flock_task_handle);
        if (s_lux_task_handle)     vTaskResume(s_lux_task_handle);
        if (s_audio_task_handle)   vTaskResume(s_audio_task_handle);
        ESP_LOGI(TAG, "Background tasks resumed after failed download");

        /* Indicate failure with rapid blue blink */
        for (int i = 0; i < 5; i++) {
            set_led(0, BRIGHT_FULL);
            vTaskDelay(pdMS_TO_TICKS(100));
            set_led(0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        return false;
    }

    /* Finalize OTA */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        }

        /* Restore power save and background tasks on failure */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (s_flock_task_handle)   vTaskResume(s_flock_task_handle);
        if (s_lux_task_handle)     vTaskResume(s_lux_task_handle);
        if (s_audio_task_handle)   vTaskResume(s_audio_task_handle);

        /* Indicate failure with rapid blue blink */
        for (int i = 0; i < 5; i++) {
            set_led(0, BRIGHT_FULL);
            vTaskDelay(pdMS_TO_TICKS(100));
            set_led(0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        return false;
    }

    /* Set boot partition */
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "OTA update successful! Restarting...");
    s_ota_state.ota_status = OTA_STATUS_SUCCESS;
    s_ota_state.download_progress_percent = 100;

    /* Indicate success with white LED */
    set_led(BRIGHT_FULL, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Restart ESP32 */
    esp_restart();
    return true;
}

void ota_register_tasks(TaskHandle_t flock, TaskHandle_t lux, TaskHandle_t audio)
{
    s_flock_task_handle = flock;
    s_lux_task_handle   = lux;
    s_audio_task_handle = audio;
    ESP_LOGI(TAG, "OTA registered %d task handle(s) for suspension during download",
             (flock ? 1 : 0) + (lux ? 1 : 0) + (audio ? 1 : 0));
}

bool ota_check_and_update(void)
{
    if (!s_ota_state.wifi_connected) {
        ESP_LOGW(TAG, "Cannot check for updates: WiFi not connected");
        return false;
    }

    ESP_LOGI(TAG, "Checking for firmware updates (current: %s)", s_ota_state.current_version);

    /* Retry loop — attempts the full version-check + download cycle up to
     * OTA_MAX_ATTEMPTS times.  Linear backoff (15 s, 30 s, 45 s …) spreads
     * retries across time so a fleet of devices does not all hammer the AP
     * simultaneously after a transient failure.                            */
    for (int attempt = 1; attempt <= OTA_MAX_ATTEMPTS; attempt++) {

        s_ota_state.ota_status = OTA_STATUS_CHECKING;

        /* ── Step 1: fetch version ──────────────────────────────────── */
        char available_version[32];
        if (!fetch_version_string(available_version, sizeof(available_version))) {
            ESP_LOGW(TAG, "Attempt %d/%d: failed to fetch version string",
                     attempt, OTA_MAX_ATTEMPTS);
            goto retry;
        }

        strncpy(s_ota_state.available_version, available_version,
                sizeof(s_ota_state.available_version) - 1);
        ESP_LOGI(TAG, "Attempt %d/%d: available version = %s",
                 attempt, OTA_MAX_ATTEMPTS, available_version);

        /* ── Step 2: compare ────────────────────────────────────────── */
        if (compare_versions(available_version, s_ota_state.current_version) <= 0) {
            ESP_LOGI(TAG, "Firmware is up to date (%s)", s_ota_state.current_version);
            s_ota_state.ota_status = OTA_STATUS_NO_UPDATE;
            return false;
        }

        ESP_LOGI(TAG, "New firmware available: %s → %s",
                 s_ota_state.current_version, available_version);

        /* ── Step 3: download + flash ───────────────────────────────── */
        if (ota_perform_update(OTA_URL)) {
            return true;   /* esp_restart() is called inside; won't return */
        }

        ESP_LOGW(TAG, "Attempt %d/%d: OTA download/flash failed",
                 attempt, OTA_MAX_ATTEMPTS);

retry:
        if (attempt < OTA_MAX_ATTEMPTS) {
            uint32_t delay_ms = (uint32_t)attempt * OTA_RETRY_BASE_DELAY_MS;
            ESP_LOGI(TAG, "Retrying in %lu s…", (unsigned long)(delay_ms / 1000));
            s_ota_state.ota_status = OTA_STATUS_FAILED;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    ESP_LOGE(TAG, "OTA failed after %d attempts", OTA_MAX_ATTEMPTS);
    s_ota_state.ota_status = OTA_STATUS_FAILED;
    return false;
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
