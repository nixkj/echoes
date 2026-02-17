/**
 * @file echoes.h
 * @brief Echoes of the Machine - Main Header
 * 
 * Hardware:
 * - ESP32-D0WD-V3
 * - Adafruit ICS-43434 I2S MEMS Microphone (SCK=32, WS=33, DOUT=35)
 * - Adafruit MAX98357A I2S Amplifier (SCK=18, WS=19, DIN=17)
 * - White LED on GPIO13 (responds to bird calls - either playing or hearing)
 * - Blue LED on GPIO2
 * - Optional: BH1750 light sensor (SDA=21, SCL=22)
 * - Optional: ALS-PT19 analog sensor (GPIO34)
 */

#ifndef ECHOES_H
#define ECHOES_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "esp_err.h"  // For esp_err_t type

/* ========================================================================
 * CONFIGURATION
 * ======================================================================== */

#define SAMPLE_RATE         16000
#define BUFFER_SIZE         512
#define GAIN                16.0f

/* Detection frequencies */
#define WHISTLE_FREQ        2000
#define VOICE_FREQ          200

/* Adaptive threshold parameters */
#define ALPHA               0.98f       // Smoothing factor
#define WHISTLE_MULTIPLIER  2.5f
#define VOICE_MULTIPLIER    2.5f
#define CLAP_MULTIPLIER     4.0f

/* Confirmation counts */
#define WHISTLE_CONFIRM     2
#define VOICE_CONFIRM       3
#define CLAP_CONFIRM        1

/* Debounce settings (in buffer reads) */
#define DEBOUNCE_BUFFERS    20

/* Audio synthesis */
#define VOLUME              0.20f
//#define VOLUME              0.01f
#define CHUNK_SIZE          512
#define MAX_BIRD_CALL_SAMPLES  48000  // 3 seconds at 16kHz (longest bird call)

/* LED brightness levels */
#define BRIGHT_OFF          0.0f
#define BRIGHT_LOW          0.15f
#define BRIGHT_MID          0.40f
#define BRIGHT_FULL         1.0f

/* VU Meter */
#define VU_MAX_BRIGHTNESS   0.75f        // Maximum LED brightness during playback (0.0 to 1.0)

/* GPIO Pins */
#define PIN_LED_WHITE       13
#define PIN_LED_BLUE        2
#define PIN_MIC_SCK         32
#define PIN_MIC_WS          33
#define PIN_MIC_SD          35
#define PIN_SPK_SCK         18
#define PIN_SPK_WS          19
#define PIN_SPK_SD          17
#define PIN_I2C_SDA         21
#define PIN_I2C_SCL         22
#define PIN_LIGHT_ANALOG    34

/* I2C Addresses */
#define BH1750_ADDR_LOW     0x23
#define BH1750_ADDR_HIGH    0x5C

/* ========================================================================
 * TYPE DEFINITIONS
 * ======================================================================== */

/**
 * @brief Detection type enumeration
 */
typedef enum {
    DETECTION_NONE = 0,
    DETECTION_WHISTLE,
    DETECTION_VOICE,
    DETECTION_CLAP
} detection_type_t;

/**
 * @brief Light sensor type
 */
typedef enum {
    LIGHT_SENSOR_NONE = 0,
    LIGHT_SENSOR_BH1750,
    LIGHT_SENSOR_ANALOG
} light_sensor_type_t;

/**
 * @brief Audio buffer for bird call storage
 */
typedef struct {
    int16_t buffer[MAX_BIRD_CALL_SAMPLES];
    size_t num_samples;
} audio_buffer_t;

/**
 * @brief Bird call information
 */
typedef struct {
    const char *function_name;
    const char *display_name;
} bird_info_t;

/**
 * @brief Detection state
 */
typedef struct {
    float running_avg_whistle;
    float running_avg_voice;
    uint8_t whistle_count;
    uint8_t voice_count;
    uint8_t clap_count;
    uint8_t debounce_counter;
} detection_state_t;

/**
 * @brief System state
 */
typedef struct {
    light_sensor_type_t light_sensor_type;
    uint8_t bh1750_addr;
    detection_state_t detection;
} system_state_t;

/**
 * @brief Hardware configuration types
 */
typedef enum {
    HW_CONFIG_FULL,      // BH1750 + Audio amplifier (full bird call system)
    HW_CONFIG_MINIMAL    // Analog sensor only (LED VU meter only)
} hardware_config_t;

/* ========================================================================
 * FUNCTION DECLARATIONS
 * ======================================================================== */

/* Initialization */
void system_init(void);
void i2s_init(void);
void led_init(void);
void light_sensor_init(void);

/* I2S Initialization (ESP-IDF v5.x) */
esp_err_t i2s_microphone_init(void);
esp_err_t i2s_speaker_init(void);

/* LED Control */
void set_led(float white_level, float blue_level);

/* Audio Processing */
void apply_gain_inplace(int16_t *buffer, size_t num_samples, float gain);
void compute_goertzel(const int16_t *buffer, size_t num_samples, 
                      float *mag_whistle, float *mag_voice);

/* Light Sensor */
float get_lux_level(void);

/* Detection */
void audio_detection_task(void *param);

/* Playback */
void play_bird_call(const char *bird_name, const audio_buffer_t *audio_buffer);

/* Lux-based bird selection */
void lux_based_birds_task(void *param);

/* Hardware Detection */
hardware_config_t get_hardware_config(void);
bool has_audio_output(void);

#endif /* SA_BIRD_SYSTEM_H */
