/**
 * @file synthesis.c
 * @brief South African Bird Call Synthesis Implementation (Static Buffer)
 */

#include "synthesis.h"
#include <string.h>
#include <math.h>
#include "esp_random.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================
 * HELPER FUNCTIONS
 * ======================================================================== */

/**
 * @brief Calculate envelope value at time t
 */
float envelope(float t, float duration, float attack, float release) {
    if (t < attack) {
        return t / attack;
    } else if (t > duration - release) {
        return (duration - t) / release;
    }
    return 1.0f;
}

/* ========================================================================
 * WAVEFORM GENERATION (writes directly to buffer at offset)
 * ======================================================================== */

size_t generate_tone(bird_synthesizer_t *synth, int16_t *buffer, size_t offset,
                     float freq, uint32_t duration_ms, float amplitude,
                     float vibrato_rate, float vibrato_depth) {
    float duration_s = duration_ms / 1000.0f;
    size_t num_samples = (size_t)(synth->sample_rate * duration_s);
    
    for (size_t i = 0; i < num_samples; i++) {
        float t = (float)i / synth->sample_rate;
        
        float freq_mod = freq;
        if (vibrato_rate > 0) {
            freq_mod = freq * (1.0f + vibrato_depth * sinf(2.0f * M_PI * vibrato_rate * t));
        }
        
        float env = envelope(t, duration_s, 0.02f, 0.05f);
        float sample_f = 32767.0f * amplitude * VOLUME * env * 
                       sinf(2.0f * M_PI * freq_mod * t);
        buffer[offset + i] = (int16_t)sample_f;
    }
    
    return num_samples;
}

size_t generate_sweep(bird_synthesizer_t *synth, int16_t *buffer, size_t offset,
                      float start_freq, float end_freq, uint32_t duration_ms,
                      float amplitude) {
    float duration_s = duration_ms / 1000.0f;
    size_t num_samples = (size_t)(synth->sample_rate * duration_s);
    
    for (size_t i = 0; i < num_samples; i++) {
        float t = (float)i / synth->sample_rate;
        float progress = t / duration_s;
        float freq = start_freq + (end_freq - start_freq) * progress;
        
        float env = envelope(t, duration_s, 0.01f, 0.03f);
        float sample_f = 32767.0f * amplitude * VOLUME * env * 
                       sinf(2.0f * M_PI * freq * t);
        buffer[offset + i] = (int16_t)sample_f;
    }
    
    return num_samples;
}

size_t generate_tremolo(bird_synthesizer_t *synth, int16_t *buffer, size_t offset,
                        float base_freq, uint32_t duration_ms, float trem_rate,
                        float trem_depth, float amplitude) {
    float duration_s = duration_ms / 1000.0f;
    size_t num_samples = (size_t)(synth->sample_rate * duration_s);
    
    for (size_t i = 0; i < num_samples; i++) {
        float t = (float)i / synth->sample_rate;
        
        float am = 1.0f - trem_depth * (1.0f - (1.0f + sinf(2.0f * M_PI * trem_rate * t)) / 2.0f);
        float env = envelope(t, duration_s, 0.02f, 0.05f);
        float sample_f = 32767.0f * amplitude * VOLUME * env * am * 
                       sinf(2.0f * M_PI * base_freq * t);
        buffer[offset + i] = (int16_t)sample_f;
    }
    
    return num_samples;
}

size_t generate_harsh(bird_synthesizer_t *synth, int16_t *buffer, size_t offset,
                      float freq, uint32_t duration_ms, uint8_t harmonics,
                      float amplitude) {
    float duration_s = duration_ms / 1000.0f;
    size_t num_samples = (size_t)(synth->sample_rate * duration_s);
    
    for (size_t i = 0; i < num_samples; i++) {
        float t = (float)i / synth->sample_rate;
        float env = envelope(t, duration_s, 0.01f, 0.08f);
        
        float sample_val = 0.0f;
        for (uint8_t h = 1; h <= harmonics; h++) {
            float harmonic_amp = 1.0f / h;
            sample_val += harmonic_amp * sinf(2.0f * M_PI * freq * h * t);
        }
        sample_val /= harmonics;
        
        float sample_f = 32767.0f * amplitude * VOLUME * env * sample_val;
        buffer[offset + i] = (int16_t)sample_f;
    }
    
    return num_samples;
}

size_t generate_silence(bird_synthesizer_t *synth, int16_t *buffer, size_t offset,
                        uint32_t duration_ms) {
    size_t num_samples = (size_t)(synth->sample_rate * duration_ms / 1000.0f);
    memset(&buffer[offset], 0, num_samples * sizeof(int16_t));
    return num_samples;
}

/* ========================================================================
 * BIRD CALL GENERATORS (append to buffer)
 * ======================================================================== */


size_t generate_piet_my_vrou(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    // Red-chested Cuckoo
    pos += generate_sweep(synth, out->buffer, pos, 1200, 1350, 250, 0.38f);
    pos += generate_silence(synth, out->buffer, pos, 120);
    pos += generate_tone(synth, out->buffer, pos, 1300, 220, 0.38f, 0, 0);
    pos += generate_silence(synth, out->buffer, pos, 120);
    pos += generate_sweep(synth, out->buffer, pos, 1400, 1100, 280, 0.38f);
    
    out->num_samples = pos;
    return pos;
}

size_t generate_cape_robin_chat(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    struct { float start; float end; uint32_t dur; } sweeps[] = {
        {2200, 2600, 180}, {2800, 2400, 160}, {2600, 3000, 170},
        {2900, 2500, 190}, {2700, 2200, 160}
    };
    
    for (int i = 0; i < 5; i++) {
        pos += generate_sweep(synth, out->buffer, pos, sweeps[i].start, 
                            sweeps[i].end, sweeps[i].dur, 0.32f);
        pos += generate_silence(synth, out->buffer, pos, 90);
    }
    
    out->num_samples = pos;
    return pos;
}

size_t generate_southern_boubou(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    pos += generate_tone(synth, out->buffer, pos, 900, 250, 0.35f, 4, 0.02f);
    pos += generate_silence(synth, out->buffer, pos, 100);
    pos += generate_sweep(synth, out->buffer, pos, 2400, 2800, 200, 0.33f);
    pos += generate_silence(synth, out->buffer, pos, 150);
    pos += generate_tone(synth, out->buffer, pos, 950, 230, 0.35f, 0, 0);
    pos += generate_silence(synth, out->buffer, pos, 90);
    pos += generate_sweep(synth, out->buffer, pos, 2500, 2900, 180, 0.33f);
    
    out->num_samples = pos;
    return pos;
}


size_t generate_red_eyed_dove(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    for (int i = 0; i < 2; i++) {
        pos += generate_tremolo(synth, out->buffer, pos, 480, 180, 8, 0.3f, 0.32f);
        pos += generate_silence(synth, out->buffer, pos, 120);
    }
    
    pos += generate_silence(synth, out->buffer, pos, 100);
    pos += generate_tremolo(synth, out->buffer, pos, 520, 200, 8, 0.3f, 0.35f);
    pos += generate_silence(synth, out->buffer, pos, 100);
    pos += generate_tremolo(synth, out->buffer, pos, 500, 220, 8, 0.3f, 0.38f);
    pos += generate_silence(synth, out->buffer, pos, 120);
    pos += generate_tremolo(synth, out->buffer, pos, 460, 250, 8, 0.3f, 0.35f);
    
    out->num_samples = pos;
    return pos;
}

size_t generate_glossy_starling(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    float freqs[] = {2800, 3200, 2600, 3400, 2900, 3100};
    
    for (int i = 0; i < 6; i++) {
        pos += generate_tone(synth, out->buffer, pos, freqs[i], 120, 0.30f, 6, 0.03f);
        pos += generate_silence(synth, out->buffer, pos, 70);
    }
    
    out->num_samples = pos;
    return pos;
}

size_t generate_spotted_eagle_owl(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    for (int i = 0; i < 2; i++) {
        pos += generate_tone(synth, out->buffer, pos, 300, 200, 0.35f, 0, 0);
        pos += generate_silence(synth, out->buffer, pos, 150);
        pos += generate_tone(synth, out->buffer, pos, 280, 350, 0.38f, 3, 0.02f);
        pos += generate_silence(synth, out->buffer, pos, 400);
    }
    
    out->num_samples = pos;
    return pos;
}

size_t generate_fork_tailed_drongo(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    struct { float start; float end; uint32_t dur; } sweeps[] = {
        {2400, 2600, 100}, {2200, 2800, 120},
        {2600, 2300, 110}, {2800, 2400, 100}
    };
    
    for (int i = 0; i < 4; i++) {
        pos += generate_sweep(synth, out->buffer, pos, sweeps[i].start, 
                            sweeps[i].end, sweeps[i].dur, 0.33f);
        pos += generate_silence(synth, out->buffer, pos, 150);
    }
    
    out->num_samples = pos;
    return pos;
}

size_t generate_cape_canary(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    for (int i = 0; i < 8; i++) {
        float freq = 2600 + (i * 150);
        pos += generate_tone(synth, out->buffer, pos, freq, 80, 0.28f, 12, 0.04f);
        pos += generate_silence(synth, out->buffer, pos, 50);
    }
    
    out->num_samples = pos;
    return pos;
}

size_t generate_southern_masked_weaver(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    for (int i = 0; i < 6; i++) {
        float freq = 2000 + (i % 3) * 300;
        pos += generate_tremolo(synth, out->buffer, pos, freq, 100, 20, 0.6f, 0.30f);
        pos += generate_silence(synth, out->buffer, pos, 60);
    }
    
    out->num_samples = pos;
    return pos;
}


size_t generate_red_billed_quelea(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;

    /*
     * Red-billed Quelea (Quelea quelea)
     *
     * Structure: a colony-bird chatter — rapid buzzy syllables at ~2–3 kHz
     * with nasal harmonic content, interspersed with short upward-sweep
     * "contact whistles".  The call is fast, dense, and energetic.
     *
     * Phrase layout (total ~1.1 s, well within 3 s budget):
     *   1. Opening chatter burst  — 4 harsh notes, tightly spaced
     *   2. Contact whistle        — upward sweep
     *   3. Second chatter burst   — 4 tremolo notes (slightly varied pitch)
     *   4. Short upward chirp     — sweep, then a brief falling close
     *   5. Final buzz             — harsh note to end
     */

    /* --- Phrase 1: opening chatter (buzzy, nasal, staccato) --- */
    pos += generate_harsh(synth, out->buffer, pos, 2200, 55, 2, 0.28f);
    pos += generate_silence(synth, out->buffer, pos, 30);
    pos += generate_harsh(synth, out->buffer, pos, 2400, 50, 2, 0.30f);
    pos += generate_silence(synth, out->buffer, pos, 28);
    pos += generate_harsh(synth, out->buffer, pos, 2300, 55, 2, 0.29f);
    pos += generate_silence(synth, out->buffer, pos, 30);
    pos += generate_harsh(synth, out->buffer, pos, 2500, 45, 3, 0.28f);
    pos += generate_silence(synth, out->buffer, pos, 55);

    /* --- Phrase 2: contact whistle — fast upward sweep --- */
    pos += generate_sweep(synth, out->buffer, pos, 1900, 3100, 110, 0.32f);
    pos += generate_silence(synth, out->buffer, pos, 45);

    /* --- Phrase 3: second chatter burst — tremolo for flock texture --- */
    pos += generate_tremolo(synth, out->buffer, pos, 2600, 65, 28, 0.55f, 0.27f);
    pos += generate_silence(synth, out->buffer, pos, 25);
    pos += generate_tremolo(synth, out->buffer, pos, 2800, 60, 28, 0.60f, 0.28f);
    pos += generate_silence(synth, out->buffer, pos, 25);
    pos += generate_tremolo(synth, out->buffer, pos, 2500, 65, 28, 0.55f, 0.27f);
    pos += generate_silence(synth, out->buffer, pos, 25);
    pos += generate_tremolo(synth, out->buffer, pos, 2700, 55, 30, 0.58f, 0.27f);
    pos += generate_silence(synth, out->buffer, pos, 50);

    /* --- Phrase 4: short rising chirp then falling close --- */
    pos += generate_sweep(synth, out->buffer, pos, 2000, 3200, 90, 0.30f);
    pos += generate_silence(synth, out->buffer, pos, 20);
    pos += generate_sweep(synth, out->buffer, pos, 3000, 2100, 75, 0.28f);
    pos += generate_silence(synth, out->buffer, pos, 40);

    /* --- Phrase 5: closing buzz --- */
    pos += generate_harsh(synth, out->buffer, pos, 2300, 70, 3, 0.26f);

    out->num_samples = pos;
    return pos;
}
/* ========================================================================
 * BIRD CALL MAPPER
 * ======================================================================== */

void bird_mapper_init(bird_call_mapper_t *mapper, uint32_t sample_rate) {
    mapper->synth.sample_rate = sample_rate;
    mapper->synth.chunk_size = CHUNK_SIZE;

    /* Default (neutral) lists — overridden by bird_mapper_update_for_lux() */

    /* Whistle -> melodic, active birds */
    mapper->whistle_birds[0] = (bird_info_t){"piet_my_vrou",   "Red-chested Cuckoo"};
    mapper->whistle_birds[1] = (bird_info_t){"cape_robin_chat", "Cape Robin-Chat"};
    mapper->whistle_birds[2] = (bird_info_t){"glossy_starling", "Glossy Starling"};
    mapper->whistle_birds[3] = (bird_info_t){"cape_canary",     "Cape Canary"};
    mapper->num_whistle_birds = 4;

    /* Voice -> gentle, mid-range birds */
    mapper->voice_birds[0] = (bird_info_t){"red_eyed_dove",    "Red-eyed Dove"};
    mapper->voice_birds[1] = (bird_info_t){"southern_boubou",  "Southern Boubou"};
    mapper->voice_birds[2] = (bird_info_t){"spotted_eagle_owl","Eagle-Owl"};
    mapper->num_voice_birds = 3;

    /* Clap -> energetic birds (no hadada or pied crow) */
    mapper->clap_birds[0] = (bird_info_t){"fork_tailed_drongo",    "Drongo"};
    mapper->clap_birds[1] = (bird_info_t){"southern_masked_weaver", "Masked Weaver"};
    mapper->clap_birds[2] = (bird_info_t){"cape_canary",            "Cape Canary"};
    mapper->clap_birds[3] = (bird_info_t){"red_billed_quelea",       "Red-billed Quelea"};
    mapper->num_clap_birds = 4;
}

bird_info_t bird_mapper_get_bird(bird_call_mapper_t *mapper,
                                  detection_type_t detection_type) {
    bird_info_t *bird_list;
    uint8_t list_len;
    uint32_t r = esp_random();        // Get 32-bit random number
    uint32_t index = r % 5;             // Range: 0–4
    
    switch (detection_type) {
        case DETECTION_WHISTLE:
            bird_list = mapper->whistle_birds;
            list_len = mapper->num_whistle_birds;
            break;
        case DETECTION_VOICE:
            bird_list = mapper->voice_birds;
            list_len = mapper->num_voice_birds;
            break;
        case DETECTION_CLAP:
            bird_list = mapper->clap_birds;
            list_len = mapper->num_clap_birds;
            break;
        default:
            return (bird_info_t){"", "Unknown"};
    }
    
    return bird_list[index % list_len];
}

size_t bird_mapper_generate_call(bird_call_mapper_t *mapper,
                                  const char *bird_function_name,
                                  audio_buffer_t *buffer) {
    
    // Function pointer lookup table
    typedef size_t (*gen_func_t)(bird_synthesizer_t*, audio_buffer_t*);
    
    struct {
        const char *name;
        gen_func_t func;
    } bird_funcs[] = {
        {"piet_my_vrou",          generate_piet_my_vrou},
        {"cape_robin_chat",       generate_cape_robin_chat},
        {"southern_boubou",       generate_southern_boubou},
        {"red_eyed_dove",         generate_red_eyed_dove},
        {"glossy_starling",       generate_glossy_starling},
        {"spotted_eagle_owl",     generate_spotted_eagle_owl},
        {"fork_tailed_drongo",    generate_fork_tailed_drongo},
        {"cape_canary",           generate_cape_canary},
        {"southern_masked_weaver",generate_southern_masked_weaver},
        {"red_billed_quelea",      generate_red_billed_quelea},
    };
    
    for (int i = 0; i < sizeof(bird_funcs) / sizeof(bird_funcs[0]); i++) {
        if (strcmp(bird_function_name, bird_funcs[i].name) == 0) {
            return bird_funcs[i].func(&mapper->synth, buffer);
        }
    }
    
    return 0;
}

void bird_mapper_update_for_lux(bird_call_mapper_t *mapper, float lux) {
    /*
     * Light-level mood mapping (no hadada ibis, no pied crow):
     *
     *  < 10 lux  : Night     — soft, mellow only (owl, dove)
     *  < 100 lux : Dawn/Dusk — gentle chorus (dove, boubou, robin)
     *  < 500 lux : Overcast  — moderate variety
     *  >= 500 lux: Sunny     — full lively set (canary, starling, drongo, weaver)
     */
    if (lux < 10.0f) {
        /* Night: very mellow — owl for all categories */
        mapper->whistle_birds[0] = (bird_info_t){"red_eyed_dove",    "Red-eyed Dove"};
        mapper->whistle_birds[1] = (bird_info_t){"southern_boubou",  "Southern Boubou"};
        mapper->num_whistle_birds = 2;

        mapper->voice_birds[0] = (bird_info_t){"spotted_eagle_owl", "Eagle-Owl"};
        mapper->voice_birds[1] = (bird_info_t){"red_eyed_dove",     "Red-eyed Dove"};
        mapper->num_voice_birds = 2;

        mapper->clap_birds[0] = (bird_info_t){"spotted_eagle_owl", "Eagle-Owl"};
        mapper->num_clap_birds = 1;
    }
    else if (lux < 100.0f) {
        /* Dawn/Dusk: gentle, mellow chorus */
        mapper->whistle_birds[0] = (bird_info_t){"cape_robin_chat",  "Cape Robin-Chat"};
        mapper->whistle_birds[1] = (bird_info_t){"red_eyed_dove",    "Red-eyed Dove"};
        mapper->num_whistle_birds = 2;

        mapper->voice_birds[0] = (bird_info_t){"red_eyed_dove",    "Red-eyed Dove"};
        mapper->voice_birds[1] = (bird_info_t){"southern_boubou",  "Southern Boubou"};
        mapper->voice_birds[2] = (bird_info_t){"spotted_eagle_owl","Eagle-Owl"};
        mapper->num_voice_birds = 3;

        mapper->clap_birds[0] = (bird_info_t){"red_eyed_dove",    "Red-eyed Dove"};
        mapper->clap_birds[1] = (bird_info_t){"southern_boubou",  "Southern Boubou"};
        mapper->num_clap_birds = 2;
    }
    else if (lux < 500.0f) {
        /* Overcast: moderate mix, leaning gentle */
        mapper->whistle_birds[0] = (bird_info_t){"piet_my_vrou",    "Red-chested Cuckoo"};
        mapper->whistle_birds[1] = (bird_info_t){"cape_robin_chat", "Cape Robin-Chat"};
        mapper->whistle_birds[2] = (bird_info_t){"glossy_starling", "Glossy Starling"};
        mapper->num_whistle_birds = 3;

        mapper->voice_birds[0] = (bird_info_t){"red_eyed_dove",    "Red-eyed Dove"};
        mapper->voice_birds[1] = (bird_info_t){"southern_boubou",  "Southern Boubou"};
        mapper->num_voice_birds = 2;

        mapper->clap_birds[0] = (bird_info_t){"fork_tailed_drongo",    "Drongo"};
        mapper->clap_birds[1] = (bird_info_t){"southern_masked_weaver", "Masked Weaver"};
        mapper->clap_birds[2] = (bird_info_t){"glossy_starling",        "Glossy Starling"};
        mapper->num_clap_birds = 3;
    }
    else {
        /* Bright/Sunny: full lively set */
        mapper->whistle_birds[0] = (bird_info_t){"piet_my_vrou",   "Red-chested Cuckoo"};
        mapper->whistle_birds[1] = (bird_info_t){"cape_robin_chat", "Cape Robin-Chat"};
        mapper->whistle_birds[2] = (bird_info_t){"glossy_starling", "Glossy Starling"};
        mapper->whistle_birds[3] = (bird_info_t){"cape_canary",     "Cape Canary"};
        mapper->num_whistle_birds = 4;

        mapper->voice_birds[0] = (bird_info_t){"red_eyed_dove",    "Red-eyed Dove"};
        mapper->voice_birds[1] = (bird_info_t){"southern_boubou",  "Southern Boubou"};
        mapper->voice_birds[2] = (bird_info_t){"spotted_eagle_owl","Eagle-Owl"};
        mapper->num_voice_birds = 3;

        mapper->clap_birds[0] = (bird_info_t){"fork_tailed_drongo",    "Drongo"};
        mapper->clap_birds[1] = (bird_info_t){"southern_masked_weaver", "Masked Weaver"};
        mapper->clap_birds[2] = (bird_info_t){"cape_canary",            "Cape Canary"};
        mapper->clap_birds[3] = (bird_info_t){"glossy_starling",        "Glossy Starling"};
        mapper->clap_birds[4] = (bird_info_t){"red_billed_quelea",       "Red-billed Quelea"};
        mapper->num_clap_birds = 5;
    }
}
