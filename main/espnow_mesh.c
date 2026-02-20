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
static bool                s_flock_mode  = false;   /* current flock state  */
static bool                s_flock_active = false;
static uint32_t            s_flock_last_ms = 0;     /* last trigger time    */

/* Timestamp (ms) of the last remote event that influenced our bird set */
static uint32_t            s_last_remote_event_ms = 0;

/* Most recent remote lux we received — applied until TTL expires */
static float               s_remote_lux      = -1.0f;

/* Our own last-known lux — to restore after TTL */
static float               s_local_lux       = -1.0f;

/* Timestamp of last sound broadcast */
static uint32_t s_last_sound_broadcast_ms = 0;

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

    /* Record arrival timestamp for flock mode detection */
    {
        uint32_t now = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
        s_rx_times[s_rx_head] = now;
        s_rx_head = (s_rx_head + 1) % FLOCK_RING_MAX;
    }

    /* Post a copy to the processing queue (non-blocking) */
    espnow_msg_t copy = *msg;
    if (s_rx_queue) {
        xQueueSendFromISR(s_rx_queue, &copy, NULL);
    }
}

static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    /* Broadcast status is always ESP_NOW_SEND_SUCCESS on the TX side;
       we don't care about delivery confirmation for broadcast. */
    (void)mac_addr;
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

    while (1) {
        if (xQueueReceive(s_rx_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {

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

                ESP_LOGI(TAG, "Remote sound (%d) → effective lux %.0f",
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

                ESP_LOGI(TAG, "Remote lux %.1f → effective lux %.0f",
                         msg.lux, effective);
                break;
            }

            default:
                break;
            }
        }

        /* TTL expiry check and Markov autonomous call check */
        espnow_mesh_tick();
        if (s_markov) markov_tick(s_markov);
    }
}

/* ========================================================================
 * PUBLIC API
 * ======================================================================== */

bool espnow_mesh_init(bird_call_mapper_t *mapper, markov_chain_t *mc)
{
    s_markov = mc;
    s_mapper = mapper;

    /* Create receive queue (holds up to 8 messages) */
    s_rx_queue = xQueueCreate(8, sizeof(espnow_msg_t));
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
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_add_peer failed: %s", esp_err_to_name(err));
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

bool espnow_mesh_is_flock_mode(void)
{
    /* Read the runtime trigger count from remote config, clamped to the
     * compile-time ring buffer size so we never read out-of-bounds.        */
    uint32_t flock_count  = remote_config_get()->flock_msg_count;
    if (flock_count < 2)              flock_count = 2;
    if (flock_count > FLOCK_RING_MAX) flock_count = FLOCK_RING_MAX;

    uint32_t flock_window = remote_config_get()->flock_window_ms;
    uint32_t now          = millis();

    /* The oldest relevant slot is the one that was written (flock_count)
     * messages ago.  Because s_rx_head always points to the NEXT write slot,
     * we walk back flock_count positions in the ring.                       */
    uint8_t oldest_slot = (uint8_t)
        ((s_rx_head + FLOCK_RING_MAX - (uint8_t)flock_count) % FLOCK_RING_MAX);
    uint32_t oldest = s_rx_times[oldest_slot];

    /* oldest == 0 means the ring hasn't been filled enough yet */
    bool newly_triggered = (oldest != 0) &&
                           ((now - oldest) <= flock_window);

    if (newly_triggered) {
        s_flock_last_ms = now;
        if (!s_flock_active) {
            s_flock_active = true;
            ESP_LOGI(TAG, "🐦 FLOCK MODE entered (%lu msgs / %lu ms window)",
                     flock_count, flock_window);
        }
    } else if (s_flock_active) {
        if ((now - s_flock_last_ms) >= remote_config_get()->flock_hold_ms) {
            s_flock_active = false;
            ESP_LOGI(TAG, "🐦 FLOCK MODE exited (hold expired)");
        }
    }

    if (s_flock_active != s_flock_mode) {
        s_flock_mode = s_flock_active;
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
