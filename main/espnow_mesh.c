/**
 * @file espnow_mesh.c
 * @brief ESP-NOW broadcast mesh implementation for Echoes of the Machine
 *
 * Design decisions
 * ----------------
 * - Broadcast-only (FF:FF:FF:FF:FF:FF).  No ACK, no pairing table to manage.
 * - Messages are tiny (8 bytes) to stay well within the 250-byte ESP-NOW limit.
 * - Received messages are handled in the ESP-NOW callback (ISR-safe) by
 *   posting to a FreeRTOS queue; a dedicated lightweight task drains the queue
 *   and calls bird_mapper_update_for_lux() or records the remote sound event.
 * - A TTL mechanism (ESPNOW_EVENT_TTL_MS) reverts the local mapper to its own
 *   lux-based selection when no remote events arrive for a while.
 */

#include "espnow_mesh.h"
#include "synthesis.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "markov.h"
#include "remote_config.h"
#include <string.h>
#include <math.h>
#include "esp_task_wdt.h"

static const char *TAG = "ESPNOW";

/* Broadcast MAC */
static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] =
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

static bird_call_mapper_t *s_mapper          = NULL;
static markov_chain_t     *s_markov          = NULL;
static QueueHandle_t       s_rx_queue        = NULL;

/* Last lux value we broadcast — used to suppress redundant transmissions */
static float               s_last_tx_lux     = -1000.0f;

/* ---- ESP-NOW flock mode detection ------------------------------------- */
/* Oversized ring buffer — FLOCK_RING_MAX slots (one per node in the fleet).
 * The actual trigger count is read at runtime from remote_config so it can
 * be tuned without reflashing.  Only the oldest FLOCK_MSG_COUNT slot is
 * ever compared against the window; the extra slots are simply unused.     */
static uint32_t            s_rx_times[FLOCK_RING_MAX] = {0};
static uint8_t             s_rx_head     = 0;       /* next slot to write   */
static bool                s_flock_active = false;
static uint32_t            s_flock_last_ms = 0;     /* last trigger time    */
static uint32_t            s_boot_ms = 0;           /* millis() at init — used for grace period */

/* Timestamp (ms) of the last remote event that influenced our bird set */
static uint32_t            s_last_remote_event_ms = 0;

/* Most recent remote lux we received — applied until TTL expires */
static float               s_remote_lux      = -1.0f;

/* Our own last-known lux — to restore after TTL */
static float               s_local_lux       = -1.0f;

/* Timestamp of last sound broadcast */
static uint32_t s_last_sound_broadcast_ms = 0;

/* Guard against double-initialisation */
static bool s_initialized = false;

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

static uint32_t millis(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ========================================================================
 * ESP-NOW CALLBACKS  (called from WiFi task context)
 * ======================================================================== */

static void on_data_recv(const esp_now_recv_info_t *recv_info,
                         const uint8_t *data, int len)
{
    if (len != sizeof(espnow_msg_t)) return;

    const espnow_msg_t *msg = (const espnow_msg_t *)data;
    if (msg->magic != ESPNOW_MAGIC) return;

    /* Record arrival timestamp for flock mode detection.
     * HEARTBEAT frames are excluded — they are radio keepalives with no
     * application meaning and must not contribute to flock detection.
     * With 25 minimal nodes heartbeating every 2 s, including them would
     * produce 75 events per 6 s window and permanently lock flock mode on. */
    if (msg->msg_type != (uint8_t)ESPNOW_MSG_HEARTBEAT) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_rx_times[s_rx_head] = now;
        s_rx_head = (s_rx_head + 1) % FLOCK_RING_MAX;
    }

    /* Post a copy to the processing queue (non-blocking). */
    espnow_msg_t copy = *msg;
    if (s_rx_queue) {
        xQueueSend(s_rx_queue, &copy, 0);
    }
}

static void on_data_sent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    /* Broadcast status is always ESP_NOW_SEND_SUCCESS on the TX side;
       we don't care about delivery confirmation for broadcast. */
    (void)tx_info;
    (void)status;
}

/* ========================================================================
 * RECEIVE PROCESSING TASK
 * ======================================================================== */

/**
 * @brief Translate a remote lux reading into a mood-biased lux value.
 *
 * Rather than overriding the local bird set entirely, we blend the remote
 * lux with the local lux so nearby nodes influence but don't fully control
 * each other.  50/50 blend keeps things cooperative without being jarring.
 */
static float blend_lux(float local, float remote)
{
    if (local < 0.0f) return remote;    // No local reading yet
    if (remote < 0.0f) return local;
    return (local * 0.5f) + (remote * 0.5f);
}

static void espnow_rx_task(void *param)
{
    (void)param;
    espnow_msg_t msg;

    /* ── Subscribe to the Task Watchdog ─────────────────────────────────
     *
     * This task runs on every node type (full and minimal).  Without this
     * subscription the TWDT has no visibility into whether the rx loop is
     * alive.  The queue receive blocks for up to 1 s per iteration, well
     * inside WDT_TIMEOUT_S, so false trips are not a concern.            */
    {
        esp_err_t wdt_err = esp_task_wdt_add(NULL);
        if (wdt_err != ESP_OK) {
            ESP_LOGW(TAG, "espnow_rx: could not subscribe to watchdog: %s",
                     esp_err_to_name(wdt_err));
        }
    }

    while (1) {
        /* Feed the TWDT unconditionally at the top of every iteration so
         * both the "message received" and "queue timeout" paths are covered. */
        esp_task_wdt_reset();

        if (xQueueReceive(s_rx_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {

            /* Minimal nodes have no Markov chain — flock timestamps are
             * already recorded in the on_data_recv callback.  Drain the
             * queue in one burst and yield, rather than looping through
             * individual messages with `continue` (which prevented the
             * periodic maintenance block below from ever running and kept
             * the task spinning at 50+ wakeups/second for no purpose).   */
            if (s_markov == NULL) {
                /* Drain remaining messages in a single burst. */
                while (xQueueReceive(s_rx_queue, &msg, 0) == pdTRUE) { }
                /* Fall through to periodic maintenance below. */
            } else {

            switch ((espnow_msg_type_t)msg.msg_type) {

            case ESPNOW_MSG_SOUND: {
                /*
                 * A neighbour detected a sound.  We shift the local bird
                 * selection toward the mood implied by that detection type:
                 *   Whistle → active/bright birds
                 *   Voice   → mellow/mid birds
                 *   Clap    → loud/energetic birds
                 *
                 * We do this by temporarily adjusting the effective lux:
                 *   Whistle/Clap → pretend it's bright (800 lux)
                 *   Voice        → pretend it's dusk (80 lux)
                 */
                float implied_lux;
                switch ((detection_type_t)msg.detection) {
                    case DETECTION_WHISTLE:   implied_lux = 800.0f; break;
                    case DETECTION_CLAP:      implied_lux = 900.0f; break;
                    case DETECTION_VOICE:     implied_lux = 80.0f;  break;
                    case DETECTION_BIRDSONG:  implied_lux = 700.0f; break;  /* melodic/daylight */
                    default:                  implied_lux = s_local_lux; break;
                }

                s_remote_lux = implied_lux;
                s_last_remote_event_ms = millis();

                float effective = blend_lux(s_local_lux, s_remote_lux);
                if (s_mapper) bird_mapper_update_for_lux(s_mapper, effective);

                /* Teach the Markov chain about this remote event */
                if (s_markov) markov_on_event(s_markov, (detection_type_t)msg.detection, s_local_lux);

                /* Demoted from ESP_LOGI: this fires ~50 times/s on minimal
                 * nodes and saturated the UART, backing up the rx task.   */
                ESP_LOGD(TAG, "Remote sound (%d) → effective lux %.0f",
                         msg.detection, effective);
                break;
            }

            case ESPNOW_MSG_LIGHT: {
                /*
                 * A neighbour is reporting its ambient light.
                 * Blend with our own reading to get a network-wide mood.
                 */
                s_remote_lux = msg.lux;
                s_last_remote_event_ms = millis();

                float effective = blend_lux(s_local_lux, s_remote_lux);
                if (s_mapper) bird_mapper_update_for_lux(s_mapper, effective);

                /* Update chain with remote light info — use markov_set_lux()
                 * rather than markov_on_event() so that routine ambient light
                 * broadcasts do NOT reset the silence timer.  Only genuine
                 * sound events (ESPNOW_MSG_SOUND) should count as "activity"
                 * for the purpose of the autonomous-call idle trigger.        */
                if (s_markov) markov_set_lux(s_markov, msg.lux);

                /* Also apply Markov lux bias to local mapper */
                if (s_markov && s_mapper) {
                    float bias = markov_get_lux_bias(s_markov);
                    bird_mapper_update_for_lux(s_mapper, effective + bias);
                }

                /* Demoted from ESP_LOGI: fires ~50 times/s on minimal nodes */
                ESP_LOGD(TAG, "Remote lux %.1f → effective lux %.0f",
                         msg.lux, effective);
                break;
            }

            case ESPNOW_MSG_HEARTBEAT:
                /* Intentionally empty — sent by minimal nodes to keep their
                 * radio hardware's 802.11 transmit state machine active.
                 * Carries no application data; all receivers discard it.   */
                break;

            default:
                break;
            }

            } /* end of else (s_markov != NULL) */
        }

        /* ── Periodic maintenance (runs on every 1-second queue timeout) ──
         *
         * TTL expiry and Markov tick are deliberately placed here, outside
         * the "message received" block, so they fire once per second rather
         * than once per incoming message.  With 25 full nodes broadcasting
         * at ~2 Hz each, running these on every message would execute them
         * ~50 times/second — wasteful and incorrect for time-based logic.
         * The 1-second queue timeout means this runs at ~1 Hz regardless
         * of message rate, which matches the intent of both functions.     */
        espnow_mesh_tick();
        if (s_markov) markov_tick(s_markov);
    }
}

/* ========================================================================
 * PUBLIC API
 * ======================================================================== */

bool espnow_mesh_init(bird_call_mapper_t *mapper, markov_chain_t *mc)
{
    if (s_initialized) {
        /* Called a second time (e.g. scheduling race during boot).
         * Just update the module references and return — the radio stack
         * is already up and the broadcast peer is already registered.   */
        ESP_LOGW(TAG, "Already initialised — updating references only");
        s_mapper = mapper;
        s_markov = mc;
        return true;
    }

    /* Guard the Markov pointer */
    s_markov = mc;
    s_mapper = mapper;

    /* Create receive queue (holds up to 8 messages) */
    s_rx_queue = xQueueCreate(32, sizeof(espnow_msg_t));  // was 8
    if (!s_rx_queue) {
        ESP_LOGE(TAG, "Failed to create RX queue");
        return false;
    }

    /* Initialise ESP-NOW */
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Register callbacks */
    esp_now_register_recv_cb(on_data_recv);
    esp_now_register_send_cb(on_data_sent);

    /*
     * Add the broadcast peer.  ESP-IDF v5.x requires peers to be added even
     * for the broadcast address.  We set encrypt = false and channel = 0
     * (current channel).
     */
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    peer.channel  = 0;      /* 0 = use current WiFi channel */
    peer.encrypt  = false;
    peer.ifidx    = ESP_IF_WIFI_STA;

    err = esp_now_add_peer(&peer);
    if (err == ESP_ERR_ESPNOW_EXIST) {
        /* Peer was registered in a previous (partial) init — update it. */
        ESP_LOGW(TAG, "Broadcast peer already exists — calling esp_now_mod_peer()");
        err = esp_now_mod_peer(&peer);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure broadcast peer: %s", esp_err_to_name(err));
        esp_now_deinit();
        return false;
    }

    /* Start the receive processing task */
    BaseType_t rc = xTaskCreate(espnow_rx_task, "espnow_rx", 3072, NULL, 4, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create espnow_rx task");
        esp_now_deinit();
        return false;
    }

    ESP_LOGI(TAG, "ESP-NOW mesh initialised (broadcast-only)");

    s_initialized = true;
    s_boot_ms     = millis();   /* start of grace period — flock suppressed for FLOCK_GRACE_MS */

    /* Seed local lux now so blend_lux() has a valid reference immediately,
     * even if the lux broadcast task hasn't fired yet.  A negative result
     * (sensor not ready) leaves s_local_lux at its default -1000.0f and
     * blend_lux() will fall back to the remote value until the first real
     * reading arrives.                                                      */
    {
        /* get_lux_level() is declared in echoes.h which is already included
         * via espnow_mesh.h → echoes.h.                                     */
        float initial_lux = get_lux_level();
        if (initial_lux >= 0.0f) {
            s_local_lux = initial_lux;
            ESP_LOGI(TAG, "Initial local lux seeded: %.1f", s_local_lux);
        }
    }

    return true;
}

void espnow_mesh_broadcast_sound(detection_type_t detection)
{
    uint32_t now = millis();
    if (now - s_last_sound_broadcast_ms < remote_config_get()->espnow_sound_throttle_ms) {
        ESP_LOGD(TAG, "Sound broadcast throttled (detection %d) — too soon", detection);
        return;
    }

    espnow_msg_t msg = {
        .magic     = ESPNOW_MAGIC,
        .msg_type  = (uint8_t)ESPNOW_MSG_SOUND,
        .detection = (uint8_t)detection,
        .reserved  = 0,
        .lux       = 0.0f,
    };

    esp_err_t err = esp_now_send(BROADCAST_MAC,
                                 (const uint8_t *)&msg,
                                 sizeof(msg));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sound broadcast failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "Broadcast sound event %d", detection);
    }
}

void espnow_mesh_broadcast_light(float lux)
{
    /* Only broadcast if the change is significant */
    if (fabsf(lux - s_last_tx_lux) < remote_config_get()->espnow_lux_threshold) return;

    /* Update local lux record before blending */
    s_local_lux   = lux;
    s_last_tx_lux = lux;

    /* Teach the chain our own lux transition */
    if (s_markov) markov_set_lux(s_markov, lux);

    espnow_msg_t msg = {
        .magic     = ESPNOW_MAGIC,
        .msg_type  = (uint8_t)ESPNOW_MSG_LIGHT,
        .detection = 0,
        .reserved  = 0,
        .lux       = lux,
    };

    esp_err_t err = esp_now_send(BROADCAST_MAC,
                                 (const uint8_t *)&msg,
                                 sizeof(msg));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Light broadcast failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Broadcast lux %.1f", lux);
    }
}

void espnow_mesh_broadcast_heartbeat(void)
{
    /* Send an ESPNOW_MSG_HEARTBEAT frame.  The payload carries no data;
     * all receivers discard it in the rx task switch.  The purpose is
     * purely to execute a complete 802.11 CSMA/CA transmit cycle on the
     * minimal node's radio hardware, keeping the transmit state machine
     * active so the AP's null-frame keepalive probes are reliably ACKed.
     *
     * This is the root-cause fix for minimal nodes silently dropping off
     * the MikroTik AP: they receive ~50 ESP-NOW frames/s from full nodes
     * but almost never transmit, leaving the radio in a receive-dominant
     * state where hardware MAC ACKs can be missed.                       */
    espnow_msg_t msg = {
        .magic     = ESPNOW_MAGIC,
        .msg_type  = (uint8_t)ESPNOW_MSG_HEARTBEAT,
        .detection = 0,
        .reserved  = 0,
        .lux       = 0.0f,
    };

    esp_err_t err = esp_now_send(BROADCAST_MAC,
                                 (const uint8_t *)&msg,
                                 sizeof(msg));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Heartbeat broadcast failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "Heartbeat broadcast sent");
    }
}

bool espnow_mesh_is_flock_mode(void)
{
    remote_config_t cfg;
    if (!remote_config_snapshot(&cfg)) {
        /* Fallback to compile-time defaults if mutex is contended */
        cfg.flock_grace_ms  = FLOCK_GRACE_MS;
        cfg.flock_msg_count = FLOCK_MSG_COUNT;
        cfg.flock_window_ms = FLOCK_WINDOW_MS;
        cfg.flock_hold_ms   = FLOCK_HOLD_MS;
    }

    /* Clamp to safe ring-buffer limits */
    uint32_t flock_count  = cfg.flock_msg_count;
    if (flock_count < 2)              flock_count = 2;
    if (flock_count > FLOCK_RING_MAX) flock_count = FLOCK_RING_MAX;

    uint32_t flock_window = cfg.flock_window_ms;
    uint32_t now          = millis();

    /* Boot grace period — suppress flock mode for cfg.flock_grace_ms after init.
     *
     * On a simultaneous restart all nodes flood each other with ESP-NOW
     * broadcasts during the OTA check / remote-config fetch window, trivially
     * crossing the flock threshold before any audio task is stable.  The
     * first node to attempt a bird call hits i2s_channel_write while the
     * speaker DMA is still settling, potentially blocking forever with the
     * LED stuck on.  Suppressing flock for the grace period gives every node
     * time to complete its boot sequence before the first flock event fires.
     * Remotely configurable — set to 0 to disable (not recommended). */
    if (s_boot_ms != 0 && (now - s_boot_ms) < cfg.flock_grace_ms) {
        return false;
    }

    uint8_t oldest_slot = (uint8_t)
        ((s_rx_head + FLOCK_RING_MAX - (uint8_t)flock_count) % FLOCK_RING_MAX);
    uint32_t oldest = s_rx_times[oldest_slot];

    bool newly_triggered = (oldest != 0) && ((now - oldest) <= flock_window);

    if (newly_triggered) {
        s_flock_last_ms = now;
        if (!s_flock_active) {
            s_flock_active = true;
            ESP_LOGI(TAG, "🐦 FLOCK MODE entered (%lu msgs / %lu ms window)",
                     flock_count, flock_window);
        }
    } else if (s_flock_active) {
        if ((now - s_flock_last_ms) >= cfg.flock_hold_ms) {
            s_flock_active = false;
            ESP_LOGI(TAG, "🐦 FLOCK MODE exited (hold expired)");
        }
    }

    return s_flock_active;
}

markov_chain_t *espnow_mesh_get_markov(void)
{
    return s_markov;
}

void espnow_mesh_tick(void)
{
    if (s_remote_lux < 0.0f) return;   /* No remote influence active */

    uint32_t now     = millis();
    uint32_t elapsed = now - s_last_remote_event_ms;

    if (elapsed >= remote_config_get()->espnow_event_ttl_ms) {
        ESP_LOGI(TAG, "Remote event TTL expired — reverting to local lux %.1f",
                 s_local_lux);
        s_remote_lux = -1.0f;
        /* Restore local lux-based selection with Markov bias */
        if (s_mapper && s_local_lux >= 0.0f) {
            float bias = s_markov ? markov_get_lux_bias(s_markov) : 0.0f;
            bird_mapper_update_for_lux(s_mapper, s_local_lux + bias);
        }
    }
}
