#include "metrics.h"
#include "battery.h" // Include Battery for voltage readings
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "neighbor_manager.h" // For Pareto ranking and centrality
#include "nvs.h"
#include "nvs_flash.h"
#include "pme.h" // Include PME for energy management
#include <math.h>
#include <string.h>

static const char *TAG = "METRICS";

static node_metrics_t current_metrics = {0};
// static adc_oneshot_unit_handle_t adc_handle = NULL; // Removed: Handled by
// battery.c
static bool metrics_initialized = false;
static uint64_t last_uptime_save = 0;
static uint64_t last_trust_save = 0;
// Mutex to protect metrics state
// We use a recursive mutex because metrics_update() calls other metrics_*
// functions
static SemaphoreHandle_t metrics_mutex = NULL;

// RTC memory for uptime persistence
#define RTC_UPTIME_MAGIC 0xABCD1234
typedef struct {
  uint32_t magic;
  uint64_t uptime_seconds;
} rtc_uptime_t;

// Trust computation state
static float hsr_ewma = 0.5f; // Initial neutral
static float pdr_ewma = 0.5f;
static float reputation_ewma = 0.5f;

// Link quality state
static float rssi_ewma = -70.0f; // Initial RSSI estimate
static float per_ewma = 0.1f;    // Initial PER estimate

// Smoothed STELLAR score to avoid re-election from brief metric dips
static float stellar_score_smoothed = 0.0f;
static bool stellar_score_smoothed_init = false;

// Global weights
float g_weight_battery = WEIGHT_BATTERY;
float g_weight_uptime = WEIGHT_UPTIME;
float g_weight_trust = WEIGHT_TRUST;
float g_weight_link_quality = WEIGHT_LINK_QUALITY;

void metrics_set_weights(float battery, float uptime, float trust,
                         float link_quality) {
  if (metrics_mutex)
    xSemaphoreTakeRecursive(metrics_mutex, portMAX_DELAY);

  g_weight_battery = battery;
  g_weight_uptime = uptime;
  g_weight_trust = trust;
  g_weight_link_quality = link_quality;
  ESP_LOGW(TAG, "Weights updated: Bat=%.2f, Up=%.2f, Trust=%.2f, LQ=%.2f",
           battery, uptime, trust, link_quality);

  if (metrics_mutex)
    xSemaphoreGiveRecursive(metrics_mutex);
}

float metrics_read_battery(void) {
  // Use PME to get the authoritative battery percentage
  uint8_t pct = pme_get_batt_pct();

  // When 0% (no battery / USB / PME not yet updated), treat as sufficient for
  // CH so we don't trigger re-election (matches clusterCreation "USB power"
  // behavior)
  if (pct == 0) {
    return 1.0f;
  }

  // Normalize to 0.0 - 1.0
  float battery_normalized = (float)pct / 100.0f;

  // Optional: Log if PME mode is critical
  pme_mode_t mode = pme_get_mode();
  if (mode == PME_MODE_CRITICAL) {
    ESP_LOGD(TAG, "PME Critical Mode detected (Battery: %d%%)", pct);
  }

  return battery_normalized;
}

uint64_t metrics_get_uptime(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("metrics", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    return 0; // First boot or NVS not available
  }

  uint64_t uptime = 0;
  size_t required_size = sizeof(uptime);
  err = nvs_get_blob(nvs_handle, "uptime", &uptime, &required_size);
  nvs_close(nvs_handle);

  if (err == ESP_OK) {
    return uptime;
  }

  return 0; // First boot
}

void metrics_persist_uptime(void) {
  // Lock handled by caller (metrics_update) usually, but to be safe:
  // (Assuming single writer for uptime mostly)

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("metrics", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to open NVS for uptime persistence");
    return;
  }

  err = nvs_set_blob(nvs_handle, "uptime", &current_metrics.uptime_seconds,
                     sizeof(current_metrics.uptime_seconds));
  if (err == ESP_OK) {
    nvs_commit(nvs_handle);
  } else {
    ESP_LOGW(TAG, "Failed to save uptime to NVS");
  }

  nvs_close(nvs_handle);
}

// Persist trust metrics to NVS so they survive panic reboots.
// Trust is the slowest-moving metric (EWMA) and most expensive to rebuild.
static void metrics_persist_trust(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("metrics", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to open NVS for trust persistence");
    return;
  }
  // Save trust as a float blob
  float trust_val = current_metrics.trust;
  err = nvs_set_blob(nvs_handle, "trust", &trust_val, sizeof(trust_val));
  if (err == ESP_OK)
    nvs_commit(nvs_handle);
  else
    ESP_LOGW(TAG, "Failed to save trust to NVS");
  nvs_close(nvs_handle);
}

// Restore trust from NVS. Returns 0.5 (neutral) if no saved value.
static float metrics_get_trust_nvs(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("metrics", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK)
    return 0.5f; // No saved value — neutral default
  float trust_val = 0.5f;
  size_t required_size = sizeof(trust_val);
  err = nvs_get_blob(nvs_handle, "trust", &trust_val, &required_size);
  nvs_close(nvs_handle);
  if (err == ESP_OK) {
    // Clamp to valid range in case of NVS corruption
    if (trust_val < 0.0f)
      trust_val = 0.0f;
    if (trust_val > 1.0f)
      trust_val = 1.0f;
    return trust_val;
  }
  return 0.5f;
}

void metrics_record_hmac_success(bool success) {
  if (metrics_mutex)
    xSemaphoreTakeRecursive(metrics_mutex, portMAX_DELAY);

  float val = success ? 1.0f : 0.0f;
  hsr_ewma = HSR_WEIGHT * val + (1.0f - HSR_WEIGHT) * hsr_ewma;
  ESP_LOGI(TAG, "[METRICS] HMAC Update: Success=%d, New HSR_EWMA=%.3f", success,
           hsr_ewma);

  if (metrics_mutex)
    xSemaphoreGiveRecursive(metrics_mutex);
}

void metrics_update_trust(float reputation) {
  if (metrics_mutex)
    xSemaphoreTakeRecursive(metrics_mutex, portMAX_DELAY);

  // Update Reputation EWMA
  reputation_ewma = REPUTATION_WEIGHT * reputation +
                    (1.0f - REPUTATION_WEIGHT) * reputation_ewma;

  // PDR derived from BLE PER (1.0 - per_ewma)
  float pdr = 1.0f - per_ewma;
  pdr_ewma = PDR_WEIGHT * pdr + (1.0f - PDR_WEIGHT) * pdr_ewma;

  ESP_LOGI(TAG,
           "[METRICS] Trust Update Input: Rep=%.3f, Current PDR=%.3f, Current "
           "HSR=%.3f",
           reputation, pdr, hsr_ewma);

  // Composite trust
  current_metrics.trust = (HSR_WEIGHT * hsr_ewma + PDR_WEIGHT * pdr_ewma +
                           REPUTATION_WEIGHT * reputation_ewma);

  // Clamp to [0, 1]
  if (current_metrics.trust < 0.0f)
    current_metrics.trust = 0.0f;
  if (current_metrics.trust > 1.0f)
    current_metrics.trust = 1.0f;

  if (metrics_mutex)
    xSemaphoreGiveRecursive(metrics_mutex);
}

void metrics_recompute_link_quality(void) {
  // Internal helper, expects mutex held by caller

  // Convert RSSI to quality (0-1): -100dBm = 0, -50dBm = 1
  // Note: This is a linear mapping of a logarithmic (dBm) scale — a deliberate
  // simplification for the research prototype. A log-to-linear conversion would
  // be more physically accurate but adds complexity without changing relative
  // ranking.
  float rssi_quality = (rssi_ewma + 100.0f) / 50.0f;
  if (rssi_quality < 0.0f)
    rssi_quality = 0.0f;
  if (rssi_quality > 1.0f)
    rssi_quality = 1.0f;

  // PER penalty: 0% PER = 1.0, 100% PER = 0.0
  float per_quality = 1.0f - per_ewma; // BLE-derived PER

  // Combined link quality: 70% RSSI quality + 30% packet success rate
  current_metrics.link_quality = 0.7f * rssi_quality + 0.3f * per_quality;

  // Clamp
  if (current_metrics.link_quality < 0.0f)
    current_metrics.link_quality = 0.0f;
  if (current_metrics.link_quality > 1.0f)
    current_metrics.link_quality = 1.0f;
}

void metrics_record_ble_reception(int successes, int failures) {
  if (metrics_mutex)
    xSemaphoreTakeRecursive(metrics_mutex, portMAX_DELAY);

  // Total events
  int total = successes + failures;
  if (total == 0) {
    if (metrics_mutex)
      xSemaphoreGiveRecursive(metrics_mutex);
    return;
  }

  // Current batch PER (Failures / Total)
  float batch_per = (float)failures / (float)total;

  // Update EWMA
  // New PER = Alpha * Batch_PER + (1-Alpha) * Old_PER
  per_ewma = PDR_EWMA_ALPHA * batch_per + (1.0f - PDR_EWMA_ALPHA) * per_ewma;

  ESP_LOGI(TAG,
           "[METRICS] BLE Reception: Success=%d, Fail=%d, BatchPER=%.2f, New "
           "PER_EWMA=%.3f",
           successes, failures, batch_per, per_ewma);

  metrics_recompute_link_quality();

  if (metrics_mutex)
    xSemaphoreGiveRecursive(metrics_mutex);
}

void metrics_update_rssi(float rssi) {
  if (metrics_mutex)
    xSemaphoreTakeRecursive(metrics_mutex, portMAX_DELAY);

  // EWMA for RSSI
  rssi_ewma = 0.1f * rssi + 0.9f * rssi_ewma;
  metrics_recompute_link_quality();

  if (metrics_mutex)
    xSemaphoreGiveRecursive(metrics_mutex);
}

void metrics_update_per(float success) {
  if (metrics_mutex)
    xSemaphoreTakeRecursive(metrics_mutex, portMAX_DELAY);

  // success: 1.0 for success, 0.0 for failure
  // PER = 1.0 - success_rate
  // We update PER EWMA directly. If success=1, PER input is 0.
  float per_input = 1.0f - success;
  per_ewma = PDR_EWMA_ALPHA * per_input + (1.0f - PDR_EWMA_ALPHA) * per_ewma;
  ESP_LOGI(TAG, "[METRICS] PER Update: Success=%.0f, New PER_EWMA=%.3f",
           success, per_ewma);
  metrics_recompute_link_quality();

  if (metrics_mutex)
    xSemaphoreGiveRecursive(metrics_mutex);
}

// Keep the old function for compatibility but implement using new ones
void metrics_update_link_quality(float rssi_ewma_val, float per) {
  if (metrics_mutex)
    xSemaphoreTakeRecursive(metrics_mutex, portMAX_DELAY);

  rssi_ewma = rssi_ewma_val;
  per_ewma = PDR_EWMA_ALPHA * per + (1.0f - PDR_EWMA_ALPHA) * per_ewma;
  metrics_recompute_link_quality();

  if (metrics_mutex)
    xSemaphoreGiveRecursive(metrics_mutex);
}

float metrics_compute_score(const node_metrics_t *metrics) {
  // Pure function, no state modification, safe to call without lock
  // (provided input 'metrics' is consistent)
  // Legacy score used when USE_STELLAR_ALGORITHM=0.
  float uptime_norm =
      (float)metrics->uptime_seconds / (UPTIME_MAX_DAYS * 86400.0f);
  if (uptime_norm > 1.0f)
    uptime_norm = 1.0f; // Clamp: prevents score > 1.0 after UPTIME_MAX_DAYS
  return (g_weight_battery * metrics->battery + g_weight_uptime * uptime_norm +
          g_weight_trust * metrics->trust +
          g_weight_link_quality * metrics->link_quality);
}

// Forward decl
void metrics_update_variance_estimates(void);
void metrics_compute_entropy_confidence(void);
void metrics_update_stellar_weights(void);
float metrics_compute_stellar_score(const node_metrics_t *metrics,
                                    int pareto_rank, float centrality);

void metrics_update(void) {
  if (!metrics_initialized) {
    return;
  }

  if (metrics_mutex)
    xSemaphoreTakeRecursive(metrics_mutex, portMAX_DELAY);

  // Update battery
  current_metrics.battery = metrics_read_battery();

  // Update uptime (increment from RTC)
  uint64_t base_uptime = metrics_get_uptime();
  uint64_t boot_time = esp_timer_get_time() / 1000000; // Convert to seconds
  current_metrics.uptime_seconds = base_uptime + boot_time;

  // Trust and link quality are updated by neighbor_manager
  // Compute composite score
  current_metrics.composite_score = metrics_compute_score(&current_metrics);

  // Persist uptime periodically (every minute)
  uint64_t now_ms = esp_timer_get_time() / 1000;
  if (now_ms - last_uptime_save > 60000) {
    metrics_persist_uptime();
    last_uptime_save = now_ms;
  }

  // Persist trust periodically (every 60s) — survives panic reboots
  if (now_ms - last_trust_save > 60000) {
    metrics_persist_trust();
    last_trust_save = now_ms;
  }

#if USE_STELLAR_ALGORITHM
  // --- STELLAR UPDATE LOOP ---
  // 3. Update weights using Lyapunov stability (Self-learning)
  // This internally calls variance and entropy updates
  metrics_update_stellar_weights();

  // 4. Compute Pareto rank: count how many neighbors this node dominates
  //    A node dominates neighbor n if it is >= on ALL metrics.
  {
    neighbor_entry_t neighbors[MAX_NEIGHBORS];
    size_t n_count = neighbor_manager_get_all(neighbors, MAX_NEIGHBORS);
    int dominated = 0;
    for (size_t i = 0; i < n_count; i++) {
      bool dominates =
          (current_metrics.battery >= neighbors[i].battery) &&
          (current_metrics.trust >= neighbors[i].trust) &&
          (current_metrics.link_quality >= neighbors[i].link_quality);
      if (dominates)
        dominated++;
    }
    current_metrics.pareto_rank = dominated;

    // 5. Compute centrality: fraction of max possible neighbors seen.
    //    This is a connectivity-based centrality (not RSSI-variance based).
    //    It differs from the RSSI-variance centrality used in election.c which
    //    measures spatial centrality. This version measures network density.
    //    Default 0.0 on boot (unknown) — will rise as neighbors are discovered.
    float centrality =
        (n_count > 0) ? ((float)n_count / (float)MAX_NEIGHBORS)
                      : 0.0f; // Unknown centrality before any neighbors found
    if (centrality > 1.0f)
      centrality = 1.0f;

    ESP_LOGI(TAG, "[STELLAR] Pareto rank=%d/%zu, Centrality=%.3f", dominated,
             n_count, centrality);

    // 6. Compute final STELLAR score with live Pareto rank and centrality
    float raw_stellar = metrics_compute_stellar_score(
        &current_metrics, current_metrics.pareto_rank, centrality);

    // Smooth score so brief dips don't trigger re-election
    if (!stellar_score_smoothed_init) {
      stellar_score_smoothed = raw_stellar;
      stellar_score_smoothed_init = true;
    } else {
      stellar_score_smoothed =
          STELLAR_SCORE_EWMA_ALPHA * raw_stellar +
          (1.0f - STELLAR_SCORE_EWMA_ALPHA) * stellar_score_smoothed;
    }
    current_metrics.stellar_score = stellar_score_smoothed;
    current_metrics.composite_score = stellar_score_smoothed;
  }
#else
  // Non-STELLAR (Legacy) Score
  current_metrics.composite_score = metrics_compute_score(&current_metrics);
#endif

  if (metrics_mutex)
    xSemaphoreGiveRecursive(metrics_mutex);
}

node_metrics_t metrics_get_current(void) {
  node_metrics_t ret;
  if (metrics_mutex)
    xSemaphoreTakeRecursive(metrics_mutex, portMAX_DELAY);
  ret = current_metrics;
  if (metrics_mutex)
    xSemaphoreGiveRecursive(metrics_mutex);
  return ret;
}

void metrics_init(void) {
  // Create recursive mutex for thread safety
  metrics_mutex = xSemaphoreCreateRecursiveMutex();
  if (metrics_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create metrics mutex!");
  }

  // ADC initialization is handled by ms_node.c (battery_init)
  // We just use pme_get_batt_pct() in metrics_read_battery()

  // Load uptime from NVS
  current_metrics.uptime_seconds = metrics_get_uptime();
  ESP_LOGI(TAG, "Loaded uptime: %llu seconds", current_metrics.uptime_seconds);

  // Initialize metrics
  current_metrics.battery = metrics_read_battery();

#if defined(DEMO_MODE) && DEMO_MODE == 1
  // Simulation: Vary initial Trust and Link Quality based on Node ID
  // Node 1: T=0.9, L=0.9
  // Node 2: T=0.8, L=0.8
  int id_offset = (g_node_id % 5);
  current_metrics.trust = 0.9f - (id_offset * 0.1f);
  current_metrics.link_quality = 0.9f - (id_offset * 0.05f);
  ESP_LOGW(TAG, "[DEMO] Initialized with SIMULATED metrics: T=%.2f, L=%.2f",
           current_metrics.trust, current_metrics.link_quality);
#else
  // Restore trust from NVS (survives panic reboots — prevents Ψ score drop)
  current_metrics.trust = metrics_get_trust_nvs();
  current_metrics.link_quality = 0.5f; // Link quality must be re-measured
  ESP_LOGI(TAG, "Restored trust from NVS: %.3f", current_metrics.trust);
#endif

  // Initialize STELLAR extensions
  current_metrics.stellar_score = 0.0f;
  current_metrics.battery_variance = 0.01f;
  current_metrics.trust_variance = 0.01f;
  current_metrics.linkq_variance = 0.01f;
  current_metrics.pareto_rank = 0;
  for (int i = 0; i < 4; i++) {
    current_metrics.entropy_confidence[i] = 0.25f;
  }

  metrics_initialized = true;

  ESP_LOGI(TAG, "Metrics system initialized with STELLAR extensions");
}

// ============================================
// STELLAR Algorithm Implementation
// Stochastic Trust-Enhanced Lyapunov-stable
// Leader Allocation and Ranking
// ============================================

// Lyapunov-stable adaptive weight vector
static stellar_weights_t g_stellar_weights = {
    .weights = {WEIGHT_BATTERY, WEIGHT_UPTIME, WEIGHT_TRUST,
                WEIGHT_LINK_QUALITY},
    .target_weights = {WEIGHT_BATTERY, WEIGHT_UPTIME, WEIGHT_TRUST,
                       WEIGHT_LINK_QUALITY},
    .lyapunov_value = 0.0f,
    .converged = false};

// Variance tracking for entropy computation (EWMA)
static float battery_variance_ewma = 0.01f;
static float trust_variance_ewma = 0.01f;
static float linkq_variance_ewma = 0.01f;
static float prev_battery = 0.5f;
static float prev_trust = 0.5f;
static float prev_linkq = 0.5f;

// Helper: Compute differential entropy H(m) = 0.5 * ln(2πe * σ²)
static float compute_differential_entropy(float variance) {
  const float TWO_PI_E = 17.0794684f; // 2πe ≈ 17.079
  if (variance < 1e-6f)
    variance = 1e-6f; // Prevent log(0)
  return 0.5f * logf(TWO_PI_E * variance);
}

// Helper: Compute entropy-based confidence for all metrics
void metrics_compute_entropy_confidence(void) {
  float entropies[4];
  float sum_exp = 0.0f;

  // Compute differential entropies for each metric
  entropies[0] = compute_differential_entropy(battery_variance_ewma);
  entropies[1] = compute_differential_entropy(
      0.001f); // Uptime is deterministic (near-zero variance)
  entropies[2] = compute_differential_entropy(trust_variance_ewma);
  entropies[3] = compute_differential_entropy(linkq_variance_ewma);

  // Softmax with negative entropy: c_i = exp(-γH_i) / Σexp(-γH_j)
  // High entropy → low exp(-γH) → low confidence
  for (int i = 0; i < 4; i++) {
    float exp_val = expf(-ENTROPY_GAMMA * entropies[i]);
    sum_exp += exp_val;
    current_metrics.entropy_confidence[i] = exp_val;
  }

  // Normalize to probability simplex (sum to 1)
  if (sum_exp > 0.0f) {
    for (int i = 0; i < 4; i++) {
      current_metrics.entropy_confidence[i] /= sum_exp;
    }
  }

  ESP_LOGD(TAG, "[STELLAR] Entropy confidence: B=%.3f U=%.3f T=%.3f L=%.3f",
           current_metrics.entropy_confidence[0],
           current_metrics.entropy_confidence[1],
           current_metrics.entropy_confidence[2],
           current_metrics.entropy_confidence[3]);
}

// Helper: Update variance estimates using EWMA for uncertainty quantification
void metrics_update_variance_estimates(void) {
  float diff;

  // Battery variance: σ²_battery ← α·(b_t - b_{t-1})² + (1-α)·σ²_battery
  diff = current_metrics.battery - prev_battery;
  battery_variance_ewma = EWMA_VARIANCE_ALPHA * (diff * diff) +
                          (1.0f - EWMA_VARIANCE_ALPHA) * battery_variance_ewma;
  prev_battery = current_metrics.battery;

  // Trust variance
  diff = current_metrics.trust - prev_trust;
  trust_variance_ewma = EWMA_VARIANCE_ALPHA * (diff * diff) +
                        (1.0f - EWMA_VARIANCE_ALPHA) * trust_variance_ewma;
  prev_trust = current_metrics.trust;

  // Link quality variance
  diff = current_metrics.link_quality - prev_linkq;
  linkq_variance_ewma = EWMA_VARIANCE_ALPHA * (diff * diff) +
                        (1.0f - EWMA_VARIANCE_ALPHA) * linkq_variance_ewma;
  prev_linkq = current_metrics.link_quality;

  // Store in metrics struct for external access
  current_metrics.battery_variance = battery_variance_ewma;
  current_metrics.trust_variance = trust_variance_ewma;
  current_metrics.linkq_variance = linkq_variance_ewma;
}

// Helper: Project weight vector onto probability simplex
static void project_onto_simplex(float *weights, int n) {
  float sum = 0.0f;

  // First, clamp to minimum weight
  for (int i = 0; i < n; i++) {
    if (weights[i] < MIN_WEIGHT_VALUE) {
      weights[i] = MIN_WEIGHT_VALUE;
    }
    sum += weights[i];
  }

  // Normalize to sum to 1
  if (sum > 0.0f) {
    for (int i = 0; i < n; i++) {
      weights[i] /= sum;
    }
  }
}

// EXPOSED FUNCTION: Needs protection!
void metrics_update_stellar_weights(void) {
  if (metrics_mutex)
    xSemaphoreTakeRecursive(metrics_mutex, portMAX_DELAY);

  // Update variance estimates first
  metrics_update_variance_estimates();

  // Compute entropy-based confidence
  metrics_compute_entropy_confidence();

  // Compute entropy-derived target weights w*(t)
  // w*_i = base_weight_i × (1 + 0.5 × (confidence_i - 0.25))
  float base_weights[4] = {WEIGHT_BATTERY, WEIGHT_UPTIME, WEIGHT_TRUST,
                           WEIGHT_LINK_QUALITY};
  float sum = 0.0f;

  for (int i = 0; i < 4; i++) {
    // Higher confidence → weight boost, lower confidence → weight reduction
    float confidence_adjustment = current_metrics.entropy_confidence[i] - 0.25f;
    g_stellar_weights.target_weights[i] =
        base_weights[i] * (1.0f + 0.5f * confidence_adjustment);
    if (g_stellar_weights.target_weights[i] < MIN_WEIGHT_VALUE) {
      g_stellar_weights.target_weights[i] = MIN_WEIGHT_VALUE;
    }
    sum += g_stellar_weights.target_weights[i];
  }

  // Normalize target weights
  for (int i = 0; i < 4; i++) {
    g_stellar_weights.target_weights[i] /= sum;
  }

  // Lyapunov gradient descent update with two-term gradient:
  // ∂V/∂w_i = (w_i - w*_i) + β·(w_i - w_eq,i)
  // Where:
  //   w*_i  = entropy-derived target weight (pulls toward high-confidence
  //   metrics) w_eq,i = equilibrium/initial weight (regularisation: prevents
  //   drift from prior) β     = regularisation strength (LYAPUNOV_BETA)
  static const float w_eq[4] = {W_EQ_BATTERY, W_EQ_UPTIME, W_EQ_TRUST,
                                W_EQ_LINK_QUALITY};
  float grad_norm_sq = 0.0f;
  for (int i = 0; i < 4; i++) {
    float diff_target =
        g_stellar_weights.weights[i] - g_stellar_weights.target_weights[i];
    float diff_eq = g_stellar_weights.weights[i] - w_eq[i];
    // Two-term gradient: convergence toward target + regularisation toward
    // equilibrium
    float gradient = diff_target + LYAPUNOV_BETA * diff_eq;

    // Gradient descent step
    g_stellar_weights.weights[i] -= LYAPUNOV_ETA * gradient;
    grad_norm_sq += gradient * gradient;
  }

  // Project onto probability simplex
  project_onto_simplex(g_stellar_weights.weights, 4);

  // Compute Lyapunov function value V(w,t) = ½‖w - w*‖² + λ‖∇J‖²
  g_stellar_weights.lyapunov_value = 0.0f;
  for (int i = 0; i < 4; i++) {
    float diff =
        g_stellar_weights.weights[i] - g_stellar_weights.target_weights[i];
    g_stellar_weights.lyapunov_value += 0.5f * diff * diff;
  }
  g_stellar_weights.lyapunov_value += LYAPUNOV_LAMBDA * grad_norm_sq;

  // Check convergence: V < ε
  g_stellar_weights.converged =
      (g_stellar_weights.lyapunov_value < CONVERGENCE_THRESHOLD);

  ESP_LOGI(TAG,
           "[STELLAR] Lyapunov weights: B=%.3f U=%.3f T=%.3f L=%.3f, V=%.5f, "
           "Conv=%d",
           g_stellar_weights.weights[0], g_stellar_weights.weights[1],
           g_stellar_weights.weights[2], g_stellar_weights.weights[3],
           g_stellar_weights.lyapunov_value, g_stellar_weights.converged);

  if (metrics_mutex)
    xSemaphoreGiveRecursive(metrics_mutex);
}

stellar_weights_t metrics_get_stellar_weights(void) {
  stellar_weights_t ret;
  if (metrics_mutex)
    xSemaphoreTakeRecursive(metrics_mutex, portMAX_DELAY);
  ret = g_stellar_weights;
  if (metrics_mutex)
    xSemaphoreGiveRecursive(metrics_mutex);
  return ret;
}

// ============================================
// Non-Linear Utility Functions
// Each function has specific mathematical shape
// ============================================

/**
 * @brief Battery utility: Concave exponential
 * φ_battery(b) = (1 - e^(-λb)) / (1 - e^(-λ))
 *
 * Properties:
 * - Concave: diminishing marginal utility
 * - φ(0) = 0, φ(1) = 1
 * - Penalizes low battery heavily
 */
float stellar_utility_battery(float battery) {
  float lambda = UTILITY_LAMBDA_B;
  float numerator = 1.0f - expf(-lambda * battery);
  float denominator = 1.0f - expf(-lambda);
  if (denominator < 1e-6f)
    denominator = 1e-6f;
  return numerator / denominator;
}

/**
 * @brief Uptime utility: Saturating tanh
 * φ_uptime(u) = tanh(λ × u)
 *
 * Properties:
 * - Saturating: old nodes don't dominate
 * - φ(0) = 0, φ(∞) → 1
 * - Prevents "zombie leader" syndrome
 */
float stellar_utility_uptime(float uptime_normalized) {
  return tanhf(UTILITY_LAMBDA_U * uptime_normalized);
}

/**
 * @brief Trust utility: Smooth-step (Hermite polynomial)
 * φ_trust(t) = t² × (3 - 2t)
 *
 * Properties:
 * - S-shaped curve
 * - Flat at extremes, steep in middle
 * - φ(0) = 0, φ(0.5) = 0.5, φ(1) = 1
 * - Emphasizes trust differentiation in mid-range
 */
float stellar_utility_trust(float trust) {
  if (trust < 0.0f)
    trust = 0.0f;
  if (trust > 1.0f)
    trust = 1.0f;
  return trust * trust * (3.0f - 2.0f * trust);
}

/**
 * @brief Link quality utility: Power utility with diminishing returns
 * φ_linkq(l) = l^(1/γ)
 *
 * Properties:
 * - Concave for γ > 1
 * - φ(0) = 0, φ(1) = 1
 * - Higher sensitivity to link quality changes at low values
 */
float stellar_utility_linkq(float link_quality) {
  if (link_quality < 0.0f)
    link_quality = 0.0f;
  if (link_quality > 1.0f)
    link_quality = 1.0f;
  return powf(link_quality, 1.0f / UTILITY_GAMMA_L);
}

/**
 * @brief Compute STELLAR score Ψ(n) for a candidate node
 *
 * Ψ(n) = Σ w̃_i(t) × φ_i(m̃_i) × κ(n) + ρ(n)
 *
 * Where:
 * - w̃_i(t) = Lyapunov-adapted weight for metric i
 * - φ_i = Non-linear utility function for metric i
 * - κ(n) = Centrality factor (penalizes edge nodes)
 * - ρ(n) = Pareto dominance bonus
 */
float metrics_compute_stellar_score(const node_metrics_t *metrics,
                                    int pareto_rank, float centrality) {
  // Get Lyapunov-adapted weights
  stellar_weights_t sw = g_stellar_weights;

  // Normalize uptime to [0, 1]
  float uptime_norm =
      (float)metrics->uptime_seconds / (UPTIME_MAX_DAYS * 86400.0f);
  if (uptime_norm > 1.0f)
    uptime_norm = 1.0f;

  // Apply non-linear utility functions
  float u_battery = stellar_utility_battery(metrics->battery);
  float u_uptime = stellar_utility_uptime(uptime_norm);
  float u_trust = stellar_utility_trust(metrics->trust);
  float u_linkq = stellar_utility_linkq(metrics->link_quality);

  // Weighted sum with Lyapunov-adapted weights
  float base_score = sw.weights[0] * u_battery + sw.weights[1] * u_uptime +
                     sw.weights[2] * u_trust + sw.weights[3] * u_linkq;

  // Pareto dominance bonus: ρ(n) = δ × (pareto_rank / max_rank)
  // Note: This Pareto rank is computed continuously in metrics_update() using
  // raw metric values (battery, trust, linkq). The election.c Pareto rank uses
  // utility-transformed values — different purposes, both valid.
  float dominance_bonus =
      PARETO_DELTA * ((float)pareto_rank / (float)MAX_NEIGHBORS);

  // Centrality factor: κ(n) = 1 / (1 + ε × (1 - centrality))
  // Higher centrality = closer to 1, lower penalty
  float centrality_factor =
      1.0f / (1.0f + CENTRALITY_EPSILON * (1.0f - centrality));

  // Final STELLAR score
  float stellar_score = base_score * centrality_factor + dominance_bonus;

  ESP_LOGI(TAG,
           "[STELLAR] Score components: u_B=%.3f u_U=%.3f u_T=%.3f u_L=%.3f, "
           "base=%.3f, κ=%.3f, ρ=%.3f, Ψ=%.4f",
           u_battery, u_uptime, u_trust, u_linkq, base_score, centrality_factor,
           dominance_bonus, stellar_score);

  return stellar_score;
}
