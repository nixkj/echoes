/**
 * @file sniffer_main.c
 * @brief Echoes of the Machine — dedicated ESP-NOW network sniffer node
 *
 * PURPOSE
 * -------
 * This firmware turns an ESP32 into a passive listener that echoes every
 * ESP-NOW frame from the Echoes fleet to a connected Raspberry Pi over USB
 * serial as a stream of newline-delimited JSON objects (JSON Lines format).
 *
 * The sniffer NEVER transmits ESP-NOW frames.  It connects to the same
 * WiFi AP as the fleet (to land on the correct channel), registers as an
 * ESP-NOW receiver, and pipes everything it hears to UART0.
 *
 * TIMESTAMPS
 * ----------
 * ts        — milliseconds.  Two modes depending on SNTP sync state:
 *
 *   ts_epoch=true   SNTP has synced.  ts is Unix epoch milliseconds
 *                   (e.g. 1748781234567).  Suitable for direct correlation
 *                   with server logs without any Pi-side annotation.
 *
 *   ts_epoch=false  SNTP has not synced (no WiFi, or sync still pending).
 *                   ts is milliseconds since sniffer boot — a relative
 *                   uptime counter useful for measuring inter-frame timing.
 *                   Wraps after ~49.7 days.
 *
 * A {"event":"SNTP_SYNCED",...} record is emitted the moment sync succeeds
 * so downstream tools know exactly when the transition occurred.
 *
 * SATURATION STATS
 * ----------------
 * Every STATS_INTERVAL_S seconds a STATS record is emitted summarising the
 * previous window.  Three complementary saturation indicators are included:
 *
 *   rate_fps       — frames received per second.  Compare against the
 *                    theoretical broadcast ceiling (~400–800 fps for 8-byte
 *                    frames on 802.11b/g at 1 Mbps with no ACK).  Normal
 *                    fleet steady-state is well under 100 fps.
 *
 *   gap_p5_us      — 5th-percentile inter-frame gap in microseconds,
 *                    derived from the hardware RX timestamp in rx_ctrl.
 *                    This is the most direct channel-contention signal:
 *                    a healthy network has gaps distributed around its
 *                    average interval; a saturated one has many gaps
 *                    clustered near the physical minimum (~500–1000 µs
 *                    for back-to-back broadcast frames).  When gap_p5_us
 *                    drops below SATURATED_GAP_P5_US the "saturated" flag
 *                    is set in the record.
 *
 *   dropped        — frames the sniffer received but could not enqueue
 *                    (print_task falling behind).  Non-zero means the
 *                    sniffer itself is the bottleneck, not the network;
 *                    increase RX_QUEUE_DEPTH or reduce print overhead.
 *                    A saturated network can drive dropped > 0 indirectly.
 *
 * Additional fields: gap_min_us, gap_median_us, rssi_min, rssi_avg,
 * noise_floor_avg, and per-type frame counts.
 *
 * Example STATS record:
 *   {"event":"STATS","ts":1748781250000,"ts_epoch":true,
 *    "window_s":10,"frames":94,"dropped":0,"rate_fps":9.4,
 *    "gap_min_us":18200,"gap_p5_us":22100,"gap_median_us":98000,
 *    "rssi_min":-78,"rssi_avg":-65,"noise_floor_avg":-95,
 *    "type_counts":{"SOUND":5,"LIGHT":42,"STATUS":3,"UNKNOWN":0},
 *    "saturated":false}
 *
 * BUILDING
 * --------
 *   cd echoes-sniffer
 *   idf.py set-target esp32
 *   idf.py menuconfig          # set WiFi SSID / Password under "WiFi Configuration"
 *   idf.py build flash monitor
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

/* =========================================================================
 * ECHOES WIRE PROTOCOL (duplicated here so the sniffer is self-contained)
 * ========================================================================= */

#define ESPNOW_MAGIC        0xEC

typedef enum {
    ESPNOW_MSG_SOUND        = 1,    /**< Sound detection event                */
    ESPNOW_MSG_LIGHT        = 2,    /**< Lux level changed significantly       */
    ESPNOW_MSG_STATUS       = 3,    /**< 30-second health heartbeat            */
} espnow_msg_type_t;

typedef enum {
    DETECTION_NONE      = 0,
    DETECTION_WHISTLE,
    DETECTION_VOICE,
    DETECTION_CLAP,
    DETECTION_BIRDSONG,
} detection_type_t;

/* Node health flags -- carried in status_flags byte of every outgoing message.
 * Old nodes (pre-7.3.0) transmitted 0x00 in this byte; interpret as unknown. */
#define ESPNOW_FLAG_NODE_FULL    (1u << 7)  /* set=FULL, clear=MINIMAL        */
#define ESPNOW_FLAG_WIFI_ASSOC   (1u << 6)  /* WiFi driver reports associated */
#define ESPNOW_FLAG_HTTP_RECENT  (1u << 5)  /* HTTP fetch < 150 s ago (E2E!)  */
#define ESPNOW_FLAG_LUX_ALIVE    (1u << 4)  /* lux_task stamped < 10 s ago    */
#define ESPNOW_FLAG_FLOCK        (1u << 3)  /* node is in flock mode          */

/* SOUND / LIGHT message -- 8 bytes.  status_flags was reserved=0x00 in
 * firmware < 7.3.0; old nodes send 0x00 which decodes as unknown (benign). */
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  msg_type;
    uint8_t  detection;     /* detection_type_t -- valid for SOUND only       */
    uint8_t  status_flags;  /* ESPNOW_FLAG_* health bits                      */
    float    lux;
} espnow_msg_t;

/* STATUS heartbeat -- 8 bytes, same magic and size as espnow_msg_t.
 * Sent every 30 s from espnow_rx_task regardless of sound/lux activity.
 * A node whose lux_task has died still emits STATUS frames, making the
 * failure observable on the sniffer without physical access.               */
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  msg_type;      /* Always ESPNOW_MSG_STATUS                       */
    uint8_t  status_flags;  /* ESPNOW_FLAG_* health bits                      */
    int8_t   rssi;          /* Node's own AP RSSI in dBm; 0 = unreadable      */
    uint8_t  http_stale_m;  /* Minutes since last good HTTP fetch; 255=sat.   */
    uint8_t  seq;           /* TX counter 0-255, wraps; gaps = air loss       */
    uint16_t uptime_m;      /* Minutes since last reboot; 65535 = saturated   */
} espnow_status_msg_t;

/* =========================================================================
 * CONFIGURATION
 * ========================================================================= */

/* CONFIG_WIFI_* are generated by Kconfig (idf.py menuconfig -> WiFi Configuration).
 * The #ifndef guards let the file compile on a first build before sdkconfig exists;
 * replace the fallback strings here if you prefer not to use menuconfig.          */
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID     "myssid"
#endif
#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD "mypassword"
#endif

#define WIFI_SSID           CONFIG_WIFI_SSID
#define WIFI_PASSWORD       CONFIG_WIFI_PASSWORD
#define WIFI_MAX_RETRIES    10

/* NTP server.  Replace with a local server IP if the installation has no
 * internet access but does have a router running NTP.                      */
#define SNTP_SERVER         "pool.ntp.org"

/* How often to emit a STATS record (seconds).                              */
#define STATS_INTERVAL_S    10

/* Size of the inter-frame gap ring buffer (entries, each uint32_t µs).
 * At 10 s windows and up to ~800 fps peak, 512 entries holds the most
 * recent ~0.6 s of gaps — enough for a representative percentile sample
 * across any burst.  Increase if you want full-window coverage at low rates. */
#define STATS_GAP_BUF_SIZE  512

/* Saturation advisory thresholds — both must be exceeded to set the flag. */
#define SATURATED_FPS           150     /* frames/s                          */
#define SATURATED_GAP_P5_US     2000    /* µs — heavy contention below this  */

/* Queue depth for the rx->print pipeline.                                  */
#define RX_QUEUE_DEPTH      64

static const char *TAG = "SNIFFER";

/* =========================================================================
 * INTERNAL TYPES
 * ========================================================================= */

typedef struct {
    uint64_t ts_ms;
    bool     ts_epoch;
    uint8_t  src_mac[ESP_NOW_ETH_ALEN];
    int8_t   rssi;
    int8_t   noise_floor;
    uint8_t  channel;
    uint8_t  raw[sizeof(espnow_msg_t)];
    int      raw_len;
    bool     is_echoes;
} rx_event_t;

/* =========================================================================
 * GLOBALS
 * ========================================================================= */

static QueueHandle_t      s_rx_queue = NULL;
static EventGroupHandle_t s_wifi_eg  = NULL;

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static int           s_wifi_retries = 0;
static volatile bool s_sntp_synced  = false;

/* ── Stats accumulators — written from WiFi task, read from stats_task ── */

static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

/* Per-window frame counters */
static uint32_t s_win_frames   = 0;
static uint32_t s_win_dropped  = 0;

/* Per-type counts */
static uint32_t s_win_sound    = 0;
static uint32_t s_win_light    = 0;
static uint32_t s_win_status   = 0;
static uint32_t s_win_unknown  = 0;

/* Signal quality accumulators */
static int32_t  s_win_rssi_sum   = 0;
static int8_t   s_win_rssi_min   = 0;     /* initialised on first frame */
static int32_t  s_win_nf_sum     = 0;     /* noise floor sum            */
static bool     s_win_first      = true;  /* guards min initialisation  */

/* Inter-frame gap ring buffer (µs, derived from hardware rx_ctrl->timestamp) */
static uint32_t s_gap_buf[STATS_GAP_BUF_SIZE];
static uint16_t s_gap_head  = 0;     /* next write slot                    */
static uint16_t s_gap_fill  = 0;     /* valid entries (capped at BUF_SIZE) */
static uint32_t s_last_hw_ts       = 0;
static bool     s_last_hw_ts_valid = false;

/* =========================================================================
 * TIMESTAMP HELPER
 * ========================================================================= */

static uint64_t get_ts_ms(bool *is_epoch)
{
    if (s_sntp_synced) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        *is_epoch = true;
        return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
    }
    *is_epoch = false;
    return (uint64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* =========================================================================
 * SNTP
 * ========================================================================= */

static void sntp_sync_cb(struct timeval *tv)
{
    s_sntp_synced = true;

    uint64_t epoch_ms = (uint64_t)tv->tv_sec * 1000ULL +
                        (uint64_t)tv->tv_usec / 1000ULL;

    printf("{\"ts\":%" PRIu64 ",\"ts_epoch\":true"
           ",\"event\":\"SNTP_SYNCED\",\"unix_s\":%" PRIu64 "}\n",
           epoch_ms, (uint64_t)tv->tv_sec);
    fflush(stdout);

    ESP_LOGI(TAG, "SNTP synced -- epoch %" PRIu64 " ms", epoch_ms);
}

static void sntp_start(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER);
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started -- server: %s", SNTP_SERVER);
}

/* =========================================================================
 * STATS — percentile helper
 * ========================================================================= */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/**
 * @brief Emit a STATS JSON record, then reset all window accumulators.
 *
 * Called from stats_task every STATS_INTERVAL_S seconds.  Acquires the
 * stats spinlock only long enough to snapshot and zero the accumulators;
 * all computation and printing happens outside the lock.
 */
static void emit_stats(void)
{
    /* ── Snapshot under lock ─────────────────────────────────────────── */
    uint32_t frames, dropped, sound, light, status, unknown;
    int32_t  rssi_sum, nf_sum;
    int8_t   rssi_min;
    bool     first;
    uint16_t gap_fill;
    /* static: moves 2 KiB off the stack.  emit_stats() is called only
     * from stats_task (single task, no reentrancy), so a static local
     * is safe and avoids the overflow that killed the previous build.  */
    static uint32_t gap_snap[STATS_GAP_BUF_SIZE];

    portENTER_CRITICAL(&s_stats_mux);

    frames  = s_win_frames;
    dropped = s_win_dropped;
    sound   = s_win_sound;
    light   = s_win_light;
    status  = s_win_status;
    unknown = s_win_unknown;
    rssi_sum = s_win_rssi_sum;
    rssi_min = s_win_rssi_min;
    nf_sum   = s_win_nf_sum;
    first    = s_win_first;
    gap_fill = s_gap_fill;

    /* Copy gap buffer — only the valid portion.
     * If the buffer wrapped, gap_fill == STATS_GAP_BUF_SIZE and all slots
     * hold valid data.  Order within the copy doesn't matter; we sort it. */
    memcpy(gap_snap, s_gap_buf, gap_fill * sizeof(uint32_t));

    /* Reset window */
    s_win_frames  = 0;
    s_win_dropped = 0;
    s_win_sound   = 0;
    s_win_light   = 0;
    s_win_status  = 0;
    s_win_unknown = 0;
    s_win_rssi_sum  = 0;
    s_win_nf_sum    = 0;
    s_win_first     = true;
    s_gap_fill      = 0;
    s_gap_head      = 0;
    /* Keep s_last_hw_ts_valid / s_last_hw_ts so the first gap of the
     * next window is measured from the last frame of this one.         */

    portEXIT_CRITICAL(&s_stats_mux);

    /* ── Compute derived values (outside lock) ───────────────────────── */
    float rate_fps = (float)frames / (float)STATS_INTERVAL_S;

    int8_t  rssi_avg     = first ? 0 : (int8_t)(rssi_sum / (int32_t)frames);
    int8_t  nf_avg       = first ? 0 : (int8_t)(nf_sum   / (int32_t)frames);

    /* Percentiles — only meaningful with at least 2 gaps */
    bool     has_gaps    = (gap_fill >= 2);
    uint32_t gap_min     = 0;
    uint32_t gap_p5      = 0;
    uint32_t gap_median  = 0;

    if (has_gaps) {
        qsort(gap_snap, gap_fill, sizeof(uint32_t), cmp_u32);
        gap_min    = gap_snap[0];
        gap_p5     = gap_snap[(gap_fill * 5) / 100];
        gap_median = gap_snap[gap_fill / 2];
    }

    /* Saturation advisory: both thresholds must be exceeded */
    bool saturated = (rate_fps >= SATURATED_FPS) &&
                     has_gaps && (gap_p5 < SATURATED_GAP_P5_US);

    /* ── Emit ────────────────────────────────────────────────────────── */
    bool is_epoch;
    uint64_t ts = get_ts_ms(&is_epoch);

    if (has_gaps) {
        printf("{\"event\":\"STATS\""
               ",\"ts\":%" PRIu64 ",\"ts_epoch\":%s"
               ",\"window_s\":%d,\"frames\":%" PRIu32 ",\"dropped\":%" PRIu32
               ",\"rate_fps\":%.1f"
               ",\"gap_min_us\":%" PRIu32
               ",\"gap_p5_us\":%" PRIu32
               ",\"gap_median_us\":%" PRIu32
               ",\"rssi_min\":%d,\"rssi_avg\":%d"
               ",\"noise_floor_avg\":%d"
               ",\"type_counts\":{\"SOUND\":%" PRIu32 ",\"LIGHT\":%" PRIu32
                                 ",\"STATUS\":%" PRIu32 ",\"UNKNOWN\":%" PRIu32 "}"
               ",\"saturated\":%s}\n",
               ts, is_epoch ? "true" : "false",
               STATS_INTERVAL_S, frames, dropped,
               rate_fps,
               gap_min, gap_p5, gap_median,
               (int)rssi_min, (int)rssi_avg, (int)nf_avg,
               sound, light, status, unknown,
               saturated ? "true" : "false");
    } else {
        /* Not enough frames for gap stats — emit nulls so the Pi script
         * can still parse the record without special-casing it.         */
        printf("{\"event\":\"STATS\""
               ",\"ts\":%" PRIu64 ",\"ts_epoch\":%s"
               ",\"window_s\":%d,\"frames\":%" PRIu32 ",\"dropped\":%" PRIu32
               ",\"rate_fps\":%.1f"
               ",\"gap_min_us\":null,\"gap_p5_us\":null,\"gap_median_us\":null"
               ",\"rssi_min\":null,\"rssi_avg\":null"
               ",\"noise_floor_avg\":null"
               ",\"type_counts\":{\"SOUND\":%" PRIu32 ",\"LIGHT\":%" PRIu32
                                 ",\"STATUS\":%" PRIu32 ",\"UNKNOWN\":%" PRIu32 "}"
               ",\"saturated\":false}\n",
               ts, is_epoch ? "true" : "false",
               STATS_INTERVAL_S, frames, dropped,
               rate_fps,
               sound, light, status, unknown);
    }

    fflush(stdout);
}

/* =========================================================================
 * STATS TASK
 * ========================================================================= */

static void stats_task(void *param)
{
    (void)param;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STATS_INTERVAL_S * 1000));
        emit_stats();
    }
}

/* =========================================================================
 * ESP-NOW RECEIVE CALLBACK  (WiFi task context — keep it short)
 * ========================================================================= */

static void on_espnow_recv(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int len)
{
    if (!s_rx_queue) return;

    /* Build the event record */
    rx_event_t evt = {0};
    evt.ts_ms     = get_ts_ms(&evt.ts_epoch);
    evt.rssi      = recv_info->rx_ctrl->rssi;
    evt.noise_floor = recv_info->rx_ctrl->noise_floor;
    evt.channel   = recv_info->rx_ctrl->channel;
    memcpy(evt.src_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);

    evt.raw_len = (len <= (int)sizeof(evt.raw)) ? len : (int)sizeof(evt.raw);
    memcpy(evt.raw, data, evt.raw_len);

    evt.is_echoes = (evt.raw_len == (int)sizeof(espnow_msg_t)) &&
                    (evt.raw[0] == ESPNOW_MAGIC);

    /* ── Update stats accumulators ───────────────────────────────────── */
    portENTER_CRITICAL(&s_stats_mux);

    s_win_frames++;

    /* Signal quality */
    if (s_win_first) {
        s_win_rssi_min = evt.rssi;
        s_win_first    = false;
    } else if (evt.rssi < s_win_rssi_min) {
        s_win_rssi_min = evt.rssi;
    }
    s_win_rssi_sum += (int32_t)evt.rssi;
    s_win_nf_sum   += (int32_t)evt.noise_floor;

    /* Per-type count */
    if (evt.is_echoes) {
        switch ((espnow_msg_type_t)evt.raw[1]) {
        case ESPNOW_MSG_SOUND:     s_win_sound++;  break;
        case ESPNOW_MSG_LIGHT:     s_win_light++;  break;
        case ESPNOW_MSG_STATUS:    s_win_status++; break;
        default:                   s_win_unknown++; break;
        }
    } else {
        s_win_unknown++;
    }

    /* Inter-frame gap from hardware µs timestamp.
     *
     * rx_ctrl->timestamp is a 32-bit hardware microsecond counter that
     * wraps every ~71 minutes.  Unsigned subtraction handles wraparound
     * correctly as long as the true gap is < 71 minutes, which is always
     * true for an active network.                                        */
    uint32_t hw_ts = recv_info->rx_ctrl->timestamp;
    if (s_last_hw_ts_valid) {
        uint32_t gap = hw_ts - s_last_hw_ts;   /* wraps safely */
        s_gap_buf[s_gap_head] = gap;
        s_gap_head = (s_gap_head + 1) % STATS_GAP_BUF_SIZE;
        if (s_gap_fill < STATS_GAP_BUF_SIZE) s_gap_fill++;
    }
    s_last_hw_ts       = hw_ts;
    s_last_hw_ts_valid = true;

    portEXIT_CRITICAL(&s_stats_mux);

    /* ── Enqueue for printing ────────────────────────────────────────── */
    if (xQueueSendFromISR(s_rx_queue, &evt, NULL) != pdTRUE) {
        portENTER_CRITICAL(&s_stats_mux);
        s_win_dropped++;
        portEXIT_CRITICAL(&s_stats_mux);
    }
}

/* =========================================================================
 * PRINT TASK  (normal task context — safe to printf)
 * ========================================================================= */

static const char *msg_type_str(uint8_t t)
{
    switch ((espnow_msg_type_t)t) {
    case ESPNOW_MSG_SOUND:     return "SOUND";
    case ESPNOW_MSG_LIGHT:     return "LIGHT";
    case ESPNOW_MSG_STATUS:    return "STATUS";
    default:                   return "UNKNOWN";
    }
}

/* Decode the ESPNOW_FLAG_* byte into a fixed-length JSON fragment:
 *   ,"node":"FULL","wifi_assoc":true,"http_recent":true,
 *   "lux_alive":true,"flock":false
 * If flags == 0x00 (old firmware or genuinely all-false) every field is
 * false/MINIMAL, which is a conservative but readable interpretation.
 * buf must be >= 96 bytes.                                                  */
static void flags_json(char *buf, size_t sz, uint8_t flags)
{
    snprintf(buf, sz,
             ",\"node\":\"%s\""
             ",\"wifi_assoc\":%s"
             ",\"http_recent\":%s"
             ",\"lux_alive\":%s"
             ",\"flock\":%s",
             (flags & ESPNOW_FLAG_NODE_FULL)   ? "FULL"  : "MINIMAL",
             (flags & ESPNOW_FLAG_WIFI_ASSOC)  ? "true"  : "false",
             (flags & ESPNOW_FLAG_HTTP_RECENT) ? "true"  : "false",
             (flags & ESPNOW_FLAG_LUX_ALIVE)   ? "true"  : "false",
             (flags & ESPNOW_FLAG_FLOCK)       ? "true"  : "false");
}

static const char *detection_str(uint8_t d)
{
    switch ((detection_type_t)d) {
    case DETECTION_NONE:     return NULL;
    case DETECTION_WHISTLE:  return "WHISTLE";
    case DETECTION_VOICE:    return "VOICE";
    case DETECTION_CLAP:     return "CLAP";
    case DETECTION_BIRDSONG: return "BIRDSONG";
    default:                 return NULL;
    }
}

static void print_task(void *param)
{
    (void)param;
    rx_event_t evt;

    while (1) {
        if (xQueueReceive(s_rx_queue, &evt, pdMS_TO_TICKS(5000)) != pdTRUE) {
            bool is_epoch;
            uint64_t ts = get_ts_ms(&is_epoch);
            printf("{\"ts\":%" PRIu64 ",\"ts_epoch\":%s"
                   ",\"event\":\"SNIFFER_ALIVE\"}\n",
                   ts, is_epoch ? "true" : "false");
            fflush(stdout);
            continue;
        }

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 evt.src_mac[0], evt.src_mac[1], evt.src_mac[2],
                 evt.src_mac[3], evt.src_mac[4], evt.src_mac[5]);

        const char *epoch_str = evt.ts_epoch ? "true" : "false";

        if (!evt.is_echoes) {
            printf("{\"ts\":%" PRIu64 ",\"ts_epoch\":%s"
                   ",\"mac\":\"%s\",\"rssi\":%d,\"ch\":%u"
                   ",\"type\":\"UNKNOWN\",\"magic\":\"0x%02X\""
                   ",\"msg_type\":\"0x%02X\",\"len\":%d}\n",
                   evt.ts_ms, epoch_str, mac_str, (int)evt.rssi, evt.channel,
                   evt.raw_len > 0 ? evt.raw[0] : 0,
                   evt.raw_len > 1 ? evt.raw[1] : 0,
                   evt.raw_len);
            fflush(stdout);
            continue;
        }

        /* flags_json() buffer -- 96 bytes covers the fixed-length fragment */
        char flags_buf[96];

        if (evt.raw[1] == (uint8_t)ESPNOW_MSG_STATUS) {
            /* ── STATUS heartbeat ──────────────────────────────────────────
             * Decode all fields from espnow_status_msg_t.
             *
             * Key fields for fault diagnosis:
             *   http_recent=false → node has lost end-to-end connectivity
             *   lux_alive=false   → lux_task has stalled (minimal nodes)
             *   http_stale_m      → minutes since last good HTTP fetch;
             *                       rising value = network degrading
             *   seq               → TX counter; gaps = air congestion / loss,
             *                       NOT a silent node
             *   rssi_node         → node's own AP RSSI (sniffer-side is
             *                       evt.rssi); compare to spot asymmetric RF
             *   uptime_m          → minutes since last reboot             */
            const espnow_status_msg_t *s =
                (const espnow_status_msg_t *)(const void *)evt.raw;
            flags_json(flags_buf, sizeof(flags_buf), s->status_flags);
            printf("{\"ts\":%" PRIu64 ",\"ts_epoch\":%s"
                   ",\"mac\":\"%s\",\"rssi_sniffer\":%d,\"ch\":%u"
                   ",\"type\":\"STATUS\""
                   ",\"seq\":%u"
                   ",\"rssi_node\":%d"
                   ",\"http_stale_m\":%u"
                   ",\"uptime_m\":%u"
                   "%s}\n",
                   evt.ts_ms, epoch_str,
                   mac_str, (int)evt.rssi, evt.channel,
                   (unsigned)s->seq,
                   (int)s->rssi,
                   (unsigned)s->http_stale_m,
                   (unsigned)s->uptime_m,
                   flags_buf);
        } else {
            /* ── SOUND / LIGHT ─────────────────────────────────────────────
             * Decode status_flags on every frame (was reserved=0 in old fw;
             * flags=0x00 is printed as all-false, which is readable).      */
            const espnow_msg_t *msg = (const espnow_msg_t *)evt.raw;
            float lux = 0.0f;
            memcpy(&lux, &msg->lux, sizeof(float));
            flags_json(flags_buf, sizeof(flags_buf), msg->status_flags);

            if (msg->msg_type == (uint8_t)ESPNOW_MSG_SOUND) {
                const char *det = detection_str(msg->detection);
                if (det) {
                    printf("{\"ts\":%" PRIu64 ",\"ts_epoch\":%s"
                           ",\"mac\":\"%s\",\"rssi\":%d,\"ch\":%u"
                           ",\"type\":\"SOUND\",\"detection\":\"%s\",\"lux\":%.2f"
                           "%s}\n",
                           evt.ts_ms, epoch_str, mac_str, (int)evt.rssi, evt.channel,
                           det, lux, flags_buf);
                } else {
                    printf("{\"ts\":%" PRIu64 ",\"ts_epoch\":%s"
                           ",\"mac\":\"%s\",\"rssi\":%d,\"ch\":%u"
                           ",\"type\":\"SOUND\",\"detection\":\"RAW_0x%02X\",\"lux\":%.2f"
                           "%s}\n",
                           evt.ts_ms, epoch_str, mac_str, (int)evt.rssi, evt.channel,
                           msg->detection, lux, flags_buf);
                }
            } else {
                printf("{\"ts\":%" PRIu64 ",\"ts_epoch\":%s"
                       ",\"mac\":\"%s\",\"rssi\":%d,\"ch\":%u"
                       ",\"type\":\"%s\",\"detection\":null,\"lux\":%.2f"
                       "%s}\n",
                       evt.ts_ms, epoch_str, mac_str, (int)evt.rssi, evt.channel,
                       msg_type_str(msg->msg_type), lux, flags_buf);
            }
        }

        fflush(stdout);
    }
}

/* =========================================================================
 * WiFi EVENT HANDLER
 * ========================================================================= */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disc =
                (wifi_event_sta_disconnected_t *)data;

            bool is_epoch;
            uint64_t ts = get_ts_ms(&is_epoch);
            printf("{\"ts\":%" PRIu64 ",\"ts_epoch\":%s"
                   ",\"event\":\"WIFI_DISCONNECTED\",\"reason\":%d}\n",
                   ts, is_epoch ? "true" : "false", (int)disc->reason);
            fflush(stdout);

            if (s_wifi_retries < WIFI_MAX_RETRIES) {
                s_wifi_retries++;
                ESP_LOGW(TAG, "WiFi disconnected -- retry %d/%d",
                         s_wifi_retries, WIFI_MAX_RETRIES);
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_wifi_retries = 0;

        esp_wifi_set_ps(WIFI_PS_NONE);

        uint8_t ch = 0;
        wifi_second_chan_t sec;
        esp_wifi_get_channel(&ch, &sec);

        bool is_epoch;
        uint64_t ts = get_ts_ms(&is_epoch);
        printf("{\"ts\":%" PRIu64 ",\"ts_epoch\":%s"
               ",\"event\":\"WIFI_CONNECTED\""
               ",\"ip\":\"" IPSTR "\",\"ch\":%u}\n",
               ts, is_epoch ? "true" : "false",
               IP2STR(&ev->ip_info.ip), ch);
        fflush(stdout);

        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

/* =========================================================================
 * WiFi INITIALISATION
 * ========================================================================= */

static bool wifi_init_sta(void)
{
    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any_id;
    esp_event_handler_instance_t inst_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_got_ip));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid,     WIFI_SSID,     sizeof(wifi_cfg.sta.ssid)     - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to AP \"%s\" ...", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return true;
    }

    ESP_LOGW(TAG, "WiFi connection failed -- running in uptime-timestamp mode");
    printf("{\"ts\":%" PRIu64 ",\"ts_epoch\":false"
           ",\"event\":\"WIFI_CONNECT_FAILED\""
           ",\"note\":\"ts will be uptime ms; SNTP unavailable\"}\n",
           (uint64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    fflush(stdout);
    return false;
}

/* =========================================================================
 * ESP-NOW INITIALISATION
 * ========================================================================= */

static void espnow_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_espnow_recv));

    esp_now_peer_info_t peer = {0};
    memset(peer.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.encrypt = false;
    peer.ifidx   = ESP_IF_WIFI_STA;

    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "Failed to add broadcast peer: %s", esp_err_to_name(err));
    }

    uint8_t ch = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&ch, &sec);

    bool is_epoch;
    uint64_t ts = get_ts_ms(&is_epoch);
    printf("{\"ts\":%" PRIu64 ",\"ts_epoch\":%s"
           ",\"event\":\"ESPNOW_READY\",\"ch\":%u}\n",
           ts, is_epoch ? "true" : "false", ch);
    fflush(stdout);

    ESP_LOGI(TAG, "ESP-NOW ready -- listening on channel %u", ch);
}

/* =========================================================================
 * ENTRY POINT
 * ========================================================================= */

void app_main(void)
{
    printf("\n");
    printf("{\"event\":\"SNIFFER_BOOT\",\"ts\":0,\"ts_epoch\":false"
           ",\"firmware\":\"echoes-sniffer\"}\n");
    fflush(stdout);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Echoes of the Machine -- Sniffer Node  ");
    ESP_LOGI(TAG, "========================================");

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS issue -- erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_event_t));
    if (!s_rx_queue) {
        ESP_LOGE(TAG, "Failed to create RX queue -- halting");
        return;
    }

    /* print_task: priority 5 — must drain the queue faster than the WiFi
     * task fills it.  Higher than stats_task to keep latency low.          */
    if (xTaskCreate(print_task, "print", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create print task -- halting");
        return;
    }

    /* stats_task: priority 2 — background, wakes once per STATS_INTERVAL_S */
    if (xTaskCreate(stats_task, "stats", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create stats task -- halting");
        return;
    }

    bool wifi_ok = wifi_init_sta();

    if (wifi_ok) {
        sntp_start();
    }

    espnow_init();

    ESP_LOGI(TAG, "Sniffer running -- fleet traffic echoed to UART0 "
                  "(stats every %ds, ts_epoch=%s)",
             STATS_INTERVAL_S, wifi_ok ? "pending SNTP sync" : "false/uptime only");
}
