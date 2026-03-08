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
#include "soc/rtc_cntl_reg.h"

static const char *TAG = "MAIN";

/* ========================================================================
 * ISR-BASED HARDWARE WATCHDOG (minimal nodes only)
 * ========================================================================
 *
 * A GP Timer fires a level-1 interrupt every second.  The ISR checks a
 * heartbeat counter that ONLY the wifi_keepalive_task increments.  If
 * the counter stops advancing for ISR_WDT_TIMEOUT_S seconds, the ISR
 * triggers an immediate hardware reset via the RTC control register.
 *
 * Why only the keepalive task feeds it:
 *   The purpose is to detect the specific failure where the keepalive
 *   task is blocked (e.g. by esp_now_send or a WiFi driver deadlock)
 *   while other tasks (audio, flock) continue running normally.  If
 *   multiple tasks feed the counter, one healthy task masks the stall
 *   of another — exactly the bug in the previous revision.
 *
 * Why RTC register write instead of esp_restart():
 *   esp_restart() calls esp_ipc_call_blocking() for cross-core sync.
 *   If the other CPU is stuck (common in WiFi driver deadlocks), the
 *   IPC never completes and the ISR hangs forever.  Writing directly
 *   to RTC_CNTL_OPTIONS0_REG triggers an immediate SoC-level reset
 *   that requires no software cooperation from either CPU.
 */

#define ISR_WDT_TIMEOUT_S  120  /* seconds without heartbeat → hardware reset */

/* Network health thresholds for the ISR WDT feeder logic.
 *
 * The ISR WDT heartbeat is only incremented when the node has proof of
 * end-to-end connectivity.  The ONLY such proof is a successful HTTP
 * config fetch (remote_config_task, every 60 s).  All local WiFi checks
 * (wifi_is_connected, esp_wifi_sta_get_ap_info, sendto) return success
 * when the AP has silently dropped the node.
 *
 * NETWORK_GRACE_MS: after boot, always feed the ISR WDT regardless of
 *   fetch status.  Covers OTA validation (60 s), random jitter (45 s),
 *   first poll interval (60 s), and HTTP timeout (8 s).
 *
 * NETWORK_DEATH_MS: after grace, if no successful fetch for this long,
 *   stop feeding the ISR WDT.  The ISR fires ISR_WDT_TIMEOUT_S later.
 *
 * Total worst-case recovery: NETWORK_DEATH_MS + ISR_WDT_TIMEOUT_S
 *   = 4 min + 2 min = 6 minutes from AP drop to hardware reset.          */
#define NETWORK_GRACE_MS   ( 2u * 60u * 1000u)   /*  2 min boot grace     */
#define NETWORK_DEATH_MS   ( 4u * 60u * 1000u)   /* 4 min stale → stop    */

static gptimer_handle_t          s_isr_wdt_timer     = NULL;
static volatile uint32_t         s_isr_wdt_heartbeat  = 0;   /* fed by tasks, checked by ISR */

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
            /* Write diagnostic state before resetting.  We are in ISR context
             * so heap size and RSSI cannot be safely read — pass 0 for both.
             * startup_write_rtc_diag() is IRAM_ATTR (pure memory writes).   */
            startup_write_rtc_diag(
                RTC_DIAG_CAUSE_ISR_WDT,
                0,   /* consecutive_failures not accessible from ISR */
                0,   /* heap not safely readable from ISR             */
                0,   /* RSSI not safely readable from ISR             */
                (uint32_t)(xTaskGetTickCountFromISR() *
                           portTICK_PERIOD_MS / 1000));
            /* Direct RTC hardware reset — no software cooperation needed.
             * Safe from ISR context, safe with both CPUs stuck.           */
            SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
            while (1) { }   /* never reached */
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

    ESP_LOGI(TAG, "ISR WDT: armed (%d s timeout, RTC reset)", ISR_WDT_TIMEOUT_S);
}

/* ========================================================================
 * WIFI KEEPALIVE (minimal nodes only)
 * ========================================================================
 *
 * This task is the SOLE feeder of the ISR hardware watchdog.  If this
 * task blocks for any reason, the ISR WDT fires after ISR_WDT_TIMEOUT_S
 * and triggers a hardware reset.  No other task feeds the ISR WDT.
 *
 * KEEPALIVE MECHANISM: a 1-byte UDP datagram to the gateway's discard
 * port (RFC 863), sent via a socket with SO_SNDTIMEO = 1 s.  This
 * guarantees sendto() NEVER blocks indefinitely.
 *
 * esp_now_send() is NOT used.  It calls esp_wifi_internal_tx() which
 * blocks indefinitely when the WiFi TX buffer is full (no timeout).
 * When the AP silently drops the node, the TX buffer fills because
 * frames can't be transmitted, and esp_now_send() blocks forever —
 * freezing the keepalive task and all its recovery mechanisms.
 *
 * PHANTOM DETECTION is NOT relied upon for recovery.  When the AP drops
 * a node silently, esp_wifi_sta_get_ap_info() returns ESP_OK from
 * cached driver state — it does not probe the AP.  wifi_is_connected()
 * also returns TRUE.  sendto() also returns success because lwIP
 * accepts the datagram regardless of whether the radio transmits it.
 * Every local check sees a healthy system.
 *
 * APPLICATION-LAYER VERIFICATION: the remote_config_task polls the
 * server every 60 seconds via HTTP.  This is the ONLY operation that
 * proves end-to-end bidirectional connectivity.  After 3 consecutive
 * failures (~3 min), it forces a WiFi stop/start.  After 5 failures
 * (~5 min), it forces a full reboot.  See remote_config.c.
 */
#define WIFI_KEEPALIVE_INTERVAL_MS  1000

/* Persistent UDP socket. */
static int s_ka_sock = -1;

static void wifi_keepalive_task(void *param)
{
    (void)param;

    /* Subscribe to the Task Watchdog. */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "keepalive: could not subscribe to TWDT: %s",
                 esp_err_to_name(wdt_err));
    }

    while (1) {
        esp_task_wdt_reset();

        /* ── Conditional ISR WDT feed ────────────────────────────────────
         *
         * Feed the hardware watchdog ONLY when we have proof the network
         * is alive.  The remote_config_task's last successful HTTP fetch
         * timestamp (last_fetch_ms) is the sole indicator.
         *
         * During the boot grace period, always feed — the first config
         * fetch hasn't happened yet and that's expected.
         *
         * After grace: if last_fetch_ms is stale (older than
         * NETWORK_DEATH_MS), STOP feeding.  The ISR fires
         * ISR_WDT_TIMEOUT_S later and triggers a hardware reset.          */
        {
            uint32_t now_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            bool boot_grace  = (now_ms < NETWORK_GRACE_MS);

            if (boot_grace) {
                s_isr_wdt_heartbeat++;
            } else {
                uint32_t last_ok = remote_config_get()->last_fetch_ms;
                if (last_ok != 0 && (now_ms - last_ok) < NETWORK_DEATH_MS) {
                    s_isr_wdt_heartbeat++;
                }
                /* else: network presumed dead — starve the ISR WDT */
            }
        }

        /* ── HTTP stuck detector ─────────────────────────────────────────
         *
         * esp_http_client's timeout_ms sets SO_RCVTIMEO / SO_SNDTIMEO on
         * the underlying socket, but does NOT apply to the TCP connect()
         * phase.  When the AP silently drops this node, the WiFi driver
         * still considers itself associated and keeps retransmitting SYN
         * frames.  lwIP's exponential-backoff retransmit timer can keep
         * connect() alive for many minutes — far beyond REMOTE_CONFIG_
         * HTTP_TIMEOUT_MS — permanently blocking remote_config_task.
         *
         * A blocked remote_config_task never increments consecutive_
         * failures and never reaches RCFG_FAILURES_HARD_REBOOT.  The ISR
         * WDT should still fire (keepalive_task is independent), but as a
         * belt-and-suspenders measure we also detect the stuck HTTP call
         * here and trigger a direct hardware reset.
         *
         * g_rcfg_http_attempt_start_ms is set just before
         * esp_http_client_perform() and cleared on return.  If it has
         * been non-zero for longer than HTTP_STUCK_TIMEOUT_MS we know
         * connect() is hung and will never time out on its own.           */
        {
            uint32_t attempt_start = g_rcfg_http_attempt_start_ms;
            if (attempt_start != 0) {
                uint32_t now_ms     = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                uint32_t attempt_ms = now_ms - attempt_start;
                if (attempt_ms > HTTP_STUCK_TIMEOUT_MS) {
                    /* Read diagnostics from task context (safe here). */
                    int8_t rssi = 0;
                    wifi_ap_record_t ap;
                    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                        rssi = ap.rssi;
                    }
                    ESP_LOGE(TAG,
                             "HTTP stuck for %lums — AP silently dropped node."
                             "  Triggering hardware reset.",
                             (unsigned long)attempt_ms);
                    startup_write_rtc_diag(
                        RTC_DIAG_CAUSE_HTTP_STUCK,
                        0,                          /* consecutive_failures N/A */
                        (uint32_t)esp_get_free_heap_size(),
                        (int32_t)rssi,
                        (uint32_t)(now_ms / 1000));
                    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
                    while (1) { }   /* never reached */
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_KEEPALIVE_INTERVAL_MS));

        /* Skip all WiFi work when the driver knows it's disconnected.
         * The ota.c event handler owns reconnection in that case.         */
        if (!wifi_is_connected()) {
            continue;
        }

        /* ── UDP keepalive probe ─────────────────────────────────────────
         *
         * 1-byte datagram to the gateway's discard port.  The frame
         * exercises the radio TX path and keeps the AP's inactivity
         * timer alive.  SO_SNDTIMEO = 1 s guarantees no infinite block.
         *
         * We do NOT check the return value to trigger reconnection:
         * sendto() returns success even when the radio can't transmit
         * (lwIP queues the packet locally).  Application-layer
         * verification is handled by remote_config_task instead.          */
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
            if (s_ka_sock < 0) continue;
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
            close(s_ka_sock);
            s_ka_sock = -1;
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
        esp_err_t cfg_err = remote_config_fetch(0);
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

//        if (hw_config == HW_CONFIG_FULL) {
            xTaskCreate(lux_based_birds_task, "lux_birds", 4096, NULL, 4, &h_lux);
//            if (h_lux)  vTaskSuspend(h_lux);
//        }

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
         * We wait here for up to 30 seconds.  If the system stays alive that long it has
         * proven stability and we mark valid, then proceed to check for updates.
         * If the firmware crashes before 30 seconds the bootloader will automatically
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
                const int total_steps  = 30000 / step_ms;   /* 30 seconds */

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

//        if (hw_config == HW_CONFIG_FULL) {
            xTaskCreate(lux_based_birds_task, "lux_birds", 4096, NULL, 4, NULL);
//        }

        xTaskCreate(flock_task, "flock", 4096, NULL, 4, NULL);
        ESP_LOGI(TAG, "Flock task started");

        xTaskCreate(demo_task, "demo", 4096, NULL, 3, NULL);
        ESP_LOGI(TAG, "Demo task started");
    } else {
        ESP_LOGI(TAG, "Echoes of the Machine running (tasks already started)");
    }

    ESP_LOGI(TAG, "System started successfully!");

    /* Remote config + keepalive tasks: started unconditionally so they run
     * even when the initial WiFi connection failed.
     *
     * If these were only created on the wifi_connected path then a failed
     * initial connection followed by isr_wdt_init() below would arm the ISR
     * WDT with no task ever feeding it → permanent 120 s reset loop.
     *
     * The WiFi reconnect timer in ota.c retries every 30 s.  Once connectivity
     * is established, remote_config_task will succeed on its next 60 s poll and
     * keepalive_task will start feeding the ISR WDT heartbeat.  The 10-minute
     * boot grace period in wifi_keepalive_task gives ample time for this.    */
    xTaskCreate(remote_config_task, "rcfg", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "Remote config polling task started");

    /* wifi_keepalive_task is the SOLE feeder of the ISR hardware watchdog.
     * If it stalls, the ISR fires and resets the SoC.
     * See wifi_keepalive_task() for the full explanation.                   */
    if (hw_config == HW_CONFIG_MINIMAL) {
        xTaskCreate(wifi_keepalive_task, "wifi_ka", 4096, NULL, 4, NULL);
        ESP_LOGI(TAG, "WiFi keepalive task started (%d s interval)",
                 WIFI_KEEPALIVE_INTERVAL_MS / 1000);
    }

    /* Arm ISR hardware watchdog — minimal nodes only.
     * Must be AFTER keepalive task is created (it feeds the heartbeat). */
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
