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

// ESP32-specific includes
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "nvs_flash.h"

static const char *TAG = "MAIN";

// Task for delayed firmware update validation
void ota_validation_task(void *param)
{
    // Wait for system to run successfully for 2 minutes
    ESP_LOGI(TAG, "OTA validation: Waiting 2 minutes before marking valid...");
    vTaskDelay(pdMS_TO_TICKS(120000));  // 2 minutes
    
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "System stable for 2 minutes - marking firmware as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    
    vTaskDelete(NULL);  // Task done
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Echoes of the Machine");
    ESP_LOGI(TAG, "Firmware Version: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "========================================");
    
    /* Initialise NVS flash (required by Markov chain persistence) */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue — erasing and reinitialising");
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Initialize LEDs FIRST (needed for ALL status feedback including OTA) */
    led_init();
    
    /* Initialize system hardware (including light sensor detection) */
    ESP_LOGI(TAG, "Initializing system...");
    system_init();

    /* Capture any errors on startup */
    startup_report_t startup_report;
    
    /* Initialize WiFi and connect */
    ESP_LOGI(TAG, "Connecting to WiFi...");
    bool wifi_connected = wifi_init_and_connect();
    
    if (wifi_connected) {
        ESP_LOGI(TAG, "WiFi connected successfully");

        /* Check for firmware updates */
        ESP_LOGI(TAG, "Checking for firmware updates...");
        bool updated = ota_check_and_update();
        
	if (updated) {
	    ESP_LOGI(TAG, "Update completed, device restarting...");
	    vTaskDelay(pdMS_TO_TICKS(1000));
	} else {
	    // Better message
	    const ota_state_t *ota_state = ota_get_state();
	    if (ota_state->ota_status == OTA_STATUS_FAILED) {
		ESP_LOGW(TAG, "Update available but failed - continuing");
	    } else {
		ESP_LOGI(TAG, "No update needed");
	    }
	}

        /* Start periodic OTA check task (checks every 24 hours) */
        // xTaskCreate(ota_task, "ota_task", 8192, NULL, 3, NULL);
        
        /* Optional: Set WiFi to light sleep mode to save power */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // Light sleep
        
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

    /* Small delay to ensure all hardware is stable before sampling */
    vTaskDelay(pdMS_TO_TICKS(500));

    // Hardware config already detected in system_init — fetch it now so
    // startup_sleep_and_sample can embed the correct node_type in the report.
    hardware_config_t hw_config = get_hardware_config();

    /* Perform startup sleep and light sensor sampling */
    ESP_LOGI(TAG, "Starting random sleep period with light sampling...");
    esp_err_t startup_err = startup_sleep_and_sample(&startup_report, hw_config);
    
    if (startup_err != ESP_OK) {
        ESP_LOGW(TAG, "Startup sampling had issues: %s", esp_err_to_name(startup_err));
        // Note: startup_report will already have error information filled in
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

    /* Send startup report */
    ESP_LOGI(TAG, "Sending startup report...");
    esp_err_t report_err = startup_send_report(&startup_report);
    
    if (report_err == ESP_OK) {
        ESP_LOGI(TAG, "Startup report sent successfully");
        // Brief white LED flash to indicate successful report
        set_led(BRIGHT_MID, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        set_led(0, 0);
    } else {
        ESP_LOGW(TAG, "Failed to send startup report: %s", esp_err_to_name(report_err));
        // Brief blue LED flash to indicate report failure
        set_led(0, BRIGHT_MID);
        vTaskDelay(pdMS_TO_TICKS(200));
        set_led(0, 0);
    }
    
    /* Initialise ESP-NOW mesh — WiFi must already be up */
    if (wifi_connected) {
        /* g_bird_mapper is an internal symbol in echoes.c; we expose it via
         * a getter so we can pass it to the mesh layer. */
        espnow_mesh_init(get_bird_mapper(), (markov_chain_t *)get_markov());
    }

    /* Start detection task */
    ESP_LOGI(TAG, "Starting Echoes of the Machine...");
    xTaskCreate(audio_detection_task, "audio_detection", 4096, NULL, 5, NULL);
    
    /* Start lux-based bird selection task - only for full hardware */
    if (hw_config == HW_CONFIG_FULL) {
        xTaskCreate(lux_based_birds_task, "lux_birds", 4096, NULL, 4, NULL);
    }
    
    ESP_LOGI(TAG, "System started successfully!");
    
    /* Log final startup summary */
    ESP_LOGI(TAG, "Startup Summary:");
    ESP_LOGI(TAG, "  MAC: %s", startup_report.mac_address);
    ESP_LOGI(TAG, "  Node Type: %s", startup_report.node_type);
    ESP_LOGI(TAG, "  Sleep Duration: %lu ms", startup_report.sleep_duration_ms);
    ESP_LOGI(TAG, "  Avg Light Level: %.2f lux (%lu samples)",
             startup_report.avg_light_level, startup_report.light_samples);
    ESP_LOGI(TAG, "  Errors: %s", startup_report.has_errors ? "YES" : "NO");
    if (startup_report.has_errors) {
        ESP_LOGI(TAG, "  Error Message: %s", startup_report.error_message);
    }
    
    /* Indicate system ready with white LED pulse */
    set_led(BRIGHT_FULL, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    set_led(0, 0);

    // Start task that will validate firmware after delay
    xTaskCreate(ota_validation_task, "ota_validate", 2048, NULL, 2, NULL);
}
