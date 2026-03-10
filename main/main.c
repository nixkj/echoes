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

#define ISR_WDT_TIMEOUT_S        120  /* seconds without heartbeat → hardware reset */
#define ISR_WDT_ALARM_INTERVAL_S   5  /* ISR fires every N seconds, not every 1 s.
                                       * keepalive increments heartbeat every 1 s, so
                                       * the counter advances 4-5× between ISR ticks.
                                       * This eliminates the 1s race between keepalive
                                       * and the ISR, and gives the CPU caches ample
                                       * time to flush the write before the next read.
                                       * Miss threshold = TIMEOUT / INTERVAL = 24.    */

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
 *   = 4 min + 2 min = 6 minutes from AP drop to hardware reset.
 *
 * ISR fires every ISR_WDT_ALARM_INTERVAL_S (5 s), not every 1 s.
 * keepalive increments every 1 s, so heartbeat advances ~5× between
 * checks.  This eliminates the 1 s race and cross-core cache issue.
 * Miss threshold = 120 / 5 = 24 ticks, same 120 s wall-clock timeout. */
#define NETWORK_GRACE_MS   ( 2u * 60u * 1000u)   /*  2 min boot grace     */
#define NETWORK_DEATH_MS   ( 4u * 60u * 1000u)   /* 4 min stale → stop    */

static gptimer_handle_t          s_isr_wdt_timer     = NULL;
static _Atomic uint32_t          s_isr_wdt_heartbeat  = 0;   /* fed by keepalive, checked by ISR */
static volatile uint64_t         s_lux_alive_ms       = 0;   /* updated by lux_task each poll   */

/* If lux_task has not updated s_lux_alive_ms within this window, keepalive
 * treats lux_task as dead and stops feeding the ISR WDT heartbeat.
 * Must be >> lux_poll_interval_ms (500 ms).  10 s = 20× the base rate.   */
#define LUX_DEAD_MS  10000u

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

    uint32_t hb = atomic_load_explicit(&s_isr_wdt_heartbeat, memory_order_relaxed);
    if (hb == s_last_hb) {
        s_miss_count++;
        if (s_miss_count >= (ISR_WDT_TIMEOUT_S / ISR_WDT_ALARM_INTERVAL_S)) {
            /* Write diagnostic state before resetting.  We are in ISR context
             * so heap size and RSSI cannot be safely read — pass 0 for both.
             * startup_write_rtc_diag() and startup_record_boot_reason() are
             * both IRAM_ATTR (pure memory writes, safe from ISR).           */
            uint32_t uptime_now = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            startup_write_rtc_diag(
                RTC_DIAG_CAUSE_ISR_WDT,
                0,           /* consecutive_failures not accessible from ISR */
                0,           /* heap not safely readable from ISR             */
                0,           /* RSSI not safely readable from ISR             */
                uptime_now);
            startup_record_boot_reason(ESP_RST_SW, uptime_now);
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
 * a 1-second alarm in ISR context.  Two tasks feed the heartbeat counter:
 * wifi_keepalive_task (directly) and lux_based_birds_task (via
 * isr_wdt_lux_feed()).  See isr_wdt_lux_feed() for the rationale.
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
        .alarm_count               = ISR_WDT_ALARM_INTERVAL_S * 1000000ULL,  /* N seconds at 1 MHz */
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

/**
 * @brief Record lux_based_birds_task liveness for the ISR WDT gate.
 *
 * Architecture — why keepalive alone is not enough:
 *
 *   wifi_keepalive_task feeds s_isr_wdt_heartbeat while end-to-end network
 *   connectivity is confirmed (last_fetch_ms fresh).  If lux_task dies
 *   silently it is removed from the TWDT watchlist, so the TWDT never fires.
 *   wifi_keepalive_task keeps running, HTTP keeps succeeding, and the ISR WDT
 *   heartbeat keeps advancing — the failure is completely masked.
 *
 * Two-signal gate in wifi_keepalive_task:
 *
 *   The keepalive feeds the heartbeat ONLY when BOTH of the following hold:
 *     1. network_ok  — last HTTP fetch was within NETWORK_DEATH_MS
 *     2. lux_ok      — this function was called within LUX_DEAD_MS
 *
 *   This function's sole job is to update s_lux_alive_ms unconditionally on
 *   every lux_task poll cycle (~500 ms).  It does NOT touch the heartbeat.
 *   The keepalive is the single writer of the heartbeat; it reads both signals
 *   and decides whether to advance it.
 *
 * Failure modes caught:
 *   WiFi dead        → network_ok=false  → keepalive stops feeding → WDT fires
 *   lux_task dead    → lux_ok=false      → keepalive stops feeding → WDT fires
 *   keepalive stalls → heartbeat stalls  → ISR WDT fires directly
 *   All healthy      → both true         → heartbeat advances      → no WDT
 *
 * Previous approach (7.2.0): lux_task incremented the heartbeat directly with
 *   a network-health gate mirroring the keepalive.  Flaw: if lux_task is alive
 *   but the network is dead, lux_task correctly stops feeding — but if lux_task
 *   is dead and the network is alive, keepalive alone keeps advancing the
 *   heartbeat, and the WDT never fires.  The gate belongs in keepalive, where
 *   both signals can be evaluated together.
 *
 * Called unconditionally from lux_based_birds_task on each iteration.
 * Safe from any task context; not safe from ISR context (not IRAM_ATTR).
 */
void isr_wdt_lux_feed(void)
{
    /* Simply stamp the current tick.  No gate here — the keepalive decides
     * whether to advance the heartbeat based on both this timestamp and the
     * network health timestamp.                                            */
    s_lux_alive_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
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
    return s_lux_alive_ms;
}

/* ========================================================================
 * WIFI KEEPALIVE (minimal nodes only)
 * ========================================================================
 *
 * This task is one of TWO feeders of the ISR hardware watchdog.  The other
 * is lux_based_birds_task, via isr_wdt_lux_feed() — see that function for
 * the full rationale.
 *
 * This task's role: detect keepalive blocked (e.g. WiFi driver deadlock,
 * esp_now_send TX-buffer full).  If this task stalls for ISR_WDT_TIMEOUT_S
 * seconds without lux_task compensating, the ISR WDT fires.
 *
 * Because lux_task feeds the counter at 500 ms intervals, a stalled
 * keepalive alone will NOT trigger the ISR WDT while lux_task is healthy.
 * The ISR WDT fires only when BOTH tasks stop feeding — i.e. when the node
 * is genuinely unresponsive, not merely when keepalive stalls briefly.
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

        /* ── ISR WDT feed gate ───────────────────────────────────────────
         *
         * Feed the hardware watchdog ONLY when we have proof that BOTH
         * end-to-end network connectivity AND lux_task are healthy.
         *
         * Two signals, evaluated here (and only here):
         *
         *   network_ok — last successful HTTP config fetch was within
         *                NETWORK_DEATH_MS.  remote_config_task updates
         *                last_fetch_ms on every successful HTTP poll (~60 s).
         *                This is the only operation that proves bidirectional
         *                connectivity; all local WiFi driver state is cached
         *                and can appear healthy even when the AP has silently
         *                dropped this node.
         *
         *   lux_ok     — isr_wdt_lux_feed() was called within LUX_DEAD_MS.
         *                lux_based_birds_task calls it unconditionally on
         *                every ~500 ms poll.  If lux_task crashes silently
         *                it is removed from the TWDT watchlist; TWDT never
         *                fires for it.  This signal detects that gap.
         *
         * During the boot grace period always feed — the first HTTP fetch
         * and lux_task's first poll haven't happened yet.
         *
         * After grace: feed only if network_ok AND lux_ok.  Either failure
         * starves the heartbeat → ISR WDT fires ISR_WDT_TIMEOUT_S later.
         *
         * Failure modes caught:
         *   WiFi dead      → network_ok=false → WDT fires            ✓
         *   lux_task dead  → lux_ok=false     → WDT fires            ✓
         *   keepalive dead → heartbeat stalls → ISR fires directly   ✓  */
        {
            uint64_t now_ms  = (uint64_t)(esp_timer_get_time() / 1000ULL);
            bool boot_grace  = (now_ms < NETWORK_GRACE_MS);

            if (boot_grace) {
                atomic_fetch_add_explicit(&s_isr_wdt_heartbeat, 1u, memory_order_relaxed);
            } else {
                uint64_t last_net = remote_config_get()->last_fetch_ms;
                bool network_ok   = (last_net != 0)
                                 && ((now_ms - last_net) < NETWORK_DEATH_MS);

                uint64_t last_lux = s_lux_alive_ms;
                bool lux_ok       = (last_lux != 0)
                                 && ((now_ms - last_lux) < LUX_DEAD_MS);

                if (network_ok && lux_ok) {
                    atomic_fetch_add_explicit(&s_isr_wdt_heartbeat, 1u, memory_order_relaxed);
                }
                /* else: network or lux_task dead — starve the ISR WDT */
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
            uint64_t attempt_start = g_rcfg_http_attempt_start_ms;
            if (attempt_start != 0) {
                uint64_t now_ms     = (uint64_t)(esp_timer_get_time() / 1000ULL);
                uint64_t attempt_ms = now_ms - attempt_start;
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
                    uint32_t uptime_s_now = (uint32_t)(now_ms / 1000);
                    startup_write_rtc_diag(
                        RTC_DIAG_CAUSE_HTTP_STUCK,
                        0,                          /* consecutive_failures N/A */
                        (uint32_t)esp_get_free_heap_size(),
                        (int32_t)rssi,
                        uptime_s_now);
                    startup_record_boot_reason(ESP_RST_SW, uptime_s_now);
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
     * remote_config.c and the ISR WDT/HTTP-stuck paths in main.c).          */
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
         * Increased from 4096 → 8192 (2026-03-10, v7.3.2) after cascade node
         * crashes in the 2026-03-09 session indicated heap/stack exhaustion.
         * All tasks bumped to 8192 for headroom; measure high-water marks
         * with uxTaskGetStackHighWaterMark() before reducing.               */

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
    xTaskCreate(remote_config_task, "rcfg", 8192, NULL, 3, NULL);
    ESP_LOGI(TAG, "Remote config polling task started");

    /* wifi_keepalive_task is one of two ISR WDT feeders (the other is
     * lux_based_birds_task via isr_wdt_lux_feed()).  If keepalive stalls
     * while lux_task is also dead, the ISR fires and resets the SoC.
     * See isr_wdt_lux_feed() in this file for the full rationale.          */
    if (hw_config == HW_CONFIG_MINIMAL) {
        xTaskCreate(wifi_keepalive_task, "wifi_ka", 8192, NULL, 4, NULL);
        ESP_LOGI(TAG, "WiFi keepalive task started (%d s interval)",
                 WIFI_KEEPALIVE_INTERVAL_MS / 1000);
    }

    /* Arm ISR hardware watchdog — minimal nodes only, AND only when WiFi
     * connected at boot.
     *
     * The ISR WDT's purpose is to catch WiFi-driver deadlocks: scenarios
     * where the radio stack is wedged and remote_config_task can no longer
     * complete an HTTP fetch.  Without a successful fetch, last_fetch_ms
     * never advances and the heartbeat starves → hardware reset.
     *
     * When the AP is absent at boot there is no WiFi stack in use, so the
     * deadlock scenario cannot occur and last_fetch_ms will never be
     * populated — arming the WDT would cause a permanent reset loop every
     * NETWORK_GRACE_MS + ISR_WDT_TIMEOUT_S seconds.
     *
     * Without the ISR WDT, the TWDT (30 s) still catches task stalls.
     * The only unguarded gap is a WiFi-driver deadlock that stalls the
     * keepalive task — which cannot happen without an active WiFi session.*/
    if (hw_config == HW_CONFIG_MINIMAL && wifi_connected) {
        isr_wdt_init();
        ESP_LOGI(TAG, "ISR WDT armed (WiFi connected)");
    } else if (hw_config == HW_CONFIG_MINIMAL) {
        ESP_LOGI(TAG, "ISR WDT skipped (no WiFi — TWDT only)");
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
