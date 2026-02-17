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
 * Minimum lux change (absolute) that triggers a broadcast.
 * Prevents flooding the network with tiny sensor fluctuations.
 */
#define ESPNOW_LUX_THRESHOLD    20.0f

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

#endif /* ESPNOW_MESH_H */
