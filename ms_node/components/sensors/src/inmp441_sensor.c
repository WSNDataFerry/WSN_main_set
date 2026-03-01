#include "inmp441_sensor.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <math.h>
#include <string.h>

static const char *TAG = "inmp441";

static i2s_chan_handle_t s_rx_handle = NULL;
static inmp441_config_t s_config = {0};
static bool s_initialized = false;
static bool s_sleeping = false;

// Trust filtering: reject samples with suspiciously high DC offset or clipping
#define MAX_DC_OFFSET 4096      // Reject if DC > 25% of 16-bit range
#define MAX_CLIPPING_RATIO 0.1f // Reject if >10% samples are clipped

static bool validate_samples(const int16_t *samples, size_t count) {
  if (!samples || count == 0)
    return false;

  // Check DC offset
  int64_t sum = 0;
  uint32_t clipped = 0;

  for (size_t i = 0; i < count; i++) {
    sum += samples[i];
    if (samples[i] >= 32700 || samples[i] <= -32700) {
      clipped++;
    }
  }

  int32_t dc_offset = (int32_t)(sum / (int64_t)count);
  if (abs(dc_offset) > MAX_DC_OFFSET) {
    ESP_LOGW(TAG, "DC offset too high: %" PRIi32 " (rejecting)", dc_offset);
    return false;
  }

  float clip_ratio = (float)clipped / count;
  if (clip_ratio > MAX_CLIPPING_RATIO) {
    ESP_LOGW(TAG, "Clipping ratio too high: %.2f%% (rejecting)",
             clip_ratio * 100);
    return false;
  }

  return true;
}

static void calculate_amplitude(const int16_t *samples, size_t count,
                                float *rms, float *peak) {
  if (!samples || count == 0) {
    *rms = 0.0f;
    *peak = 0.0f;
    return;
  }

  double sum_squares = 0.0;
  int16_t max_abs = 0;

  for (size_t i = 0; i < count; i++) {
    int32_t val = samples[i];
    sum_squares += (double)(val * val);

    int16_t abs_val = (int16_t)abs(val);
    if (abs_val > max_abs) {
      max_abs = abs_val;
    }
  }

  *rms = (float)sqrt(sum_squares / count) / 32768.0f;
  *peak = (float)max_abs / 32768.0f;
}

esp_err_t inmp441_init(const inmp441_config_t *config) {
  if (s_initialized) {
    ESP_LOGW(TAG, "Already initialized");
    return ESP_OK;
  }

  if (!config)
    return ESP_ERR_INVALID_ARG;

  // Validate pins
  if (config->ws_pin < 0 || config->sck_pin < 0 || config->sd_pin < 0) {
    ESP_LOGE(TAG, "Invalid pin configuration");
    return ESP_ERR_INVALID_ARG;
  }

  memcpy(&s_config, config, sizeof(inmp441_config_t));

  // Configure I2S in standard RX mode
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = s_config.buffer_samples;

  esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
    return ret;
  }

  // Standard I2S configuration for INMP441
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_config.sample_rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          (i2s_data_bit_width_t)s_config.bits_per_sample, I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = (gpio_num_t)s_config.sck_pin,
              .ws = (gpio_num_t)s_config.ws_pin,
              .dout = I2S_GPIO_UNUSED,
              .din = (gpio_num_t)s_config.sd_pin,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };

  ret = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init STD mode: %s", esp_err_to_name(ret));
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;
    return ret;
  }

  ret = i2s_channel_enable(s_rx_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable I2S: %s", esp_err_to_name(ret));
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;
    return ret;
  }

  s_initialized = true;
  s_sleeping = false;

  ESP_LOGI(TAG,
           "Initialized: rate=%" PRIu32 "Hz bits=%" PRIu32
           " pins(WS=%d,SCK=%d,SD=%d)",
           s_config.sample_rate, (uint32_t)s_config.bits_per_sample,
           s_config.ws_pin, s_config.sck_pin, s_config.sd_pin);

  return ESP_OK;
}

esp_err_t inmp441_deinit(void) {
  if (!s_initialized)
    return ESP_OK;

  if (s_rx_handle) {
    i2s_channel_disable(s_rx_handle);
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;
  }

  s_initialized = false;
  ESP_LOGI(TAG, "Deinitialized");
  return ESP_OK;
}

esp_err_t inmp441_read(inmp441_reading_t *reading) {
  if (!s_initialized || !reading)
    return ESP_ERR_INVALID_STATE;
  if (s_sleeping)
    return ESP_ERR_INVALID_STATE;

  memset(reading, 0, sizeof(inmp441_reading_t));

  // Allocate sample buffer
  size_t buffer_size = s_config.buffer_samples * sizeof(int16_t);
  int16_t *buffer = malloc(buffer_size);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes", (unsigned)buffer_size);
    return ESP_ERR_NO_MEM;
  }

  // Read I2S data with shorter timeout to avoid ISR conflicts
  // Use multiple small reads instead of one long blocking read
  size_t bytes_read = 0;
  size_t total_read = 0;
  const size_t chunk_size = 512;             // Read in smaller chunks
  const uint32_t timeout_per_chunk_ms = 100; // Shorter timeout

  for (size_t offset = 0; offset < buffer_size && total_read < buffer_size;
       offset += chunk_size) {
    size_t to_read = (buffer_size - offset) < chunk_size
                         ? (buffer_size - offset)
                         : chunk_size;
    bytes_read = 0;

    esp_err_t ret =
        i2s_channel_read(s_rx_handle, (uint8_t *)buffer + offset, to_read,
                         &bytes_read, pdMS_TO_TICKS(timeout_per_chunk_ms));

    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
      ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
      free(buffer);
      return ret;
    }

    total_read += bytes_read;

    if (bytes_read < to_read) {
      // Timeout or end of data, stop reading
      break;
    }

    // Yield periodically to allow other tasks and ISRs to run
    taskYIELD();
  }

  size_t samples_read = total_read / sizeof(int16_t);

  if (samples_read == 0) {
    ESP_LOGW(TAG, "No samples read");
    free(buffer);
    reading->valid = false;
    return ESP_OK;
  }

  // Trust filtering: validate data
  reading->valid = validate_samples(buffer, samples_read);

  if (reading->valid) {
    // Calculate metrics
    calculate_amplitude(buffer, samples_read, &reading->rms_amplitude,
                        &reading->peak_amplitude);

    reading->samples = buffer;
    reading->count = samples_read;
    reading->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    ESP_LOGD(TAG, "Captured %u samples: RMS=%.3f Peak=%.3f",
             (unsigned)samples_read, reading->rms_amplitude,
             reading->peak_amplitude);
  } else {
    free(buffer);
    reading->samples = NULL;
    reading->count = 0;
    ESP_LOGW(TAG, "Samples rejected by trust filter");
  }

  return ESP_OK;
}

esp_err_t inmp441_get_level(float *rms_db) {
  if (!s_initialized || !rms_db)
    return ESP_ERR_INVALID_STATE;
  if (s_sleeping)
    return ESP_ERR_INVALID_STATE;

  inmp441_reading_t reading = {0};
  esp_err_t ret = inmp441_read(&reading);

  if (ret == ESP_OK && reading.valid && reading.samples) {
    // Convert RMS to dB (relative to full scale)
    if (reading.rms_amplitude > 0.0f) {
      *rms_db = 20.0f * log10f(reading.rms_amplitude);
    } else {
      *rms_db = -96.0f; // 16-bit noise floor
    }

    free(reading.samples);
  } else {
    *rms_db = -96.0f;
  }

  return ret;
}

esp_err_t inmp441_sleep(void) {
  if (!s_initialized)
    return ESP_ERR_INVALID_STATE;
  if (s_sleeping)
    return ESP_OK;

  esp_err_t ret = i2s_channel_disable(s_rx_handle);
  if (ret == ESP_OK) {
    s_sleeping = true;
    ESP_LOGI(TAG, "Entered sleep mode");
  }
  return ret;
}

esp_err_t inmp441_wake(void) {
  if (!s_initialized)
    return ESP_ERR_INVALID_STATE;
  if (!s_sleeping)
    return ESP_OK;

  esp_err_t ret = i2s_channel_enable(s_rx_handle);
  if (ret == ESP_OK) {
    s_sleeping = false;
    ESP_LOGI(TAG, "Woke from sleep");
  }
  return ret;
}

esp_err_t inmp441_set_sample_rate(uint32_t rate) {
  if (!s_initialized)
    return ESP_ERR_INVALID_STATE;

  // Reconfigure requires reinit in this driver version
  ESP_LOGI(TAG, "Sample rate change requires reinit (new rate: %" PRIu32 " Hz)",
           rate);
  s_config.sample_rate = rate;

  return ESP_OK; // Will take effect on next init
}

esp_err_t inmp441_set_buffer_size(size_t samples) {
  if (!s_initialized)
    return ESP_ERR_INVALID_STATE;

  s_config.buffer_samples = samples;
  ESP_LOGI(TAG, "Buffer size changed to %u samples", (unsigned)samples);

  return ESP_OK;
}

esp_err_t inmp441_raw_check(void) {
  if (!s_initialized) {
    ESP_LOGD(TAG, "INMP441 not initialized (hardware not connected)");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG,
           "INMP441 I2S check: initialized and ready (rate=%" PRIu32
           "Hz, bits=%" PRIu32 ")",
           s_config.sample_rate, (uint32_t)s_config.bits_per_sample);
  return ESP_OK;
}
