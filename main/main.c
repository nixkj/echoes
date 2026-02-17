/**
 * @file main.c
 * @brief Main application entry point with OTA support
 * 
 * Initializes WiFi, checks for firmware updates, then starts Echoes of the Machine
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"

#include "echoes.h"
#include "synthesis.h"
#include "ota.h"

// ESP32-specific includes
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
#include "esp_check.h"

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
    
    /* Initialize system hardware */
    ESP_LOGI(TAG, "Initializing system...");
    system_init();
    
    /* Initialize WiFi and connect */
    ESP_LOGI(TAG, "Connecting to WiFi...");
    if (wifi_init_and_connect()) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        
        /* Check for firmware updates */
        ESP_LOGI(TAG, "Checking for firmware updates...");
        bool updated = ota_check_and_update();
        
        if (updated) {
            /* Device will restart after update */
            ESP_LOGI(TAG, "Update completed, device restarting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            ESP_LOGI(TAG, "No update needed, continuing with normal operation");
        }
        
        /* Start periodic OTA check task (checks every 24 hours) */
        // xTaskCreate(ota_task, "ota_task", 8192, NULL, 3, NULL);
        
        /* Optional: Disconnect WiFi to save power if not needed for operation */
        /* Uncomment the line below if you want to disconnect after initial check */
        // wifi_disconnect();
	/* Alternative: */
	esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // Light sleep
        
    } else {
        ESP_LOGI(TAG, "WiFi connection failed - continuing without OTA");
        /* Flash blue LED to indicate no WiFi */
        for (int i = 0; i < 3; i++) {
            set_led(0, BRIGHT_FULL);
            vTaskDelay(pdMS_TO_TICKS(200));
            set_led(0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    // Initialize hardware
    ESP_LOGI(TAG, "Initializing audio hardware...");
    led_init();
    ESP_ERROR_CHECK(i2s_microphone_init());
    
    // Check hardware config (already detected in system_init)
    hardware_config_t hw_config = get_hardware_config();
    
    if (hw_config == HW_CONFIG_FULL) {
        ESP_LOGI(TAG, "Full hardware detected - initializing speaker");
        ESP_ERROR_CHECK(i2s_speaker_init());
    } else {
        ESP_LOGI(TAG, "Minimal hardware detected - speaker disabled, LED VU mode");
    }
    
    /* Start detection task */
    ESP_LOGI(TAG, "Starting Echoes of the Machine...");
    xTaskCreate(audio_detection_task, "audio_detection", 4096, NULL, 5, NULL);
    
    /* Start lux-based bird selection task - only for full hardware */
    if (hw_config == HW_CONFIG_FULL) {
        // xTaskCreate(lux_based_birds_task, "lux_birds", 4096, NULL, 4, NULL);
    }
    
    ESP_LOGI(TAG, "System started successfully!");
    
    /* Indicate system ready with white LED pulse */
    set_led(BRIGHT_FULL, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    set_led(0, 0);

    // Start task that will validate firmware after delay
    xTaskCreate(ota_validation_task, "ota_validate", 2048, NULL, 2, NULL);
}
