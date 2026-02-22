/**
 * @file markov.c
 * @brief Network-aware Markov chain implementation for Echoes of the Machine
 *
 * See markov.h for a full design description.
 */

#include "markov.h"
#include "echoes.h"
#include "remote_config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <math.h>

static const char *TAG = "MARKOV";

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

static uint32_t markov_millis(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ========================================================================
 * CONVERSION HELPERS
 * ======================================================================== */

light_band_t markov_lux_to_band(float lux)
{
    if (lux < 10.0f)  return LIGHT_BAND_NIGHT;
    if (lux < 100.0f) return LIGHT_BAND_DAWN;
    if (lux < 500.0f) return LIGHT_BAND_OVERCAST;
    return LIGHT_BAND_SUNNY;
}

uint8_t markov_make_state(detection_type_t detection, light_band_t band)
{
    /* Map detection_type_t to a 0-based index:
     *   DETECTION_NONE     → 0  (IDLE row)
     *   DETECTION_WHISTLE  → 1
     *   DETECTION_VOICE    → 2
     *   DETECTION_CLAP     → 3
     *   DETECTION_BIRDSONG → 0  (IDLE row — the Markov state space has four
     *                            sound rows; birdsong is not modelled as a
     *                            separate row.  Birdsong events still update
     *                            the chain via markov_on_event() but are
     *                            treated as ambient/idle transitions so they
     *                            do not inflate whistle/voice/clap counts.)  */
    uint8_t d;
    switch (detection) {
        case DETECTION_WHISTLE: d = 1; break;
        case DETECTION_VOICE:   d = 2; break;
        case DETECTION_CLAP:    d = 3; break;
        default:                d = 0; break;  /* IDLE (includes BIRDSONG) */
    }
    return MARKOV_STATE(d, (uint8_t)band);
}

void markov_decode_state(uint8_t state, uint8_t *detection_idx, light_band_t *band)
{
    if (state >= MARKOV_NUM_STATES) {
        *detection_idx = 0;
        *band = LIGHT_BAND_NIGHT;
        return;
    }
    if (state == MARKOV_STATE_STARTUP) {
        *detection_idx = 0;
        *band = LIGHT_BAND_DAWN;
        return;
    }
    *detection_idx = state / LIGHT_BAND_COUNT;
    *band          = (light_band_t)(state % LIGHT_BAND_COUNT);
}

const char *markov_state_name(uint8_t state)
{
    /* Static string table: 17 states */
    static const char *names[MARKOV_NUM_STATES] = {
        "IDLE+NIGHT",    "IDLE+DAWN",    "IDLE+OVERCAST",    "IDLE+SUNNY",
        "WHISTLE+NIGHT", "WHISTLE+DAWN", "WHISTLE+OVERCAST", "WHISTLE+SUNNY",
        "VOICE+NIGHT",   "VOICE+DAWN",   "VOICE+OVERCAST",   "VOICE+SUNNY",
        "CLAP+NIGHT",    "CLAP+DAWN",    "CLAP+OVERCAST",    "CLAP+SUNNY",
        "STARTUP"
    };
    if (state < MARKOV_NUM_STATES) return names[state];
    return "UNKNOWN";
}

/* ========================================================================
 * PRIOR SEEDING
 *
 * Ecologically motivated defaults for a South African outdoor installation:
 *
 *   - Most activity is during DAWN and SUNNY bands (birds are most active).
 *   - CLAP events tend to be followed by more energetic events (startle).
 *   - WHISTLE events have moderate continuation probability.
 *   - VOICE events often lead back to ambient / IDLE.
 *   - NIGHT is mostly IDLE with occasional owl-like responses.
 *   - All cells start at MARKOV_LAPLACE_K; the priors add extra weight.
 * ======================================================================== */

static void seed_priors(markov_chain_t *mc)
{
    /* Fill with Laplace smoothing base */
    for (int i = 0; i < MARKOV_NUM_STATES; i++)
        for (int j = 0; j < MARKOV_NUM_STATES; j++)
            mc->counts[i][j] = (uint16_t)MARKOV_LAPLACE_K;

    /* Helper macro: add extra weight to a specific transition */
#define ADD_PRIOR(from, to, weight) \
    mc->counts[(from)][(to)] += (uint16_t)(weight)

    /* --- STARTUP always transitions toward dawn-band idle --- */
    ADD_PRIOR(MARKOV_STATE_STARTUP, MARKOV_STATE(0, LIGHT_BAND_DAWN),    8);
    ADD_PRIOR(MARKOV_STATE_STARTUP, MARKOV_STATE(0, LIGHT_BAND_OVERCAST),4);

    /* --- IDLE states: high self-loop during night, dissolve toward activity
     *     during day                                                         */
    ADD_PRIOR(MARKOV_STATE(0, LIGHT_BAND_NIGHT),    MARKOV_STATE(0, LIGHT_BAND_NIGHT),    12);
    ADD_PRIOR(MARKOV_STATE(0, LIGHT_BAND_NIGHT),    MARKOV_STATE(2, LIGHT_BAND_NIGHT),     3); /* voice is mellow */
    ADD_PRIOR(MARKOV_STATE(0, LIGHT_BAND_DAWN),     MARKOV_STATE(1, LIGHT_BAND_DAWN),      6); /* whistle at dawn */
    ADD_PRIOR(MARKOV_STATE(0, LIGHT_BAND_DAWN),     MARKOV_STATE(2, LIGHT_BAND_DAWN),      5);
    ADD_PRIOR(MARKOV_STATE(0, LIGHT_BAND_OVERCAST), MARKOV_STATE(1, LIGHT_BAND_OVERCAST),  5);
    ADD_PRIOR(MARKOV_STATE(0, LIGHT_BAND_OVERCAST), MARKOV_STATE(2, LIGHT_BAND_OVERCAST),  5);
    ADD_PRIOR(MARKOV_STATE(0, LIGHT_BAND_SUNNY),    MARKOV_STATE(1, LIGHT_BAND_SUNNY),     8);
    ADD_PRIOR(MARKOV_STATE(0, LIGHT_BAND_SUNNY),    MARKOV_STATE(3, LIGHT_BAND_SUNNY),     6);

    /* --- WHISTLE tends to continue or escalate to clap in bright light --- */
    ADD_PRIOR(MARKOV_STATE(1, LIGHT_BAND_DAWN),     MARKOV_STATE(1, LIGHT_BAND_DAWN),      5);
    ADD_PRIOR(MARKOV_STATE(1, LIGHT_BAND_DAWN),     MARKOV_STATE(0, LIGHT_BAND_DAWN),      4);
    ADD_PRIOR(MARKOV_STATE(1, LIGHT_BAND_OVERCAST), MARKOV_STATE(1, LIGHT_BAND_OVERCAST),  4);
    ADD_PRIOR(MARKOV_STATE(1, LIGHT_BAND_OVERCAST), MARKOV_STATE(3, LIGHT_BAND_OVERCAST),  3);
    ADD_PRIOR(MARKOV_STATE(1, LIGHT_BAND_SUNNY),    MARKOV_STATE(1, LIGHT_BAND_SUNNY),     6);
    ADD_PRIOR(MARKOV_STATE(1, LIGHT_BAND_SUNNY),    MARKOV_STATE(3, LIGHT_BAND_SUNNY),     5); /* escalate */
    ADD_PRIOR(MARKOV_STATE(1, LIGHT_BAND_NIGHT),    MARKOV_STATE(0, LIGHT_BAND_NIGHT),     6); /* quieten */

    /* --- VOICE resolves gently --- */
    ADD_PRIOR(MARKOV_STATE(2, LIGHT_BAND_NIGHT),    MARKOV_STATE(0, LIGHT_BAND_NIGHT),     8);
    ADD_PRIOR(MARKOV_STATE(2, LIGHT_BAND_DAWN),     MARKOV_STATE(0, LIGHT_BAND_DAWN),      6);
    ADD_PRIOR(MARKOV_STATE(2, LIGHT_BAND_DAWN),     MARKOV_STATE(1, LIGHT_BAND_DAWN),      4);
    ADD_PRIOR(MARKOV_STATE(2, LIGHT_BAND_OVERCAST), MARKOV_STATE(0, LIGHT_BAND_OVERCAST),  5);
    ADD_PRIOR(MARKOV_STATE(2, LIGHT_BAND_SUNNY),    MARKOV_STATE(1, LIGHT_BAND_SUNNY),     5);
    ADD_PRIOR(MARKOV_STATE(2, LIGHT_BAND_SUNNY),    MARKOV_STATE(3, LIGHT_BAND_SUNNY),     3);

    /* --- CLAP is energetic — high self-loop in bright, resolves at night --- */
    ADD_PRIOR(MARKOV_STATE(3, LIGHT_BAND_NIGHT),    MARKOV_STATE(0, LIGHT_BAND_NIGHT),    10);
    ADD_PRIOR(MARKOV_STATE(3, LIGHT_BAND_DAWN),     MARKOV_STATE(1, LIGHT_BAND_DAWN),      5);
    ADD_PRIOR(MARKOV_STATE(3, LIGHT_BAND_DAWN),     MARKOV_STATE(0, LIGHT_BAND_DAWN),      4);
    ADD_PRIOR(MARKOV_STATE(3, LIGHT_BAND_OVERCAST), MARKOV_STATE(3, LIGHT_BAND_OVERCAST),  4);
    ADD_PRIOR(MARKOV_STATE(3, LIGHT_BAND_OVERCAST), MARKOV_STATE(1, LIGHT_BAND_OVERCAST),  4);
    ADD_PRIOR(MARKOV_STATE(3, LIGHT_BAND_SUNNY),    MARKOV_STATE(3, LIGHT_BAND_SUNNY),     7);
    ADD_PRIOR(MARKOV_STATE(3, LIGHT_BAND_SUNNY),    MARKOV_STATE(1, LIGHT_BAND_SUNNY),     5);

#undef ADD_PRIOR
}

/* ========================================================================
 * PROBABILITY NORMALISATION
 * ======================================================================== */

static void recompute_probs(markov_chain_t *mc)
{
    for (int i = 0; i < MARKOV_NUM_STATES; i++) {
        uint32_t row_sum = 0;
        for (int j = 0; j < MARKOV_NUM_STATES; j++)
            row_sum += mc->counts[i][j];

        float inv = (row_sum > 0) ? (1.0f / (float)row_sum) : 0.0f;
        for (int j = 0; j < MARKOV_NUM_STATES; j++)
            mc->probs[i][j] = mc->counts[i][j] * inv;
    }
    mc->probs_dirty = false;
}

/* ========================================================================
 * NVS PERSISTENCE
 * ======================================================================== */

static void nvs_load(markov_chain_t *mc)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MARKOV_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved chain found (%s) — using priors",
                 esp_err_to_name(err));
        return;
    }

    size_t expected = sizeof(mc->counts);
    size_t actual   = expected;
    err = nvs_get_blob(handle, MARKOV_NVS_KEY, mc->counts, &actual);
    nvs_close(handle);

    if (err == ESP_OK && actual == expected) {
        ESP_LOGI(TAG, "Loaded transition counts from NVS (%u bytes)", (unsigned)actual);
    } else {
        ESP_LOGW(TAG, "NVS data invalid (%s, size=%u) — resetting to priors",
                 esp_err_to_name(err), (unsigned)actual);
        seed_priors(mc);
    }
}

static void nvs_save_counts(markov_chain_t *mc)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MARKOV_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(handle, MARKOV_NVS_KEY, mc->counts, sizeof(mc->counts));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Chain counts saved to NVS");
    } else {
        ESP_LOGW(TAG, "NVS save failed: %s", esp_err_to_name(err));
    }
}

/* ========================================================================
 * AUTONOMOUS CALL LOGIC
 * ======================================================================== */

/**
 * @brief Sample the next state from the current row using inverse CDF.
 *
 * Uses esp_random() for hardware randomness, which is cheap on ESP32.
 */
static uint8_t sample_next_state(markov_chain_t *mc)
{
    if (mc->probs_dirty) recompute_probs(mc);

    /* Draw a uniform float in [0, 1) */
    float u = (float)(esp_random() & 0xFFFFFF) / (float)0x1000000;

    float cumulative = 0.0f;
    const float *row = mc->probs[mc->current_state];
    for (int j = 0; j < MARKOV_NUM_STATES; j++) {
        cumulative += row[j];
        if (u < cumulative) return (uint8_t)j;
    }
    return MARKOV_NUM_STATES - 1;  /* fallback: last state */
}

/**
 * @brief Fire an autonomous bird call for the predicted next state.
 *
 * Converts the state back to a (detection_type, light_band) pair and asks
 * the mapper to generate and play an appropriate call.
 *
 * For IDLE states this is a no-op (don't play a call for silence).
 */
static void fire_autonomous_call(markov_chain_t *mc, uint8_t predicted_state)
{
    if (!mc->mapper) return;

    uint8_t     det_idx;
    light_band_t band;
    markov_decode_state(predicted_state, &det_idx, &band);

    /* det_idx 0 = IDLE — don't play anything for an IDLE prediction */
    if (det_idx == 0) {
        ESP_LOGD(TAG, "Autonomous: predicted IDLE — skipping call");
        return;
    }

    /* Map detection index back to detection_type_t */
    detection_type_t det;
    switch (det_idx) {
        case 1:  det = DETECTION_WHISTLE; break;
        case 2:  det = DETECTION_VOICE;   break;
        case 3:  det = DETECTION_CLAP;    break;
        default: det = DETECTION_NONE;    break;
    }

    /* Temporarily apply a lux matching the predicted band so the mapper
     * selects an appropriate bird for that mood.                        */
    float representative_lux;
    switch (band) {
        case LIGHT_BAND_NIGHT:    representative_lux =   2.0f; break;
        case LIGHT_BAND_DAWN:     representative_lux =  50.0f; break;
        case LIGHT_BAND_OVERCAST: representative_lux = 200.0f; break;
        case LIGHT_BAND_SUNNY:    representative_lux = 800.0f; break;
        default:                  representative_lux = 200.0f; break;
    }

    /* Temporarily apply a lux matching the predicted band so the mapper
     * selects an appropriate bird for that mood.
     *
     * NOTE: The three-step sequence (update → get → restore) is NOT atomic.
     * A concurrent lux task or ESP-NOW event could update the mapper between
     * steps.  Because autonomous calls are rare (≥45 s apart) and lux changes
     * are slow (500 ms poll), the window for collision is tiny and the only
     * observable effect would be a single bird chosen from the "wrong" list
     * for one call.  The next lux poll will correct it.  Not worth the
     * complexity of holding the mapper lock across the whole playback path.
     */
    bird_mapper_update_for_lux(mc->mapper, representative_lux);

    bird_info_t bird = bird_mapper_get_bird(mc->mapper, det);

    /* Restore mapper to actual local lux before any blocking call so that
     * if playback is skipped (speaker busy) the mapper is still correct. */
    if (mc->local_lux >= 0.0f)
        bird_mapper_update_for_lux(mc->mapper, mc->local_lux);

    if (bird.function_name == NULL || bird.function_name[0] == '\0') return;

    /* Use the application's shared audio buffer rather than malloc-ing ~96 KB.
     * play_bird_call() acquires the playback mutex internally, which also
     * serialises access to the shared buffer.                               */
    audio_buffer_t *buf = get_audio_buffer();

    ESP_LOGI(TAG, "🤖 Autonomous call: %s (predicted state %s)",
             bird.display_name, markov_state_name(predicted_state));

    bird_mapper_generate_call(mc->mapper, bird.function_name, buf);
    play_bird_call(bird.display_name, buf);
}

/* ========================================================================
 * PUBLIC API
 * ======================================================================== */

void markov_init(markov_chain_t *mc, bird_call_mapper_t *mapper)
{
    memset(mc, 0, sizeof(*mc));
    mc->mapper         = mapper;
    mc->current_state  = MARKOV_STATE_STARTUP;
    mc->local_lux      = -1.0f;
    mc->probs_dirty    = true;

    /* Seed with priors first, then try to overlay saved counts */
    seed_priors(mc);
    nvs_load(mc);   /* overwrites counts if valid NVS data exists */

    mc->probs_dirty = true;   /* force recompute after load */

    uint32_t now = markov_millis();
    mc->last_event_ms      = now;
    mc->last_autonomous_ms = now;

    ESP_LOGI(TAG, "Markov chain ready — %d states, current: %s",
             MARKOV_NUM_STATES, markov_state_name(mc->current_state));
    markov_log_top_transitions(mc);
}

void markov_on_event(markov_chain_t *mc, detection_type_t detection, float lux)
{
    /* Use provided lux or fall back to last known local lux */
    float effective_lux = (lux >= 0.0f) ? lux : mc->local_lux;
    if (effective_lux < 0.0f) effective_lux = 200.0f;  /* sensible default */

    light_band_t band      = markov_lux_to_band(effective_lux);
    uint8_t      new_state = markov_make_state(detection, band);

    uint8_t from = mc->current_state;
    uint8_t to   = new_state;

    /* Increment transition count (cap at UINT16_MAX to avoid overflow) */
    if (mc->counts[from][to] < UINT16_MAX)
        mc->counts[from][to]++;

    mc->current_state = to;
    mc->probs_dirty   = true;
    mc->last_event_ms = markov_millis();
    mc->events_since_save++;

    ESP_LOGD(TAG, "Transition: %s → %s (count=%u)",
             markov_state_name(from), markov_state_name(to),
             mc->counts[from][to]);

    /* Periodic NVS save */
    if (mc->events_since_save >= MARKOV_NVS_SAVE_INTERVAL) {
        nvs_save_counts(mc);
        mc->events_since_save = 0;
    }
}

void markov_set_lux(markov_chain_t *mc, float lux)
{
    mc->local_lux = lux;

    /* If we're currently in an IDLE state, update to the correct light band
     * without treating it as a sound event.                                 */
    uint8_t det_idx;
    light_band_t current_band;
    markov_decode_state(mc->current_state, &det_idx, &current_band);

    if (det_idx == 0 && mc->current_state != MARKOV_STATE_STARTUP) {
        /* In an IDLE state — silently update the band component */
        light_band_t new_band = markov_lux_to_band(lux);
        if (new_band != current_band) {
            mc->current_state = MARKOV_STATE(0, (uint8_t)new_band);
            ESP_LOGD(TAG, "Light band update → idle state %s",
                     markov_state_name(mc->current_state));
        }
    }
}

float markov_get_lux_bias(const markov_chain_t *mc)
{
    /* The bias is derived from the current state's "energy level":
     *
     *   IDLE:    0  (no bias)
     *   VOICE:  -1  (pull toward mellow)
     *   WHISTLE: 0  (neutral)
     *   CLAP:   +1  (pull toward lively)
     *
     * The light band scales the magnitude:
     *   NIGHT:    × 0.25
     *   DAWN:     × 0.5
     *   OVERCAST: × 0.75
     *   SUNNY:    × 1.0
     *
     * The result is then scaled by MARKOV_MAX_LUX_BIAS.              */

    if (mc->current_state == MARKOV_STATE_STARTUP) return 0.0f;

    uint8_t     det_idx;
    light_band_t band;
    markov_decode_state(mc->current_state, &det_idx, &band);

    float energy;
    switch (det_idx) {
        case 0: energy =  0.0f; break;  /* IDLE    */
        case 1: energy =  0.0f; break;  /* WHISTLE */
        case 2: energy = -1.0f; break;  /* VOICE   */
        case 3: energy = +1.0f; break;  /* CLAP    */
        default:energy =  0.0f; break;
    }

    float band_scale;
    switch (band) {
        case LIGHT_BAND_NIGHT:    band_scale = 0.25f; break;
        case LIGHT_BAND_DAWN:     band_scale = 0.50f; break;
        case LIGHT_BAND_OVERCAST: band_scale = 0.75f; break;
        case LIGHT_BAND_SUNNY:    band_scale = 1.00f; break;
        default:                  band_scale = 0.75f; break;
    }

    return energy * band_scale * MARKOV_MAX_LUX_BIAS;
}

void markov_tick(markov_chain_t *mc)
{
    uint32_t now     = markov_millis();
    uint32_t silence = now - mc->last_event_ms;

    const remote_config_t *cfg = remote_config_get();

    if (silence >= cfg->markov_idle_trigger_ms) {
        /* Check autonomous cooldown */
        uint32_t since_last_auto = now - mc->last_autonomous_ms;
        if (since_last_auto >= cfg->markov_autonomous_cooldown_ms) {

            /* Recompute probs if needed */
            if (mc->probs_dirty) recompute_probs(mc);

            uint8_t predicted = sample_next_state(mc);
            ESP_LOGI(TAG, "Network silent for %lus — autonomous call (predicted: %s)",
                     (unsigned long)(silence / 1000),
                     markov_state_name(predicted));

            fire_autonomous_call(mc, predicted);

            mc->last_autonomous_ms = now;

            /* Advance the chain state as if we observed this event
             * (the call we just made IS an event from our node's perspective) */
            uint8_t det_idx;
            light_band_t band;
            markov_decode_state(predicted, &det_idx, &band);
            /* Only advance if it was a sound event, not IDLE */
            if (det_idx > 0) {
                detection_type_t det;
                switch (det_idx) {
                    case 1: det = DETECTION_WHISTLE; break;
                    case 2: det = DETECTION_VOICE;   break;
                    case 3: det = DETECTION_CLAP;    break;
                    default: det = DETECTION_NONE;   break;
                }
                markov_on_event(mc, det, mc->local_lux);
            }
        }
    }
}

void markov_save(markov_chain_t *mc)
{
    nvs_save_counts(mc);
    mc->events_since_save = 0;
}

void markov_reset(markov_chain_t *mc)
{
    ESP_LOGW(TAG, "Resetting Markov chain to priors");
    seed_priors(mc);
    mc->probs_dirty   = true;
    mc->current_state = MARKOV_STATE_STARTUP;

    /* Erase NVS entry */
    nvs_handle_t handle;
    if (nvs_open(MARKOV_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, MARKOV_NVS_KEY);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void markov_log_top_transitions(markov_chain_t *mc)
{
    if (mc->probs_dirty) recompute_probs(mc);

    uint8_t cur = mc->current_state;
    ESP_LOGI(TAG, "Top transitions from %s:", markov_state_name(cur));

    /* Simple selection sort for top 3 */
    float top_p[3]    = {-1, -1, -1};
    uint8_t top_s[3]  = {0, 0, 0};

    for (int j = 0; j < MARKOV_NUM_STATES; j++) {
        float p = mc->probs[cur][j];
        for (int k = 0; k < 3; k++) {
            if (p > top_p[k]) {
                /* Shift down */
                for (int l = 2; l > k; l--) {
                    top_p[l] = top_p[l-1];
                    top_s[l] = top_s[l-1];
                }
                top_p[k] = p;
                top_s[k] = (uint8_t)j;
                break;
            }
        }
    }

    for (int k = 0; k < 3; k++) {
        if (top_p[k] > 0.0f)
            ESP_LOGI(TAG, "  #%d  %-20s  %.1f%%",
                     k + 1, markov_state_name(top_s[k]), top_p[k] * 100.0f);
    }
}
