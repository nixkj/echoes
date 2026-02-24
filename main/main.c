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
#include <math.h>

#include "echoes.h"
#include "synthesis.h"
#include "ota.h"
#include "startup.h"
#include "espnow_mesh.h"
#include "markov.h"
#include "remote_config.h"
#include "esp_task_wdt.h"

// ESP32-specific includes
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "esp_mac.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    led_init();   /* Default initialisation is full on */

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

    /* Stagger boot across the fleet using the last MAC byte as a seed.
     * Spreads 49 nodes over ~10 s, preventing the AP and server from being
     * hit by all nodes simultaneously on a mass power-on.                  */
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        uint32_t jitter_ms = ((uint32_t)mac[5] * 200) % 10000;
        ESP_LOGI(TAG, "Startup jitter: %lu ms (MAC tail: %02X)", jitter_ms, mac[5]);
        vTaskDelay(pdMS_TO_TICKS(jitter_ms));
    }

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

    /* Initialise the Task Watchdog Timer.
     * audio_detection_task subscribes itself and calls esp_task_wdt_reset()
     * each time i2s_channel_read() returns.  If the microphone peripheral
     * stalls for WDT_TIMEOUT_S seconds the TWDT fires a panic → reboot.
     * esp_task_wdt_reconfigure() is used first in case ESP-IDF auto-started
     * the TWDT at boot (CONFIG_ESP_TASK_WDT_INIT=y); on failure we call
     * esp_task_wdt_init() instead.                                         */
    {
        const esp_task_wdt_config_t wdt_cfg = {
            .timeout_ms     = WDT_TIMEOUT_S * 1000,
            .idle_core_mask = 0,     /* don't watch idle tasks */
            .trigger_panic  = true,  /* panic → reboot on timeout */
        };
        esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_cfg);
        if (wdt_err == ESP_ERR_INVALID_STATE) {
            wdt_err = esp_task_wdt_init(&wdt_cfg);
        }
        if (wdt_err == ESP_OK) {
            ESP_LOGI(TAG, "Task watchdog: %ds timeout, panic on trigger", WDT_TIMEOUT_S);
        } else {
            ESP_LOGW(TAG, "Could not configure task watchdog: %s", esp_err_to_name(wdt_err));
        }
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

        /* Stack note: audio_detection_task does floating-point DSP (Goertzel,
         * adaptive thresholds) and calls into synthesis from the same stack.
         * 4096 bytes is sufficient on Xtensa with the hardware FPU, but if
         * intermittent watchdog resets appear in the field this task's stack
         * should be the first thing to increase (try 8192).               */
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
         * We wait here for up to 1 minute.  If the system stays alive that long it has
         * proven stability and we mark valid, then proceed to check for updates.
         * If the firmware crashes before 1 minute the bootloader will automatically
         * roll back to the previous image on the next boot — which is the intended
         * safety behaviour.                                                          */
        {
            const esp_partition_t *running = esp_ota_get_running_partition();
            esp_ota_img_states_t img_state;
            if (esp_ota_get_state_partition(running, &img_state) == ESP_OK &&
                img_state == ESP_OTA_IMG_PENDING_VERIFY) {

                ESP_LOGI(TAG, "OTA validation: new firmware detected — waiting 1 min to confirm stability...");

                /* Slow blue pulse while waiting — signals "validating" without appearing dead. */
                const int pulse_ms     = 2000;   /* one full breath cycle */
                const int step_ms      = 50;
                const int total_steps  = 60000 / step_ms;   /* 1 minutes */

                for (int i = 0; i < total_steps; i++) {
                    float phase     = (float)(i % (pulse_ms / step_ms)) / (pulse_ms / step_ms);
                    float intensity = 0.3f * (0.5f + 0.5f * sinf(2.0f * M_PI * phase));
                    set_led(0.0f, intensity);
                    vTaskDelay(pdMS_TO_TICKS(step_ms));
                }
                set_led(0.0f, 0.0f);

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

        /* Initialise ESP-NOW before resuming tasks.
         *
         * flock_task attempts a broadcast within milliseconds of being
         * resumed.  If espnow_mesh_init() has not been called yet the
         * send fails with "esp now not init!" and the first light/sound
         * event is lost.  Initialising here guarantees the stack is ready
         * before any task can call it.                                    */
        espnow_mesh_init(get_bird_mapper(), (markov_chain_t *)get_markov());

        /* Indicate system ready with white LED pulse — fired before vTaskResume
         * so app_main has sole LED ownership; resumed tasks (priority 4–5)
         * would otherwise preempt before set_led(0,0) runs.               */
        set_led(BRIGHT_FULL, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        set_led(0, 0);

        /* Resume application tasks now that OTA is resolved */
        if (h_audio)  vTaskResume(h_audio);
        if (h_lux)    vTaskResume(h_lux);
        if (h_flock)  vTaskResume(h_flock);
        ESP_LOGI(TAG, "Application tasks resumed");

        /* Enable modem sleep NOW — after OTA — so the download is never
         * disrupted by the radio sleeping between DTIM beacons.           */
        //esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
	
	/* Keep Wi-Fi on */
        esp_wifi_set_ps(WIFI_PS_NONE);

        /* Start remote config polling task (60-second interval) */
        xTaskCreate(remote_config_task, "rcfg", 4096, NULL, 3, NULL);
        ESP_LOGI(TAG, "Remote config polling task started");

        /* OPTIONAL: periodic OTA polling task (disabled by default).
         *
         * The default workflow above checks for updates once at boot and is
         * the recommended approach for a gallery installation — it keeps the
         * boot sequence predictable and avoids mid-session interruptions.
         *
         * If you need the device to poll for updates while running (e.g. for
         * long-running deployments where rebooting every node is impractical),
         * uncomment the line below.  OTA_CHECK_INTERVAL_MS in ota.h controls
         * the poll interval (default: once per day).
         *
         * NOTE: if enabled, store the application task handles in static or
         * global variables (not the stack-local h_audio/h_lux/h_flock above,
         * which are out of scope by the time the periodic check fires) and
         * call ota_register_tasks() with them so ota_perform_update() can
         * suspend the audio/lux/flock tasks during the download.  Without
         * this the download will compete with I2S and ESP-NOW traffic.
         */
        // xTaskCreate(ota_task, "ota_poll", 4096, NULL, 2, NULL);

    } else {
        ESP_LOGI(TAG, "WiFi connection failed - continuing without OTA and startup report");

        /* Flash blue LED to indicate no WiFi */
        for (int i = 0; i < 3; i++) {
            set_led(0, BRIGHT_FULL);
            vTaskDelay(pdMS_TO_TICKS(200));
            set_led(0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    
    /* Start application tasks — only when WiFi was NOT connected (when WiFi
     * IS connected the tasks were already created and resumed above).      */
    if (!wifi_connected) {
        ESP_LOGI(TAG, "Starting Echoes of the Machine (no-WiFi path)...");

        /* Initialise ESP-NOW before creating tasks for the same reason as
         * the WiFi path above — flock_task will attempt a broadcast
         * immediately on first run.                                        */
        espnow_mesh_init(get_bird_mapper(), (markov_chain_t *)get_markov());

        /* Indicate system ready with white LED pulse — fired before xTaskCreate
         * so app_main has sole LED ownership; created tasks (priority 4–5)
         * would otherwise preempt before set_led(0,0) runs.               */
        set_led(BRIGHT_FULL, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        set_led(0, 0);

        /* Stack: see WiFi path above for sizing rationale. */
        xTaskCreate(audio_detection_task, "audio_detection", 4096, NULL, 5, NULL);

        if (hw_config == HW_CONFIG_FULL) {
            xTaskCreate(lux_based_birds_task, "lux_birds", 4096, NULL, 4, NULL);
        }

        xTaskCreate(flock_task, "flock", 4096, NULL, 4, NULL);
        ESP_LOGI(TAG, "Flock task started");
    } else {
        ESP_LOGI(TAG, "Echoes of the Machine running (tasks already started)");
    }

    ESP_LOGI(TAG, "System started successfully!");

    /* Log final startup summary */
    ESP_LOGI(TAG, "Startup Summary:");
    ESP_LOGI(TAG, "  MAC:      %s", startup_report.mac_address);
    ESP_LOGI(TAG, "  Type:     %s", startup_report.node_type);
    ESP_LOGI(TAG, "  WiFi:     %s", wifi_connected ? "connected" : "no connection — startup report not sent");
    if (wifi_connected) {
        ESP_LOGI(TAG, "  Errors:   %s", startup_report.has_errors ? startup_report.error_message : "none");
    }
}
