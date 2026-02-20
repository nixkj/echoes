/**
 * @file main.c
 * @brief Main application entry point with OTA support and startup reporting
 * 
 * Initializes WiFi, sends startup report, checks for firmware updates, 
 * then starts Echoes of the Machine
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"

#include "echoes.h"
#include "synthesis.h"
#include "ota.h"
#include "startup.h"
#include "espnow_mesh.h"
#include "markov.h"
#include "remote_config.h"

// ESP32-specific includes
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "nvs_flash.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    led_init();
    set_led(0, BRIGHT_FULL);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Echoes of the Machine");
    ESP_LOGI(TAG, "Firmware Version: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "========================================");
    
    /* Initialise NVS flash (required by Markov chain persistence) */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue — erasing and reinitialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* Initialise remote config with defaults (works even without WiFi) */
    remote_config_init();

    /* Initialize system hardware (including light sensor detection) */
    ESP_LOGI(TAG, "Initializing system...");
    system_init();

    /* Capture any errors on startup */
    startup_report_t startup_report;
    
    /* Initialize WiFi and connect */
    ESP_LOGI(TAG, "Connecting to WiFi...");
    bool wifi_connected = wifi_init_and_connect();

    /* Small delay to ensure all hardware is stable before sampling */
    vTaskDelay(pdMS_TO_TICKS(500));

    // Hardware config already detected in system_init — fetch it now so
    // hw_config is needed to embed the correct node_type in the report.
    hardware_config_t hw_config = get_hardware_config();

    /* Capture identity and send report before doing anything else */
    ESP_LOGI(TAG, "Capturing device identity...");
    esp_err_t startup_err = startup_capture_identity(&startup_report, hw_config);
    if (startup_err != ESP_OK) {
        ESP_LOGW(TAG, "Identity capture failed: %s", esp_err_to_name(startup_err));
    }

    if (wifi_connected) {
        ESP_LOGI(TAG, "Sending startup report...");
        esp_err_t report_err = startup_send_report(&startup_report);
        if (report_err == ESP_OK) {
            ESP_LOGI(TAG, "Startup report sent successfully");
            set_led(BRIGHT_MID, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
            set_led(0, 0);
        } else {
            ESP_LOGW(TAG, "Failed to send startup report: %s", esp_err_to_name(report_err));
            set_led(0, BRIGHT_MID);
            vTaskDelay(pdMS_TO_TICKS(200));
            set_led(0, 0);
        }
    } else {
        ESP_LOGI(TAG, "WiFi not connected — skipping startup report");
    }

    // Initialize audio hardware
    ESP_LOGI(TAG, "Initializing audio hardware...");
    ESP_ERROR_CHECK(i2s_microphone_init());
    
    if (hw_config == HW_CONFIG_FULL) {
        ESP_LOGI(TAG, "Full hardware detected - initializing speaker");
        ESP_ERROR_CHECK(i2s_speaker_init());
    } else {
        ESP_LOGI(TAG, "Minimal hardware detected - speaker disabled, LED VU mode");
    }


    if (wifi_connected) {
        ESP_LOGI(TAG, "WiFi connected successfully");

        /* Fetch remote config from server (best-effort — defaults used on failure) */
        ESP_LOGI(TAG, "Fetching remote configuration...");
        esp_err_t cfg_err = remote_config_fetch();
        if (cfg_err == ESP_OK) {
            ESP_LOGI(TAG, "Remote config applied successfully");
        } else {
            ESP_LOGW(TAG, "Remote config fetch failed (%s) — using defaults",
                     esp_err_to_name(cfg_err));
        }

        /* Check for firmware updates.
         *
         * We create the application tasks BEFORE calling ota_check_and_update()
         * so that their handles can be passed to ota_register_tasks().  The OTA
         * code suspends these tasks during the download to reduce RF contention,
         * then resumes them on failure (on success the device restarts).
         *
         * Tasks are created in a suspended state (via xTaskCreate followed by
         * vTaskSuspend) — they will not run until ota_check_and_update() returns
         * (either after a successful update+restart, or after all retry attempts
         * are exhausted).  We resume them explicitly below.
         *
         * NOTE: lux_based_birds_task is only created for full hardware, so its
         * handle may be NULL — ota_register_tasks() accepts NULL safely.
         */

        /* -- Pre-create tasks in suspended state for OTA registration -- */
        TaskHandle_t h_audio  = NULL;
        TaskHandle_t h_lux    = NULL;
        TaskHandle_t h_flock  = NULL;

        xTaskCreate(audio_detection_task, "audio_detection", 4096, NULL, 5, &h_audio);
        if (h_audio)  vTaskSuspend(h_audio);

        if (hw_config == HW_CONFIG_FULL) {
            xTaskCreate(lux_based_birds_task, "lux_birds", 4096, NULL, 4, &h_lux);
            if (h_lux)  vTaskSuspend(h_lux);
        }

        xTaskCreate(flock_task, "flock", 4096, NULL, 4, &h_flock);
        if (h_flock)  vTaskSuspend(h_flock);

        /* Register handles so OTA can suspend/resume them around the download */
        ota_register_tasks(h_flock, h_lux, h_audio);

        /* Confirm the running firmware is valid before attempting a new OTA update.
         *
         * If this image was installed via OTA it will be in ESP_OTA_IMG_PENDING_VERIFY.
         * esp_ota_begin() refuses to flash while the running partition is unconfirmed
         * (ESP_ERR_OTA_ROLLBACK_INVALID_STATE).
         *
         * We wait here for up to 2 minutes.  If the system stays alive that long it has
         * proven stability and we mark valid, then proceed to check for updates.
         * If the firmware crashes before 2 minutes the bootloader will automatically
         * roll back to the previous image on the next boot — which is the intended
         * safety behaviour.                                                          */
        {
            const esp_partition_t *running = esp_ota_get_running_partition();
            esp_ota_img_states_t img_state;
            if (esp_ota_get_state_partition(running, &img_state) == ESP_OK &&
                img_state == ESP_OTA_IMG_PENDING_VERIFY) {

                ESP_LOGI(TAG, "OTA validation: new firmware detected — waiting 2 min to confirm stability...");
                vTaskDelay(pdMS_TO_TICKS(120000));   /* 2 minutes */

                /* Re-check state in case something reset it during the wait */
                if (esp_ota_get_state_partition(running, &img_state) == ESP_OK &&
                    img_state == ESP_OTA_IMG_PENDING_VERIFY) {
                    ESP_LOGI(TAG, "System stable — marking firmware valid");
                    esp_ota_mark_app_valid_cancel_rollback();
                }
            }
        }

        /* Check for updates — retries internally up to OTA_MAX_ATTEMPTS times */
        ESP_LOGI(TAG, "Checking for firmware updates...");
        bool updated = ota_check_and_update();

        if (updated) {
            ESP_LOGI(TAG, "Update completed, device restarting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            const ota_state_t *ota_state = ota_get_state();
            if (ota_state->ota_status == OTA_STATUS_FAILED) {
                ESP_LOGW(TAG, "Update available but failed after all retries - continuing");
            } else {
                ESP_LOGI(TAG, "No update needed");
            }
        }

        /* Resume application tasks now that OTA is resolved */
        if (h_audio)  vTaskResume(h_audio);
        if (h_lux)    vTaskResume(h_lux);
        if (h_flock)  vTaskResume(h_flock);
        ESP_LOGI(TAG, "Application tasks resumed");

        /* Enable modem sleep NOW — after OTA — so the download is never
         * disrupted by the radio sleeping between DTIM beacons.           */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

        /* Start remote config polling task (60-second interval) */
        xTaskCreate(remote_config_task, "rcfg", 4096, NULL, 3, NULL);
        ESP_LOGI(TAG, "Remote config polling task started");

    } else {
        ESP_LOGI(TAG, "WiFi connection failed - continuing without OTA and startup report");
        
        /* Add error to report for logging purposes */
        startup_report.has_errors = true;
        snprintf(startup_report.error_message, sizeof(startup_report.error_message),
                 "WiFi connection failed");
        
        /* Flash blue LED to indicate no WiFi */
        for (int i = 0; i < 3; i++) {
            set_led(0, BRIGHT_FULL);
            vTaskDelay(pdMS_TO_TICKS(200));
            set_led(0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    
    /* Initialise ESP-NOW mesh.
     * ESP-NOW is peer-to-peer and runs directly on the WiFi radio layer —
     * it does NOT require a connection to an AP.  wifi_init_and_connect()
     * calls esp_wifi_start() unconditionally, so the radio is always up by
     * this point regardless of whether the AP connection succeeded.        */
    espnow_mesh_init(get_bird_mapper(), (markov_chain_t *)get_markov());

    /* Start application tasks — only when WiFi was NOT connected (when WiFi
     * IS connected the tasks were already created and resumed above).      */
    if (!wifi_connected) {
        ESP_LOGI(TAG, "Starting Echoes of the Machine (no-WiFi path)...");
        xTaskCreate(audio_detection_task, "audio_detection", 4096, NULL, 5, NULL);

        if (hw_config == HW_CONFIG_FULL) {
            xTaskCreate(lux_based_birds_task, "lux_birds", 4096, NULL, 4, NULL);
        }

        xTaskCreate(flock_task, "flock", 4096, NULL, 4, NULL);
        ESP_LOGI(TAG, "Chaos mode task started");
    } else {
        ESP_LOGI(TAG, "Echoes of the Machine running (tasks already started)");
    }
    
    ESP_LOGI(TAG, "System started successfully!");
    
    /* Log final startup summary */
    ESP_LOGI(TAG, "Startup Summary:");
    ESP_LOGI(TAG, "  MAC: %s", startup_report.mac_address);
    ESP_LOGI(TAG, "  Node Type: %s", startup_report.node_type);
    ESP_LOGI(TAG, "  Errors: %s", startup_report.has_errors ? "YES" : "NO");
    if (startup_report.has_errors) {
        ESP_LOGI(TAG, "  Error Message: %s", startup_report.error_message);
    }
    
    /* Indicate system ready with white LED pulse */
    set_led(BRIGHT_FULL, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    set_led(0, 0);
}
