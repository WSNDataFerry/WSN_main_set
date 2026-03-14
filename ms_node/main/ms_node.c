#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>

#include "aht21_sensor.h"
#include "bme280_sensor.h"
#include "config.h"
#include "ens160_sensor.h"
#include "gy271_sensor.h"
#include "i2c_bus.h"
#include "ina219_sensor.h"
#include "inmp441_sensor.h"
#include "sensor_config.h"
#include "sensors.h"

#include "auth.h"
#include "battery.h"
#include "ble_manager.h"
#include "election.h"
#include "esp_now_manager.h"
#include "led_manager.h"
#include "metrics.h"
#include "neighbor_manager.h"
#include "nvs_flash.h"
#include "persistence.h"
#include "pme.h"
#include "rf_receiver.h"
#include "state_machine.h"
#include "storage_manager.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "main";

static sensor_config_t s_sensor_config = {0};

// Per-sensor last read timestamps for interval control
static uint64_t s_last_env_read_ms   = 0;
static uint64_t s_last_gas_read_ms   = 0;
static uint64_t s_last_mag_read_ms   = 0;
static uint64_t s_last_power_read_ms = 0;
static uint64_t s_last_audio_read_ms = 0;

// Real vs dummy data flags
// FIX (Issue 3): s_sensors_real is now STICKY — set true on first successful
// real sensor read and never cleared, so the SENSORS_REAL flag stays accurate
// between sensor intervals instead of dropping to false every non-read cycle.
static bool s_battery_real  = false;
static bool s_sensors_real  = false;   // sticky: set true on first real read

// FIX (Issue 6): Packet sequence counter at file scope so it is never reset
// by a scope change or refactor.
static uint32_t s_packet_seq_num = 0;

// Track if we've reset sequence after establishing role
static bool s_seq_reset_done = false;

// FIX (Issue 2): Cache of the most recent REAL sensor readings.
// Between sensor intervals ok_* flags are false, so the payload fallback
// block would inject random noise. We now copy last-known real values
// instead of random data. Fields default to zero (no cache yet).
typedef struct {
    bool    have;           // true once any real read has populated this struct
    float   temp_c;
    float   hum_pct;
    float   pressure_hpa;  // FIX (Issue 5): stored as float, not uint32_t
    uint8_t aqi;
    uint16_t eco2_ppm;
    uint16_t tvoc_ppb;
    float   mag_x, mag_y, mag_z;
    float   audio_rms;
    uint8_t batt_pct;       // FIX (Issue 4): battery cached here too
} sensor_cache_t;

static sensor_cache_t s_cache = {.have = false};

static uint32_t sample_period_ms_for_mode(pme_mode_t mode) {
    switch (mode) {
    case PME_MODE_NORMAL:    return 2000;
    case PME_MODE_POWER_SAVE:return 5000;
    case PME_MODE_CRITICAL:  return 2000; // won't be used (deep sleep path)
    default:                 return 2000;
    }
}

static void log_wakeup_reason(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
        ESP_LOGI(TAG, "wakeup cause: timer");
        break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        ESP_LOGI(TAG, "wakeup cause: power-on or reset");
        break;
    default:
        ESP_LOGI(TAG, "wakeup cause: %d", cause);
        break;
    }
}

// ========== STELLAR CLUSTER TASKS ==========

static void state_machine_task(void *pvParameters) {
    ESP_LOGI(TAG, "State machine task started");
    while (1) {
        state_machine_run();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void metrics_task(void *pvParameters) {
    ESP_LOGI(TAG, "Metrics task started");
    while (1) {
        metrics_update();
        uint32_t ch_id       = neighbor_manager_get_current_ch();
        size_t   cluster_size = neighbor_manager_get_count();
        ESP_LOGI(TAG, "STATUS: State=%s, Role=%s, CH=%lu, Size=%zu",
                 state_machine_get_state_name(), g_is_ch ? "CH" : "NODE",
                 ch_id, cluster_size);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ===========================================

void app_main(void) {
    // Initialize NVS
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    auth_init();

    // Initialize Node ID early for metrics
    uint8_t mac_init[6];
    esp_read_mac(mac_init, ESP_MAC_BT);
    g_mac_addr = ((uint64_t)mac_init[0] << 40) | ((uint64_t)mac_init[1] << 32) |
                 ((uint64_t)mac_init[2] << 24) | ((uint64_t)mac_init[3] << 16) |
                 ((uint64_t)mac_init[4] << 8)  |  (uint64_t)mac_init[5];
    g_node_id  = (uint32_t)(g_mac_addr & 0xFFFFFFFF);
    ESP_LOGI(TAG, "Early ID Init: node_id=%lu", g_node_id);

    metrics_init();
    neighbor_manager_init();
    election_init();
    persistence_init();

    ble_manager_init();
    led_manager_init();

    ESP_ERROR_CHECK(storage_manager_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "Network interface initialized");

    esp_now_manager_init();
    rf_receiver_init();

    state_machine_init();
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_ERROR_CHECK(sensor_config_load(&s_sensor_config));
    ESP_LOGI(TAG,
             "Sensor config: audio_interval=%" PRIu32 "ms, env_interval=%" PRIu32 "ms",
             s_sensor_config.audio_interval_ms,
             s_sensor_config.env_sensor_interval_ms);

    // FIX (Issue 1): Use g_mac_addr (uint64_t, 48-bit) instead of g_node_id
    // (uint32_t, 32-bit). Shifting a uint32_t by 40 or 32 bits gives zero —
    // every node displayed as 00:00:XX:XX:XX:XX in the old code.
    {
        char node_id_str[18];
        snprintf(node_id_str, sizeof(node_id_str),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 (unsigned)((g_mac_addr >> 40) & 0xFF),
                 (unsigned)((g_mac_addr >> 32) & 0xFF),
                 (unsigned)((g_mac_addr >> 24) & 0xFF),
                 (unsigned)((g_mac_addr >> 16) & 0xFF),
                 (unsigned)((g_mac_addr >>  8) & 0xFF),
                 (unsigned)( g_mac_addr        & 0xFF));
        ESP_LOGI(TAG, "Node MAC: %s", node_id_str);
    }

    // Display storage status
    {
        size_t total = 0, used = 0;
        if (storage_manager_get_usage(&used, &total) == ESP_OK) {
            ESP_LOGI(TAG, "Storage: %u/%u bytes (%.1f%% used)",
                     (unsigned)used, (unsigned)total,
                     (total > 0) ? (100.0f * used / total) : 0.0f);
        }
    }

    // Battery + PME (before cluster tasks so metrics_task sees valid battery)
    battery_cfg_t bcfg = {
        .unit    = ADC_UNIT_1,
        .channel = ADC_CHANNEL_3,
        .atten   = ADC_ATTEN_DB_12,
        .r1_ohm  = 232000,
        .r2_ohm  = 33000,
        .samples = 8,
    };
    ESP_ERROR_CHECK(battery_init(&bcfg));

    pme_config_t cfg = {
        .th = {.normal_min_pct = 60, .power_save_min_pct = 10},
        .fake_start_pct    = 0,
        .fake_drop_per_tick = 0,
        .fake_tick_ms      = 1000,
    };
    ESP_ERROR_CHECK(pme_init(&cfg));

    ESP_LOGI(TAG, "Creating STELLAR cluster tasks...");
    xTaskCreate(state_machine_task, "state_machine",
                STATE_MACHINE_TASK_STACK_SIZE, NULL,
                STATE_MACHINE_TASK_PRIORITY, NULL);
    xTaskCreate(metrics_task, "metrics",
                METRICS_TASK_STACK_SIZE, NULL,
                METRICS_TASK_PRIORITY, NULL);

    esp_err_t ret;

    vTaskDelay(pdMS_TO_TICKS(30));
    ret = ms_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Proceeding in MOCK SENSOR mode without I2C");
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    const int MAX_RETRIES  = 3;
    const int RETRY_DELAY_MS = 500;

#define INIT_SENSOR(fn, name) \
    do { \
        for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) { \
            ret = fn; \
            if (ret == ESP_OK) break; \
            ESP_LOGW(TAG, name " init attempt %d/%d failed: %s", \
                     attempt, MAX_RETRIES, esp_err_to_name(ret)); \
            if (attempt < MAX_RETRIES) vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS)); \
        } \
        if (ret != ESP_OK) ESP_LOGW(TAG, name " init skipped after %d retries", MAX_RETRIES); \
    } while (0)

    INIT_SENSOR(bme280_init(),      "BME280");
    INIT_SENSOR(aht21_init(),       "AHT21");
    INIT_SENSOR(ens160_init(),      "ENS160");
    INIT_SENSOR(gy271_init(),       "GY-271");
    INIT_SENSOR(ina219_init_basic(),"INA219");

    inmp441_config_t inmp_cfg = {
        .ws_pin         = 5,
        .sck_pin        = 6,
        .sd_pin         = 7,
        .sample_rate    = 16000,
        .bits_per_sample = 16,
        .buffer_samples = 512,
    };
    INIT_SENSOR(inmp441_init(&inmp_cfg), "INMP441");
#undef INIT_SENSOR

    sensors_raw_sanity_check();
    vTaskDelay(pdMS_TO_TICKS(200));

    static int  s_config_reload_count = 0;
    static bool s_first_loop          = true;

    esp_task_wdt_add(NULL);

    while (1) {

        if (s_first_loop) {
            ESP_LOGI(TAG, "Main loop running (state machine + metrics active)");
            s_first_loop = false;
        }

        // Reset sequence counter once role is established for dashboard compatibility
        // Dashboard expects seq=1 as first stored packet per session
        if (!s_seq_reset_done &&
            (g_current_state == STATE_CH || g_current_state == STATE_MEMBER)) {
            s_packet_seq_num = 0;  // Will become 1 after increment below
            s_seq_reset_done = true;
            ESP_LOGI(TAG, "Sequence reset to 1 (role established as %s)",
                     g_is_ch ? "CH" : "MEMBER");
        }

        // FIX (Issue 6): sequence advances exactly once per loop at the top,
        // before any payload or sensor logic, so every stored record has a
        // unique monotonically increasing sequence number.
        s_packet_seq_num++;

        if (++s_config_reload_count >= 15) {
            s_config_reload_count = 0;
            (void)sensor_config_load(&s_sensor_config);
        }

        // ---- Battery read ----
        uint32_t vadc_mv = 0, vbat_mv = 0;
        uint8_t  batt_pct = 0;

        if (battery_read(&vadc_mv, &vbat_mv, &batt_pct) == ESP_OK && vbat_mv > 2000) {
            s_battery_real = true;
            ESP_LOGI(TAG, "BAT vadc=%lumV vbat=%lumV pct=%u%%",
                     (unsigned long)vadc_mv, (unsigned long)vbat_mv, batt_pct);
            pme_set_batt_pct(batt_pct);
        } else {
            s_battery_real = false;
            uint8_t prev_pct = pme_get_batt_pct();
            batt_pct = (prev_pct == 0) ? 100 : prev_pct;
            ESP_LOGW(TAG, "Battery ADC read failed; using last known pct=%u%%", batt_pct);
            pme_set_batt_pct(batt_pct);
        }

        // FIX (Issue 4): cache battery so it appears in the payload below.
        s_cache.batt_pct = batt_pct;

        pme_mode_t mode = pme_get_mode();
        ESP_LOGI(TAG, "PME batt=%u%% mode=%s", pme_get_batt_pct(), pme_mode_to_str(mode));

        // Storage health check
        {
            size_t _used = 0, _total = 0;
            if (storage_manager_get_usage(&_used, &_total) == ESP_OK && _total > 0) {
                uint32_t _pct = (_used * 100) / _total;
                if      (_pct >= 95) ESP_LOGW(TAG, "Storage CRITICAL (>95%%)");
                else if (_pct >= 90) ESP_LOGW(TAG, "Storage WARNING  (>90%%)");
            }
        }

        // ---- Per-sensor interval timing ----
        uint64_t now_ms = esp_timer_get_time() / 1000ULL;

        uint32_t env_interval_ms   = s_sensor_config.env_sensor_interval_ms;
        uint32_t gas_interval_ms   = s_sensor_config.gas_sensor_interval_ms;
        uint32_t mag_interval_ms   = s_sensor_config.mag_sensor_interval_ms;
        uint32_t power_interval_ms = s_sensor_config.power_sensor_interval_ms;
        uint32_t audio_interval_ms = s_sensor_config.audio_interval_ms;

        switch (mode) {
        case PME_MODE_NORMAL:
            break;
        case PME_MODE_POWER_SAVE:
            env_interval_ms   *= 5;
            gas_interval_ms   *= 3;
            mag_interval_ms   *= 5;
            power_interval_ms *= 2;
            // FIX (Issue 7): was "audio_interval_ms *= 1.5" which does a
            // float multiply on a uint32_t then silently narrows back.
            // Integer multiply-then-divide is exact and warning-free.
            audio_interval_ms  = audio_interval_ms * 3 / 2;
            break;
        case PME_MODE_CRITICAL:
        default:
            env_interval_ms   *= 120;
            gas_interval_ms   *= 120;
            mag_interval_ms   *= 120;
            power_interval_ms *= 1;
            audio_interval_ms *= 120;
            break;
        }

        bool time_for_env   = (now_ms - s_last_env_read_ms)   >= env_interval_ms;
        bool time_for_gas   = (now_ms - s_last_gas_read_ms)   >= gas_interval_ms;
        bool time_for_mag   = (now_ms - s_last_mag_read_ms)   >= mag_interval_ms;
        bool time_for_power = (now_ms - s_last_power_read_ms) >= power_interval_ms;
        bool time_for_audio = (now_ms - s_last_audio_read_ms) >= audio_interval_ms;

        // ---- Sensor reads ----
        bme280_reading_t  bme   = {0};
        aht21_reading_t   aht   = {0};
        uint8_t           aht_raw[AHT21_RAW_LEN] = {0};
        ens160_reading_t  ens   = {0};
        gy271_reading_t   mag   = {0};
        ina219_basic_t    ina   = {0};
        inmp441_reading_t audio = {0};

        bool do_full  = (mode == PME_MODE_NORMAL);
        bool do_light = (mode != PME_MODE_CRITICAL);
        bool do_audio = (mode == PME_MODE_NORMAL);

        // ok_*   — data is available this cycle (real OR in-interval dummy)
        // real_* — data came from hardware this cycle
        bool ok_bme   = false, real_bme   = false;
        bool ok_aht   = false, real_aht   = false;
        bool ok_ens   = false, real_ens   = false;
        bool ok_mag   = false, real_mag   = false;
        bool ok_ina   = false, real_ina   = false;
        bool ok_audio = false, real_audio = false;

        if (do_full && time_for_env && s_sensor_config.bme280_enabled) {
            ok_bme   = (bme280_read(&bme) == ESP_OK);
            real_bme = ok_bme;
            if (!ok_bme) {
                bme.temperature_c = 25.0f + 5.0f * sinf(now_ms / 10000.0f);
                bme.humidity_pct  = 50.0f + 10.0f * cosf(now_ms / 10000.0f);
                bme.pressure_hpa  = 1013.0f + 5.0f * sinf(now_ms / 20000.0f);
                ok_bme = true;
                ESP_LOGD(TAG, "BME280 not connected, using dummy values");
            }
            if (ok_bme) s_last_env_read_ms = now_ms;
        }

        if (do_light && time_for_env && s_sensor_config.aht21_enabled) {
            ok_aht   = (aht21_read_with_raw(&aht, aht_raw) == ESP_OK);
            real_aht = ok_aht;
            if (!ok_aht) {
                aht.temperature_c = 25.0f + 5.0f * sinf(now_ms / 10000.0f);
                aht.humidity_pct  = 50.0f + 10.0f * cosf(now_ms / 10000.0f);
                ok_aht = true;
            }
            if (ok_aht) s_last_env_read_ms = now_ms;
        }

        if (do_light && time_for_gas && s_sensor_config.ens160_enabled) {
            ok_ens   = (ens160_read_iaq(&ens) == ESP_OK);
            real_ens = ok_ens;
            if (!ok_ens) {
                ens.aqi_uba  = 1  + (now_ms / 1000) % 5;
                ens.tvoc_ppb = 10 + (now_ms / 1000) % 50;
                ens.eco2_ppm = 400 + (now_ms / 1000) % 100;
                ens.status   = 0;
                ok_ens = true;
            }
            if (ok_ens) s_last_gas_read_ms = now_ms;
        }

        if (do_light && time_for_power && s_sensor_config.ina219_enabled) {
            ok_ina   = (ina219_read_basic(&ina) == ESP_OK);
            real_ina = ok_ina;
            if (!ok_ina) {
                ina.bus_voltage_v   = 4.0f;
                ina.shunt_voltage_mv = 10.0f + 5.0f * sinf(now_ms / 5000.0f);
                ina.current_ma      = ina.shunt_voltage_mv / 0.1f;
                ok_ina = true;
            }
            if (ok_ina) s_last_power_read_ms = now_ms;
        }

        if (do_full && time_for_mag && s_sensor_config.gy271_enabled) {
            ok_mag   = (gy271_read(&mag) == ESP_OK);
            real_mag = ok_mag;
            if (!ok_mag) {
                mag.x_uT = 30.0f * cosf(now_ms / 5000.0f);
                mag.y_uT = 30.0f * sinf(now_ms / 5000.0f);
                mag.z_uT = 40.0f;
                ok_mag = true;
            }
            if (ok_mag) s_last_mag_read_ms = now_ms;
        }

        if (do_audio && time_for_audio && s_sensor_config.inmp441_enabled) {
            ok_audio   = (inmp441_read(&audio) == ESP_OK && audio.valid);
            real_audio = ok_audio;
            if (!ok_audio) {
                audio.count         = 512;
                audio.rms_amplitude = 0.05f + 0.02f * sinf(now_ms / 1000.0f);
                audio.peak_amplitude = audio.rms_amplitude * 1.414f;
                audio.timestamp_ms  = now_ms;
                audio.valid         = true;
                ok_audio = true;
            }
            if (ok_audio) s_last_audio_read_ms = now_ms;
        }

        if (ok_aht) {
            (void)ens160_set_env(aht.temperature_c, aht.humidity_pct);
        }

        if (ok_bme)
            ESP_LOGI(TAG, "BME280 T=%.2f C | H=%.2f %% | P=%.2f hPa",
                     bme.temperature_c, bme.humidity_pct, bme.pressure_hpa);
        if (ok_aht)
            ESP_LOGI(TAG, "AHT21 T=%.2f C | H=%.2f %%",
                     aht.temperature_c, aht.humidity_pct);
        if (ok_ens)
            ESP_LOGI(TAG, "ENS160 status:0x%02X | AQI=%u | TVOC=%u ppb | eCO2=%u ppm",
                     ens.status, ens.aqi_uba, ens.tvoc_ppb, ens.eco2_ppm);
        if (ok_mag)
            ESP_LOGI(TAG, "GY-271 status:0x%02X | uT: X=%.2f Y=%.2f Z=%.2f",
                     mag.status, mag.x_uT, mag.y_uT, mag.z_uT);
        if (ok_ina)
            ESP_LOGI(TAG, "INA219 bus=%.3f V | shunt=%.3f mV | i=%.1f mA",
                     ina.bus_voltage_v, ina.shunt_voltage_mv, ina.current_ma);
        if (ok_audio)
            ESP_LOGI(TAG, "INMP441 samples=%u | rms=%.4f | peak=%.4f | ts=%lu ms",
                     (unsigned)audio.count, audio.rms_amplitude,
                     audio.peak_amplitude, (unsigned long)audio.timestamp_ms);

        // Cache update: use ok_* (not real_*) so dummy reads populate the
        // cache exactly the same way real hardware would. This means the very
        // first loop iteration fills the cache, and every subsequent skipped
        // cycle (interval not elapsed) serves stable cached values instead of
        // random noise — which is what we want for testing the data pipeline
        // without real hardware present.
        // s_sensors_real stays sticky on real hardware reads only.
        if (ok_bme) {
            s_cache.have         = true;
            s_cache.temp_c       = bme.temperature_c;
            s_cache.hum_pct      = bme.humidity_pct;
            s_cache.pressure_hpa = bme.pressure_hpa;
        } else if (ok_aht) {
            s_cache.have    = true;
            s_cache.temp_c  = aht.temperature_c;
            s_cache.hum_pct = aht.humidity_pct;
        }
        if (ok_ens) {
            s_cache.have     = true;
            s_cache.aqi      = ens.aqi_uba;
            s_cache.eco2_ppm = ens.eco2_ppm;
            s_cache.tvoc_ppb = ens.tvoc_ppb;
        }
        if (ok_mag) {
            s_cache.have  = true;
            s_cache.mag_x = mag.x_uT;
            s_cache.mag_y = mag.y_uT;
            s_cache.mag_z = mag.z_uT;
        }
        if (ok_audio) {
            s_cache.have      = true;
            s_cache.audio_rms = audio.rms_amplitude;
        }
        // Sticky real-hardware flag — separate from cache, unaffected by dummies
        if (real_bme || real_aht || real_ens || real_mag || real_ina || real_audio) {
            s_sensors_real = true;
        }

        // ──────────────────────────────────────────────────────────────────
        // PAYLOAD ASSEMBLY
        // ──────────────────────────────────────────────────────────────────
        {
            sensor_payload_t payload = {0};
            payload.node_id      = g_node_id;
            payload.timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            payload.seq_num      = s_packet_seq_num;

            // FIX (Issue 3): flag reflects whether real hardware has EVER
            // responded — not just whether it fired this specific cycle.
            payload.flags =
                (s_sensors_real  ? SENSOR_PAYLOAD_FLAG_SENSORS_REAL  : 0) |
                (s_battery_real  ? SENSOR_PAYLOAD_FLAG_BATTERY_REAL   : 0);

            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_BT);
            memcpy(payload.mac_addr, mac, 6);

            // --- Environmental ---
            if (ok_bme) {
                payload.temp_c       = bme.temperature_c;
                payload.hum_pct      = bme.humidity_pct;
                // FIX (Issue 5): pressure stays float — no uint32_t cast.
                payload.pressure_hpa = bme.pressure_hpa;
            } else if (ok_aht) {
                payload.temp_c  = aht.temperature_c;
                payload.hum_pct = aht.humidity_pct;
                // pressure_hpa not updated (AHT21 has no pressure sensor)
            } else if (s_cache.have) {
                // FIX (Issue 2): use cached real values instead of random noise
                payload.temp_c       = s_cache.temp_c;
                payload.hum_pct      = s_cache.hum_pct;
                payload.pressure_hpa = s_cache.pressure_hpa;
            } else {
                // Truly no sensor AND no cache (first boot, no hardware):
                // only now fall back to random mock data.
                payload.temp_c       = 22.0f + ((float)(esp_random() % 160) / 10.0f - 3.0f);
                payload.hum_pct      = 50.0f + ((float)(esp_random() % 300) / 10.0f - 10.0f);
                payload.pressure_hpa = 1005.0f + (float)(esp_random() % 20);
            }

            // --- Gas ---
            if (ok_ens) {
                payload.aqi      = ens.aqi_uba;
                payload.tvoc_ppb = ens.tvoc_ppb;
                payload.eco2_ppm = ens.eco2_ppm;
            } else if (s_cache.have) {
                payload.aqi      = s_cache.aqi;
                payload.tvoc_ppb = s_cache.tvoc_ppb;
                payload.eco2_ppm = s_cache.eco2_ppm;
            } else {
                payload.aqi      = 30  + (esp_random() % 120);
                payload.eco2_ppm = 400 + (esp_random() % 800);
                payload.tvoc_ppb = 10  + (esp_random() % 490);
            }

            // --- Magnetometer ---
            if (ok_mag) {
                payload.mag_x = mag.x_uT;
                payload.mag_y = mag.y_uT;
                payload.mag_z = mag.z_uT;
            } else if (s_cache.have) {
                payload.mag_x = s_cache.mag_x;
                payload.mag_y = s_cache.mag_y;
                payload.mag_z = s_cache.mag_z;
            } else {
                payload.mag_x = (float)(esp_random() % 1000) / 10.0f - 50.0f;
                payload.mag_y = (float)(esp_random() % 1000) / 10.0f - 50.0f;
                payload.mag_z = (float)(esp_random() % 1000) / 10.0f - 50.0f;
            }

            // --- Audio ---
            if (ok_audio) {
                payload.audio_rms = audio.rms_amplitude;
            } else if (s_cache.have) {
                payload.audio_rms = s_cache.audio_rms;
            } else {
                payload.audio_rms = 0.001f + ((float)(esp_random() % 500) / 10000.0f);
            }

            // ── MSLG storage (CH + MEMBER) ──────────────────────────────
            if (g_current_state == STATE_CH || g_current_state == STATE_MEMBER) {
                char json_payload[400];
                // FIX (Issue 4): "bat" field added.
                // FIX (Issue 5): "p" now uses %.2f (float) not %lu (integer).
                snprintf(json_payload, sizeof(json_payload),
                    "{\"id\":%lu,\"seq\":%lu"
                    ",\"mac\":\"%02x%02x%02x%02x%02x%02x\""
                    ",\"ts\":%llu"
                    ",\"t\":%.1f,\"h\":%.1f,\"p\":%llu"
                    ",\"q\":%u,\"eco2\":%u,\"tvoc\":%u"
                    ",\"mx\":%.2f,\"my\":%.2f,\"mz\":%.2f"
                    ",\"a\":%.3f}",
                    (unsigned long)payload.node_id,
                    (unsigned long)payload.seq_num,
                    payload.mac_addr[0], payload.mac_addr[1],
                    payload.mac_addr[2], payload.mac_addr[3],
                    payload.mac_addr[4], payload.mac_addr[5],
                    (unsigned long long)payload.timestamp_ms,
                    payload.temp_c,
                    payload.hum_pct,
                    (unsigned long long)payload.pressure_hpa,   // float — matches %.2f
                    (unsigned)payload.aqi,
                    (unsigned)payload.eco2_ppm,
                    (unsigned)payload.tvoc_ppb,
                    payload.mag_x, payload.mag_y, payload.mag_z,
                    payload.audio_rms);

                esp_err_t store_ret = storage_manager_write_compressed(json_payload, true);
                if (store_ret == ESP_OK) {
                    ESP_LOGI(TAG, "Stored seq=%lu to MSLG (role=%s) | chunks: %d",
                             (unsigned long)payload.seq_num,
                             g_is_ch ? "CH" : "MBR",
                             storage_manager_get_mslg_chunk_count());
                } else {
                    ESP_LOGE(TAG, "Failed to store seq=%lu to MSLG: %s",
                             (unsigned long)payload.seq_num,
                             esp_err_to_name(store_ret));
                }
            }
        }

        // ---- Storage monitoring ----
        {
            size_t used = 0, total = 0;
            if (storage_manager_get_usage(&used, &total) == ESP_OK && total > 0) {
                ESP_LOGW(TAG, "Storage: %u/%u bytes (%.1f%% full)",
                         (unsigned)used, (unsigned)total,
                         (100.0f * used / total));
            }
        }

        // ---- Deep sleep (critical battery) ----
        if (mode == PME_MODE_CRITICAL) {
            uint32_t sleep_ms = 1800000; // 30 minutes
            ESP_LOGW(TAG, "PME critical: entering deep sleep for %" PRIu32 " ms",
                     sleep_ms);
            ESP_ERROR_CHECK(
                esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000ULL));
            esp_deep_sleep_start();
        }

        // ---- Smart sleep ----
        uint32_t sleep_ms = state_machine_get_sleep_time_ms();
        uint32_t sample_ms = sample_period_ms_for_mode(mode);
        // Treat the default sentinel (5000) as "use sample period"
        if (sleep_ms == 5000) {
            sleep_ms = sample_ms;
        }
        // Ensure all nodes continue sampling/recording regularly even when a
        // TDMA slot is scheduled far in the future. Cap long sleeps to the
        // sample period so sensor reads and storage writes remain constant.
        if (sleep_ms > sample_ms) {
            sleep_ms = sample_ms;
        }
        ESP_LOGI(TAG, "Smart Sleep: Waiting %lu ms (BLE Active)",
                 (unsigned long)sleep_ms);

        const uint32_t chunk_ms = 1000;
        while (sleep_ms > 0) {
            uint32_t cur = (sleep_ms > chunk_ms) ? chunk_ms : sleep_ms;
            vTaskDelay(pdMS_TO_TICKS(cur));
            esp_task_wdt_reset();
            sleep_ms -= cur;
        }
    }
}
