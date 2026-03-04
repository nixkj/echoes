/**
 * @file ota.c
 * @brief OTA Firmware Update Implementation
 */

#include "ota.h"
#include "echoes.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
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

/* After WIFI_MAXIMUM_RETRY rapid attempts all fail (e.g. AP is down), we
 * back off and retry every WIFI_RECONNECT_DELAY_MS.  This keeps the node
 * reconnecting automatically when the AP recovers without hammering it. */
#define WIFI_RECONNECT_DELAY_MS  30000
static TimerHandle_t s_reconnect_timer = NULL;

static void wifi_reconnect_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    ESP_LOGI("WIFI", "Attempting reconnect after backoff...");
    s_retry_num = 0;        /* reset so the disconnect handler gets fresh retries */
    esp_wifi_connect();
}

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
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        /* Disable modem sleep immediately on L2 association — before DHCP
         * completes and before IP_EVENT_STA_GOT_IP fires.
         *
         * Previously PS_NONE was only applied in the IP_EVENT_STA_GOT_IP
         * handler.  If DHCP fails after a mid-session reconnect (e.g. a
         * REQUEST packet is lost in RF congestion), GOT_IP never fires and
         * the node stays in WIFI_PS_MIN_MODEM indefinitely.  Many APs
         * deauthenticate clients that sleep continuously for 10–30 minutes
         * without sending any frames, causing minimal nodes — which have no
         * lux task and therefore no regular outbound traffic — to silently
         * vanish from the network.  Full nodes are immune because the 500 ms
         * lux broadcast keeps the radio active and prevents the AP's idle
         * timer from expiring.
         *
         * Applying PS_NONE here guarantees the radio stays awake through the
         * DHCP exchange and beyond, on every connection attempt, regardless
         * of whether the IP layer subsequently succeeds.                    */
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "WiFi associated — modem sleep disabled");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ota_state.wifi_connected = false;
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry connecting to WiFi (%d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
        } else {
            /* Rapid retries exhausted — start a backoff timer so we keep
             * trying every WIFI_RECONNECT_DELAY_MS until the AP recovers.
             * The timer resets s_retry_num so each backoff attempt gets a
             * fresh burst of WIFI_MAXIMUM_RETRY quick retries.             */
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            if (s_reconnect_timer != NULL &&
                xTimerIsTimerActive(s_reconnect_timer) == pdFALSE) {
                ESP_LOGW(TAG, "WiFi retries exhausted — backing off %d s before next attempt",
                         WIFI_RECONNECT_DELAY_MS / 1000);
                xTimerStart(s_reconnect_timer, 0);
            }
        }
        ESP_LOGI(TAG, "WiFi disconnected");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        /* Stop the backoff timer if it was running — connection is up */
        if (s_reconnect_timer != NULL &&
            xTimerIsTimerActive(s_reconnect_timer) == pdTRUE) {
            xTimerStop(s_reconnect_timer, 0);
        }
        /* Re-apply PS_NONE now that the IP layer is confirmed up.
         * The primary application is in WIFI_EVENT_STA_CONNECTED above,
         * which covers the window between L2 association and DHCP completion.
         * Calling it again here is a cheap belt-and-suspenders guard in case
         * any future code path inadvertently re-enables power saving between
         * association and GOT_IP.                                           */
        esp_wifi_set_ps(WIFI_PS_NONE);
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
            .ssid               = CONFIG_WIFI_SSID,
            .password           = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            /* Explicit values — do not rely on zero-initialisation defaults
             * which can vary across ESP-IDF versions.
             *
             * failure_retry_cnt: number of auth/assoc failures before the
             * driver reports WIFI_REASON_CONNECTION_FAIL.  3 is a reasonable
             * balance between giving the AP time to respond and not waiting
             * too long on a genuinely unreachable AP.
             *
             * listen_interval: how many beacon intervals the STA may sleep
             * between listening for buffered frames.  1 = wake every beacon.
             * Belt-and-suspenders alongside WIFI_PS_NONE — ensures the radio
             * is never configured to sleep even if PS_NONE is somehow lost. */
            .failure_retry_cnt  = 3,
            .listen_interval    = 1,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* Create the backoff reconnect timer (one-shot; restarted by the
     * disconnect handler each time retries are exhausted).               */
    s_reconnect_timer = xTimerCreate("wifi_reconnect",
                                     pdMS_TO_TICKS(WIFI_RECONNECT_DELAY_MS),
                                     pdFALSE,          /* one-shot */
                                     NULL,
                                     wifi_reconnect_timer_cb);
    /* This timer is the sole mid-session WiFi recovery mechanism.  If it
     * cannot be created (heap exhaustion at boot) the node will connect
     * once at startup but will NOT reconnect after any AP drop — it will
     * silently disappear from the network until power-cycled.  Treat this
     * as a hard error so it shows up in logs and the startup report.      */
    if (s_reconnect_timer == NULL) {
        ESP_LOGE(TAG, "FATAL: Failed to create WiFi reconnect timer — "
                      "node will not recover from mid-session WiFi drops");
    }

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

bool wifi_reconnect_timer_ok(void)
{
    return (s_reconnect_timer != NULL);
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
 * @return positive if v1 > v2, 0 if equal, negative if v1 < v2, -1 if malformed
 */
static int compare_versions(const char *v1, const char *v2)
{
    int v1_major = 0, v1_minor = 0, v1_patch = 0;
    int v2_major = 0, v2_minor = 0, v2_patch = 0;

    /* Check sscanf return values — a malformed version string (e.g. an HTTP
     * error page snippet) would silently leave variables at 0, making the
     * comparison report "up to date" and suppressing a valid OTA update.   */
    if (sscanf(v1, "%d.%d.%d", &v1_major, &v1_minor, &v1_patch) != 3) {
        ESP_LOGE(TAG, "Malformed version string v1: '%s'", v1);
        return -1;
    }
    if (sscanf(v2, "%d.%d.%d", &v2_major, &v2_minor, &v2_patch) != 3) {
        ESP_LOGE(TAG, "Malformed version string v2: '%s'", v2);
        return -1;
    }

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
    /* Tasks are already suspended by app_main before ota_check_and_update()
     * is called.  This re-suspend is a safe no-op for tasks that are already
     * in the suspended state; it is kept here so ota_perform_update() remains
     * callable standalone (e.g. from ota_task) without requiring the caller to
     * pre-suspend.  When called from app_main's boot path the calls are no-ops. */
    if (s_flock_task_handle)   vTaskSuspend(s_flock_task_handle);
    if (s_lux_task_handle)     vTaskSuspend(s_lux_task_handle);
    if (s_audio_task_handle)   vTaskSuspend(s_audio_task_handle);
    ESP_LOGI(TAG, "Background tasks confirmed suspended for OTA download");

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

        /* Do NOT resume tasks here — app_main calls ota_resume_tasks()
         * after espnow_mesh_init() and i2s_microphone_init() complete,
         * ensuring tasks never run before their hardware is ready.      */

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

        /* Do NOT resume tasks here — see ota_resume_tasks() comment above. */

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

        /* Do NOT resume tasks here — see ota_resume_tasks() comment above. */

        /* Indicate failure with rapid blue blink */
        for (int i = 0; i < 5; i++) {
            set_led(0, BRIGHT_FULL);
            vTaskDelay(pdMS_TO_TICKS(100));
            set_led(0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

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

void ota_resume_tasks(void)
{
    /* Resume order: audio first (highest priority — starts feeding the WDT
     * immediately), then lux and flock.  All three must only be called after
     * espnow_mesh_init() and i2s_microphone_init() have completed in app_main,
     * which is guaranteed because app_main calls this function explicitly after
     * those initialisations rather than letting ota_perform_update() call it
     * from inside an OTA failure path.                                        */
    if (s_audio_task_handle) {
        vTaskResume(s_audio_task_handle);
        ESP_LOGI(TAG, "audio_detection task resumed");
    }
    if (s_lux_task_handle) {
        vTaskResume(s_lux_task_handle);
        ESP_LOGI(TAG, "lux_birds task resumed");
    }
    if (s_flock_task_handle) {
        vTaskResume(s_flock_task_handle);
        ESP_LOGI(TAG, "flock task resumed");
    }
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
