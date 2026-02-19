/**
 * @file synthesis.h
 * @brief South African Bird Call Synthesis
 */

#ifndef SYNTHESIS_H
#define SYNTHESIS_H

#include <stdint.h>
#include <stddef.h>
#include "echoes.h"

/* ========================================================================
 * SYNTHESIZER STRUCTURE
 * ======================================================================== */

typedef struct {
    uint32_t sample_rate;
    size_t chunk_size;
} bird_synthesizer_t;

/* ========================================================================
 * ENVELOPE AND WAVEFORM GENERATION
 * ======================================================================== */

/**
 * @brief Calculate envelope value at time t
 */
float envelope(float t, float duration, float attack, float release);

/**
 * @brief Generate a tone into buffer at offset
 * @return Number of samples written
 */
size_t generate_tone(bird_synthesizer_t *synth, int16_t *buffer, size_t offset,
                     float freq, uint32_t duration_ms, float amplitude, 
                     float vibrato_rate, float vibrato_depth);

/**
 * @brief Generate a frequency sweep into buffer at offset
 * @return Number of samples written
 */
size_t generate_sweep(bird_synthesizer_t *synth, int16_t *buffer, size_t offset,
                      float start_freq, float end_freq, uint32_t duration_ms, 
                      float amplitude);

/**
 * @brief Generate tremolo effect into buffer at offset
 * @return Number of samples written
 */
size_t generate_tremolo(bird_synthesizer_t *synth, int16_t *buffer, size_t offset,
                        float base_freq, uint32_t duration_ms, float trem_rate, 
                        float trem_depth, float amplitude);

/**
 * @brief Generate harsh call with harmonics into buffer at offset
 * @return Number of samples written
 */
size_t generate_harsh(bird_synthesizer_t *synth, int16_t *buffer, size_t offset,
                      float freq, uint32_t duration_ms, uint8_t harmonics,
                      float amplitude);

/**
 * @brief Generate silence into buffer at offset
 * @return Number of samples written
 */
size_t generate_silence(bird_synthesizer_t *synth, int16_t *buffer, size_t offset,
                        uint32_t duration_ms);

/* ========================================================================
 * BIRD CALL GENERATORS
 * ======================================================================== */

size_t generate_piet_my_vrou(bird_synthesizer_t *synth, audio_buffer_t *out);
size_t generate_cape_robin_chat(bird_synthesizer_t *synth, audio_buffer_t *out);
size_t generate_southern_boubou(bird_synthesizer_t *synth, audio_buffer_t *out);
size_t generate_red_eyed_dove(bird_synthesizer_t *synth, audio_buffer_t *out);
size_t generate_glossy_starling(bird_synthesizer_t *synth, audio_buffer_t *out);
size_t generate_spotted_eagle_owl(bird_synthesizer_t *synth, audio_buffer_t *out);
size_t generate_fork_tailed_drongo(bird_synthesizer_t *synth, audio_buffer_t *out);
size_t generate_cape_canary(bird_synthesizer_t *synth, audio_buffer_t *out);
size_t generate_southern_masked_weaver(bird_synthesizer_t *synth, audio_buffer_t *out);
size_t generate_red_billed_quelea(bird_synthesizer_t *synth, audio_buffer_t *out);
size_t generate_paradise_flycatcher(bird_synthesizer_t *synth, audio_buffer_t *out);

/* ========================================================================
 * BIRD CALL MAPPER
 * ======================================================================== */

#define MAX_BIRDS_PER_CATEGORY 8

typedef struct bird_call_mapper_t {
    bird_synthesizer_t synth;
    bird_info_t whistle_birds[MAX_BIRDS_PER_CATEGORY];
    uint8_t num_whistle_birds;
    bird_info_t voice_birds[MAX_BIRDS_PER_CATEGORY];
    uint8_t num_voice_birds;
    bird_info_t clap_birds[MAX_BIRDS_PER_CATEGORY];
    uint8_t num_clap_birds;
    bird_info_t birdsong_birds[MAX_BIRDS_PER_CATEGORY];  /**< Responds to DETECTION_BIRDSONG */
    uint8_t num_birdsong_birds;
} bird_call_mapper_t;

/**
 * @brief Initialize bird call mapper
 */
void bird_mapper_init(bird_call_mapper_t *mapper, uint32_t sample_rate);

/**
 * @brief Get bird for detection type
 */
bird_info_t bird_mapper_get_bird(bird_call_mapper_t *mapper,
                                  detection_type_t detection_type);

/**
 * @brief Generate bird call into buffer
 * @return Number of samples generated
 */
size_t bird_mapper_generate_call(bird_call_mapper_t *mapper,
                                  const char *bird_function_name,
                                  audio_buffer_t *buffer);

/**
 * @brief Update bird lists based on lux level
 */
void bird_mapper_update_for_lux(bird_call_mapper_t *mapper, float lux);

#endif /* SA_BIRD_SYNTHESIS_H */
