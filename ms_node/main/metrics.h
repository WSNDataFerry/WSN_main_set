#ifndef METRICS_H
#define METRICS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  float battery;           // 0.0 - 1.0
  uint64_t uptime_seconds; // Persistent across sleep
  float trust;             // 0.0 - 1.0
  float link_quality;      // 0.0 - 1.0
  float composite_score;   // Legacy weighted sum (for backward compatibility)

  // ============================================
  // STELLAR Algorithm Extensions
  // ============================================
  float stellar_score;         // STELLAR score Ψ(n)
  float battery_variance;      // σ²_battery (uncertainty)
  float trust_variance;        // σ²_trust (uncertainty)
  float linkq_variance;        // σ²_linkq (uncertainty)
  float entropy_confidence[4]; // c_i for each metric (battery, uptime, trust,
                               // linkq)
  int pareto_rank;             // Number of nodes this node dominates
} node_metrics_t;

// Lyapunov-stable adaptive weight vector for STELLAR
typedef struct {
  float weights[4]; // Current adaptive weights [battery, uptime, trust, linkq]
  float target_weights[4]; // Entropy-derived optimal weights w*(t)
  float lyapunov_value;    // V(w, t) - Lyapunov candidate function value
  bool converged;          // True if weights have converged (V < ε)
} stellar_weights_t;

// Global weights (configurable at runtime) - Legacy support
extern float g_weight_battery;
extern float g_weight_uptime;
extern float g_weight_trust;
extern float g_weight_link_quality;

/**
 * @brief Set metrics weights at runtime
 */
void metrics_set_weights(float battery, float uptime, float trust,
                         float link_quality);

/**
 * @brief Initialize metrics collection system
 */
void metrics_init(void);

/**
 * @brief Update all metrics
 */
void metrics_update(void);

/**
 * @brief Get current node metrics
 */
node_metrics_t metrics_get_current(void);

/**
 * @brief Compute composite election score
 * @param metrics Node metrics
 * @return Composite score
 */
float metrics_compute_score(const node_metrics_t *metrics);

/**
 * @brief Update trust metric based on Reputation (passive)
 * @param reputation Neighbor reputation (0.0-1.0)
 * Note: HSR and PDR are now calculated internally from BLE stats
 */
void metrics_update_trust(float reputation);

/**
 * @brief Record HMAC verification result for HSR calculation
 * @param success True if HMAC was valid
 */
void metrics_record_hmac_success(bool success);

/**
 * @brief Record BLE reception stats (success vs missed)
 * @param successes Number of packets received
 * @param failures Number of packets missed (based on sequence number gaps)
 */
void metrics_record_ble_reception(int successes, int failures);

/**
 * @brief Update link quality from RSSI and PER
 * @param rssi_ewma RSSI exponential weighted moving average
 * @param per Packet error rate (0.0-1.0)
 */
void metrics_update_link_quality(float rssi_ewma, float per);
void metrics_update_rssi(float rssi);
void metrics_update_per(float success);

/**
 * @brief Record BLE packet reception stats
 * @param received Number of packets received (usually 1)
 * @param missed Number of packets missed (based on sequence number gaps)
 */
void metrics_record_ble_reception(int received, int missed);

/**
 * @brief Read battery percentage from ADC
 * @return Battery level (0.0-1.0)
 */
float metrics_read_battery(void);

/**
 * @brief Get uptime from RTC (persistent)
 * @return Uptime in seconds
 */
uint64_t metrics_get_uptime(void);

/**
 * @brief Persist uptime to RTC memory
 */
void metrics_persist_uptime(void);

// ============================================
// STELLAR Algorithm Functions
// ============================================

/**
 * @brief Get current Lyapunov-adapted weight vector
 * @return stellar_weights_t containing adaptive weights and convergence status
 */
stellar_weights_t metrics_get_stellar_weights(void);

/**
 * @brief Update weight vector using Lyapunov gradient descent
 * Implements: dw/dt = -η∇V(w,t) - β(w - w_eq) with simplex projection
 */
void metrics_update_stellar_weights(void);

/**
 * @brief Compute entropy-based confidence for all metrics
 * Uses differential entropy H(m) = 0.5 * ln(2πe * σ²)
 */
void metrics_compute_entropy_confidence(void);

/**
 * @brief Update variance estimates for uncertainty quantification
 */
void metrics_update_variance_estimates(void);

/**
 * @brief Compute STELLAR score Ψ(n) for a candidate
 * @param metrics Node metrics
 * @param pareto_rank Number of nodes this candidate dominates
 * @param centrality Network centrality factor (0-1)
 * @return STELLAR score Ψ(n)
 */
float metrics_compute_stellar_score(const node_metrics_t *metrics,
                                    int pareto_rank, float centrality);

/**
 * @brief Battery utility function: concave exponential
 * φ_battery(b) = (1 - e^(-λb)) / (1 - e^(-λ))
 */
float stellar_utility_battery(float battery);

/**
 * @brief Uptime utility function: saturating tanh
 * φ_uptime(u) = tanh(λ * u / u_max)
 */
float stellar_utility_uptime(float uptime_normalized);

/**
 * @brief Trust utility function: smooth-step
 * φ_trust(t) = t² * (3 - 2t)
 */
float stellar_utility_trust(float trust);

// ============================================
// Sensor Data Payload
// ============================================
#define SENSOR_PAYLOAD_FLAG_SENSORS_REAL 0x01  // At least one sensor read succeeded
#define SENSOR_PAYLOAD_FLAG_BATTERY_REAL 0x02   // Battery ADC read succeeded

typedef struct {
  uint32_t node_id;
  float temp_c;
  float hum_pct;
  uint32_t pressure_hpa;
  uint16_t eco2_ppm;
  uint16_t tvoc_ppb;
  uint16_t aqi;
  float audio_rms;
  float mag_x;
  float mag_y;
  float mag_z;
  uint64_t timestamp_ms; // Upgraded to 64-bit for long uptimes
  uint32_t seq_num;      // Monotonic sequence number for PDR
  uint8_t mac_addr[6];   // Source MAC for provenance
  uint8_t flags;         // SENSOR_PAYLOAD_FLAG_*: real vs dummy data
} sensor_payload_t;

/**
 * @brief Set the latest sensor data payload
 */
void metrics_set_sensor_data(const sensor_payload_t *data);

/**
 * @brief Get the latest sensor data payload
 */
void metrics_get_sensor_data(sensor_payload_t *data);

/**
 * @brief Link quality utility function: power utility
 * φ_linkq(l) = l^(1/γ)
 */
float stellar_utility_linkq(float link_quality);

#endif // METRICS_H
