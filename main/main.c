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
#include "esp_random.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "driver/gptimer.h"

static const char *TAG = "MAIN";

/* ========================================================================
 * ISR-BASED HARDWARE WATCHDOG
 * ========================================================================
 *
 * The Task Watchdog Timer (TWDT) monitors whether FreeRTOS tasks call
 * esp_task_wdt_reset() within a timeout.  It CANNOT detect the failure
 * mode that kills minimal nodes:
 *
 *   - All tasks continue running and feeding the TWDT.
 *   - The WiFi driver is wedged (AP dropped the node silently).
 *   - esp_now_send() blocks the keepalive task on the full TX buffer.
 *   - The node is alive internally but network-dead.
 *
 * The TWDT sees a perfectly healthy system and never fires.
 *
 * This ISR-based watchdog fills the gap.  A hardware GP Timer fires a
 * level-1 interrupt every second.  The ISR checks a volatile heartbeat
 * counter that is incremented by the wifi_keepalive_task on every
 * non-blocked iteration.  If the counter stops advancing for
 * ISR_WDT_TIMEOUT_S seconds, the ISR calls esp_restart().
 *
 * Because this runs from a hardware timer interrupt (not a FreeRTOS
 * task), it fires even if:
 *   - All tasks are deadlocked
 *   - esp_now_send() is blocking a task
 *   - The FreeRTOS scheduler is stuck
 *
 * The only failure modes this cannot catch are:
 *   - Global interrupt masking (caught by the Interrupt WDT / IWDT)
 *   - Hardware clock failure (caught by the RTC WDT)
 */

#define ISR_WDT_TIMEOUT_S  90   /* seconds without heartbeat → reboot */

static gptimer_handle_t  s_isr_wdt_timer     = NULL;
volatile uint32_t        s_isr_wdt_heartbeat  = 0;   /* fed by tasks, checked by ISR */

/**
 * @brief GP Timer alarm callback — runs in ISR context every 1 second.
 *
 * Compares the heartbeat counter against its previous value.  If unchanged
 * for ISR_WDT_TIMEOUT_S consecutive seconds, forces a hard restart.
 * IRAM_ATTR ensures the function is in instruction RAM (required for ISR).
 */
static bool IRAM_ATTR isr_wdt_alarm_cb(gptimer_handle_t timer,
                                        const gptimer_alarm_event_data_t *edata,
                                        void *user_ctx)
{
    static uint32_t s_last_hb    = 0;
    static uint32_t s_miss_count = 0;

    uint32_t hb = s_isr_wdt_heartbeat;
    if (hb == s_last_hb) {
        s_miss_count++;
        if (s_miss_count >= ISR_WDT_TIMEOUT_S) {
            esp_restart();
            /* Does not return */
        }
    } else {
        s_last_hb    = hb;
        s_miss_count = 0;
    }
    return false;   /* no high-priority task wakeup needed */
}

/**
 * @brief Initialise the ISR-based hardware watchdog.
 *
 * Call once from app_main after all tasks are running.  The timer fires
 * a 1-second alarm in ISR context.  Any task can feed the watchdog by
 * incrementing s_isr_wdt_heartbeat.
 */
static void isr_wdt_init(void)
{
    gptimer_config_t timer_cfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,   /* 1 MHz → 1 µs per tick */
    };
    esp_err_t err = gptimer_new_timer(&timer_cfg, &s_isr_wdt_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ISR WDT: failed to create timer: %s", esp_err_to_name(err));
        return;
    }

    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count               = 1000000,   /* 1 second (1 MHz clock) */
        .reload_count              = 0,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_event_callbacks_t cbs = {
        .on_alarm = isr_wdt_alarm_cb,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_isr_wdt_timer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s_isr_wdt_timer, &alarm_cfg));
    ESP_ERROR_CHECK(gptimer_enable(s_isr_wdt_timer));
    ESP_ERROR_CHECK(gptimer_start(s_isr_wdt_timer));

    ESP_LOGI(TAG, "ISR WDT: armed (%d s timeout)", ISR_WDT_TIMEOUT_S);
}

/* ========================================================================
 * WIFI KEEPALIVE (minimal nodes only)
 * ========================================================================
 *
 * ROOT CAUSE: Minimal nodes receive ~50 ESP-NOW frames/s from full nodes
 * but rarely transmit.  The MikroTik AP's null-frame probe goes un-ACKed,
 * the AP silently removes the client, and WIFI_EVENT_STA_DISCONNECTED
 * never fires — the node vanishes with no software-visible event.
 *
 * KEEPALIVE: a 1-byte UDP datagram to the gateway's discard port (9).
 * This is a unicast 802.11 DATA frame that:
 *   (a) keeps the AP's per-client inactivity timer alive, AND
 *   (b) exercises the radio's CSMA/CA transmit path (carrier sense →
 *       backoff → transmit → wait for MAC ACK from the AP).
 * The socket has SO_SNDTIMEO = 1 s, so sendto() can NEVER block
 * indefinitely — unlike esp_now_send() which has no timeout and can
 * block forever when the WiFi TX buffer is full.
 *
 * PHANTOM DETECTION: esp_wifi_sta_get_ap_info() returns the actual
 * association state from the hardware.  When wifi_is_connected() is
 * TRUE (driver flag) but get_ap_info() fails (not actually associated),
 * the AP dropped us silently.  We trigger reconnection.
 *
 * RECOVERY ESCALATION (3 levels):
 *   1. Phantom detected → esp_wifi_disconnect() (normal reconnect)
 *   2. After PHANTOM_ESCALATION_COUNT failures → esp_wifi_stop/start
 *   3. After ISR_WDT_TIMEOUT_S without heartbeat → ISR WDT reboots
 *
 * The ISR-based hardware watchdog (see above) monitors a heartbeat
 * counter that this task increments each iteration.  If the task is
 * blocked (e.g. by a previous bug where esp_now_send blocked), the
 * ISR fires and reboots the node.
 *
 * IMPORTANT: this task does NOT call esp_now_send().  The original
 * heartbeat broadcast used esp_now_send() which calls
 * esp_wifi_internal_tx().  When the AP has silently dropped the node,
 * the WiFi TX buffer fills because frames can't be transmitted, and
 * esp_now_send() blocks indefinitely waiting for buffer space — there
 * is no timeout parameter.  This blocked the keepalive task entirely,
 * preventing phantom detection and the hard restart timer from running.
 * The UDP sendto() with SO_SNDTIMEO is strictly superior: it exercises
 * the same radio transmit path, generates a unicast frame (which the
 * AP ACKs at the MAC layer — broadcast is unacknowledged), and has a
 * guaranteed 1-second timeout.
 */
#define WIFI_KEEPALIVE_INTERVAL_MS  1000

/* Minimum gap (ms) between phantom-guard disconnects. */
#define KEEPALIVE_PHANTOM_SUPPRESS_MS  6000

/* How often (loop iterations) we run esp_wifi_sta_get_ap_info(). */
#define PHANTOM_CHECK_INTERVAL  10

/* After this many phantom→disconnect cycles without recovery, escalate
 * to a full Wi-Fi stack restart (stop+start). */
#define PHANTOM_ESCALATION_COUNT  5

/* Persistent UDP socket. */
static int s_ka_sock = -1;

/* Last phantom-guard disconnect timestamp. */
static uint32_t s_phantom_disconnect_ms = 0;

static void wifi_keepalive_task(void *param)
{
    (void)param;

    /* Subscribe to the Task Watchdog. */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "keepalive: could not subscribe to watchdog: %s",
                 esp_err_to_name(wdt_err));
    }

    uint8_t phantom_check_counter = 0;
    uint8_t phantom_failures      = 0;

    while (1) {
        esp_task_wdt_reset();

        /* ── Feed the ISR hardware watchdog ──────────────────────────────
         *
         * This is the FIRST thing in the loop body — before any operation
         * that could conceivably block.  As long as we reach this point
         * every second, the ISR watchdog stays happy.  If anything below
         * blocks (which should no longer be possible after removing
         * esp_now_send), the ISR WDT fires after ISR_WDT_TIMEOUT_S. */
        s_isr_wdt_heartbeat++;

        vTaskDelay(pdMS_TO_TICKS(WIFI_KEEPALIVE_INTERVAL_MS));

        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        /* ── Skip all Wi-Fi work when disconnected ───────────────────────
         *
         * When wifi_is_connected() is FALSE, the ota.c event handler owns
         * the reconnect.  Calling any Wi-Fi API here during the association
         * handshake can wedge the driver's internal state machine.         */
        if (!wifi_is_connected()) {
            continue;
        }

        /* ── Below here: driver thinks we are connected ──────────────── */

        /* ── Phantom-connection guard (throttled) ────────────────────────
         *
         * Check every PHANTOM_CHECK_INTERVAL iterations whether the AP
         * still knows about us.  wifi_is_connected() is TRUE but the AP
         * may have silently removed our association.                       */
        if (++phantom_check_counter >= PHANTOM_CHECK_INTERVAL) {
            phantom_check_counter = 0;

            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
                /* Phantom: driver says connected, hardware says not. */
                if ((now_ms - s_phantom_disconnect_ms) >= KEEPALIVE_PHANTOM_SUPPRESS_MS) {
                    phantom_failures++;

                    if (phantom_failures >= PHANTOM_ESCALATION_COUNT) {
                        ESP_LOGW(TAG, "Keepalive: %d phantom cycles — "
                                      "restarting Wi-Fi stack",
                                 phantom_failures);
                        s_phantom_disconnect_ms = now_ms;
                        if (s_ka_sock >= 0) {
                            close(s_ka_sock);
                            s_ka_sock = -1;
                        }
                        esp_wifi_disconnect();
                        esp_wifi_stop();
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        esp_wifi_start();   /* fires STA_START → connect */
                        phantom_failures = 0;
                    } else {
                        ESP_LOGW(TAG, "Keepalive: phantom [%d/%d] — "
                                      "triggering reconnect",
                                 phantom_failures, PHANTOM_ESCALATION_COUNT);
                        s_phantom_disconnect_ms = now_ms;
                        esp_wifi_disconnect();
                    }
                }
                continue;   /* skip UDP probe this cycle */
            } else {
                /* AP confirmed — reset failure counter. */
                if (phantom_failures > 0) {
                    ESP_LOGI(TAG, "Keepalive: AP confirmed — "
                                  "clearing %d phantom failure(s)",
                             phantom_failures);
                }
                phantom_failures = 0;
            }
        }

        /* ── UDP discard keepalive ───────────────────────────────────────
         *
         * 1-byte datagram to the gateway's discard port (RFC 863).
         * SO_SNDTIMEO = 1 s guarantees this NEVER blocks indefinitely. */
        esp_netif_t *netif = esp_netif_get_default_netif();
        if (netif == NULL) continue;

        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
            ip_info.gw.addr == 0) {
            ESP_LOGD(TAG, "Keepalive: no gateway IP, skipping UDP probe");
            continue;
        }

        if (s_ka_sock < 0) {
            s_ka_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s_ka_sock < 0) {
                ESP_LOGW(TAG, "Keepalive: socket() failed (%d)", errno);
                continue;
            }
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            setsockopt(s_ka_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }

        struct sockaddr_in dest = {
            .sin_family      = AF_INET,
            .sin_port        = htons(9),
            .sin_addr.s_addr = ip_info.gw.addr,
        };

        uint8_t buf = 0;
        int ret = sendto(s_ka_sock, &buf, sizeof(buf), 0,
                         (struct sockaddr *)&dest, sizeof(dest));

        if (ret < 0) {
            ESP_LOGW(TAG, "Keepalive: sendto failed (errno %d)", errno);
            close(s_ka_sock);
            s_ka_sock = -1;
        } else {
            ESP_LOGD(TAG, "Keepalive: UDP probe sent to gw " IPSTR,
                     IP2STR(&ip_info.gw));
        }
    }
}


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

    /* Stagger boot across the fleet using a hardware RNG value.
     *
     * The previous approach used mac[5] as a seed, which caused collision
     * when nodes from the same manufacturing batch share sequential MACs
     * (identical mac[5] bytes → same jitter → simultaneous AP association).
     * esp_random() draws from the hardware RNG, giving a genuinely uniform
     * distribution across all 50 nodes regardless of MAC assignment.
     *
     * Range: 0 – 9 999 ms  (~10 s window for 50 nodes).                   */
    {
        uint32_t jitter_ms = esp_random() % 10000;
        ESP_LOGI(TAG, "Startup jitter: %lu ms (hardware RNG)", jitter_ms);
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

    /* Flag reconnect-timer failure in the startup report so the server can
     * alert on nodes that will not auto-recover from mid-session WiFi drops. */
    if (!wifi_reconnect_timer_ok()) {
        startup_report.has_errors = true;
        snprintf(startup_report.error_message,
                 sizeof(startup_report.error_message),
                 "WiFi reconnect timer alloc failed — no mid-session recovery");
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

        /* Suspend the scheduler while creating tasks so that a newly created
         * higher-priority task (audio=5, lux/flock=4 vs app_main=1) cannot
         * preempt between xTaskCreate and vTaskSuspend.  Without this guard
         * the audio task runs immediately on creation, hits mic_chan==NULL,
         * and spams I2S errors until OTA suspends it.
         * vTaskSuspend() is safe to call with the scheduler suspended —
         * it marks the TCB as suspended without triggering a context switch.
         * When xTaskResumeAll() returns all three tasks are already in the
         * suspended state so the scheduler will not switch to any of them.  */
        vTaskSuspendAll();

        xTaskCreate(audio_detection_task, "audio_detection", 4096, NULL, 5, &h_audio);
        if (h_audio)  vTaskSuspend(h_audio);

        if (hw_config == HW_CONFIG_FULL) {
            xTaskCreate(lux_based_birds_task, "lux_birds", 4096, NULL, 4, &h_lux);
            if (h_lux)  vTaskSuspend(h_lux);
        }

        xTaskCreate(flock_task, "flock", 4096, NULL, 4, &h_flock);
        if (h_flock)  vTaskSuspend(h_flock);

        /* Demo task: idles (polls remote config) until DEMO_MODE is enabled.
         * Priority 3 — below audio (5) and lux/flock (4) so it never starves
         * detection.  Not registered with OTA: it sleeps in 500 ms chunks,
         * holds no I2S or DMA resources, and will self-suspend on the next
         * remote_config_get() call which returns demo_mode=false during OTA. */
        xTaskCreate(demo_task, "demo", 4096, NULL, 3, NULL);

        xTaskResumeAll();

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
         * before any task can call it. No Markov for minimal nodes.         */
	espnow_mesh_init(get_bird_mapper(),
                 (get_hardware_config() == HW_CONFIG_FULL) ? (markov_chain_t *)get_markov() : NULL);

        /* Initialise audio hardware HERE — as late as possible, immediately
         * before the tasks that consume it are resumed.  Enabling the I2S DMA
         * early (e.g. before OTA/remote-config) causes the RX ring buffer to
         * overflow for 60–120 s, leaving the driver in a state where the first
         * i2s_channel_read(portMAX_DELAY) never unblocks → white LED stuck. */
        ESP_LOGI(TAG, "Initializing audio hardware...");
        ESP_ERROR_CHECK(i2s_microphone_init());
        if (hw_config == HW_CONFIG_FULL) {
            ESP_LOGI(TAG, "Full hardware — initializing speaker");
            ESP_ERROR_CHECK(i2s_speaker_init());
        } else {
            ESP_LOGI(TAG, "Minimal hardware — LED VU mode");
        }
        /* 100 ms: ICS-43434 startup time after channel enable before first read. */
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Indicate system ready with white LED pulse — fired before vTaskResume
         * so app_main has sole LED ownership; resumed tasks (priority 4–5)
         * would otherwise preempt before set_led(0,0) runs.               */
        set_led(BRIGHT_FULL, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        set_led(0, 0);

        /* Resume application tasks now that OTA is resolved and all hardware
         * is ready.  ota_resume_tasks() is always called here, not inside
         * ota_perform_update(), so tasks never start before espnow_mesh_init()
         * and i2s_microphone_init() have completed above.                  */
        ota_resume_tasks();
        ESP_LOGI(TAG, "Application tasks resumed");

        /* esp_wifi_set_ps(WIFI_PS_NONE) is now called in the IP_EVENT_STA_GOT_IP
         * event handler (ota.c) so it applies on every (re)connect, including
         * during the OTA validation window.  No call needed here.           */

        /* Start remote config polling task (60-second interval) */
        xTaskCreate(remote_config_task, "rcfg", 4096, NULL, 3, NULL);
        ESP_LOGI(TAG, "Remote config polling task started");

        /* Keepalive task: minimal nodes only.
         *
         * Full nodes generate outbound 802.11 frames every ~500 ms via the
         * lux broadcast, so their AP inactivity timer never expires.
         * Minimal nodes have no such traffic; without this task they will
         * silently disappear from the AP's wireless registration table.
         *
         * This task also owns phantom detection and recovery escalation.
         * See wifi_keepalive_task() for the full explanation.
         *
         * IMPORTANT: this task does NOT use esp_now_send() for keepalive.
         * esp_now_send() can block indefinitely when the WiFi TX buffer is
         * full (no timeout parameter), which previously froze the entire
         * task.  The UDP probe (sendto with SO_SNDTIMEO=1s) is used
         * instead — it exercises the same radio transmit path and has a
         * guaranteed timeout.                                              */
        if (hw_config == HW_CONFIG_MINIMAL) {
            xTaskCreate(wifi_keepalive_task, "wifi_ka", 4096, NULL, 4, NULL); // was 2
            ESP_LOGI(TAG, "WiFi keepalive task started (%d s interval)",
                     WIFI_KEEPALIVE_INTERVAL_MS / 1000);
        }

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
         * immediately on first run. No Markov on minimal nodes.            */
	espnow_mesh_init(get_bird_mapper(),
                 (get_hardware_config() == HW_CONFIG_FULL) ? (markov_chain_t *)get_markov() : NULL);

        /* Initialise audio hardware HERE — same reasoning as WiFi path above.
         * No OTA delay on this path, but keeping init here ensures both paths
         * are consistent and the DMA never sits idle before its consumer. */
        ESP_LOGI(TAG, "Initializing audio hardware...");
        ESP_ERROR_CHECK(i2s_microphone_init());
        if (hw_config == HW_CONFIG_FULL) {
            ESP_LOGI(TAG, "Full hardware — initializing speaker");
            ESP_ERROR_CHECK(i2s_speaker_init());
        } else {
            ESP_LOGI(TAG, "Minimal hardware — LED VU mode");
        }
        /* 100 ms: ICS-43434 startup time after channel enable before first read. */
        vTaskDelay(pdMS_TO_TICKS(100));

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

        xTaskCreate(demo_task, "demo", 4096, NULL, 3, NULL);
        ESP_LOGI(TAG, "Demo task started");
    } else {
        ESP_LOGI(TAG, "Echoes of the Machine running (tasks already started)");
    }

    ESP_LOGI(TAG, "System started successfully!");

    /* ── ISR-based hardware watchdog ─────────────────────────────────────
     *
     * Arm the GP Timer ISR watchdog AFTER all tasks have been created
     * and started.  The keepalive task (minimal nodes only) feeds the
     * heartbeat counter on every iteration.  If it stops (e.g. because
     * a WiFi API call blocks indefinitely), the ISR fires after
     * ISR_WDT_TIMEOUT_S and reboots the node.
     *
     * On full nodes (no keepalive task), the audio_detection_task feeds
     * the ISR watchdog via the same counter — added below.
     *
     * This catches failure modes invisible to the Task WDT:
     *   - esp_now_send() blocking on a full TX buffer
     *   - WiFi driver internal deadlock
     *   - FreeRTOS scheduler hang                                         */
    if (hw_config == HW_CONFIG_MINIMAL) {
        isr_wdt_init();
    }

    /* Log final startup summary */
    ESP_LOGI(TAG, "Startup Summary:");
    ESP_LOGI(TAG, "  MAC:      %s", startup_report.mac_address);
    ESP_LOGI(TAG, "  Type:     %s", startup_report.node_type);
    ESP_LOGI(TAG, "  WiFi:     %s", wifi_connected ? "connected" : "no connection — startup report not sent");
    if (wifi_connected) {
        ESP_LOGI(TAG, "  Errors:   %s", startup_report.has_errors ? startup_report.error_message : "none");
    }
}
