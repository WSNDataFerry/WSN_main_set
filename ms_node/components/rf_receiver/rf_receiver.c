#include "rf_receiver.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "RF_RX";

static rmt_channel_handle_t rx_chan = NULL;
static QueueHandle_t rx_queue = NULL;
static bool s_trigger_received = false;
static bool rmt_callback_error = false;
static bool s_rf_enabled = true;  // RF receiver enabled/disabled state
static rmt_symbol_word_t raw_symbols[RMT_RX_SYMBOLS];

static rmt_receive_config_t g_receive_config = {
  .signal_range_min_ns = RMT_MIN_PULSE_NS,
  .signal_range_max_ns = RCSWITCH_T_US * 35 * 1000,
};

static bool is_duration(uint32_t val, uint32_t target_us) {
  uint32_t min = target_us * (1.0f - RCSWITCH_TOLERANCE);
  uint32_t max = target_us * (1.0f + RCSWITCH_TOLERANCE);
  return (val >= min && val <= max);
}

static void get_rmt_symbol(const rmt_symbol_word_t *symbols, size_t index,
                           uint32_t *val, uint32_t *level) {
  // Use raw bitwise access to avoid member name issues (val0 vs val vs val1
  // etc)
  const uint32_t *raw_words = (const uint32_t *)symbols;
  size_t word_index = index / 2;
  uint32_t raw = raw_words[word_index];

  if (index % 2 == 0) {
    // Symbol 0: Duration is lower 15 bits, Level is bit 15
    *val = raw & 0x7FFF;
    *level = (raw >> 15) & 1;
  } else {
    // Symbol 1: Duration is bits 16-30, Level is bit 31
    *val = (raw >> 16) & 0x7FFF;
    *level = (raw >> 31) & 1;
  }
}

static uint32_t parse_rcswitch(const rmt_symbol_word_t *symbols, size_t count) {
  if (count < 49)
    return 0;

  int sync_idx = -1;
  uint32_t val, level, next_val, next_level;

  for (int i = 0; i < count - 48; i++) {
    get_rmt_symbol(symbols, i, &val, &level);
    get_rmt_symbol(symbols, i + 1, &next_val, &next_level);

    if (level == 1 && is_duration(val, RCSWITCH_T_US) && next_level == 0 &&
        is_duration(next_val, RCSWITCH_T_US * 31)) {
      sync_idx = i + 2; 
      break;
    }
  }

  if (sync_idx < 0)
    return 0;

  uint32_t value = 0;
  for (int i = 0; i < 24; i++) {
    int idx = sync_idx + (i * 2);
    if (idx + 1 >= count)
      return 0;

    uint32_t high, high_lvl, low, low_lvl;
    get_rmt_symbol(symbols, idx, &high, &high_lvl);
    get_rmt_symbol(symbols, idx + 1, &low, &low_lvl);

    value <<= 1;
    if (is_duration(high, RCSWITCH_T_US) &&
        is_duration(low, RCSWITCH_T_US * 3)) {
    } else if (is_duration(high, RCSWITCH_T_US * 3) &&
               is_duration(low, RCSWITCH_T_US)) {
      value |= 1;
    } else {
      return 0;
    }
  }
  ESP_LOGI(TAG, "Decoded value: %lu", value);
  return value;
}

void rf_receiver_force_trigger(void) {
  s_trigger_received = true;
  ESP_LOGI(TAG, "Manual trigger force-set");
}

bool rf_receiver_check_trigger(void) {
    if (rx_queue == NULL || !s_rf_enabled) {
        return false;
    }

    if (s_trigger_received) {
        s_trigger_received = false;
        return true;
    }

    if(rmt_callback_error){
      ESP_LOGE(TAG, "Failed to restart RMT receive: %s", esp_err_to_name(ESP_FAIL));
      return false;
    }

    rmt_rx_done_event_data_t edata;
    if (xQueueReceive(rx_queue, &edata, 0) != pdTRUE) {
        return false;
    }

  ESP_LOGD(TAG, "rmt event: num_symbols=%u", (unsigned)edata.num_symbols);

  // ---- SAFE COPY BEFORE RESTARTING RMT ----
  size_t copy_count = edata.num_symbols;

  if (copy_count > RMT_RX_SYMBOLS) {
    ESP_LOGW(TAG, "Received %u symbols, truncating to %u", (unsigned)copy_count, (unsigned)RMT_RX_SYMBOLS);
    copy_count = RMT_RX_SYMBOLS;
  }

  rmt_symbol_word_t *local_copy = malloc(copy_count * sizeof(rmt_symbol_word_t));
  if (local_copy == NULL) {
    ESP_LOGE(TAG, "Failed to allocate local copy buffer (%u symbols)", (unsigned)copy_count);
    return false;
  }

  memcpy(local_copy,
       edata.received_symbols,
       copy_count * sizeof(rmt_symbol_word_t));

  /* Re-arm RMT receive now that we've made a safe copy of the received symbols.
     Doing the receive/re-arm from task context avoids racing the driver's
     internal ringbuffer with the ISR / queued event pointers which can lead
     to "user buffer too small" truncation messages. */
  esp_err_t err = rmt_receive(rx_chan, raw_symbols, sizeof(raw_symbols), &g_receive_config);
  if (err != ESP_OK) {
      rmt_callback_error = true;
      ESP_LOGE(TAG, "Failed to restart RMT receive: %s", esp_err_to_name(err));
      free(local_copy);
      return false;
  }

  // ---- Parse safely copied data ----
  uint32_t code = parse_rcswitch(local_copy, copy_count);

    if (code == RF_EXPECTED_CODE) {
        ESP_LOGI(TAG, "RF TRIGGER RECEIVED: Code %lu", code);
        free(local_copy);
        return true;
    }

  if (code != 0) {
    ESP_LOGD(TAG, "RF Ignored Code: %lu", code);
    free(local_copy);
  } else {
    // Decoding failed. Log a small sample of the received symbol timings
    // (levels and durations) to help debug timing/tolerance issues.
    const int sample_count = (copy_count < 40) ? copy_count : 40;
    char buf[512];
    size_t off = 0;
    off += snprintf(buf + off, sizeof(buf) - off, "Sample symbols (%d):", sample_count);
    for (int i = 0; i < sample_count; i++) {
      uint32_t v, l;
      get_rmt_symbol(local_copy, i, &v, &l);
      off += snprintf(buf + off, sizeof(buf) - off, " %c%lu", l ? 'H' : 'L', v);
      if (off >= sizeof(buf) - 32) break;
    }
    ESP_LOGD(TAG, "%s", buf);
  }

    free(local_copy);

    return false;
}

static bool rmt_callback(rmt_channel_handle_t rx_chan,
                         const rmt_rx_done_event_data_t *edata,
                         void *user_ctx) {
    BaseType_t high_task_wakeup = pdFALSE;
    
    if (rx_queue != NULL) {
        xQueueSendFromISR(rx_queue, edata, &high_task_wakeup);
    }
  /* Do not re-arm RMT from ISR/callback. The receive/re-arm is performed from
     task context after a safe copy of the received symbols to avoid races
     with the driver's internal buffers (which can cause truncated messages). */
    return high_task_wakeup == pdTRUE;
}

esp_err_t rf_receiver_init(void) {
    ESP_LOGI(TAG, "Initializing RF Receiver on GPIO %d", RF_RECEIVER_GPIO);

    rmt_rx_channel_config_t rx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = RMT_CHAN_MEM_SYMBOLS,
        .gpio_num = RF_RECEIVER_GPIO,
    };

    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_chan_config, &rx_chan));

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = rmt_callback,
    };

    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_chan, &cbs, NULL));
    ESP_ERROR_CHECK(rmt_enable(rx_chan));

    rx_queue = xQueueCreate(10, sizeof(rmt_rx_done_event_data_t));
    if (rx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create RX queue");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(
        rmt_receive(rx_chan, raw_symbols, sizeof(raw_symbols), &g_receive_config)
    );

    return ESP_OK;
}

void rf_receiver_disable(void) {
    ESP_LOGI(TAG, "RF receiver disabled - ignoring triggers during UAV onboarding");
    s_rf_enabled = false;
    
    // Clear any pending trigger to prevent race conditions
    s_trigger_received = false;
    
    // Drain the queue to ignore any pending RF events
    // NOTE: This consumes RMT events without re-arming the RMT receive.
    // rf_receiver_enable() MUST re-arm the RMT to restore operation.
    if (rx_queue) {
        rmt_rx_done_event_data_t edata;
        while (xQueueReceive(rx_queue, &edata, 0) == pdTRUE) {
            // Discard events
        }
    }
}

void rf_receiver_enable(void) {
    ESP_LOGI(TAG, "RF receiver re-enabled - listening for triggers");
    
    // Clear any stale trigger state
    s_trigger_received = false;
    rmt_callback_error = false;
    
    // Drain any events that arrived while disabled (ISR may have queued them)
    // These would be stale and must not trigger onboarding
    if (rx_queue) {
        rmt_rx_done_event_data_t edata;
        int drained = 0;
        while (xQueueReceive(rx_queue, &edata, 0) == pdTRUE) {
            drained++;
        }
        if (drained > 0) {
            ESP_LOGW(TAG, "Drained %d stale RF events from queue", drained);
        }
    }
    
    // Re-arm the RMT receive - this is CRITICAL because:
    // 1. During disable, the ISR may have fired and queued an event
    // 2. That event was drained (either by disable or above) without re-arming
    // 3. RMT is now in "done" state and won't receive until re-armed
    // 4. Even if no events fired, re-arming is safe (idempotent for active RMT)
    if (rx_chan) {
        esp_err_t err = rmt_receive(rx_chan, raw_symbols, sizeof(raw_symbols), &g_receive_config);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "RMT receive re-armed successfully");
        } else if (err == ESP_ERR_INVALID_STATE) {
            // RMT is already receiving - that's fine
            ESP_LOGI(TAG, "RMT already active, no re-arm needed");
        } else {
            ESP_LOGE(TAG, "Failed to re-arm RMT receive: %s", esp_err_to_name(err));
            rmt_callback_error = true;
        }
    }
    
    s_rf_enabled = true;
}
