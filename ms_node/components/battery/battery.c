#include "battery.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "battery";

static battery_cfg_t s_cfg;
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_has_cali = false;

static uint8_t pct_from_vbat_mv(uint32_t vbat_mv)
{
    // Simple linear approximation for now:
    // 4200mV -> 100%, 3300mV -> 0%
    const int v_full = 4200;
    const int v_empty = 3300;

    if ((int)vbat_mv <= v_empty) return 0;
    if ((int)vbat_mv >= v_full)  return 100;

    float pct = ((float)((int)vbat_mv - v_empty) * 100.0f) / (float)(v_full - v_empty);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
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

    // Try calibration (best accuracy)
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
        ESP_LOGW(TAG, "ADC calibration not available, will use rough conversion");
    }

    ESP_LOGI(TAG, "Battery ADC init: unit=%d ch=%d atten=%d R1=%lu R2=%lu samples=%u",
             (int)s_cfg.unit, (int)s_cfg.channel, (int)s_cfg.atten,
             (unsigned long)s_cfg.r1_ohm, (unsigned long)s_cfg.r2_ohm,
             (unsigned)s_cfg.samples);

    return ESP_OK;
}

esp_err_t battery_read(uint32_t *vadc_mv, uint32_t *vbat_mv, uint8_t *pct)
{
    if (!vadc_mv || !vbat_mv || !pct) return ESP_ERR_INVALID_ARG;
    if (!s_adc) return ESP_ERR_INVALID_STATE;

    const uint16_t n = (s_cfg.samples == 0) ? 1 : s_cfg.samples;

    uint32_t acc_mv = 0;

    // Read and average
    for (uint16_t i = 0; i < n; i++) {
        int raw = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc, s_cfg.channel, &raw));

        int mv = 0;

        if (s_has_cali) {
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_cali, raw, &mv));
        } else {
            // Rough fallback:
            // With 2.5dB attenuation, full-scale is roughly ~1500mV.
            // This is not perfect, but keeps you moving.
            mv = (int)((raw / 4095.0f) * 1500.0f);
        }

        acc_mv += (uint32_t)mv;
    }

    uint32_t v_adc = acc_mv / n;

    // VBAT = Vadc * (R1+R2) / R2
    uint64_t num = (uint64_t)v_adc * (uint64_t)(s_cfg.r1_ohm + s_cfg.r2_ohm);
    uint32_t v_bat = (uint32_t)(num / (uint64_t)s_cfg.r2_ohm);

    *vadc_mv = v_adc;
    *vbat_mv = v_bat;
    *pct = pct_from_vbat_mv(v_bat);

    return ESP_OK;
}