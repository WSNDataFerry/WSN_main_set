#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "hal/adc_types.h"

typedef struct {
    adc_unit_t unit;         // ADC_UNIT_1
    adc_channel_t channel;   // e.g. ADC_CHANNEL_0 (GPIO1)
    adc_atten_t atten;       // ADC_ATTEN_DB_2_5 for ~1.3V
    uint32_t r1_ohm;         // top resistor (battery+ -> sense)
    uint32_t r2_ohm;         // bottom resistor (sense -> gnd)
    uint16_t samples;        // averaging samples (e.g. 32)
} battery_cfg_t;

esp_err_t battery_init(const battery_cfg_t *cfg);

/**
 * Reads battery sense and returns:
 *  - vadc_mv: voltage at ADC pin (BAT_SENSE) in mV
 *  - vbat_mv: calculated battery voltage in mV
 *  - pct: percentage 0-100 (simple mapping)
 */
esp_err_t battery_read(uint32_t *vadc_mv, uint32_t *vbat_mv, uint8_t *pct);