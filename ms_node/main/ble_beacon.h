#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "pme.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_beacon_init(void);
void ble_beacon_update(uint8_t batt_pct, pme_mode_t mode);

#ifdef __cplusplus
}
#endif
