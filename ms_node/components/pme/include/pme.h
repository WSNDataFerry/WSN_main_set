#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PME_MODE_NORMAL = 0,
    PME_MODE_POWER_SAVE,
    PME_MODE_CRITICAL,
} pme_mode_t;

typedef struct {
    uint8_t normal_min_pct;       // >= this => NORMAL
    uint8_t power_save_min_pct;   // >= this => POWER_SAVE
    // < power_save_min_pct => CRITICAL
} pme_thresholds_t;

typedef struct {
    pme_thresholds_t th;

    // Fake battery config (kept for testing / fallback)
    uint8_t  fake_start_pct;
    uint8_t  fake_drop_per_tick;
    uint32_t fake_tick_ms;
} pme_config_t;

esp_err_t  pme_init(const pme_config_t *cfg);

/**
 * Fake tick (drops battery over time) - will do nothing once real battery is being fed
 */
void       pme_tick(void);

uint8_t    pme_get_batt_pct(void);
pme_mode_t pme_get_mode(void);

const char *pme_mode_to_str(pme_mode_t mode);

/**
 * Real battery input: set battery % from ADC mapping.
 * This disables fake ticking automatically.
 */
void       pme_set_batt_pct(uint8_t pct);

#ifdef __cplusplus
}
#endif