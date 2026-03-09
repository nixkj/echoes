/**
 * @file espnow_mesh.h
 * @brief ESP-NOW broadcast mesh for Echoes of the Machine
 *
 * Each node broadcasts short messages when:
 *   - A sound event is detected (whistle / voice / clap)
 *   - The light level changes significantly
 *
 * Receiving nodes use these messages to bias their bird-call selection
 * toward the appropriate mood (mellow for low light, lively for bright).
 *
 * All transmissions are broadcast (FF:FF:FF:FF:FF:FF) so no pairing is
 * required and every node in range receives every message.
 */

#ifndef ESPNOW_MESH_H
#define ESPNOW_MESH_H

#include <stdint.h>
#include <stdbool.h>
#include "echoes.h"       // detection_type_t, bird_call_mapper_t
#include "synthesis.h"    // bird_call_mapper_t
#include "markov.h"

/* ========================================================================
 * CONFIGURATION
 * ======================================================================== */

/**
 * Minimum time (ms) between consecutive sound broadcasts from this node.
 * Prevents flooding during rapid repeated detections.
 */
#define ESPNOW_SOUND_THROTTLE_MS    3000   // 3 seconds — tune based on testing

/**
 * Minimum lux change (absolute) that triggers a broadcast.
 * Prevents flooding the network with tiny sensor fluctuations.
 * Lowered to 5 lux for dark indoor installations where phone screens
 * and torches create changes in the 5–200 lux range.
 */
#define ESPNOW_LUX_THRESHOLD    12.0f

/**
 * How long (ms) a remote event influences local bird selection before
 * the node reverts to its own lux-based defaults.
 */
#define ESPNOW_EVENT_TTL_MS     30000   // 30 seconds

/* ========================================================================
 * MESSAGE FORMAT
 * ======================================================================== */

#define ESPNOW_MAGIC    0xEC  /**< Simple sanity byte to filter alien traffic */

typedef enum {
    ESPNOW_MSG_SOUND   = 1, /**< A sound event was detected                  */
    ESPNOW_MSG_LIGHT   = 2, /**< Light level changed significantly            */
    ESPNOW_MSG_STATUS  = 3, /**< Periodic node health heartbeat               */
} espnow_msg_type_t;

/* ── Node health flags — carried in status_flags byte of every message ───
 *
 * Included in all outgoing messages so that every broadcast (not just the
 * 30-second STATUS heartbeat) carries a snapshot of the node's health at
 * that instant.  Five bits; bits 2-0 are reserved for future use.
 *
 * Interpretation when receiving:
 *   WIFI_ASSOC   — driver association still OK.  Note: this does NOT mean
 *                  end-to-end connectivity — the AP can drop a node while
 *                  still reporting association.
 *   HTTP_RECENT  — a successful config fetch occurred within the last 150 s
 *                  (2.5 × the 60 s poll interval).  This IS end-to-end proof.
 *   LUX_ALIVE    — lux_task has stamped its keep-alive within LUX_DEAD_MS
 *                  (10 s).  If clear on a minimal node, lux_task has stalled.
 *   FLOCK        — node is currently in flock mode.
 *   NODE_FULL    — set=full node (BH1750 + audio), clear=minimal node.
 *
 * Backward compatibility: older nodes transmitted 0x00 in the byte that is
 * now status_flags.  Receivers that don't understand the flags will simply
 * see flags=0x00, which is indistinguishable from a healthy minimal node
 * at night with no flock — a benign interpretation.                        */
#define ESPNOW_FLAG_NODE_FULL    (1u << 7)
#define ESPNOW_FLAG_WIFI_ASSOC   (1u << 6)
#define ESPNOW_FLAG_HTTP_RECENT  (1u << 5)
#define ESPNOW_FLAG_LUX_ALIVE    (1u << 4)
#define ESPNOW_FLAG_FLOCK        (1u << 3)
/* bits 2-0: reserved */

/**
 * @brief SOUND / LIGHT on-wire message.
 *
 * Total size: 8 bytes.  The `reserved` byte from earlier revisions is now
 * `status_flags`; on-wire size is unchanged so older receivers are unaffected.
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;         /**< Always ESPNOW_MAGIC                          */
    uint8_t  msg_type;      /**< ESPNOW_MSG_SOUND or ESPNOW_MSG_LIGHT         */
    uint8_t  detection;     /**< detection_type_t (valid for SOUND messages)  */
    uint8_t  status_flags;  /**< ESPNOW_FLAG_* health bits (was reserved=0)   */
    float    lux;           /**< Current lux reading (valid for LIGHT msgs)   */
} espnow_msg_t;

/**
 * @brief Periodic health heartbeat — ESPNOW_MSG_STATUS.
 *
 * Sent every ESPNOW_STATUS_INTERVAL_MS from espnow_rx_task regardless of
 * whether any sound or light events have occurred.  This means a node with
 * a dead lux_task (no lux broadcasts) is still visible on the sniffer every
 * 30 seconds, making the failure mode observable before physical intervention.
 *
 * Same 8-byte footprint as espnow_msg_t.  The on_data_recv callback already
 * gates on len == sizeof(espnow_msg_t), and both structs compile to 8 bytes
 * (enforced by the static assert in espnow_mesh.c).
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;         /**< Always ESPNOW_MAGIC                          */
    uint8_t  msg_type;      /**< Always ESPNOW_MSG_STATUS                     */
    uint8_t  status_flags;  /**< ESPNOW_FLAG_* health bits                    */
    int8_t   rssi;          /**< AP RSSI in dBm as reported by this node;
                             *   0 = could not read (e.g. not associated).
                             *   Note: this is the node's own measurement, not
                             *   the sniffer-side RSSI.                        */
    uint8_t  http_stale_m;  /**< Minutes since last successful HTTP fetch;
                             *   0  = fresh (fetched within the last minute)
                             *   255 = saturated (≥ 255 min since last fetch) */
    uint8_t  seq;           /**< Per-node TX sequence counter, wraps at 255.
                             *   Incremented on every STATUS broadcast.
                             *   Gaps in the sniffer indicate dropped packets
                             *   (air congestion) rather than a silent node.   */
    uint16_t uptime_m;      /**< Minutes since last reboot, saturates at
                             *   65535 (~45 days).                             */
} espnow_status_msg_t;

/* ========================================================================
 * PUBLIC API
 * ======================================================================== */

/**
 * @brief Initialise ESP-NOW.
 *
 * Must be called AFTER WiFi has been initialised (wifi_init_and_connect or
 * at least esp_wifi_start). The WiFi interface is needed for the radio even
 * when ESP-NOW is used standalone.
 *
 * @param mapper  Pointer to the application's bird_call_mapper_t so received
 *                messages can update bird selection immediately.
 * @return true on success
 */
bool espnow_mesh_init(bird_call_mapper_t *mapper, markov_chain_t *mc);

/**
 * @brief Broadcast a sound-detection event to all peers.
 *
 * Call this whenever a whistle / voice / clap is confirmed locally.
 *
 * @param detection  The detection type that was triggered.
 */
void espnow_mesh_broadcast_sound(detection_type_t detection);

/**
 * @brief Broadcast the current lux level to all peers (if changed enough).
 *
 * Compares @p lux against the last broadcast value; only transmits when the
 * difference exceeds ESPNOW_LUX_THRESHOLD.
 *
 * @param lux  Current lux reading.
 */
void espnow_mesh_broadcast_light(float lux);


/**
 * @brief Periodic maintenance — call from a task or main loop.
 *
 * Expires stale remote-event influence after ESPNOW_EVENT_TTL_MS so the node
 * smoothly returns to its own light-level-based selection.
 */
void espnow_mesh_tick(void);

/**
 * @brief Get pointer to the markov chain managed by the mesh layer.
 */
markov_chain_t *espnow_mesh_get_markov(void);

/**
 * @brief Returns true when the network is in "flock mode".
 *
 * Flock mode fires when FLOCK_MSG_COUNT (default 12, overridable via
 * remote_config.flock_msg_count) messages arrive within FLOCK_WINDOW_MS.
 * While active, the flock_task plays bird calls with a 60 % Quelea /
 * 40 % random split and drives the LED into a rapid strobe.
 *
 * The state self-clears after FLOCK_HOLD_MS of inactivity.
 */
bool espnow_mesh_is_flock_mode(void);

#endif /* ESPNOW_MESH_H */
