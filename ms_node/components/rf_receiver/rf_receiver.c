#include "rf_receiver.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "RF_RX";

static rmt_channel_handle_t rx_chan = NULL;
static QueueHandle_t rx_queue = NULL;
#define RMT_SYMBOL_BUF_SIZE 256  // Increased from 64 to reduce "buffer too small" log spam
static rmt_symbol_word_t raw_symbols[RMT_SYMBOL_BUF_SIZE];

// RMT Configuration
#define RMT_RESOLUTION_HZ 1000000 // 1MHz, 1us per tick

// Protocol 1: T approx 350us
// Sync: H(1), L(31)  -> H=350us, L=10850us
// 0:    H(1), L(3)   -> H=350us, L=1050us
// 1:    H(3), L(1)   -> H=1050us, L=350us
// Tolerance: +/- 40%

#define RCSWITCH_T_US 350
#define RCSWITCH_TOLERANCE 0.40f

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
  // Need at least Sync + 24 bits (48 symbols)
  if (count < 49)
    return 0;

  // Search for Sync
  int sync_idx = -1;
  uint32_t val, level, next_val, next_level;

  for (int i = 0; i < count - 48; i++) {
    get_rmt_symbol(symbols, i, &val, &level);
    get_rmt_symbol(symbols, i + 1, &next_val, &next_level);

    // Sync: High near 350, Low near 10850
    if (level == 1 && is_duration(val, RCSWITCH_T_US) && next_level == 0 &&
        is_duration(next_val, RCSWITCH_T_US * 31)) {
      sync_idx = i + 2; // Start of data
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
      // Bit 0
    } else if (is_duration(high, RCSWITCH_T_US * 3) &&
               is_duration(low, RCSWITCH_T_US)) {
      // Bit 1
      value |= 1;
    } else {
      return 0;
    }
  }
  return value;
}

static bool s_trigger_received = false;

void rf_receiver_force_trigger(void) {
  s_trigger_received = true;
  ESP_LOGI(TAG, "Manual trigger force-set");
}

bool rf_receiver_check_trigger(void) {
  if (rx_queue == NULL) {
    return false;
  }

  // Return cached trigger if we already found one (and haven't consumed it yet?
  // Actually, this func consumes it).
  if (s_trigger_received) {
    s_trigger_received = false;
    return true;
  }

  rmt_rx_done_event_data_t edata;
  // Non-blocking check
  while (xQueueReceive(rx_queue, &edata, 0) == pdTRUE) {
    // Process received symbols
    uint32_t code = parse_rcswitch(edata.received_symbols, edata.num_symbols);

    // Re-enable rx immediately
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 1000,
        .signal_range_max_ns = 12000000, // 12ms max (Sync is ~11ms)
    };
    rmt_receive(rx_chan, raw_symbols, sizeof(raw_symbols), &receive_config);

    if (code == RF_EXPECTED_CODE) {
      ESP_LOGI(TAG, "RF TRIGGER RECEIVED: Code %lu", code);
      return true; // Return immediately
    } else if (code != 0) {
      ESP_LOGD(TAG, "RF Ignored Code: %lu", code);
    }
  }
  return false;
}

static bool rmt_callback(rmt_channel_handle_t rx_chan,
                         const rmt_rx_done_event_data_t *edata,
                         void *user_ctx) {
  BaseType_t high_task_wakeup = pdFALSE;
  xQueueSendFromISR(rx_queue, edata, &high_task_wakeup);
  return high_task_wakeup == pdTRUE;
}

esp_err_t rf_receiver_init(void) {
  ESP_LOGI(TAG, "Initializing RF Receiver on GPIO %d", RF_RECEIVER_GPIO);

  rmt_rx_channel_config_t rx_chan_config = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = RMT_RESOLUTION_HZ,
      .mem_block_symbols = RMT_SYMBOL_BUF_SIZE,
      .gpio_num = RF_RECEIVER_GPIO,
  };

  esp_err_t err = rmt_new_rx_channel(&rx_chan_config, &rx_chan);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "RF Receiver init failed (no free RMT channel): %s - "
                  "UAV trigger disabled, cluster will run normally",
             esp_err_to_name(err));
    return err;
  }

  rmt_rx_event_callbacks_t cbs = {
      .on_recv_done = rmt_callback,
  };
  err = rmt_rx_register_event_callbacks(rx_chan, &cbs, NULL);
  if (err != ESP_OK) {
    rmt_del_channel(rx_chan);
    rx_chan = NULL;
    return err;
  }
  err = rmt_enable(rx_chan);
  if (err != ESP_OK) {
    rmt_del_channel(rx_chan);
    rx_chan = NULL;
    return err;
  }

  rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));

  rmt_receive_config_t receive_config = {
      .signal_range_min_ns = 1000,
      .signal_range_max_ns = 12000000, // 12ms max
  };
  err = rmt_receive(rx_chan, raw_symbols, sizeof(raw_symbols), &receive_config);
  if (err != ESP_OK) {
    rmt_disable(rx_chan);
    rmt_del_channel(rx_chan);
    rx_chan = NULL;
    vQueueDelete(rx_queue);
    rx_queue = NULL;
    return err;
  }

  return ESP_OK;
}
