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
#include "esp_random.h"
#include "esp_task_wdt.h"

static const char *TAG = "ECHOES";

/* Global system state */
static system_state_t g_system_state = {0};
static bird_call_mapper_t g_bird_mapper = {0};
static markov_chain_t g_markov = {0};
static audio_buffer_t g_audio_buffer = {0};  // Static buffer for bird calls
static SemaphoreHandle_t s_playback_mutex = NULL;  // Prevents concurrent I2S writes
static SemaphoreHandle_t s_led_mutex      = NULL;  // Serialises LEDC duty updates across tasks

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
        .duty       = 65535,   /* full brightness — LEDC takes over from gpio_set_level */
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

                /* BH1750 initialisation sequence — two-step, per datasheet §5.
                 *
                 * Step 1: Power On (0x01).
                 *   The chip starts in power-down state after reset.  Sending
                 *   the measurement opcode directly (0x10) without first sending
                 *   Power On has been observed to leave certain BH1750 / clone
                 *   variants non-responsive: every subsequent i2c_master_receive()
                 *   returns an error and get_lux_level() returns -1.0 for minutes
                 *   until an external electrical event (e.g. a transient from the
                 *   speaker amp) causes the chip to recover.  The explicit 0x01
                 *   guarantees a clean power-on state on all variants.
                 *
                 * Step 2: Continuously H-Resolution Mode (0x10), followed by a
                 *   180 ms wait (datasheet §2 — max measurement time for H-res
                 *   mode).  lux_based_birds_task is started in a suspended state
                 *   and not resumed until after OTA/remote-config, which takes
                 *   tens of seconds, so this delay adds no observable latency in
                 *   practice; it is here as a guarantee for any future code path
                 *   that initialises the sensor closer to first use.            */
                uint8_t cmd_power_on = 0x01;
                ret = i2c_master_transmit(bh1750_handle,
                                          &cmd_power_on,
                                          1,
                                          100);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "BH1750 Power On failed: %s — falling back to ADC",
                             esp_err_to_name(ret));
                    i2c_master_bus_rm_device(bh1750_handle);
                    bh1750_handle = NULL;
                    goto bh1750_fallback;
                }

                vTaskDelay(pdMS_TO_TICKS(10));   /* datasheet: ≥1 ms after power-on */

                uint8_t cmd = 0x10;

                ret = i2c_master_transmit(bh1750_handle,
                                          &cmd,
                                          1,
                                          100);   /* 100 ms max — never block forever */

                if (ret == ESP_OK) {
                    /* Wait for the first measurement to complete before any read.
                     * The BH1750 output register is the live ADC integrator — reads
                     * issued before the first 120–180 ms measurement cycle completes
                     * return 0 or 1 raw count regardless of ambient light.          */
                    vTaskDelay(pdMS_TO_TICKS(180));
                    ESP_LOGI(TAG,
                             "BH1750 light sensor detected at 0x%02X",
                             BH1750_ADDR_LOW);
                    return;  // Done — no fallback needed
                }

                /* 0x10 transmit failed — remove device and fall to ADC */
                i2c_master_bus_rm_device(bh1750_handle);
                bh1750_handle = NULL;
            }

bh1750_fallback:
            /* Device add or transmit failed — delete the bus so the handle
             * does not leak.  Fall through to the ADC path below.          */
            i2c_del_master_bus(bus_handle);
            bus_handle = NULL;
        } else {
            /* BH1750 not present — this is a minimal node.  Free the bus
             * immediately so the handle does not leak.  On minimal nodes this
             * branch is taken on every boot; without this free the heap loses
             * ~100 bytes per reset cycle until adc_oneshot_new_unit() eventually
             * panics and triggers an infinite reboot loop.                   */
            ESP_LOGI(TAG, "BH1750 not found — releasing I2C bus (minimal node)");
            i2c_del_master_bus(bus_handle);
            bus_handle = NULL;
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
    if (get_hardware_config() == HW_CONFIG_FULL) {
        markov_init(&g_markov, &g_bird_mapper);
    } else {
        // Minimal nodes never use it — zero it out so espnow_mesh_init sees NULL
        memset(&g_markov, 0, sizeof(g_markov));
    }

    // Create playback mutex (guards the I2S speaker channel)
    s_playback_mutex = xSemaphoreCreateMutex();
    configASSERT(s_playback_mutex != NULL);

    // Create LED mutex (serialises ledc_set_duty/update_duty pairs across tasks).
    // Needed on minimal nodes where audio_detection_task (VU meter) and
    // flock_task (strobe) both call set_led() concurrently from different cores.
    s_led_mutex = xSemaphoreCreateMutex();
    configASSERT(s_led_mutex != NULL);
    
    ESP_LOGI(TAG, "System initialized");
}

/* ========================================================================
 * LED CONTROL
 * ======================================================================== */

void set_led(float white_level, float blue_level) {
    /* Cache the suppression state so the mutex inside remote_config_is_quiet_hours()
     * is not hit on every LED update (flock mode strobes at 60 ms intervals).
     * Re-evaluate every ~1 second — more than responsive enough for mode changes. */
    static bool     s_suppressed       = false;
    static uint32_t s_suppress_check_ms = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if ((now - s_suppress_check_ms) >= 1000u) {
        s_suppress_check_ms = now;
        const remote_config_t *cfg = remote_config_get();
        s_suppressed = cfg->silent_mode || remote_config_is_quiet_hours();
    }
    if (s_suppressed) {
        white_level = 0.0f;
        blue_level  = 0.0f;
    }

    /* Protect the ledc_set_duty + ledc_update_duty pair.  Without this mutex
     * two tasks running on different cores can interleave their writes: task A
     * calls set_duty(white) then task B calls set_duty(white) before either
     * calls update_duty(), leaving the hardware latched at B's value for both
     * channels — the wrong brightness.
     *
     * Timeout: 10 ms.  A healthy caller holds the mutex for only ~5 µs
     * (two set_duty + two update_duty calls).  If another task holds it for
     * longer than 10 ms something is badly wrong; we skip the update rather
     * than blocking the VU meter or flock strobe indefinitely.             */
    if (s_led_mutex == NULL ||
        xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        /* Mutex not yet created (pre-system_init path) or contended for too
         * long — best-effort write without the lock.                        */
        uint32_t duty_white = (uint32_t)(65535.0f * white_level);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_white);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        uint32_t duty_blue = (uint32_t)(65535.0f * blue_level);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty_blue);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        return;
    }

    uint32_t duty_white = (uint32_t)(65535.0f * white_level);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_white);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    uint32_t duty_blue = (uint32_t)(65535.0f * blue_level);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty_blue);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);

    xSemaphoreGive(s_led_mutex);
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

        if (ret != ESP_OK) return -1.0f;

        uint16_t raw = ((uint16_t)data[0] << 8) | data[1];

        /* BH1750 single-poll artifact guard.
         *
         * The BH1750 output register is the live ADC integrator in continuous
         * H-resolution mode.  A read that coincidentally aligns with the first
         * few milliseconds of a new 120 ms measurement cycle catches the
         * integrator near-empty, returning 0–2 raw counts regardless of
         * ambient light (0–1.67 lux).  This is not an I2C error — the call
         * returns ESP_OK with valid framing, just a physically wrong value.
         *
         * Fix: if raw <= BH1750_SANITY_RAW, wait 10 ms (well past the ~2 ms
         * transition window but a negligible fraction of the 500 ms poll cycle)
         * and read again.  If the room really is that dark, both reads return a
         * low value and we trust the retry result.  If it was an artifact, the
         * retry lands mid-measurement and returns the correct accumulated count.
         *
         * BH1750_SANITY_RAW = 3 counts = 2.5 lux — safely above the 1–2 count
         * artifact floor while below any plausible room-light reading.          */
#define BH1750_SANITY_RAW  3u
        if (raw <= BH1750_SANITY_RAW) {
            vTaskDelay(pdMS_TO_TICKS(10));
            ret = i2c_master_receive(
                bh1750_handle,
                data,
                sizeof(data),
                100 / portTICK_PERIOD_MS
            );
            if (ret != ESP_OK) return -1.0f;
            raw = ((uint16_t)data[0] << 8) | data[1];
        }

        return raw / 1.2f;   // BH1750 lux conversion
    }
    else if (g_system_state.light_sensor_type == LIGHT_SENSOR_ANALOG) {

        if (adc_handle == NULL) return -1.0f;

        /* Multi-sample averaging with zero-rejection.
         *
         * A single adc_oneshot_read() on the ALS-PT19 can occasionally
         * return 0 due to charge-injection from the ADC's internal sampling
         * capacitor discharging through the sensor's high source impedance
         * (~50 kΩ).  Taking ADC_OVERSAMPLE readings and averaging only the
         * non-zero results suppresses these artefacts without masking genuine
         * darkness — if ALL reads return 0 the room really is dark and we
         * return 0.0 correctly.
         *
         * Four back-to-back oneshot reads add ~200 µs overhead total, which
         * is immaterial at the 500 ms LUX_POLL_INTERVAL_MS call rate.      */
#define ADC_OVERSAMPLE  4
        int32_t adc_sum   = 0;
        int     adc_valid = 0;

        for (int s = 0; s < ADC_OVERSAMPLE; s++) {
            int raw = 0;
            if (adc_oneshot_read(adc_handle, ADC_CHANNEL_6, &raw) == ESP_OK
                    && raw > 0) {
                adc_sum += raw;
                adc_valid++;
            }
        }

        /* All reads returned 0 → room is genuinely dark, return 0.0.
         * At least one non-zero read → average those only.           */
        if (adc_valid == 0) return 0.0f;
        return (float)(adc_sum / adc_valid) * 0.24f;
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
static float smooth_vu_level(float current, float target)
{
    /* Off immediately on silence — no trailing glow in the dead zone. */
    if (target == 0.0f) {
        return 0.0f;
    }

    /* Jump on immediately from silence so the first beat is never missed. */
    if (current == 0.0f && target > 0.0f) {
        return target;
    }

    /* Fast blend for level changes — 0.3 on current keeps a little inertia
     * to prevent single-frame flicker between adjacent stepped levels.     */
    const float FAST_SMOOTH = 0.3f;
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
    const remote_config_t *_cfg = remote_config_get();
    if (_cfg->silent_mode || _cfg->sound_off) {
        ESP_LOGD(TAG, "Sound suppressed (%s): skipping '%s'",
                 _cfg->silent_mode ? "silent_mode" : "sound_off", bird_name);
        return;
    }
    if (remote_config_is_quiet_hours()) {
        ESP_LOGD(TAG, "Quiet hours: skipping '%s'", bird_name);
        return;
    }

    ESP_LOGI(TAG, "🐦 Playing: %s (%zu samples)", bird_name, audio_buffer->num_samples);

    int16_t global_peak = 1;
    for (size_t i = 0; i < audio_buffer->num_samples; i++) {
        int16_t av = audio_buffer->buffer[i] < 0 ? -audio_buffer->buffer[i]
                                                  :  audio_buffer->buffer[i];
        if (av > global_peak) global_peak = av;
    }

    float vol_scale = get_volume_for_lux(get_lux_level());
    const remote_config_t *_pcfg = remote_config_get();
    /* Apply the remote VOLUME parameter.  synthesis.c bakes the compile-time
     * VOLUME macro (0.20f) into each buffer; we scale relative to that default
     * so the dashboard slider takes effect without touching the synthesised
     * samples directly.  At cfg->volume == 0.20 the ratio is 1.0 (unity). */
    vol_scale *= (_pcfg->volume / 0.20f);
    set_led(_pcfg->vu_max_brightness * vol_scale, BRIGHT_OFF);

    size_t total_bytes = audio_buffer->num_samples * sizeof(int16_t);
    size_t bytes_sent  = 0;
    /* Protected by s_playback_mutex — all callers of _play_locked hold it. */
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
		            pdMS_TO_TICKS(2000));   // 2s timeout — not portMAX_DELAY
	if (ret == ESP_ERR_TIMEOUT) {
	    ESP_LOGW(TAG, "_play_locked: I2S write timeout — aborting playback");
	    break;
	} else if (ret != ESP_OK || bytes_written == 0) {
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
    /* Brief delay AFTER releasing the mutex so the DMA ring has time to drain
     * before the next caller starts writing new data.  Placing it outside the
     * lock is intentional: a waiting caller will acquire the mutex and begin
     * generating the next call (a CPU-only step) during this window, which is
     * fine — i2s_channel_write() is the only operation that touches the DMA
     * hardware, and that happens inside _play_locked() under the mutex.     */
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
    /* See play_bird_call() for explanation of why this delay is correctly
     * placed outside the mutex.                                            */
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
    /* See play_bird_call() for explanation of why this delay is correctly
     * placed outside the mutex.                                            */
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

    /* Wait for mic_chan to be set by i2s_microphone_init() in app_main.
     * Normally the scheduler guard in app_main (vTaskSuspendAll around
     * xTaskCreate+vTaskSuspend) prevents this task from running before the
     * hardware is ready, but this poll is a defensive backstop against any
     * future ordering change.  mic_chan is a static global initialised to
     * NULL; i2s_microphone_init() sets it before enabling the channel.    */
    while (mic_chan == NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (g_hw_config == HW_CONFIG_FULL) {
        ESP_LOGI(TAG, "🎤 Listening for whistles, voice, claps, and birdsong...");
    } else {
        ESP_LOGI(TAG, "🎤 LED VU meter mode active (digital stepped output)");
    }

    /* Subscribe this task to the task watchdog.
     *
     * Previously i2s_channel_read used portMAX_DELAY, which meant a stalled
     * I2S peripheral would cause the task to block forever — the WDT would
     * eventually fire and reboot the node, but the reboot loop repeated
     * indefinitely because the stall recurred on every boot.
     *
     * The fix: use a 5-second timeout instead of portMAX_DELAY.  A healthy
     * ICS-43434 delivers a 512-sample buffer every ~32 ms, so 5 s is many
     * orders of magnitude more than any legitimate delay.  On timeout the
     * channel is cycled (disable → re-enable) to flush the DMA ring and
     * recover without a full reboot.  The WDT is now a last-resort backstop
     * for failure modes the timeout recovery cannot handle.               */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not subscribe audio task to watchdog: %s",
                 esp_err_to_name(wdt_err));
    }

#define I2S_READ_TIMEOUT_MS  5000   /* 5 s — vastly more than the ~32 ms per buffer */

    while (1) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(
            mic_chan,
            buffer,
            BUFFER_SIZE * sizeof(int16_t),
            &bytes_read,
            pdMS_TO_TICKS(I2S_READ_TIMEOUT_MS)
        );
        esp_task_wdt_reset();  /* fed every iteration whether read succeeded or timed out */

        if (ret == ESP_ERR_TIMEOUT) {
            /* Peripheral stalled — cycle the channel to recover without rebooting. */
            ESP_LOGW(TAG, "I2S read timeout — cycling mic channel");
            i2s_channel_disable(mic_chan);
            vTaskDelay(pdMS_TO_TICKS(50));
            i2s_channel_enable(mic_chan);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (ret != ESP_OK || bytes_read == 0) {
            ESP_LOGE(TAG, "I2S read error: %s — reinitialising mic channel",
                     esp_err_to_name(ret));
            i2s_channel_disable(mic_chan);
            vTaskDelay(pdMS_TO_TICKS(50));
            i2s_channel_enable(mic_chan);
            vTaskDelay(pdMS_TO_TICKS(50));
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
            
            // Snapshot config once per loop — all fields are mutually consistent
            remote_config_t cfg_snap;
            if (!remote_config_snapshot(&cfg_snap)) {
                vTaskDelay(pdMS_TO_TICKS(4));
                continue;
            }
            const remote_config_t *cfg = &cfg_snap;

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

            /* Apply noise-floor minimums so adaptive thresholds never collapse
             * to near-zero in a quiet/dark room.  Without this, electrical
             * noise from the MEMS mic constantly clears thresh_v and drains
             * debounce, preventing softer detections (voice) from confirming.
             * These floors are remotely configurable for per-installation tuning. */
            float thresh_w    = fmaxf(state->running_avg_whistle  * cfg->whistle_multiplier,
                                      cfg->noise_floor_whistle);
            float thresh_v    = fmaxf(state->running_avg_voice    * cfg->voice_multiplier,
                                      cfg->noise_floor_voice);
            float thresh_b    = fmaxf(state->running_avg_birdsong * cfg->birdsong_multiplier,
                                      cfg->noise_floor_birdsong);
            float thresh_clap = fmaxf(thresh_w * cfg->clap_multiplier, thresh_v * 2.0f);
            
            /* Periodic diagnostic log — helps tune thresholds during testing.
             * At 4 ms/buffer this fires roughly every 5 seconds.            */
            static uint32_t s_diag_count = 0;
            if (++s_diag_count >= 1250) {
                s_diag_count = 0;
                ESP_LOGI(TAG, "THR w:%.0f v:%.0f b:%.0f | MAG w:%.0f v:%.0f b:%.0f",
                         thresh_w, thresh_v, thresh_b, mag_w, mag_v, mag_b);
            }

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
                        bird_info_t bird = bird_mapper_get_bird(&g_bird_mapper, DETECTION_CLAP);
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
                        bird_info_t bird = bird_mapper_get_bird(&g_bird_mapper, DETECTION_BIRDSONG);
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
                        bird_info_t bird = bird_mapper_get_bird(&g_bird_mapper, DETECTION_WHISTLE);
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
                        bird_info_t bird = bird_mapper_get_bird(&g_bird_mapper, DETECTION_VOICE);
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
            vu_level = smooth_vu_level(vu_level, target_vu);
            
            // Update white LED to show audio level
            // Blue LED stays off in minimal mode
            set_led(vu_level, 0.0f);

	    // ─────────────────────────────────────────────────────────────
            // FEED THE WATCHDOG EVERY ITERATION
            // This is the critical line. The loop runs at ~1 ms, so feeding
            // the WDT here guarantees that even if the node is in the
            // "stuck with white LED on" state, the watchdog will still
            // trigger a clean reboot instead of hanging forever.
            // ─────────────────────────────────────────────────────────────
            esp_task_wdt_reset();
            
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
    remote_config_t cfg_snap;
    if (!remote_config_snapshot(&cfg_snap)) {
        /* Fallback: use defaults to avoid silence */
        return 0.5f;
    }
    const remote_config_t *cfg = &cfg_snap;
    if (lux <= cfg->volume_lux_min) return cfg->volume_scale_min;
    if (lux >= cfg->volume_lux_max) return cfg->volume_scale_max;
    float t = (lux - cfg->volume_lux_min) / (cfg->volume_lux_max - cfg->volume_lux_min);
    return cfg->volume_scale_min + t * (cfg->volume_scale_max - cfg->volume_scale_min);
}


/* ========================================================================
 * LUX-BASED BIRD SELECTION
 * ======================================================================== */

/* Forward declaration — full definition is in flock_task section below */
static const bird_info_t k_all_birds[];
#define NUM_ALL_BIRDS  11u

void lux_based_birds_task(void *param) {
    if (g_system_state.light_sensor_type == LIGHT_SENSOR_NONE) {
        ESP_LOGW(TAG, "Lux-based bird selection disabled (no sensor)");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Lux-based bird selection enabled");

    /* Subscribe to the task watchdog.  generate_and_play_bird_call() can block
     * for up to 3 s (longest bird call) + 4 s mutex wait = ~7 s per call.
     * WDT_TIMEOUT_S is 120 s so a single call never trips it, but a deadlock
     * (e.g. mutex permanently held by a crashed task) would be caught.     */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "lux_task: could not subscribe to watchdog: %s",
                 esp_err_to_name(wdt_err));
    }

    /* Forward declaration — defined in main.c.
     * lux_based_birds_task is the second ISR WDT feeder on minimal nodes.
     * See isr_wdt_lux_feed() in main.c for the full rationale.           */
    extern void isr_wdt_lux_feed(void);

    float last_acted_lux = -1000.0f;  /* lux at the last mapper update */
    float prev_lux       = -1.0f;     /* reading from the previous tick */

    /* s_zero_confirm: counts consecutive near-zero polls.
     * Full logic is documented at the gate itself, below.                */
#define LUX_NEAR_ZERO      2.0f   /* lux threshold for "effectively dark"  */
#define LUX_ZERO_CONFIRM   6      /* consecutive polls required to confirm  */
    int s_zero_confirm = 0;

    while (1) {
        esp_task_wdt_reset();  /* fed each poll cycle -- proves task is alive */

        /* Feed the ISR WDT heartbeat on minimal nodes.
         *
         * wifi_keepalive_task feeds the same counter, but if lux_task dies
         * silently (removed from TWDT watchlist) keepalive alone would keep
         * the counter advancing indefinitely — masking the failure.  By
         * requiring lux_task to also feed the counter, the ISR WDT fires
         * ISR_WDT_TIMEOUT_S after this task stops running, even if
         * wifi_keepalive_task is still healthy.
         *
         * On full nodes isr_wdt_lux_feed() is a no-op increment of an
         * unwatched counter (the GP timer is never started on full nodes),
         * so the guard is belt-and-suspenders rather than strictly required. */
        if (get_hardware_config() == HW_CONFIG_MINIMAL) {
            isr_wdt_lux_feed();
        }

        remote_config_t cfg_snap;
        if (!remote_config_snapshot(&cfg_snap)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        const remote_config_t *cfg = &cfg_snap;

        float lux = get_lux_level();

        if (lux < 0.0f) {
            vTaskDelay(pdMS_TO_TICKS(cfg->lux_poll_interval_ms));
            continue;
        }

        /* Near-zero confirmation gate.
         *
         * Bug in previous version: the condition was
         *   lux <= LUX_NEAR_ZERO && prev_lux > LUX_NEAR_ZERO
         * which only triggered on the FIRST transition into the near-zero
         * range.  On the second consecutive zero poll prev_lux had already
         * been set to the held near-zero value, so the condition was false
         * and the zero broadcast through -- producing the consistent ~1.0 s
         * (2-poll) artifact seen in the sniffer logs.
         *
         * Fix: gate on lux <= LUX_NEAR_ZERO unconditionally.  Every near-
         * zero poll increments the counter; only once LUX_ZERO_CONFIRM
         * consecutive polls have been accumulated does the code fall through
         * to the flash/delta logic.  The counter resets on any poll that
         * reads above LUX_NEAR_ZERO.
         *
         * LUX_ZERO_CONFIRM = 6: at 500 ms/poll this requires 3 s of
         * sustained near-zero before broadcasting.  Live captures show
         * the ALS-PT19 artifact lasts exactly 4 consecutive polls (~2 s);
         * 6 polls gives 2 full polls of margin above that while adding
         * only 3 s of latency to genuine sustained darkness events.        */
        if (lux <= LUX_NEAR_ZERO) {
            s_zero_confirm++;
            if (s_zero_confirm < LUX_ZERO_CONFIRM) {
                /* Still accumulating -- update prev_lux so raw_delta is
                 * computed correctly when we eventually confirm, but skip
                 * all flash/broadcast logic for this poll.                */
                prev_lux = lux;
                vTaskDelay(pdMS_TO_TICKS(cfg->lux_poll_interval_ms));
                continue;
            }
            /* Confirmed -- fall through and let flash/delta logic handle it */
        } else {
            s_zero_confirm = 0;  /* reset on any lit reading               */
        }

        float delta     = lux - last_acted_lux;
        float raw_delta = (prev_lux >= 0.0f) ? (lux - prev_lux) : 0.0f;
        float last_lux  = prev_lux;
        prev_lux = lux;

        /* Flash detection using remote config thresholds. */
        bool is_flash = (fabsf(raw_delta) >= cfg->lux_flash_threshold) ||
                        (fabsf(raw_delta) >= cfg->lux_flash_min_abs &&
                         last_lux > 0.0f &&
                         fabsf(raw_delta) >= last_lux * cfg->lux_flash_percent);

        if (is_flash) {
            ESP_LOGI(TAG, "⚡ Flash: %.1f → %.1f lux (Δ%.1f)", last_lux, lux, raw_delta);

            /* Pick a random bird from the full catalogue so every flash
             * produces a surprise response.  We avoid repeating the
             * most recent flash bird for variety.                         */
            static uint32_t s_last_flash_idx = 0xFFFFFFFF;
            uint32_t flash_idx;
            do {
                flash_idx = (uint32_t)(esp_random() % NUM_ALL_BIRDS);
            } while (flash_idx == s_last_flash_idx && NUM_ALL_BIRDS > 1);
            s_last_flash_idx = flash_idx;
            const bird_info_t *flash_bird = &k_all_birds[flash_idx];

            /* Still teach the Markov chain the implied detection so the
             * chain learns from flash events.                             */
            detection_type_t flash_det = (raw_delta > 0.0f)
                                         ? DETECTION_WHISTLE
                                         : DETECTION_VOICE;
            markov_on_event(&g_markov, flash_det, lux);

            if (has_audio_output()) {
                float bias = markov_get_lux_bias(&g_markov);
                bird_mapper_update_for_lux(&g_bird_mapper, lux + bias);
                ESP_LOGI(TAG, "⚡ Flash bird: %s", flash_bird->display_name);
                generate_and_play_bird_call(&g_bird_mapper,
                                            flash_bird->function_name,
                                            flash_bird->display_name);
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
 * FLOCK MODE TASK
 *
 * Monitors espnow_mesh_is_flock_mode().  While active, plays bird calls
 * with the following split:
 *   60 % → Red-billed Quelea  (colony signifier — the whole flock speaks)
 *   40 % → random bird from the full catalogue (avoids repetition)
 *
 * Each call is preceded by a rapid LED strobe.  The task yields between
 * every call so audio detection and other tasks are not starved.
 * ======================================================================== */

/* Full catalogue of every synthesised bird.
 * Forward-declared above so lux_based_birds_task can reference k_all_birds
 * for flash events.  Defined here once.                                     */
static const bird_info_t k_all_birds[] = {
    { "piet_my_vrou",          "Piet-my-vrou"            },
    { "cape_robin_chat",        "Cape Robin-Chat"         },
    { "southern_boubou",        "Southern Boubou"         },
    { "red_eyed_dove",          "Red-eyed Dove"           },
    { "glossy_starling",        "Glossy Starling"         },
    { "spotted_eagle_owl",      "Spotted Eagle-Owl"       },
    { "fork_tailed_drongo",     "Fork-tailed Drongo"      },
    { "cape_canary",            "Cape Canary"             },
    { "southern_masked_weaver", "Southern Masked Weaver"  },
    { "red_billed_quelea",      "Red-billed Quelea"       },
    { "paradise_flycatcher",    "Paradise Flycatcher"     },
};
/* Verify NUM_ALL_BIRDS matches the table at compile time.
 * If you add or remove a bird, update the #define above — this assert
 * will catch any mismatch before it becomes a runtime bug.             */
_Static_assert(sizeof(k_all_birds) / sizeof(k_all_birds[0]) == NUM_ALL_BIRDS,
               "NUM_ALL_BIRDS does not match k_all_birds table — update the #define");

/* Index of Quelea in k_all_birds — used for the 60 % selection */
#define QUELEA_IDX  9u

void flock_task(void *param)
{
    (void)param;

    /* Verify QUELEA_IDX points to the right entry — catches table reordering at runtime. */
    assert(strcmp(k_all_birds[QUELEA_IDX].function_name, "red_billed_quelea") == 0);

    /* Subscribe to the task watchdog here, before the hardware branch, so
     * both the LED-only and full-audio paths are covered.  Each loop below
     * resets the WDT at the top of every iteration so the 120-second timeout
     * is never reached during normal idle operation.                        */
    {
        esp_err_t wdt_err = esp_task_wdt_add(NULL);
        if (wdt_err != ESP_OK) {
            ESP_LOGW(TAG, "flock_task: could not subscribe to watchdog: %s",
                     esp_err_to_name(wdt_err));
        }
    }

    if (!has_audio_output()) {
        /* Minimal hardware: LED-only flock strobe */
        ESP_LOGI(TAG, "🐦 Flock task running (LED-only mode)");
        bool was_flock = false;
        while (1) {
            esp_task_wdt_reset();   /* keep WDT fed on every cycle */
            bool flock = espnow_mesh_is_flock_mode();
            if (flock) {
                set_led(BRIGHT_FULL, BRIGHT_OFF);
                vTaskDelay(pdMS_TO_TICKS(60));
                set_led(BRIGHT_OFF,  BRIGHT_OFF);
                vTaskDelay(pdMS_TO_TICKS(60));
            } else {
                if (was_flock) set_led(BRIGHT_OFF, BRIGHT_OFF);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            was_flock = flock;
        }
    }

    ESP_LOGI(TAG, "🐦 Flock task running (full audio mode)");

    uint32_t last_random_idx = 0xFFFFFFFF;  /* avoid immediate repetition in 40 % path */

    while (1) {
        esp_task_wdt_reset();   /* reset at top so idle path never starves the WDT */

        if (!espnow_mesh_is_flock_mode()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        const remote_config_t *cfg = remote_config_get();

        /* ---- 60 / 40 bird selection ----------------------------------- */
        const bird_info_t *bird;
        uint32_t roll = esp_random() % 100u;

        if (roll < FLOCK_QUELEA_PERCENT) {
            /* 60 % path — always Quelea */
            bird = &k_all_birds[QUELEA_IDX];
            ESP_LOGI(TAG, "🐦 Flock (Quelea 60%%): %s", bird->display_name);
        } else {
            /* 40 % path — random from full catalogue, skip Quelea and
             * avoid repeating the previous random pick                   */
            uint32_t idx;
            do {
                idx = esp_random() % NUM_ALL_BIRDS;
            } while (idx == QUELEA_IDX ||
                     (idx == last_random_idx && NUM_ALL_BIRDS > 2));
            last_random_idx = idx;
            bird = &k_all_birds[idx];
            ESP_LOGI(TAG, "🐦 Flock (random 40%%): %s", bird->display_name);
        }

        /* ---- LED strobe before call ----------------------------------- */
        set_led(BRIGHT_FULL, BRIGHT_OFF);
        vTaskDelay(pdMS_TO_TICKS(40));
        set_led(BRIGHT_OFF, BRIGHT_OFF);
        vTaskDelay(pdMS_TO_TICKS(40));
        set_led(BRIGHT_FULL, BRIGHT_OFF);
        vTaskDelay(pdMS_TO_TICKS(40));
        set_led(BRIGHT_OFF, BRIGHT_OFF);

        /* ---- Play (blocking) ------------------------------------------ */
        generate_and_play_bird_call(&g_bird_mapper,
                                    bird->function_name,
                                    bird->display_name);

        /* ---- Inter-call gap ------------------------------------------- */
        uint32_t gap = cfg->flock_call_gap_ms;
        if (gap < 50) gap = 50;
        vTaskDelay(pdMS_TO_TICKS(gap));
    }
}

/**
 * @brief Documentary / demo mode task.
 *
 * Enabled via DEMO_MODE remote config flag.  Fires bird calls autonomously
 * on full nodes at DEMO_INTERVAL_MS intervals with no human interaction.
 * Each call is broadcast over ESP-NOW so the entire mesh responds and flock
 * mode can trigger naturally — producing the rich, distributed audio of a
 * live audience session, suitable for unattended documentary recording.
 *
 * Minimal nodes (no speaker) exit immediately; they respond to the ESP-NOW
 * broadcasts via their LED VU meter and the normal flock strobe path.
 *
 * Design notes
 * ─────────────
 * • Bird selection rotates through all 11 species, never repeating the
 *   previous pick, to mimic natural variety.
 * • The detection type broadcast over ESP-NOW cycles WHISTLE → VOICE →
 *   BIRDSONG → CLAP so every mood is represented across a recording session.
 * • Markov chain is updated so NVS statistics reflect demo activity.
 * • The WDT is fed every 500 ms inside the sleep loop so a long interval
 *   (e.g. 60 s) never trips the 120 s timeout.
 * • Respects SILENT_MODE, SOUND_OFF, and quiet hours via the existing
 *   _play_locked() guard inside generate_and_play_bird_call().
 */
void demo_task(void *param)
{
    (void)param;

    /* Minimal nodes have no speaker — exit; they participate via ESP-NOW. */
    if (!has_audio_output()) {
        ESP_LOGI(TAG, "🎬 Demo task: minimal node — no speaker, exiting");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "demo_task: could not subscribe to watchdog: %s",
                 esp_err_to_name(wdt_err));
    }

    ESP_LOGI(TAG, "🎬 Demo task running (full node — waiting for DEMO_MODE)");

    uint32_t last_bird_idx  = 0xFFFFFFFF;  /* avoid immediate repetition */
    uint8_t  det_cycle      = 0;           /* cycles through 4 detection types */
    uint8_t  event_counter  = 0;           /* counts calls; every 4th is a flash */
    bool     flash_polarity = true;        /* alternates rise/fall for variety  */

    while (1) {
        esp_task_wdt_reset();

        /* Poll cheaply while demo mode is off — check every second. */
        if (!remote_config_get()->demo_mode) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Take a consistent snapshot for this call cycle. */
        remote_config_t cfg;
        if (!remote_config_snapshot(&cfg)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!cfg.demo_mode) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* ── Bird selection: random, no immediate repeat ─────────── */
        uint32_t idx;
        do {
            idx = esp_random() % NUM_ALL_BIRDS;
        } while (idx == last_bird_idx && NUM_ALL_BIRDS > 1);
        last_bird_idx = idx;
        const bird_info_t *bird = &k_all_birds[idx];

        float lux = get_lux_level();

        /* ── Event type: every 4th call is a flash, others are sound.
         *
         * SOUND events (3 out of 4 calls):
         *   • Teach the Markov chain a detection.
         *   • Broadcast a sound message → receiving nodes update mood
         *     AND increment the flock-mode counter.
         *
         * FLASH events (1 in 4 calls), mirroring lux_based_birds_task:
         *   • Simulate a rapid lux change (spike up or dip down, alternating).
         *   • Derive flash_det from direction (rise → WHISTLE, fall → VOICE),
         *     exactly as the real flash handler does.
         *   • Teach the Markov chain that detection.
         *   • Broadcast the simulated lux value → receiving nodes update
         *     bird-mapper mood (but do NOT increment the flock counter —
         *     light broadcasts are excluded from that path by design).
         *   • Play using the simulated lux + Markov bias so bird selection
         *     reflects the "bright flash" or "sudden darkness" mood.
         *
         * The 3:1 ratio keeps flock-mode triggering primarily driven by the
         * sound broadcasts (which increment the counter), while flash events
         * add the light-change texture that varies the bird palette.       */
        bool is_flash = ((event_counter++ & 3u) == 3u);

        if (is_flash) {
            /* Simulate a lux spike (rise) or dip (fall), alternating each time. */
            float base_lux    = (lux >= 0.0f) ? lux : 200.0f;
            float sim_lux     = flash_polarity
                                ? (base_lux + cfg.lux_flash_threshold * 2.0f)  /* bright flash  */
                                : fmaxf(0.0f, base_lux - cfg.lux_flash_threshold * 2.0f); /* sudden dip */
            flash_polarity = !flash_polarity;

            detection_type_t flash_det = flash_polarity   /* polarity already flipped above */
                                         ? DETECTION_VOICE    /* falling → mellow */
                                         : DETECTION_WHISTLE; /* rising  → active */

            markov_on_event(&g_markov, flash_det, sim_lux);

            float bias = markov_get_lux_bias(&g_markov);
            bird_mapper_update_for_lux(&g_bird_mapper, sim_lux + bias);

            espnow_mesh_broadcast_light(sim_lux);

            ESP_LOGI(TAG, "🎬 Demo ⚡ Flash: %s  [%s lux %.0f→%.0f]",
                     bird->display_name,
                     flash_det == DETECTION_WHISTLE ? "RISE" : "FALL",
                     base_lux, sim_lux);
        } else {
            /* ── Sound event: cycle detection type for variety ────── */
            detection_type_t det;
            switch (det_cycle++ & 3u) {
                case 0:  det = DETECTION_WHISTLE;  break;
                case 1:  det = DETECTION_VOICE;    break;
                case 2:  det = DETECTION_BIRDSONG; break;
                default: det = DETECTION_CLAP;     break;
            }

            markov_on_event(&g_markov, det, lux);
            espnow_mesh_broadcast_sound(det);

            ESP_LOGI(TAG, "🎬 Demo 🔊 Sound: %s  [%s]", bird->display_name,
                     det == DETECTION_WHISTLE  ? "WHISTLE"  :
                     det == DETECTION_VOICE    ? "VOICE"    :
                     det == DETECTION_BIRDSONG ? "BIRDSONG" : "CLAP");
        }

        /* ── Play (respects SILENT_MODE, SOUND_OFF, quiet hours) ── */
        generate_and_play_bird_call(&g_bird_mapper,
                                    bird->function_name,
                                    bird->display_name);

        /* ── Inter-call gap — WDT fed every 500 ms with ±10 % jitter ──
         * esp_random() gives a uniform 32-bit value.  We map it to
         * [0, interval/5) — one fifth of the base interval — then
         * subtract half of that to centre the jitter around zero:
         *
         *   jitter ∈ [ -(interval×0.1), +(interval×0.1) ]
         *
         * The signed arithmetic is done in int64 to avoid wrap-around
         * on the subtraction before the result is clamped to 1 000 ms. */
        uint32_t interval = cfg.demo_interval_ms;
        if (interval < 1000u) interval = 1000u;
        {
            uint32_t jitter_range = interval / 5u;          /* 20 % window  */
            uint32_t jitter_raw   = (jitter_range > 0u)
                                    ? (esp_random() % jitter_range)
                                    : 0u;
            int64_t jittered = (int64_t)interval
                               - (int64_t)(jitter_range / 2u)
                               + (int64_t)jitter_raw;
            if (jittered < 1000) jittered = 1000;
            interval = (uint32_t)jittered;
        }

        uint32_t slept = 0;
        while (slept < interval) {
            esp_task_wdt_reset();
            uint32_t chunk = (interval - slept > 500u) ? 500u : (interval - slept);
            vTaskDelay(pdMS_TO_TICKS(chunk));
            slept += chunk;
            /* Exit sleep early if demo mode is disabled mid-interval. */
            if (!remote_config_get()->demo_mode) break;
        }
    }
}


/**
 * @brief Return pointer to the global bird mapper (used by main to pass to ESP-NOW)
 */
bird_call_mapper_t *get_bird_mapper(void)
{
    return &g_bird_mapper;
}

audio_buffer_t *get_audio_buffer(void)
{
    return &g_audio_buffer;
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

