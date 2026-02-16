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

    // Initialise hardware
    led_init();
    ESP_ERROR_CHECK(i2s_microphone_init());
    ESP_ERROR_CHECK(i2s_speaker_init());
    
    /* Start detection task */
    ESP_LOGI(TAG, "Starting Echoes of the Machine...");
    xTaskCreate(audio_detection_task, "audio_detection", 4096, NULL, 5, NULL);
    
    /* Start lux-based bird selection task */
    //xTaskCreate(lux_based_birds_task, "lux_birds", 4096, NULL, 4, NULL);
    
    ESP_LOGI(TAG, "System started successfully!");
    
    /* Indicate system ready with white LED pulse */
    set_led(BRIGHT_FULL, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    set_led(0, 0);
}
