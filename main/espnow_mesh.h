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

/** Channel must match the channel used by the WiFi STA interface. */
#define ESPNOW_CHANNEL          1

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
    ESPNOW_MSG_SOUND = 1,   /**< A sound event was detected */
    ESPNOW_MSG_LIGHT = 2,   /**< Light level changed significantly */
} espnow_msg_type_t;

/**
 * @brief On-wire message — kept deliberately small (< 250 bytes ESP-NOW limit).
 *
 * Total size: 8 bytes.
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;         /**< Always ESPNOW_MAGIC                          */
    uint8_t  msg_type;      /**< espnow_msg_type_t                            */
    uint8_t  detection;     /**< detection_type_t (valid for SOUND messages)  */
    uint8_t  reserved;      /**< Padding / future use                         */
    float    lux;           /**< Current lux reading (valid for LIGHT msgs)   */
} espnow_msg_t;

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
