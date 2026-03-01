#include "pme.h"

#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "pme";

static pme_config_t s_cfg;
static bool s_inited = false;

/* When true, pme_tick() will simulate battery dropping.
   Once you call pme_set_batt_pct(), this becomes false and pme_tick() stops changing state. */
static bool s_use_fake = true;

static uint8_t s_batt_pct = 100;
static pme_mode_t s_mode = PME_MODE_NORMAL;

static uint64_t s_last_tick_ms = 0;

static pme_mode_t compute_mode(uint8_t pct)
{
    if (pct >= s_cfg.th.normal_min_pct) {
        return PME_MODE_NORMAL;
    }
    if (pct >= s_cfg.th.power_save_min_pct) {
        return PME_MODE_POWER_SAVE;
    }
    return PME_MODE_CRITICAL;
}

esp_err_t pme_init(const pme_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    s_cfg = *cfg;

    if (s_cfg.fake_start_pct > 100) s_cfg.fake_start_pct = 100;
    if (s_cfg.fake_drop_per_tick == 0) s_cfg.fake_drop_per_tick = 1;
    if (s_cfg.fake_tick_ms == 0) s_cfg.fake_tick_ms = 5000;

    s_batt_pct = s_cfg.fake_start_pct;
    s_mode = compute_mode(s_batt_pct);

    s_last_tick_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    s_use_fake = true;     // start in fake mode until real batt is fed
    s_inited = true;

    ESP_LOGI(TAG, "PME init batt=%u%% (fake enabled tick=%ums drop=%u%%)",
             s_batt_pct, (unsigned)s_cfg.fake_tick_ms, s_cfg.fake_drop_per_tick);

    return ESP_OK;
}

void pme_tick(void)
{
    if (!s_inited) return;
    if (!s_use_fake) return;   // real battery is driving us now

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if ((now_ms - s_last_tick_ms) < s_cfg.fake_tick_ms) {
        return;
    }

    s_last_tick_ms = now_ms;

    if (s_batt_pct > 0) {
        if (s_batt_pct > s_cfg.fake_drop_per_tick) {
            s_batt_pct = (uint8_t)(s_batt_pct - s_cfg.fake_drop_per_tick);
        } else {
            s_batt_pct = 0;
        }
    }

    s_mode = compute_mode(s_batt_pct);
}

uint8_t pme_get_batt_pct(void)
{
    return s_batt_pct;
}

pme_mode_t pme_get_mode(void)
{
    return s_mode;
}

const char *pme_mode_to_str(pme_mode_t mode)
{
    switch (mode) {
        case PME_MODE_NORMAL: return "NORMAL";
        case PME_MODE_POWER_SAVE: return "POWER_SAVE";
        case PME_MODE_CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

void pme_set_batt_pct(uint8_t pct)
{
    if (!s_inited) return;

    if (pct > 100) pct = 100;

    s_use_fake = false;          // IMPORTANT: disable fake ticking now
    s_batt_pct = pct;
    s_mode = compute_mode(s_batt_pct);
}