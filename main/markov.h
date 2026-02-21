/**
 * @file markov.h
 * @brief Network-aware Markov chain for Echoes of the Machine
 *
 * Models the joint distribution of (detection_type × light_band) observed
 * across the ESP-NOW mesh.  Each node independently maintains a 17×17
 * transition count matrix that is updated every time any node on the network
 * fires an event (local OR received via ESP-NOW).
 *
 * States
 * ------
 *  0–3   : IDLE   × {NIGHT, DAWN, OVERCAST, SUNNY}   (no sound, just ambient)
 *  4–7   : WHISTLE× {NIGHT, DAWN, OVERCAST, SUNNY}
 *  8–11  : VOICE  × {NIGHT, DAWN, OVERCAST, SUNNY}
 *  12–15 : CLAP   × {NIGHT, DAWN, OVERCAST, SUNNY}
 *  16    : STARTUP (initial / unknown)
 *
 *  Total: MARKOV_NUM_STATES = 17
 *
 * Outputs
 * -------
 *  1. Lux bias   — current state maps to a signed lux offset that is added
 *                  to the raw sensor reading before bird selection.
 *  2. Autonomous call — when the network has been silent for
 *                  MARKOV_IDLE_TRIGGER_MS ms, the chain samples the most
 *                  probable next state and fires a bird call for it.
 *
 * Persistence
 * -----------
 *  The raw transition count matrix is saved to NVS (namespace "markov",
 *  key "counts") after every MARKOV_NVS_SAVE_INTERVAL updates.
 *  On boot the saved matrix is restored; if no saved data is found the matrix
 *  is seeded with ecologically plausible Laplace-smoothed priors.
 */

#ifndef MARKOV_H
#define MARKOV_H

#include <stdint.h>
#include <stdbool.h>
#include "echoes.h"
#include "synthesis.h"

/* ========================================================================
 * CONFIGURATION — tune these for your installation
 * ======================================================================== */

/** Total number of compound states (4 detections × 4 bands + 1 startup). */
#define MARKOV_NUM_STATES           17

/** Initial pseudo-count added to every cell to avoid zero-probability
 *  transitions (Laplace / add-k smoothing).  Higher = slower learning. */
#define MARKOV_LAPLACE_K            2.0f

/** Number of events between NVS saves (reduces flash write cycles). */
#define MARKOV_NVS_SAVE_INTERVAL    100

/** How long (ms) without any network event before autonomous playback fires. */
#define MARKOV_IDLE_TRIGGER_MS      45000   /* 45 s */

/** Minimum gap (ms) between consecutive autonomous calls. */
#define MARKOV_AUTONOMOUS_COOLDOWN_MS  15000   /* 15 s */

/** Maximum lux bias (lux units) the chain can apply in either direction. */
#define MARKOV_MAX_LUX_BIAS         300.0f

/** NVS namespace and key used for persistence. */
#define MARKOV_NVS_NAMESPACE        "markov"
#define MARKOV_NVS_KEY              "counts"

/* ========================================================================
 * LIGHT BAND ENUMERATION
 * (mirrors the thresholds in bird_mapper_update_for_lux)
 * ======================================================================== */

typedef enum {
    LIGHT_BAND_NIGHT    = 0,   /**< lux <  10   */
    LIGHT_BAND_DAWN     = 1,   /**< lux <  100  */
    LIGHT_BAND_OVERCAST = 2,   /**< lux <  500  */
    LIGHT_BAND_SUNNY    = 3,   /**< lux >= 500  */
    LIGHT_BAND_COUNT    = 4
} light_band_t;

/* ========================================================================
 * STATE INDEX HELPERS
 * ======================================================================== */

/** Encode a (detection × band) pair as a state index [0..15].
 *  detection: 0=IDLE, 1=WHISTLE, 2=VOICE, 3=CLAP
 *  band:      0=NIGHT … 3=SUNNY                                          */
#define MARKOV_STATE(detection_idx, band_idx) \
    ((uint8_t)((detection_idx) * LIGHT_BAND_COUNT + (band_idx)))

/** Special startup state. */
#define MARKOV_STATE_STARTUP        16

/* ========================================================================
 * MARKOV CHAIN STRUCTURE
 * ======================================================================== */

/**
 * @brief Markov chain instance.
 *
 * Kept opaque-ish — access via the API functions below rather than directly
 * touching the fields, except for diagnostics.
 */
typedef struct markov_chain_t {
    /** Raw transition counts [from][to].  Persisted to NVS. */
    uint16_t counts[MARKOV_NUM_STATES][MARKOV_NUM_STATES];

    /** Normalised transition probabilities [from][to].  Recomputed on demand. */
    float probs[MARKOV_NUM_STATES][MARKOV_NUM_STATES];

    /** Index of the current state. */
    uint8_t current_state;

    /** Timestamp (ms) of the last event processed. */
    uint32_t last_event_ms;

    /** Timestamp (ms) of the last autonomous call. */
    uint32_t last_autonomous_ms;

    /** Events processed since the last NVS save. */
    uint16_t events_since_save;

    /** Whether probability rows need recomputing. */
    bool probs_dirty;

    /** Pointer to the application bird mapper. */
    bird_call_mapper_t *mapper;

    /** Most recent local lux (for state composition). */
    float local_lux;
} markov_chain_t;

/* ========================================================================
 * PUBLIC API
 * ======================================================================== */

/**
 * @brief Initialise the Markov chain.
 *
 * Attempts to load a previously saved count matrix from NVS.  If none is
 * found, seeds with Laplace-smoothed priors that favour ecologically
 * plausible sequences (e.g. DAWN events leading to OVERCAST events).
 *
 * @param mc      Chain instance to initialise.
 * @param mapper  Application bird mapper (for autonomous call generation).
 */
void markov_init(markov_chain_t *mc, bird_call_mapper_t *mapper);

/**
 * @brief Notify the chain that a (detection, lux) event just occurred.
 *
 * Call this for BOTH locally detected events AND events received via ESP-NOW.
 * This is the primary learning input.
 *
 * @param mc         Chain instance.
 * @param detection  Detection type (NONE = treat as IDLE).
 * @param lux        Ambient lux at the time of the event (local sensor value;
 *                   use current local lux even for remote events).
 */
void markov_on_event(markov_chain_t *mc, detection_type_t detection, float lux);

/**
 * @brief Update local lux without a sound event (call from lux task).
 *
 * Keeps the chain aware of the current light band so autonomous calls are
 * composed with the correct compound state.
 *
 * @param mc   Chain instance.
 * @param lux  Current local lux.
 */
void markov_set_lux(markov_chain_t *mc, float lux);

/**
 * @brief Compute the lux bias suggested by the current chain state.
 *
 * Returns a signed value in [-MARKOV_MAX_LUX_BIAS, +MARKOV_MAX_LUX_BIAS].
 * Add this to the raw sensor lux before calling bird_mapper_update_for_lux().
 *
 * @param mc  Chain instance.
 * @return    Signed lux bias.
 */
float markov_get_lux_bias(const markov_chain_t *mc);

/**
 * @brief Periodic tick — call from a task at roughly 1 Hz.
 *
 * Checks whether the idle threshold has been reached and, if so, samples the
 * chain to fire an autonomous bird call.  Also triggers pending NVS saves.
 *
 * @param mc  Chain instance.
 */
void markov_tick(markov_chain_t *mc);

/**
 * @brief Force an immediate NVS save (e.g. before a planned reboot).
 *
 * @param mc  Chain instance.
 */
void markov_save(markov_chain_t *mc);

/**
 * @brief Erase saved NVS data and reset counts to priors.
 *
 * Useful for factory-reset or during development.
 *
 * @param mc  Chain instance.
 */
void markov_reset(markov_chain_t *mc);

/**
 * @brief Log the top-3 most probable next states from the current state.
 *
 * Diagnostic helper — outputs via ESP_LOGI.
 *
 * @param mc  Chain instance.
 */
void markov_log_top_transitions(markov_chain_t *mc);

/* ========================================================================
 * CONVERSION HELPERS (also used by espnow_mesh.c)
 * ======================================================================== */

/**
 * @brief Map a lux value to the corresponding light band.
 */
light_band_t markov_lux_to_band(float lux);

/**
 * @brief Map a detection_type_t + light_band_t to a state index.
 *
 * detection_type_t DETECTION_NONE → IDLE row (0).
 */
uint8_t markov_make_state(detection_type_t detection, light_band_t band);

/**
 * @brief Decode a state index back to a (detection_idx, band) pair.
 */
void markov_decode_state(uint8_t state, uint8_t *detection_idx, light_band_t *band);

/** Human-readable state name for logging. */
const char *markov_state_name(uint8_t state);

#endif /* MARKOV_H */
