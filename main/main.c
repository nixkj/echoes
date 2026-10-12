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
#include <stdatomic.h>

#include "echoes.h"
#include "synthesis.h"
#include "ota.h"
#include "startup.h"
#include "espnow_mesh.h"
#include "markov.h"
#include "remote_config.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

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
#include "hal/wdt_hal.h"
#include "soc/rtc.h"

static const char *TAG = "MAIN";

/* ========================================================================
 * HARDWARE WATCHDOG (all nodes since 7.5.6) — RTC WDT
 * ========================================================================
 *
 * Previous design: a GPTimer interrupt counted heartbeats from the
 * keepalive task and, on timeout, wrote RTC_CNTL_SW_SYS_RST directly.
 * It could not recover the "stopped polling, white LED on" lockup because:
 *
 *   - The GPTimer, the TWDT and the interrupt WDT all run from the APB
 *     clock and need the CPUs/interrupts to do anything.  So does the LEDC
 *     peripheral driving the white LED.  A fault that stops that clock or
 *     wedges the CPUs freezes the LED at its current level AND silences
 *     every one of those watchdogs at the same time.
 *   - The raw SW_SYS_RST skipped everything IDF's own restart does first:
 *     arming the RTC WDT, moving the CPU off the PLL back to XTAL, resetting
 *     the other core.  If anything after that reset hangs, nothing is armed
 *     to pull the chip out.
 *
 * New design: the RTC watchdog.  It is clocked by the RTC slow oscillator,
 * lives in the RTC power domain, and resets the chip in hardware with no
 * CPU, interrupt, APB clock or software involvement.  Stage action
 * RESET_RTC resets the digital core AND the RTC domain — the closest thing
 * to a power cycle the chip can do to itself.  It is free after boot (the
 * app disables the bootloader's use of it during startup).
 *
 * Feed gate: LOCAL health only — wifi_keepalive_task is running and
 * lux_task is alive.  Network/server reachability is deliberately NOT part
 * of the gate: nodes must run autonomously when the Pi or the AP is down
 * (README: "devices fall back to built-in defaults if the server is
 * unreachable").  Network recovery is handled separately and only when
 * peers prove the server is up — see remote_config_task.              */

#define HW_WDT_TIMEOUT_S         120  /* seconds without a feed → full chip reset */

/* Boot grace: always feed for the first 2 minutes — lux_task is created
 * suspended and only resumed after OTA / remote-config at boot.          */
#define BOOT_GRACE_MS      ( 2u * 60u * 1000u)

static wdt_hal_context_t s_hw_wdt       = RWDT_HAL_CONTEXT_DEFAULT();
static bool              s_hw_wdt_armed = false;

/* _Atomic uint32_t storing seconds: uint32_t is natively atomic on the
 * 32-bit Xtensa LX6, uint64_t is not.                                    */
static _Atomic uint32_t          s_lux_alive_s        = 0;   /* seconds; updated by lux_task     */

/* If lux_task has not updated s_lux_alive_s within this window, keepalive
 * treats lux_task as dead and stops feeding the hardware watchdog.
 * 30 s (was 10 s): on full nodes a flash response blocks lux_task for up to
 * ~8 s (4 s speaker-mutex wait + 3 s bird call + synthesis).  A short miss
 * would only pause feeding — the RTC WDT still needs 120 s without a feed —
 * but 30 s keeps the "starving" log meaningful.                          */
#define LUX_DEAD_MS  30000u

static void hw_wdt_init(void)
{
    uint32_t ticks = (uint32_t)((uint64_t)HW_WDT_TIMEOUT_S * rtc_clk_slow_freq_get_hz());

    wdt_hal_init(&s_hw_wdt, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&s_hw_wdt);
    wdt_hal_config_stage(&s_hw_wdt, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_enable(&s_hw_wdt);
    wdt_hal_feed(&s_hw_wdt);
    wdt_hal_write_protect_enable(&s_hw_wdt);
    s_hw_wdt_armed = true;

    ESP_LOGI(TAG, "HW WDT: RTC watchdog armed (%d s timeout, full chip reset)",
             HW_WDT_TIMEOUT_S);
}

static void hw_wdt_feed(void)
{
    if (!s_hw_wdt_armed) return;
    wdt_hal_write_protect_disable(&s_hw_wdt);
    wdt_hal_feed(&s_hw_wdt);
    wdt_hal_write_protect_enable(&s_hw_wdt);
}

/**
 * @brief Record lux_based_birds_task liveness for the HW WDT gate.
 *
 * wifi_keepalive_task feeds the RTC watchdog only while lux_task has
 * stamped s_lux_alive_s within LUX_DEAD_MS.  If lux_task dies silently it
 * drops off the TWDT watchlist, so without this check nothing would notice.
 *
 * Failure modes caught:
 *   lux_task dead        → lux_ok=false → keepalive stops feeding → reset
 *   keepalive stalls     → no feed                                → reset
 *   chip / clock frozen  → no feed                                → reset
 *   Pi or AP down        → still fed — node keeps running autonomously
 *
 * Called unconditionally from lux_based_birds_task on each iteration.
 * Safe from any task context; not safe from ISR context (not IRAM_ATTR).
 */
void isr_wdt_lux_feed(void)
{
    /* Simply stamp the current time; the keepalive decides whether to feed. */
    s_lux_alive_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

/**
 * @brief Return the tick count (ms) of the last lux_task keep-alive stamp.
 *
 * Called by espnow_mesh.c to populate ESPNOW_FLAG_LUX_ALIVE in outgoing
 * messages.  Returns 0 until lux_task has completed its first poll cycle.
 * Declared extern in espnow_mesh.c to avoid a circular header dependency.
 */
uint64_t main_get_lux_alive_ms(void)
{
    return (uint64_t)s_lux_alive_s * 1000ULL;
}

/* ========================================================================
 * WIFI KEEPALIVE (all nodes since 7.5.6)
 * ========================================================================
 *
 * This task is the sole feeder of the RTC hardware watchdog (see
 * isr_wdt_lux_feed() for the gate).  If it stalls for HW_WDT_TIMEOUT_S
 * seconds — WiFi driver deadlock, frozen chip — the RTC WDT resets the chip.
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
 * server every 60 seconds via HTTP.  After RCFG_FAILURES_HARD_REBOOT
 * failures it reboots — but ONLY if peers report via ESP-NOW that the
 * server is reachable (this node alone is cut off).  If nobody can reach
 * the server the node stays up and runs autonomously.  See remote_config.c.
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

        /* ── HW WDT feed gate ───────────────────────────────────────────
         *
         * Local health only: this task is running (or we would not be here)
         * and lux_task stamped s_lux_alive_s within LUX_DEAD_MS.  Server or
         * AP availability is deliberately ignored so the installation runs
         * autonomously without the Pi.  Always feed during boot grace.    */
        {
            /* Starvation is announced once in RTC memory so that, if the
             * RTC WDT fires, the next boot's startup report says why.  A
             * hardware reset cannot write anything at the moment it fires. */
            static bool s_starving = false;

            uint64_t now_ms   = (uint64_t)(esp_timer_get_time() / 1000ULL);
            uint64_t last_lux = (uint64_t)s_lux_alive_s * 1000ULL;
            bool lux_ok       = (last_lux != 0)
                             && ((now_ms - last_lux) < LUX_DEAD_MS);
            bool feed         = (now_ms < BOOT_GRACE_MS) || lux_ok;

            if (!feed && !s_starving) {
                ESP_LOGE(TAG, "HW WDT: lux_task stalled — chip reset in ~%d s",
                         HW_WDT_TIMEOUT_S);
                startup_write_rtc_diag(RTC_DIAG_CAUSE_ISR_WDT, 0,
                                       (uint32_t)esp_get_free_heap_size(), 0,
                                       (uint32_t)(now_ms / 1000));
            }
            if (feed) {
                if (s_starving) {
                    ESP_LOGW(TAG, "HW WDT: lux_task alive again — feeding");
                    startup_clear_rtc_diag();
                }
                hw_wdt_feed();
            }
            s_starving = !feed;
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
         * failures and never reaches RCFG_FAILURES_HARD_REBOOT, so we detect
         * the stuck call here.  We only reboot if peers report the server
         * is reachable: if the Pi or AP is down for everyone, a reboot
         * cannot help and the node should keep running autonomously.  If
         * the server comes back later, peer evidence appears and the reboot
         * then gets this node polling again.
         *
         * g_rcfg_http_attempt_start_ms is set just before
         * esp_http_client_perform() and cleared on return.  If it has
         * been non-zero for longer than HTTP_STUCK_TIMEOUT_MS we know
         * connect() is hung and will never time out on its own.           */
        {
            uint64_t attempt_start = g_rcfg_http_attempt_start_ms;
            if (attempt_start != 0) {
                uint64_t now_ms     = (uint64_t)(esp_timer_get_time() / 1000ULL);
                uint64_t attempt_ms = now_ms - attempt_start;
                if (attempt_ms > HTTP_STUCK_TIMEOUT_MS &&
                    attempt_ms > HTTP_STUCK_PEER_PROOF_MS &&
                    espnow_mesh_peer_reports_server_ok(PEER_SERVER_OK_RECENT_MS)) {
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
                    uint32_t uptime_s_now = (uint32_t)(now_ms / 1000);
                    startup_write_rtc_diag(
                        RTC_DIAG_CAUSE_HTTP_STUCK,
                        0,                          /* consecutive_failures N/A */
                        (uint32_t)esp_get_free_heap_size(),
                        (int32_t)rssi,
                        uptime_s_now);
                    startup_record_boot_reason(ESP_RST_SW, uptime_s_now);
                    echoes_hard_restart();
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

    /* ── Boot-reason stamp ───────────────────────────────────────────────────
     * Read and record the reset reason for THIS boot as early as possible —
     * before WiFi, before any tasks, before anything that could crash.
     *
     * If this boot ends in an uncontrolled crash (PANIC, TASK_WDT, etc.) the
     * stamp survives in RTC_NOINIT and the NEXT boot's startup report will
     * carry prev_boot_reset_reason = "PANIC" (or whatever), giving us the
     * crash type even when WiFi never reconnects.
     *
     * uptime_s = 0 here; it gets overwritten with the real uptime immediately
     * before every deliberate reset (startup_record_boot_reason call sites in
     * remote_config.c and the HW WDT/HTTP-stuck paths in main.c).          */
    esp_reset_reason_t boot_reason = esp_reset_reason();
    startup_record_boot_reason(boot_reason, 0);

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
         * NOTE: lux_based_birds_task handle may be NULL if creation failed —
         * ota_register_tasks() accepts NULL safely.
         */

        /* -- Pre-create tasks in suspended state for OTA registration -- */
        TaskHandle_t h_audio  = NULL;
        TaskHandle_t h_lux    = NULL;
        TaskHandle_t h_flock  = NULL;

        /* Stack note: audio_detection_task does floating-point DSP (Goertzel,
         * adaptive thresholds) and calls into synthesis from the same stack.
         * Increased from 4096 → 8192 (v7.4.0) after cascade node crashes
         * indicated heap/stack exhaustion.  All tasks bumped to 8192 for
         * headroom; measure high-water marks with uxTaskGetStackHighWaterMark()
         * before reducing.                                                   */

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

        xTaskCreate(audio_detection_task, "audio_detection", 8192, NULL, 5, &h_audio);
        if (h_audio)  vTaskSuspend(h_audio);

        xTaskCreate(lux_based_birds_task, "lux_birds", 8192, NULL, 4, &h_lux);
        if (h_lux)  vTaskSuspend(h_lux);

        xTaskCreate(flock_task, "flock", 8192, NULL, 4, &h_flock);
        if (h_flock)  vTaskSuspend(h_flock);

        /* Demo task: idles (polls remote config) until DEMO_MODE is enabled.
         * Priority 3 — below audio (5) and lux/flock (4) so it never starves
         * detection.  Not registered with OTA: it sleeps in 500 ms chunks,
         * holds no I2S or DMA resources, and will self-suspend on the next
         * remote_config_get() call which returns demo_mode=false during OTA. */
        xTaskCreate(demo_task, "demo", 8192, NULL, 3, NULL);

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

                ESP_LOGI(TAG, "OTA validation: new firmware detected — waiting 30 s to confirm stability...");

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
        xTaskCreate(audio_detection_task, "audio_detection", 8192, NULL, 5, NULL);

        xTaskCreate(lux_based_birds_task, "lux_birds", 8192, NULL, 4, NULL);

        xTaskCreate(flock_task, "flock", 8192, NULL, 4, NULL);
        ESP_LOGI(TAG, "Flock task started");

        xTaskCreate(demo_task, "demo", 8192, NULL, 3, NULL);
        ESP_LOGI(TAG, "Demo task started");
    } else {
        ESP_LOGI(TAG, "Echoes of the Machine running (tasks already started)");
    }

    ESP_LOGI(TAG, "System started successfully!");

    /* Remote config + keepalive tasks: started unconditionally so they run
     * even when the initial WiFi connection failed.  The WiFi reconnect
     * timer in ota.c retries every 30 s; once connected, remote_config_task
     * picks up live settings on its next poll.  Until then the node runs on
     * NVS / built-in defaults.                                             */
    xTaskCreate(remote_config_task, "rcfg", 8192, NULL, 3, NULL);
    ESP_LOGI(TAG, "Remote config polling task started");

    /* wifi_keepalive_task is the sole feeder of the RTC hardware watchdog.
     * See isr_wdt_lux_feed() for the (local-health-only) feed gate.
     * Runs on every node type since 7.5.6.                                */
    xTaskCreate(wifi_keepalive_task, "wifi_ka", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "WiFi keepalive task started (%d s interval)",
             WIFI_KEEPALIVE_INTERVAL_MS / 1000);

    /* Arm the RTC hardware watchdog on every node — with or without WiFi.
     *
     * The feed gate uses only local health (keepalive + lux_task alive), so
     * a missing AP or Pi can never starve it; there is no reset-loop risk on
     * the no-WiFi path, and that path gets the same protection against chip
     * lockups.
     *
     * 7.5.5 armed minimal nodes only.  Its first night (2026-10-11/12) had no
     * minimal-node lockups but one full node (id 7) locked up at 03:11 and
     * stayed down until power-cycled, so 7.5.6 arms full nodes too.      */
    hw_wdt_init();

    /* Log final startup summary */
    ESP_LOGI(TAG, "Startup Summary:");
    ESP_LOGI(TAG, "  MAC:      %s", startup_report.mac_address);
    ESP_LOGI(TAG, "  Type:     %s", startup_report.node_type);
    ESP_LOGI(TAG, "  WiFi:     %s", wifi_connected ? "connected" : "no connection — startup report not sent");
    if (wifi_connected) {
        ESP_LOGI(TAG, "  Errors:   %s", startup_report.has_errors ? startup_report.error_message : "none");
    }
}
