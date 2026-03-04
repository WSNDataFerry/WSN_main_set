#include "battery.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"  // for esp_rom_delay_us

static const char *TAG = "battery";

static battery_cfg_t s_cfg;
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_has_cali = false;

// ============================================================================
// Calibrated constants from working test code
// ============================================================================
// Calibrated voltage divider ratio (from actual measurements, not theoretical)
// Theoretical: (232000+33000)/33000 = 8.03, but measured calibration gives 7.83
#define CALIBRATED_DIVIDER_RATIO  7.83f

// ADC calibration offset to compensate for ADC reading slightly low
#define ADC_CALIBRATION_OFFSET_V  0.020f  // 20mV

// Battery voltage range for 3S LiPo
#define BATTERY_MIN_VOLTAGE_V     9.0f    // 0% (3.0V per cell)
#define BATTERY_MAX_VOLTAGE_V     12.6f   // 100% (4.2V per cell)

// ============================================================================

static uint8_t pct_from_voltage(float voltage_v)
{
    // 3S LiPo battery pack voltage mapping:
    // 12.6V -> 100% (fully charged, 4.2V per cell)
    // 9.0V  -> 0%   (discharged, 3.0V per cell)
    
    if (voltage_v <= BATTERY_MIN_VOLTAGE_V) return 0;
    if (voltage_v >= BATTERY_MAX_VOLTAGE_V) return 100;

    float pct = ((voltage_v - BATTERY_MIN_VOLTAGE_V) /
                 (BATTERY_MAX_VOLTAGE_V - BATTERY_MIN_VOLTAGE_V)) * 100.0f;
    return (uint8_t)(pct + 0.5f);
}

esp_err_t battery_init(const battery_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = s_cfg.unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = s_cfg.atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_cfg.channel, &chan_cfg));

    // Try calibration (best accuracy) - use curve fitting on ESP32-S3
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id = s_cfg.unit,
        .chan = s_cfg.channel,
        .atten = s_cfg.atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal_cfg, &s_cali) == ESP_OK) {
        s_has_cali = true;
        ESP_LOGI(TAG, "ADC calibration: curve fitting enabled");
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!s_has_cali) {
        adc_cali_line_fitting_config_t cal_cfg = {
            .unit_id = s_cfg.unit,
            .atten = s_cfg.atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&cal_cfg, &s_cali) == ESP_OK) {
            s_has_cali = true;
            ESP_LOGI(TAG, "ADC calibration: line fitting enabled");
        }
    }
#endif

    if (!s_has_cali) {
        ESP_LOGW(TAG, "ADC calibration not available, will use uncalibrated conversion");
    }

    ESP_LOGI(TAG, "Battery ADC init: unit=%d ch=%d atten=%d divider_ratio=%.2f samples=%u",
             (int)s_cfg.unit, (int)s_cfg.channel, (int)s_cfg.atten,
             CALIBRATED_DIVIDER_RATIO, (unsigned)s_cfg.samples);

    return ESP_OK;
}

esp_err_t battery_read(uint32_t *vadc_mv, uint32_t *vbat_mv, uint8_t *pct)
{
    if (!vadc_mv || !vbat_mv || !pct) return ESP_ERR_INVALID_ARG;
    if (!s_adc) return ESP_ERR_INVALID_STATE;

    const uint16_t n = (s_cfg.samples == 0) ? 8 : s_cfg.samples;

    // Collect samples with inter-sample delays (like the working test code)
    int valid_samples = 0;
    long sum_raw = 0;

    for (uint16_t i = 0; i < n; i++) {
        int raw = 0;
        esp_err_t ret = adc_oneshot_read(s_adc, s_cfg.channel, &raw);
        if (ret == ESP_OK) {
            sum_raw += raw;
            valid_samples++;
        } else {
            ESP_LOGW(TAG, "adc_oneshot_read failed on sample %d: %s", i, esp_err_to_name(ret));
        }
        // Short delay between samples to let ADC sampling cap charge through high-Z divider
        esp_rom_delay_us(5000);  // 5ms delay like working test code
    }

    if (valid_samples == 0) {
        ESP_LOGW(TAG, "No valid ADC samples");
        return ESP_ERR_INVALID_RESPONSE;
    }

    int avg_raw = (int)(sum_raw / valid_samples);

    // Convert raw ADC to voltage using calibration or fallback
    int voltage_mv = 0;
    if (s_has_cali) {
        esp_err_t ret = adc_cali_raw_to_voltage(s_cali, avg_raw, &voltage_mv);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "adc_cali_raw_to_voltage failed: %s, using uncalibrated", esp_err_to_name(ret));
            // Fallback for DB_12 attenuation: full scale ~3100mV
            voltage_mv = (avg_raw * 3100) / 4095;
        }
    } else {
        // Uncalibrated fallback for DB_12 attenuation
        voltage_mv = (avg_raw * 3100) / 4095;
    }

    // Apply calibration offset and compute battery voltage using calibrated ratio
    float adc_pin_v = (voltage_mv / 1000.0f) + ADC_CALIBRATION_OFFSET_V;
    float battery_v = adc_pin_v * CALIBRATED_DIVIDER_RATIO;

    // Convert to mV for output
    *vadc_mv = (uint32_t)(adc_pin_v * 1000.0f);
    *vbat_mv = (uint32_t)(battery_v * 1000.0f);
    *pct = pct_from_voltage(battery_v);

    ESP_LOGI(TAG, "BAT: avg_raw=%d adc_pin=%.3fV battery=%.2fV pct=%d%% (samples=%d/%d)",
             avg_raw, adc_pin_v, battery_v, *pct, valid_samples, n);

    return ESP_OK;
}