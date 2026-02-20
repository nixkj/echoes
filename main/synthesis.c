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
        /* RMS-style normalization: dividing by harmonics is far too aggressive.
         * sqrt(harmonics) preserves perceived loudness of harmonic-rich calls. */
        sample_val /= sqrtf((float)harmonics);
        
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
    pos += generate_sweep(synth, out->buffer, pos, 1200, 1380, 260, 0.72f);
    pos += generate_silence(synth, out->buffer, pos, 110);
    pos += generate_tone(synth, out->buffer, pos, 1320, 230, 0.70f, 0, 0);
    pos += generate_silence(synth, out->buffer, pos, 110);
    pos += generate_sweep(synth, out->buffer, pos, 1420, 1080, 290, 0.72f);
    
    out->num_samples = pos;
    return pos;
}

size_t generate_cape_robin_chat(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    struct { float start; float end; uint32_t dur; } sweeps[] = {
        {2200, 2650, 190}, {2850, 2420, 170}, {2650, 3050, 180},
        {2950, 2520, 200}, {2750, 2200, 170}
    };
    
    for (int i = 0; i < 5; i++) {
        pos += generate_sweep(synth, out->buffer, pos, sweeps[i].start, 
                            sweeps[i].end, sweeps[i].dur, 0.65f);
        pos += generate_silence(synth, out->buffer, pos, 80);
    }
    
    out->num_samples = pos;
    return pos;
}

size_t generate_southern_boubou(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    pos += generate_tone(synth, out->buffer, pos, 900, 260, 0.68f, 4, 0.02f);
    pos += generate_silence(synth, out->buffer, pos, 95);
    pos += generate_sweep(synth, out->buffer, pos, 2400, 2850, 210, 0.65f);
    pos += generate_silence(synth, out->buffer, pos, 140);
    pos += generate_tone(synth, out->buffer, pos, 950, 240, 0.68f, 0, 0);
    pos += generate_silence(synth, out->buffer, pos, 80);
    pos += generate_sweep(synth, out->buffer, pos, 2550, 2950, 190, 0.65f);
    
    out->num_samples = pos;
    return pos;
}


size_t generate_red_eyed_dove(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    for (int i = 0; i < 2; i++) {
        pos += generate_tremolo(synth, out->buffer, pos, 480, 180, 8, 0.3f, 0.52f);
        pos += generate_silence(synth, out->buffer, pos, 120);
    }
    
    pos += generate_silence(synth, out->buffer, pos, 100);
    pos += generate_tremolo(synth, out->buffer, pos, 520, 200, 8, 0.3f, 0.55f);
    pos += generate_silence(synth, out->buffer, pos, 100);
    pos += generate_tremolo(synth, out->buffer, pos, 500, 220, 8, 0.3f, 0.58f);
    pos += generate_silence(synth, out->buffer, pos, 120);
    pos += generate_tremolo(synth, out->buffer, pos, 460, 250, 8, 0.3f, 0.55f);
    
    out->num_samples = pos;
    return pos;
}

size_t generate_glossy_starling(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    float freqs[] = {2800, 3200, 2600, 3400, 2900, 3100};
    
    for (int i = 0; i < 6; i++) {
        pos += generate_tone(synth, out->buffer, pos, freqs[i], 130, 0.62f, 6, 0.03f);
        pos += generate_silence(synth, out->buffer, pos, 60);
    }
    
    out->num_samples = pos;
    return pos;
}

size_t generate_spotted_eagle_owl(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    for (int i = 0; i < 2; i++) {
        pos += generate_tone(synth, out->buffer, pos, 300, 200, 0.55f, 0, 0);
        pos += generate_silence(synth, out->buffer, pos, 150);
        pos += generate_tone(synth, out->buffer, pos, 280, 350, 0.58f, 3, 0.02f);
        pos += generate_silence(synth, out->buffer, pos, 400);
    }
    
    out->num_samples = pos;
    return pos;
}

size_t generate_fork_tailed_drongo(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    struct { float start; float end; uint32_t dur; } sweeps[] = {
        {2400, 2650, 110}, {2200, 2850, 130},
        {2650, 2300, 120}, {2850, 2400, 110}
    };
    
    for (int i = 0; i < 4; i++) {
        pos += generate_sweep(synth, out->buffer, pos, sweeps[i].start, 
                            sweeps[i].end, sweeps[i].dur, 0.68f);
        pos += generate_silence(synth, out->buffer, pos, 140);
    }
    
    out->num_samples = pos;
    return pos;
}

size_t generate_cape_canary(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    for (int i = 0; i < 8; i++) {
        float freq = 2600 + (i * 150);
        pos += generate_tone(synth, out->buffer, pos, freq, 90, 0.60f, 12, 0.04f);
        pos += generate_silence(synth, out->buffer, pos, 45);
    }
    
    out->num_samples = pos;
    return pos;
}

size_t generate_southern_masked_weaver(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;
    
    for (int i = 0; i < 6; i++) {
        float freq = 2000 + (i % 3) * 300;
        pos += generate_tremolo(synth, out->buffer, pos, freq, 110, 20, 0.6f, 0.62f);
        pos += generate_silence(synth, out->buffer, pos, 50);
    }
    
    out->num_samples = pos;
    return pos;
}


/**
 * @brief Internal helpers for Quelea that bypass the global VOLUME constant.
 *
 * Every other generator multiplies by VOLUME (0.20) which caps output at 20%
 * of full scale even at amplitude=1.0.  Quelea is a flood-signal bird —
 * it must be unmistakably loud.  These private helpers write at full synthesis
 * amplitude, and generate_red_billed_quelea() applies a post-process gain
 * boost at the end to reach the desired level independently of VOLUME.
 */
static size_t _quelea_sweep(bird_synthesizer_t *synth, int16_t *buf, size_t off,
                            float f0, float f1, uint32_t ms, float amp)
{
    float dur = ms / 1000.0f;
    size_t n   = (size_t)(synth->sample_rate * dur);
    for (size_t i = 0; i < n; i++) {
        float t    = (float)i / synth->sample_rate;
        float freq = f0 + (f1 - f0) * (t / dur);
        float env  = envelope(t, dur, 0.01f, 0.03f);
        buf[off + i] = (int16_t)(32767.0f * amp * env * sinf(2.0f * M_PI * freq * t));
    }
    return n;
}

static size_t _quelea_tremolo(bird_synthesizer_t *synth, int16_t *buf, size_t off,
                               float freq, uint32_t ms, float trate, float tdepth, float amp)
{
    float dur = ms / 1000.0f;
    size_t n   = (size_t)(synth->sample_rate * dur);
    for (size_t i = 0; i < n; i++) {
        float t  = (float)i / synth->sample_rate;
        float am = 1.0f - tdepth * (1.0f - (1.0f + sinf(2.0f * M_PI * trate * t)) / 2.0f);
        float env = envelope(t, dur, 0.02f, 0.05f);
        buf[off + i] = (int16_t)(32767.0f * amp * env * am * sinf(2.0f * M_PI * freq * t));
    }
    return n;
}

static size_t _quelea_harsh(bird_synthesizer_t *synth, int16_t *buf, size_t off,
                             float freq, uint32_t ms, uint8_t harmonics, float amp)
{
    float dur = ms / 1000.0f;
    size_t n   = (size_t)(synth->sample_rate * dur);
    for (size_t i = 0; i < n; i++) {
        float t   = (float)i / synth->sample_rate;
        float env = envelope(t, dur, 0.01f, 0.08f);
        float s   = 0.0f;
        for (uint8_t h = 1; h <= harmonics; h++)
            s += (1.0f / h) * sinf(2.0f * M_PI * freq * h * t);
        s /= sqrtf((float)harmonics);
        buf[off + i] = (int16_t)(32767.0f * amp * env * s);
    }
    return n;
}

size_t generate_red_billed_quelea(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;

    /*
     * Red-billed Quelea — colony chatter, unmistakably LOUD.
     * Written with VOLUME-bypass helpers so the global 0.20 master level
     * does not apply.  A post-process gain of 4.0× is applied at the end
     * so Quelea sits well above every other bird in perceived loudness.
     *
     * Phrase layout (~1.4 s total):
     *   1. Opening chatter burst  — 4 harsh staccato notes
     *   2. Contact whistle        — fast upward sweep
     *   3. Colony tremolo burst   — 4 rapid tremolo notes
     *   4. Rising chirp + fall    — paired sweeps
     *   5. Dense re-burst         — 2 harsh + 1 tremolo
     *   6. Closing sweep + buzz
     */

    /* --- Phrase 1: opening chatter --- */
    pos += _quelea_harsh  (synth, out->buffer, pos, 2200, 60, 2, 0.82f);
    pos += generate_silence(synth, out->buffer, pos, 28);
    pos += _quelea_harsh  (synth, out->buffer, pos, 2400, 55, 2, 0.85f);
    pos += generate_silence(synth, out->buffer, pos, 25);
    pos += _quelea_harsh  (synth, out->buffer, pos, 2300, 60, 2, 0.83f);
    pos += generate_silence(synth, out->buffer, pos, 28);
    pos += _quelea_harsh  (synth, out->buffer, pos, 2500, 50, 3, 0.80f);
    pos += generate_silence(synth, out->buffer, pos, 45);

    /* --- Phrase 2: contact whistle --- */
    pos += _quelea_sweep  (synth, out->buffer, pos, 1900, 3200, 120, 0.90f);
    pos += generate_silence(synth, out->buffer, pos, 40);

    /* --- Phrase 3: colony tremolo burst --- */
    pos += _quelea_tremolo(synth, out->buffer, pos, 2600, 70, 28, 0.55f, 0.85f);
    pos += generate_silence(synth, out->buffer, pos, 22);
    pos += _quelea_tremolo(synth, out->buffer, pos, 2850, 65, 28, 0.60f, 0.88f);
    pos += generate_silence(synth, out->buffer, pos, 22);
    pos += _quelea_tremolo(synth, out->buffer, pos, 2500, 70, 28, 0.55f, 0.85f);
    pos += generate_silence(synth, out->buffer, pos, 22);
    pos += _quelea_tremolo(synth, out->buffer, pos, 2700, 60, 30, 0.58f, 0.87f);
    pos += generate_silence(synth, out->buffer, pos, 40);

    /* --- Phrase 4: rising chirp + fall --- */
    pos += _quelea_sweep  (synth, out->buffer, pos, 2000, 3400, 100, 0.88f);
    pos += generate_silence(synth, out->buffer, pos, 18);
    pos += _quelea_sweep  (synth, out->buffer, pos, 3200, 2100,  80, 0.85f);
    pos += generate_silence(synth, out->buffer, pos, 35);

    /* --- Phrase 5: dense re-burst --- */
    pos += _quelea_harsh  (synth, out->buffer, pos, 2350, 55, 2, 0.83f);
    pos += generate_silence(synth, out->buffer, pos, 22);
    pos += _quelea_harsh  (synth, out->buffer, pos, 2600, 50, 2, 0.85f);
    pos += generate_silence(synth, out->buffer, pos, 22);
    pos += _quelea_tremolo(synth, out->buffer, pos, 2750, 80, 30, 0.60f, 0.88f);
    pos += generate_silence(synth, out->buffer, pos, 30);

    /* --- Phrase 6: closing sweep + final buzz --- */
    pos += _quelea_sweep  (synth, out->buffer, pos, 2100, 3300, 110, 0.88f);
    pos += generate_silence(synth, out->buffer, pos, 20);
    pos += _quelea_harsh  (synth, out->buffer, pos, 2300,  80, 3, 0.82f);

    out->num_samples = pos;

    /*
     * Post-process gain: synth->quelea_gain (default 1.5×, set from remote
     * config via echoes.c before each call).  Quelea bypasses the global
     * VOLUME constant so this is its sole loudness control.
     */
    float quelea_gain = synth->quelea_gain;
    for (size_t i = 0; i < pos; i++) {
        int32_t v = (int32_t)((float)out->buffer[i] * quelea_gain);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        out->buffer[i] = (int16_t)v;
    }

    return pos;
}
/**
 * @brief African Paradise Flycatcher (Terpsiphone viridis)
 *
 * A fast, liquid, high-pitched song — cascading phrases in the 3–4 kHz range
 * with rapid descending sweeps and short tonal notes.  This is the signature
 * response to DETECTION_BIRDSONG: high-freq dominant, melodic, energetic.
 *
 * Structure:
 *   1. Opening liquid phrase  — three fast descending sweeps (3.8→2.8 kHz)
 *   2. Ascending trill        — rapid upward tone burst
 *   3. Core phrase (×2)       — two-note motif: quick sweep + held note
 *   4. Cascading finish       — five fast sweeps descending in pitch
 */
size_t generate_paradise_flycatcher(bird_synthesizer_t *synth, audio_buffer_t *out) {
    size_t pos = 0;

    /* --- Phrase 1: opening liquid cascades --- */
    pos += generate_sweep(synth, out->buffer, pos, 3800, 2900, 130, 0.68f);
    pos += generate_silence(synth, out->buffer, pos, 35);
    pos += generate_sweep(synth, out->buffer, pos, 3600, 2750, 120, 0.70f);
    pos += generate_silence(synth, out->buffer, pos, 35);
    pos += generate_sweep(synth, out->buffer, pos, 3400, 2600, 110, 0.68f);
    pos += generate_silence(synth, out->buffer, pos, 60);

    /* --- Phrase 2: ascending trill burst --- */
    pos += generate_tone(synth, out->buffer, pos, 3200, 90, 0.65f, 14, 0.05f);
    pos += generate_silence(synth, out->buffer, pos, 25);
    pos += generate_tone(synth, out->buffer, pos, 3500, 90, 0.68f, 14, 0.05f);
    pos += generate_silence(synth, out->buffer, pos, 25);
    pos += generate_tone(synth, out->buffer, pos, 3800, 100, 0.70f, 14, 0.05f);
    pos += generate_silence(synth, out->buffer, pos, 55);

    /* --- Phrase 3: characteristic two-note motif, repeated twice --- */
    for (int rep = 0; rep < 2; rep++) {
        pos += generate_sweep(synth, out->buffer, pos, 3600, 2800, 100, 0.70f);
        pos += generate_silence(synth, out->buffer, pos, 20);
        pos += generate_tone(synth, out->buffer, pos, 2800, 160, 0.65f, 6, 0.03f);
        pos += generate_silence(synth, out->buffer, pos, 60);
    }

    /* --- Phrase 4: cascading finish — five rapid descending sweeps --- */
    float start_freqs[] = {3900, 3650, 3400, 3150, 2900};
    float end_freqs[]   = {3000, 2800, 2600, 2400, 2200};
    for (int i = 0; i < 5; i++) {
        pos += generate_sweep(synth, out->buffer, pos, start_freqs[i], end_freqs[i], 85, 0.65f - i * 0.03f);
        pos += generate_silence(synth, out->buffer, pos, 28);
    }

    out->num_samples = pos;
    return pos;
}

/* ========================================================================
 * BIRD CALL MAPPER
 * ======================================================================== */

void bird_mapper_init(bird_call_mapper_t *mapper, uint32_t sample_rate) {
    mapper->synth.sample_rate = sample_rate;
    mapper->synth.chunk_size = CHUNK_SIZE;
    mapper->synth.quelea_gain = 1.2f;   /* default; overwritten by echoes.c from remote config */   /* default; overwritten by echoes.c from remote config */

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

    /* CLAP: percussive, rapid, aggressive */
    mapper->clap_birds[0] = (bird_info_t){"fork_tailed_drongo",     "Fork-tailed Drongo"};
    mapper->clap_birds[1] = (bird_info_t){"glossy_starling",         "Glossy Starling"};
    mapper->clap_birds[2] = (bird_info_t){"southern_masked_weaver",  "Masked Weaver"};
    mapper->clap_birds[3] = (bird_info_t){"red_billed_quelea",       "Red-billed Quelea"};
    mapper->num_clap_birds = 4;

    /* BIRDSONG: high-frequency melodic detections — richly patterned songs */
    mapper->birdsong_birds[0] = (bird_info_t){"paradise_flycatcher", "Paradise Flycatcher"};
    mapper->birdsong_birds[1] = (bird_info_t){"cape_robin_chat",      "Cape Robin-Chat"};
    mapper->birdsong_birds[2] = (bird_info_t){"cape_canary",          "Cape Canary"};
    mapper->num_birdsong_birds = 3;
}

bird_info_t bird_mapper_get_bird(bird_call_mapper_t *mapper,
                                  detection_type_t detection_type) {
    bird_info_t *bird_list;
    uint8_t list_len;
    
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
        case DETECTION_BIRDSONG:
            bird_list = mapper->birdsong_birds;
            list_len = mapper->num_birdsong_birds;
            break;
        default:
            return (bird_info_t){"", "Unknown"};
    }

    if (list_len == 0) return (bird_info_t){"", "Unknown"};

    /* Modulo directly against list_len — the previous code did (r % 5) % list_len
     * which skewed selection toward lower indices whenever list_len < 5.       */
    uint32_t index = esp_random() % list_len;
    return bird_list[index];
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
        {"red_billed_quelea",     generate_red_billed_quelea},
        {"paradise_flycatcher",   generate_paradise_flycatcher},
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

        /* Night birdsong: unlikely but map to owl + dove (most atmospheric) */
        mapper->birdsong_birds[0] = (bird_info_t){"spotted_eagle_owl", "Eagle-Owl"};
        mapper->birdsong_birds[1] = (bird_info_t){"red_eyed_dove",     "Red-eyed Dove"};
        mapper->num_birdsong_birds = 2;
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

        /* Dawn birdsong: chorus birds kicking off the day */
        mapper->birdsong_birds[0] = (bird_info_t){"cape_robin_chat",     "Cape Robin-Chat"};
        mapper->birdsong_birds[1] = (bird_info_t){"paradise_flycatcher", "Paradise Flycatcher"};
        mapper->num_birdsong_birds = 2;
    }
    else if (lux < 500.0f) {
        /* Overcast: moderate mix */
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
        mapper->clap_birds[3] = (bird_info_t){"red_billed_quelea",      "Red-billed Quelea"};
        mapper->num_clap_birds = 4;

        mapper->birdsong_birds[0] = (bird_info_t){"paradise_flycatcher", "Paradise Flycatcher"};
        mapper->birdsong_birds[1] = (bird_info_t){"cape_robin_chat",     "Cape Robin-Chat"};
        mapper->birdsong_birds[2] = (bird_info_t){"cape_canary",         "Cape Canary"};
        mapper->num_birdsong_birds = 3;
    }
    else {
        /* Bright/Sunny: full lively set */
        mapper->whistle_birds[0] = (bird_info_t){"piet_my_vrou",    "Red-chested Cuckoo"};
        mapper->whistle_birds[1] = (bird_info_t){"cape_robin_chat", "Cape Robin-Chat"};
        mapper->whistle_birds[2] = (bird_info_t){"glossy_starling", "Glossy Starling"};
        mapper->whistle_birds[3] = (bird_info_t){"cape_canary",     "Cape Canary"};
        mapper->num_whistle_birds = 4;

        mapper->voice_birds[0] = (bird_info_t){"red_eyed_dove",    "Red-eyed Dove"};
        mapper->voice_birds[1] = (bird_info_t){"southern_boubou",  "Southern Boubou"};
        mapper->voice_birds[2] = (bird_info_t){"spotted_eagle_owl","Eagle-Owl"};
        mapper->num_voice_birds = 3;

        mapper->clap_birds[0] = (bird_info_t){"fork_tailed_drongo",    "Fork-tailed Drongo"};
        mapper->clap_birds[1] = (bird_info_t){"glossy_starling",        "Glossy Starling"};
        mapper->clap_birds[2] = (bird_info_t){"southern_masked_weaver", "Masked Weaver"};
        mapper->clap_birds[3] = (bird_info_t){"red_billed_quelea",      "Red-billed Quelea"};
        mapper->num_clap_birds = 4;

        mapper->birdsong_birds[0] = (bird_info_t){"paradise_flycatcher", "Paradise Flycatcher"};
        mapper->birdsong_birds[1] = (bird_info_t){"cape_canary",         "Cape Canary"};
        mapper->birdsong_birds[2] = (bird_info_t){"cape_robin_chat",     "Cape Robin-Chat"};
        mapper->num_birdsong_birds = 3;
    }
}
