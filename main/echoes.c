/**
 * @file echoes.c
 * @brief Echoes of the Machine implementation
 */

#include "echoes.h"
#include "synthesis.h"
#include "espnow_mesh.h"
#include "markov.h"
#include "remote_config.h"
#include <string.h>
#include <stdlib.h>

// ESP32-specific includes
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ECHOES";

/* Global system state */
static system_state_t g_system_state = {0};
static bird_call_mapper_t g_bird_mapper = {0};
static markov_chain_t g_markov = {0};
static audio_buffer_t g_audio_buffer = {0};  // Static buffer for bird calls
static SemaphoreHandle_t s_playback_mutex = NULL;  // Prevents concurrent I2S writes

/* Global hardware configuration */
static hardware_config_t g_hw_config = HW_CONFIG_FULL;

/* Goertzel coefficients (precomputed) */
static float g_coeff_whistle;
static float g_coeff_voice;
static float g_coeff_birdsong;   /**< Precomputed coefficient for BIRDSONG_FREQ */

/* I2S */
static i2s_chan_handle_t mic_chan = NULL;   // global handle for microphone channel
static i2s_chan_handle_t spk_chan = NULL;

/* I2C */
static i2c_master_bus_handle_t bus_handle = NULL;
static i2c_master_dev_handle_t bh1750_handle = NULL;

/* ADC */
static adc_oneshot_unit_handle_t adc_handle = NULL;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static hardware_config_t detect_hardware_config(void);

/* ========================================================================
 * INITIALIZATION
 * ======================================================================== */

void led_init(void) {
    // Configure LED PWM
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_16_BIT,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);
    
    // White LED
    ledc_channel_config_t ledc_channel_white = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PIN_LED_WHITE,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel_white);
    
    // Blue LED
    ledc_channel_config_t ledc_channel_blue = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PIN_LED_BLUE,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel_blue);
    
    ESP_LOGI(TAG, "LEDs initialized");
}

esp_err_t i2s_microphone_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &mic_chan));

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,   // or try I2S_DATA_BIT_WIDTH_32BIT
        I2S_SLOT_MODE_MONO
    );

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_SCK,
            .ws   = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_MIC_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_chan));

    ESP_LOGI(TAG, "Microphone (I2S RX) initialized @ %d Hz", SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t i2s_speaker_init(void)
{
    // Configure I2S channel with explicit DMA buffer settings
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_1,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,          // Increased: 8×512 = 4096 frames = 256 ms headroom
        .dma_frame_num = 512,       // Number of frames per DMA buffer
        .auto_clear = true,         // Auto clear DMA buffer on underflow (prevents looping!)
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &spk_chan, NULL));

    // Same MSB mono config for output
    i2s_std_slot_config_t slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_MONO
    );

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_SPK_SCK,
            .ws   = PIN_SPK_WS,
            .dout = PIN_SPK_SD,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(spk_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(spk_chan));

    ESP_LOGI(TAG, "Speaker (I2S TX) initialized @ %d Hz", SAMPLE_RATE);
    return ESP_OK;
}

void light_sensor_init(void)
{
    g_system_state.light_sensor_type = LIGHT_SENSOR_NONE;
    esp_err_t ret;

    // Create I2C bus
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ret = i2c_new_master_bus(&bus_config, &bus_handle);

    if (ret == ESP_OK) {
        // Try BH1750
        ret = i2c_master_probe(bus_handle,
                               BH1750_ADDR_LOW,
                               100 / portTICK_PERIOD_MS);

        if (ret == ESP_OK) {
            // Add device
            i2c_device_config_t dev_config = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = BH1750_ADDR_LOW,
                .scl_speed_hz = 100000,
            };

            ret = i2c_master_bus_add_device(bus_handle,
                                            &dev_config,
                                            &bh1750_handle);

            if (ret == ESP_OK) {
                g_system_state.bh1750_addr = BH1750_ADDR_LOW;
                g_system_state.light_sensor_type = LIGHT_SENSOR_BH1750;

                // Initialize BH1750 (continuous high-res mode = 0x10)
                uint8_t cmd = 0x10;

                ret = i2c_master_transmit(bh1750_handle,
                                          &cmd,
                                          1,
                                          -1);

                if (ret == ESP_OK) {
                    ESP_LOGI(TAG,
                             "BH1750 light sensor detected at 0x%02X",
                             BH1750_ADDR_LOW);
                    return;  // Done — no fallback needed
                }

                // If init failed, remove device
                i2c_master_bus_rm_device(bh1750_handle);
                bh1750_handle = NULL;
            }
        }
    }

    // Fallback to analog sensor
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };

    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(adc_handle,
                                   ADC_CHANNEL_6,
                                   &chan_cfg)
    );

    g_system_state.light_sensor_type = LIGHT_SENSOR_ANALOG;
    ESP_LOGI(TAG, "Using analog light sensor on GPIO34");
}

void system_init(void) {
    memset(&g_system_state, 0, sizeof(g_system_state));
    
    // Initialize detection state
    g_system_state.detection.running_avg_whistle  = 1000.0f;
    g_system_state.detection.running_avg_voice    = 1000.0f;
    g_system_state.detection.running_avg_birdsong = 1000.0f;
    
    // Precompute Goertzel coefficients using remote config (or defaults)
    {
        const remote_config_t *cfg = remote_config_get();
        g_coeff_whistle  = 2.0f * cosf(2.0f * M_PI * cfg->whistle_freq  / SAMPLE_RATE);
        g_coeff_voice    = 2.0f * cosf(2.0f * M_PI * cfg->voice_freq    / SAMPLE_RATE);
        g_coeff_birdsong = 2.0f * cosf(2.0f * M_PI * cfg->birdsong_freq / SAMPLE_RATE);
    }
    
    // Initialize light sensor (this detects BH1750 vs analog)
    light_sensor_init();
    
    // Detect hardware configuration based on light sensor
    g_hw_config = detect_hardware_config();
    
    // Initialize bird mapper
    bird_mapper_init(&g_bird_mapper, SAMPLE_RATE);

    // Initialize Markov chain (NVS must already be initialised by app_main)
    markov_init(&g_markov, &g_bird_mapper);

    // Create playback mutex (guards the I2S speaker channel)
    s_playback_mutex = xSemaphoreCreateMutex();
    configASSERT(s_playback_mutex != NULL);
    
    ESP_LOGI(TAG, "System initialized");
}

/* ========================================================================
 * LED CONTROL
 * ======================================================================== */

void set_led(float white_level, float blue_level) {
    uint32_t duty_white = (uint32_t)(65535.0f * white_level);
    
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_white);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    
    // Blue LED disabled (uncomment to enable)
    // uint32_t duty_blue = (uint32_t)(65535.0f * blue_level);
    // ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty_blue);
    // ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    
    (void)blue_level;  // Suppress unused parameter warning
}

/* ========================================================================
 * AUDIO PROCESSING
 * ======================================================================== */

void apply_gain_inplace(int16_t *buffer, size_t num_samples, float gain) {
    for (size_t i = 0; i < num_samples; i++) {
        int32_t v = (int32_t)(buffer[i] * gain);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        buffer[i] = (int16_t)v;
    }
}

void compute_goertzel(const int16_t *buffer, size_t num_samples,
                      float *mag_whistle, float *mag_voice, float *mag_birdsong) {
    float q1_w = 0.0f, q2_w = 0.0f;
    float q1_v = 0.0f, q2_v = 0.0f;
    float q1_b = 0.0f, q2_b = 0.0f;
    
    for (size_t i = 0; i < num_samples; i++) {
        float v = (float)buffer[i];
        
        // Whistle frequency (2000 Hz)
        float q0_w = g_coeff_whistle * q1_w - q2_w + v;
        q2_w = q1_w;
        q1_w = q0_w;
        
        // Voice frequency (200 Hz)
        float q0_v = g_coeff_voice * q1_v - q2_v + v;
        q2_v = q1_v;
        q1_v = q0_v;

        // Birdsong frequency (3500 Hz)
        float q0_b = g_coeff_birdsong * q1_b - q2_b + v;
        q2_b = q1_b;
        q1_b = q0_b;
    }
    
    float mag_sq_w = q1_w * q1_w + q2_w * q2_w - g_coeff_whistle  * q1_w * q2_w;
    float mag_sq_v = q1_v * q1_v + q2_v * q2_v - g_coeff_voice    * q1_v * q2_v;
    float mag_sq_b = q1_b * q1_b + q2_b * q2_b - g_coeff_birdsong * q1_b * q2_b;
    
    *mag_whistle  = sqrtf(mag_sq_w > 0.0f ? mag_sq_w : 0.0f);
    *mag_voice    = sqrtf(mag_sq_v > 0.0f ? mag_sq_v : 0.0f);
    *mag_birdsong = sqrtf(mag_sq_b > 0.0f ? mag_sq_b : 0.0f);
}

/* ========================================================================
 * LIGHT SENSOR
 * ======================================================================== */

float get_lux_level(void)
{
    if (g_system_state.light_sensor_type == LIGHT_SENSOR_BH1750) {

        uint8_t data[2];
        esp_err_t ret = i2c_master_receive(
            bh1750_handle,
            data,
            sizeof(data),
            100 / portTICK_PERIOD_MS
        );

        if (ret == ESP_OK) {
            uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
            return raw / 1.2f;   // BH1750 lux conversion
        }

        return -1.0f;
    }
    else if (g_system_state.light_sensor_type == LIGHT_SENSOR_ANALOG) {

        int adc_raw;
        esp_err_t ret = adc_oneshot_read(adc_handle,
                                         ADC_CHANNEL_6,
                                         &adc_raw);

        if (ret == ESP_OK) {
            return adc_raw * 0.24f;   // Your rough lux estimate
        }

        return -1.0f;
    }

    return -1.0f;
}

/* ========================================================================
 * HARDWARE DETECTION
 * ======================================================================== */

/**
 * @brief Detect which hardware configuration is present
 */
static hardware_config_t detect_hardware_config(void)
{
    // BH1750 presence determines hardware type
    if (g_system_state.light_sensor_type == LIGHT_SENSOR_BH1750) {
        ESP_LOGI(TAG, "Hardware detected: FULL (BH1750 + Audio)");
        return HW_CONFIG_FULL;
    } else {
        ESP_LOGI(TAG, "Hardware detected: MINIMAL (Analog sensor, LED VU only)");
        return HW_CONFIG_MINIMAL;
    }
}

/**
 * @brief Calculate VU meter level from audio buffer (digital/stepped version)
 */
static float calculate_vu_level(const int16_t *buffer, size_t num_samples)
{
    // Calculate RMS (Root Mean Square) of audio signal
    float sum_squares = 0.0f;
    for (size_t i = 0; i < num_samples; i++) {
        float sample = (float)buffer[i] / 32768.0f;  // Normalize to -1.0 to 1.0
        sum_squares += sample * sample;
    }
    float rms = sqrtf(sum_squares / num_samples);
    
    // Scale to reasonable range (reduced sensitivity)
    float level = rms * 5.0f;  // Much less sensitive than before (was 20.0f)
    
    // Digital/stepped output levels with wider dead zone
    if (level < 0.15f) {
        // Dead zone - LED off for quiet/silence (wider threshold)
        return 0.0f;
    } else if (level < 0.30f) {
        // Level 1 - Low
        return 0.20f;
    } else if (level < 0.50f) {
        // Level 2 - Medium-low
        return 0.40f;
    } else if (level < 0.70f) {
        // Level 3 - Medium
        return 0.60f;
    } else if (level < 0.90f) {
        // Level 4 - Medium-high
        return 0.80f;
    } else {
        // Level 5 - High (max)
        return remote_config_get()->vu_max_brightness;
    }
}

/**
 * @brief Smooth VU meter updates with hysteresis (prevents rapid flickering)
 */
static float smooth_vu_level(float current, float target, float smooth_factor)
{
    (void)smooth_factor;  // Unused - kept for API compatibility
    
    // If target is zero (dead zone), turn off immediately
    if (target == 0.0f) {
        return 0.0f;
    }
    
    // If we're off and target is on, turn on immediately
    if (current == 0.0f && target > 0.0f) {
        return target;
    }
    
    // For level changes, use fast transitions
    const float FAST_SMOOTH = 0.3f;  // Fast response for level changes
    return current * FAST_SMOOTH + target * (1.0f - FAST_SMOOTH);
}

/* ========================================================================
 * DETECTION TASK
 * ======================================================================== */


/* =========================================================================
 * PLAYBACK — all four entry points defined here, before audio_detection_task
 *
 *  _play_locked()                        stream to I2S; caller holds mutex
 *  play_bird_call()                      4 s wait; used by markov.c
 *  generate_and_play_bird_call()         4 s wait; used by lux task
 *  generate_and_play_bird_call_nowait()  0 ms; used by detection task
 *                                        (never blocks i2s_channel_read)
 * ======================================================================== */

static void _play_locked(const char *bird_name, const audio_buffer_t *audio_buffer)
{
    ESP_LOGI(TAG, "🐦 Playing: %s (%zu samples)", bird_name, audio_buffer->num_samples);

    int16_t global_peak = 1;
    for (size_t i = 0; i < audio_buffer->num_samples; i++) {
        int16_t av = audio_buffer->buffer[i] < 0 ? -audio_buffer->buffer[i]
                                                  :  audio_buffer->buffer[i];
        if (av > global_peak) global_peak = av;
    }

    float vol_scale = get_volume_for_lux(get_lux_level());
    const remote_config_t *_pcfg = remote_config_get();
    set_led(_pcfg->vu_max_brightness * vol_scale, BRIGHT_OFF);

    size_t total_bytes = audio_buffer->num_samples * sizeof(int16_t);
    size_t bytes_sent  = 0;
    static int16_t s_scaled_chunk[CHUNK_SIZE];

    while (bytes_sent < total_bytes) {
        size_t bytes_to_send    = CHUNK_SIZE * sizeof(int16_t);
        size_t samples_in_chunk = CHUNK_SIZE;
        if (bytes_sent + bytes_to_send > total_bytes) {
            bytes_to_send    = total_bytes - bytes_sent;
            samples_in_chunk = bytes_to_send / sizeof(int16_t);
        }
        const int16_t *chunk_ptr = audio_buffer->buffer + (bytes_sent / sizeof(int16_t));
        int16_t peak = 1;
        for (size_t i = 0; i < samples_in_chunk; i++) {
            int32_t scaled = (int32_t)(chunk_ptr[i] * vol_scale);
            if (scaled >  32767) scaled =  32767;
            if (scaled < -32768) scaled = -32768;
            s_scaled_chunk[i] = (int16_t)scaled;
            int16_t av = scaled < 0 ? (int16_t)-scaled : (int16_t)scaled;
            if (av > peak) peak = av;
        }
        set_led(((float)peak / (float)global_peak) * _pcfg->vu_max_brightness * vol_scale, BRIGHT_OFF);

        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(spk_chan,
                            (const uint8_t *)s_scaled_chunk,
                            samples_in_chunk * sizeof(int16_t),
                            &bytes_written,
                            portMAX_DELAY);
        if (ret != ESP_OK || bytes_written == 0) {
            ESP_LOGE(TAG, "I2S write error: %s (written=%zu)", esp_err_to_name(ret), bytes_written);
            break;
        }
        bytes_sent += bytes_written;
    }
    set_led(BRIGHT_OFF, BRIGHT_OFF);
}

void play_bird_call(const char *bird_name, const audio_buffer_t *audio_buffer)
{
    if (spk_chan == NULL) {
        ESP_LOGW(TAG, "play_bird_call: no speaker — skipping '%s'", bird_name);
        return;
    }
    if (s_playback_mutex == NULL ||
        xSemaphoreTake(s_playback_mutex, pdMS_TO_TICKS(4000)) != pdTRUE) {
        ESP_LOGW(TAG, "play_bird_call: speaker busy, skipping '%s'", bird_name);
        return;
    }
    _play_locked(bird_name, audio_buffer);
    xSemaphoreGive(s_playback_mutex);
    vTaskDelay(pdMS_TO_TICKS(100));
}

void generate_and_play_bird_call(bird_call_mapper_t *mapper,
                                 const char *function_name,
                                 const char *display_name)
{
    if (spk_chan == NULL) return;
    if (s_playback_mutex == NULL ||
        xSemaphoreTake(s_playback_mutex, pdMS_TO_TICKS(4000)) != pdTRUE) {
        ESP_LOGW(TAG, "generate_and_play: speaker busy, skipping '%s'", display_name);
        return;
    }
    bird_mapper_generate_call(mapper, function_name, &g_audio_buffer);
    _play_locked(display_name, &g_audio_buffer);
    xSemaphoreGive(s_playback_mutex);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void generate_and_play_bird_call_nowait(bird_call_mapper_t *mapper,
                                               const char *function_name,
                                               const char *display_name)
{
    if (spk_chan == NULL) return;
    /* 0 ms: if the speaker is busy, skip immediately so i2s_channel_read
     * in the detection loop is never starved.                            */
    if (s_playback_mutex == NULL ||
        xSemaphoreTake(s_playback_mutex, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Detection: speaker busy, skipping '%s'", display_name);
        return;
    }
    bird_mapper_generate_call(mapper, function_name, &g_audio_buffer);
    _play_locked(display_name, &g_audio_buffer);
    xSemaphoreGive(s_playback_mutex);
    vTaskDelay(pdMS_TO_TICKS(100));
}

void audio_detection_task(void *param) {
    int16_t *buffer = (int16_t*)malloc(BUFFER_SIZE * sizeof(int16_t));
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        vTaskDelete(NULL);
        return;
    }
    
    detection_state_t *state = &g_system_state.detection;
    set_led(BRIGHT_OFF, BRIGHT_OFF);
    
    float vu_level = 0.0f;  // Current VU meter level
    
    ESP_LOGI(TAG, "Audio detection task started");
    
    if (g_hw_config == HW_CONFIG_FULL) {
        ESP_LOGI(TAG, "🎤 Listening for whistles, voice, claps, and birdsong...");
    } else {
        ESP_LOGI(TAG, "🎤 LED VU meter mode active (digital stepped output)");
    }
    
    while (1) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(
            mic_chan,
            buffer,
            BUFFER_SIZE * sizeof(int16_t),
            &bytes_read,
            portMAX_DELAY
        );

        if (ret != ESP_OK || bytes_read == 0) {
            ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t num_samples = bytes_read / sizeof(int16_t);
        
        // Apply gain from remote config
        apply_gain_inplace(buffer, num_samples, remote_config_get()->gain);
        
        // Branch based on hardware configuration
        if (g_hw_config == HW_CONFIG_FULL) {
            /* ============================================================
             * FULL SYSTEM: Sound detection and bird call playback
             * ============================================================ */
            
            // Snapshot config once per loop for consistency
            const remote_config_t *cfg = remote_config_get();

            // Recompute Goertzel coefficients if frequencies changed
            {
                float new_cw = 2.0f * cosf(2.0f * M_PI * cfg->whistle_freq  / SAMPLE_RATE);
                float new_cv = 2.0f * cosf(2.0f * M_PI * cfg->voice_freq    / SAMPLE_RATE);
                if (new_cw != g_coeff_whistle || new_cv != g_coeff_voice) {
                    g_coeff_whistle  = new_cw;
                    g_coeff_voice    = new_cv;
                    g_coeff_birdsong = 2.0f * cosf(2.0f * M_PI * cfg->birdsong_freq / SAMPLE_RATE);
                    ESP_LOGI(TAG, "Goertzel: whistle=%luHz voice=%luHz birdsong=%luHz",
                             (unsigned long)cfg->whistle_freq,
                             (unsigned long)cfg->voice_freq,
                             (unsigned long)cfg->birdsong_freq);
                }
                /* Keep synth parameters in sync with remote config */
                g_bird_mapper.synth.quelea_gain = cfg->quelea_gain;
            }

            // Compute frequencies
            float mag_w, mag_v, mag_b;
            compute_goertzel(buffer, num_samples, &mag_w, &mag_v, &mag_b);
            
            // Update adaptive thresholds
            state->running_avg_whistle  = ALPHA * state->running_avg_whistle  + (1.0f - ALPHA) * mag_w;
            state->running_avg_voice    = ALPHA * state->running_avg_voice    + (1.0f - ALPHA) * mag_v;
            state->running_avg_birdsong = ALPHA * state->running_avg_birdsong + (1.0f - ALPHA) * mag_b;
            
            float thresh_w    = state->running_avg_whistle  * cfg->whistle_multiplier;
            float thresh_v    = state->running_avg_voice    * cfg->voice_multiplier;
            float thresh_b    = state->running_avg_birdsong * cfg->birdsong_multiplier;
            float thresh_clap = fmaxf(thresh_w * cfg->clap_multiplier, thresh_v * 2.0f);
            
            // Handle debounce
            if (state->debounce_counter > 0) {
                state->debounce_counter--;
                state->whistle_count   = 0;
                state->voice_count     = 0;
                state->clap_count      = 0;
                state->birdsong_count  = 0;
            }
            else {
                // Check for clap (broadband impulse — both bands spike simultaneously)
                if (mag_w > thresh_clap && mag_v > thresh_v * 1.5f) {
                    state->clap_count++;
                    state->whistle_count = state->voice_count = state->birdsong_count = 0;
                    
                    if (state->clap_count >= cfg->clap_confirm) {
                        ESP_LOGI(TAG, "👏 CLAP! (w:%.0f, v:%.0f)", mag_w, mag_v);
                        espnow_mesh_broadcast_sound(DETECTION_CLAP);
                        markov_on_event(&g_markov, DETECTION_CLAP, get_lux_level());
                        
                        bird_info_t bird;
                        if (espnow_mesh_is_flooded()) {
                            bird = (bird_info_t){"red_billed_quelea", "Red-billed Quelea"};
                        } else {
                            bird = bird_mapper_get_bird(&g_bird_mapper, DETECTION_CLAP);
                        }
                        generate_and_play_bird_call_nowait(&g_bird_mapper, bird.function_name, bird.display_name);
                        
                        state->clap_count = 0;
                        state->debounce_counter = cfg->debounce_buffers;
                    }
                }
                // Check for birdsong (3500 Hz dominant, with mid-freq presence, no strong voice)
                // Spectral signature: mag_b exceeds threshold AND is stronger than mid by
                // BIRDSONG_HF_RATIO AND there is some (but not dominant) mid-freq energy.
                else if (mag_b > thresh_b &&
                         mag_b > mag_w * cfg->birdsong_hf_ratio &&
                         mag_w > thresh_w * cfg->birdsong_mf_min &&
                         mag_v < thresh_v) {
                    state->birdsong_count++;
                    state->whistle_count = state->voice_count = state->clap_count = 0;

                    if (state->birdsong_count >= cfg->birdsong_confirm) {
                        ESP_LOGI(TAG, "🐦 BIRDSONG! (b:%.0f, w:%.0f, v:%.0f)", mag_b, mag_w, mag_v);
                        espnow_mesh_broadcast_sound(DETECTION_BIRDSONG);
                        markov_on_event(&g_markov, DETECTION_BIRDSONG, get_lux_level());

                        bird_info_t bird;
                        if (espnow_mesh_is_flooded()) {
                            bird = (bird_info_t){"red_billed_quelea", "Red-billed Quelea"};
                        } else {
                            bird = bird_mapper_get_bird(&g_bird_mapper, DETECTION_BIRDSONG);
                        }
                        generate_and_play_bird_call_nowait(&g_bird_mapper, bird.function_name, bird.display_name);

                        state->birdsong_count = 0;
                        state->debounce_counter = cfg->debounce_buffers;
                    }
                }
                // Check for whistle (high frequency, narrow band — no significant 3500 Hz content)
                else if (mag_w > thresh_w && (mag_w / (mag_v + 1.0f)) > 3.0f &&
                         mag_b < thresh_b) {
                    state->whistle_count++;
                    state->clap_count = state->voice_count = state->birdsong_count = 0;
                    
                    if (state->whistle_count >= cfg->whistle_confirm) {
                        ESP_LOGI(TAG, "🎵 WHISTLE! (w:%.0f, v:%.0f)", mag_w, mag_v);
                        espnow_mesh_broadcast_sound(DETECTION_WHISTLE);
                        markov_on_event(&g_markov, DETECTION_WHISTLE, get_lux_level());
                        
                        bird_info_t bird;
                        if (espnow_mesh_is_flooded()) {
                            bird = (bird_info_t){"red_billed_quelea", "Red-billed Quelea"};
                        } else {
                            bird = bird_mapper_get_bird(&g_bird_mapper, DETECTION_WHISTLE);
                        }
                        generate_and_play_bird_call_nowait(&g_bird_mapper, bird.function_name, bird.display_name);
                        
                        state->whistle_count = 0;
                        state->debounce_counter = cfg->debounce_buffers;
                    }
                }
                // Check for voice (low frequency)
                else if (mag_v > thresh_v) {
                    state->voice_count++;
                    state->whistle_count = state->clap_count = state->birdsong_count = 0;
                    
                    if (state->voice_count >= cfg->voice_confirm) {
                        ESP_LOGI(TAG, "🗣️ VOICE! (w:%.0f, v:%.0f)", mag_w, mag_v);
                        espnow_mesh_broadcast_sound(DETECTION_VOICE);
                        markov_on_event(&g_markov, DETECTION_VOICE, get_lux_level());
                        
                        bird_info_t bird;
                        if (espnow_mesh_is_flooded()) {
                            bird = (bird_info_t){"red_billed_quelea", "Red-billed Quelea"};
                        } else {
                            bird = bird_mapper_get_bird(&g_bird_mapper, DETECTION_VOICE);
                        }
                        generate_and_play_bird_call_nowait(&g_bird_mapper, bird.function_name, bird.display_name);
                        
                        state->voice_count = 0;
                        state->debounce_counter = cfg->debounce_buffers;
                    }
                }
                // No detection
                else {
                    state->whistle_count = state->voice_count = state->clap_count = state->birdsong_count = 0;
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(4));
            
        } else {
            /* ============================================================
             * MINIMAL SYSTEM: LED VU meter only
             * ============================================================ */
            
            // Calculate VU level from audio buffer (digital stepped output)
            float target_vu = calculate_vu_level(buffer, num_samples);
            
            // Apply hysteresis to prevent flickering
            vu_level = smooth_vu_level(vu_level, target_vu, 0.0f);  // 3rd param unused but kept for compatibility
            
            // Update white LED to show audio level
            // Blue LED stays off in minimal mode
            set_led(vu_level, 0.0f);
            
            // Small delay
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    
    free(buffer);
    vTaskDelete(NULL);
}

/* ========================================================================
 * PLAYBACK
 * ======================================================================== */

/* ========================================================================
 * VOLUME SCALING
 * ======================================================================== */

/**
 * @brief Compute playback volume multiplier from current lux level.
 *
 * Scales linearly between VOLUME_SCALE_MIN (dark) and VOLUME_SCALE_MAX
 * (bright).  This is applied at render time inside play_bird_call() as a
 * per-sample gain so synthesis amplitudes remain unchanged.
 */
float get_volume_for_lux(float lux)
{
    const remote_config_t *cfg = remote_config_get();
    if (lux <= cfg->volume_lux_min) return cfg->volume_scale_min;
    if (lux >= cfg->volume_lux_max) return cfg->volume_scale_max;
    float t = (lux - cfg->volume_lux_min) / (cfg->volume_lux_max - cfg->volume_lux_min);
    return cfg->volume_scale_min + t * (cfg->volume_scale_max - cfg->volume_scale_min);
}



/* ========================================================================
 * LUX-BASED BIRD SELECTION
 * ======================================================================== */

void lux_based_birds_task(void *param) {
    if (g_system_state.light_sensor_type == LIGHT_SENSOR_NONE) {
        ESP_LOGW(TAG, "Lux-based bird selection disabled (no sensor)");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Lux-based bird selection enabled");

    float last_acted_lux = -1000.0f;  /* lux at the last mapper update */
    float prev_lux       = -1.0f;     /* reading from the previous tick */

    while (1) {
        const remote_config_t *cfg = remote_config_get();

        float lux = get_lux_level();

        if (lux < 0.0f) {
            vTaskDelay(pdMS_TO_TICKS(cfg->lux_poll_interval_ms));
            continue;
        }

        float delta     = lux - last_acted_lux;
        float raw_delta = (prev_lux >= 0.0f) ? (lux - prev_lux) : 0.0f;
        float last_lux  = prev_lux;
        prev_lux = lux;

        /* Flash detection using remote config thresholds */
        bool is_flash = (fabsf(raw_delta) >= cfg->lux_flash_threshold) ||
                        (fabsf(raw_delta) >= cfg->lux_flash_min_abs &&
                         prev_lux > 0.0f &&
                         fabsf(raw_delta) >= prev_lux * cfg->lux_flash_percent);

        if (is_flash) {
            ESP_LOGI(TAG, "⚡ Flash: %.1f → %.1f lux (Δ%.1f)", last_lux, lux, raw_delta);

            /* Bright flash → WHISTLE (alert); sudden dark → VOICE (quiet) */
            detection_type_t flash_det = (raw_delta > 0.0f)
                                         ? DETECTION_WHISTLE
                                         : DETECTION_VOICE;

            markov_on_event(&g_markov, flash_det, lux);

            if (has_audio_output()) {
                float bias = markov_get_lux_bias(&g_markov);
                bird_mapper_update_for_lux(&g_bird_mapper, lux + bias);
                bird_info_t bird;
                if (espnow_mesh_is_flooded()) {
                    bird = (bird_info_t){"red_billed_quelea", "Red-billed Quelea"};
                } else {
                    bird = bird_mapper_get_bird(&g_bird_mapper, flash_det);
                }
                generate_and_play_bird_call(&g_bird_mapper, bird.function_name, bird.display_name);
            }

            /* Force broadcast regardless of ESPNOW_LUX_THRESHOLD */
            espnow_mesh_broadcast_light(lux);
            last_acted_lux = lux;

        } else if (fabsf(delta) >= cfg->lux_change_threshold) {
            /* Gradual change: update mapper + Markov + broadcast. */
            markov_set_lux(&g_markov, lux);
            float bias = markov_get_lux_bias(&g_markov);
            bird_mapper_update_for_lux(&g_bird_mapper, lux + bias);
            espnow_mesh_broadcast_light(lux);
            last_acted_lux = lux;

            const char *band;
            if      (lux < 10.0f)  band = "NIGHT";
            else if (lux < 100.0f) band = "DAWN/DUSK";
            else if (lux < 500.0f) band = "CLOUDY DAY";
            else                   band = "SUNNY DAY";
            ESP_LOGD(TAG, "☀️ Light: %.1f lux (%s)", lux, band);
        }

        vTaskDelay(pdMS_TO_TICKS(cfg->lux_poll_interval_ms));
    }

    vTaskDelete(NULL);
}

/* ========================================================================
 * HARDWARE CONFIGURATION HELPERS
 * ======================================================================== */

/**
 * @brief Return pointer to the global bird mapper (used by main to pass to ESP-NOW)
 */
bird_call_mapper_t *get_bird_mapper(void)
{
    return &g_bird_mapper;
}

markov_chain_t *get_markov(void)
{
    return &g_markov;
}

/**
 * @brief Check if audio output (speaker) is available
 */
bool has_audio_output(void)
{
    return (g_hw_config == HW_CONFIG_FULL);
}

/**
 * @brief Get hardware configuration
 */
hardware_config_t get_hardware_config(void)
{
    return g_hw_config;
}

