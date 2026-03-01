#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// INMP441 I2S MEMS microphone driver for bio-acoustic monitoring

typedef struct {
    int ws_pin;      // Word Select (LRCLK)
    int sck_pin;     // Serial Clock (BCLK)
    int sd_pin;      // Serial Data In (DIN)
    uint32_t sample_rate; // Hz (16000, 44100, etc.)
    uint8_t bits_per_sample; // 16 or 32
    size_t buffer_samples;   // Number of samples to capture per read
} inmp441_config_t;

typedef struct {
    int16_t *samples;     // PCM samples (16-bit)
    size_t count;         // Number of samples
    float rms_amplitude;  // RMS amplitude (0.0-1.0)
    float peak_amplitude; // Peak amplitude (0.0-1.0)
    uint32_t timestamp_ms; // Capture timestamp
    bool valid;           // Data validity flag (trust filtering)
} inmp441_reading_t;

// Initialize I2S microphone with config
esp_err_t inmp441_init(const inmp441_config_t *config);

// Deinitialize and release resources
esp_err_t inmp441_deinit(void);

// Capture audio samples (blocking)
esp_err_t inmp441_read(inmp441_reading_t *reading);

// Quick sound level check (non-blocking, returns RMS only)
esp_err_t inmp441_get_level(float *rms_db);

// Power management
esp_err_t inmp441_sleep(void);   // Low power mode
esp_err_t inmp441_wake(void);    // Resume from sleep

// Configuration
esp_err_t inmp441_set_sample_rate(uint32_t rate);
esp_err_t inmp441_set_buffer_size(size_t samples);

#ifdef __cplusplus
}
#endif
