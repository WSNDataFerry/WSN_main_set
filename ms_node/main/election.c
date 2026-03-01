#include "election.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "metrics.h"
#include "neighbor_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

static const char *TAG = "ELECTION";

static uint64_t election_window_start = 0;
static SemaphoreHandle_t s_election_mutex = NULL;

// ============================================
// STELLAR Algorithm Data Structures
// ============================================

// Extended candidate structure for STELLAR election
typedef struct {
  uint32_t node_id;
  float raw_metrics[4];    // [battery, uptime_norm, trust, linkq]
  float utility_values[4]; // After utility transformation φ_i(m_i)
  float stellar_score;     // Ψ(n) - final STELLAR score
  int pareto_rank;         // Number of nodes this candidate dominates
  float centrality;        // Network centrality factor κ(n)
  bool is_self;
  bool on_pareto_frontier; // True if non-dominated
} stellar_candidate_t;

// Legacy candidate structure (for backward compatibility)
typedef struct {
  uint32_t node_id;
  float score;
  float link_quality;
  float battery;
  float trust;
  bool is_self;
} candidate_t;

// ============================================
// Legacy Election (for USE_STELLAR_ALGORITHM = 0)
// ============================================

static int compare_candidates(const void *a, const void *b) {
  const candidate_t *ca = (const candidate_t *)a;
  const candidate_t *cb = (const candidate_t *)b;

  // Primary: score (descending)
  if (ca->score > cb->score)
    return -1;
  if (ca->score < cb->score)
    return 1;

  // Tie-break 1: link quality (descending)
  if (ca->link_quality > cb->link_quality)
    return -1;
  if (ca->link_quality < cb->link_quality)
    return 1;

  // Tie-break 2: battery (descending)
  if (ca->battery > cb->battery)
    return -1;
  if (ca->battery < cb->battery)
    return 1;

  // Tie-break 3: trust (descending)
  if (ca->trust > cb->trust)
    return -1;
  if (ca->trust < cb->trust)
    return 1;

  // Tie-break 4: lower node_id (ascending)
  if (ca->node_id < cb->node_id)
    return -1;
  if (ca->node_id > cb->node_id)
    return 1;

  return 0;
}

// ============================================
// STELLAR Algorithm: Pareto Frontier
// ============================================

/**
 * @brief Check if candidate a Pareto-dominates candidate b
 * a ≻ b iff ∀i: u_i(a) ≥ u_i(b) AND ∃j: u_j(a) > u_j(b)
 */
static bool pareto_dominates(const stellar_candidate_t *a,
                             const stellar_candidate_t *b) {
  bool at_least_one_greater = false;

  for (int i = 0; i < 4; i++) {
    if (a->utility_values[i] < b->utility_values[i]) {
      return false; // a is worse in at least one dimension
    }
    if (a->utility_values[i] > b->utility_values[i]) {
      at_least_one_greater = true;
    }
  }

  return at_least_one_greater; // a dominates b
}

/**
 * @brief Extract Pareto frontier and compute dominance ranks
 * P = {n ∈ N : ¬∃n' ∈ N such that n' ≻ n}
 */
static void compute_pareto_frontier(stellar_candidate_t *candidates,
                                    size_t count) {
  // Compute dominance count for each candidate
  for (size_t i = 0; i < count; i++) {
    candidates[i].pareto_rank = 0;
    candidates[i].on_pareto_frontier = true;

    for (size_t j = 0; j < count; j++) {
      if (i == j)
        continue;

      if (pareto_dominates(&candidates[i], &candidates[j])) {
        candidates[i].pareto_rank++; // i dominates one more node
      }
      if (pareto_dominates(&candidates[j], &candidates[i])) {
        candidates[i].on_pareto_frontier = false; // i is dominated
      }
    }
  }
}

// ============================================
// STELLAR Algorithm: Nash Bargaining Solution
// ============================================

/**
 * @brief Nash Bargaining Selection from Pareto frontier
 * n* = argmax_{n ∈ P} ∏ᵢ (u_i(n) - d_i)^α_i
 *
 * Using log-sum for numerical stability:
 * n* = argmax_{n ∈ P} Σᵢ α_i × log(u_i(n) - d_i)
 */
static uint32_t nash_bargaining_selection(stellar_candidate_t *candidates,
                                          size_t count) {
  float disagreement[4] = {DISAGREE_BATTERY, DISAGREE_UPTIME, DISAGREE_TRUST,
                           DISAGREE_LINKQ};
  stellar_weights_t sw = metrics_get_stellar_weights();

  float max_nash_product = -1e9f;
  uint32_t winner = 0;
  int winner_idx = -1;

  for (size_t i = 0; i < count; i++) {
    // Nash Bargaining is applied ONLY to Pareto-optimal candidates.
    // Non-Pareto candidates are dominated in at least one dimension and should
    // not be CH. This is the correct mathematical formulation of NBS:
    //   n* = argmax_{n ∈ P} Σ_i α_i × log(u_i(n) - d_i)
    // where P is the Pareto frontier.
    if (!candidates[i].on_pareto_frontier)
      continue;

    // Nash product: ∏ (u_i - d_i)^α_i  (using log-sum)
    float nash_product = 0.0f;
    bool valid = true;

    for (int j = 0; j < 4; j++) {
      float surplus = candidates[i].utility_values[j] - disagreement[j];
      if (surplus <= 0.0f) {
        valid = false; // Below disagreement point
        break;
      }
      nash_product += sw.weights[j] * logf(surplus); // α_i × log(u_i - d_i)
    }

    // Tie-break by lowest node_id so all nodes pick the same winner
    if (valid && (nash_product > max_nash_product ||
                  (fabsf(nash_product - max_nash_product) < 1e-6f &&
                   (winner == 0 || candidates[i].node_id < winner)))) {
      max_nash_product = nash_product;
      winner = candidates[i].node_id;
      winner_idx = (int)i;
    }
  }

  if (winner_idx >= 0) {
    ESP_LOGI(TAG, "[STELLAR] Nash winner: node_%lu, Nash product (log)=%.4f",
             winner, max_nash_product);
  }

  return winner;
}

// ============================================
// STELLAR Algorithm: Centrality Computation
// ============================================

/**
 * @brief Compute network centrality based on RSSI variance
 * κ(n) = 1 - σ²_spatial(n) / max_variance
 *
 * Lower RSSI variance = more central position (spatially).
 * Note: This RSSI-variance centrality is used for self only in the election.
 * Remote nodes receive a default κ=0.8 because each node only knows its own
 * RSSI variance — sharing RSSI data would require a gossip protocol (future
 * work).
 */
static float compute_centrality(const neighbor_entry_t *neighbors,
                                size_t count) {
  if (count == 0)
    return 1.0f; // Only node in cluster = perfectly central

  // Compute mean RSSI
  float mean_rssi = 0.0f;
  for (size_t i = 0; i < count; i++) {
    mean_rssi += neighbors[i].rssi_ewma;
  }
  mean_rssi /= (float)count;

  // Compute RSSI variance
  float variance = 0.0f;
  for (size_t i = 0; i < count; i++) {
    float diff = neighbors[i].rssi_ewma - mean_rssi;
    variance += diff * diff;
  }
  variance /= (float)count;

  // Normalize: typical RSSI variance is 0-400 (dBm²)
  float normalized_variance = variance / 400.0f;
  if (normalized_variance > 1.0f)
    normalized_variance = 1.0f;

  // Higher centrality = lower variance
  return 1.0f - normalized_variance;
}

// ============================================
// STELLAR Election Main Function
// ============================================

/**
 * @brief Run STELLAR CH election algorithm
 *
 * Phase 1: Compute utility values φ_i(m_i) using non-linear functions
 * Phase 2: Extract Pareto frontier P
 * Phase 3: Apply Nash Bargaining Solution to select from P
 */
static uint32_t election_run_stellar(void) {
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "Starting STELLAR CH Election");
  ESP_LOGI(TAG, "========================================");

  // Update Lyapunov weights
  metrics_update_stellar_weights();

  // Get neighbors
  neighbor_entry_t neighbors[MAX_NEIGHBORS];
  size_t neighbor_count = neighbor_manager_get_all(neighbors, MAX_NEIGHBORS);

  // Build candidate list
  stellar_candidate_t candidates[MAX_NEIGHBORS + 1];
  size_t candidate_count = 0;

  // Add self
  node_metrics_t self_metrics = metrics_get_current();
  extern uint32_t g_node_id;

  float uptime_norm =
      (float)self_metrics.uptime_seconds / (UPTIME_MAX_DAYS * 86400.0f);
  if (uptime_norm > 1.0f)
    uptime_norm = 1.0f;

  candidates[candidate_count].node_id = g_node_id;
  candidates[candidate_count].raw_metrics[0] = self_metrics.battery;
  candidates[candidate_count].raw_metrics[1] = uptime_norm;
  candidates[candidate_count].raw_metrics[2] = self_metrics.trust;
  candidates[candidate_count].raw_metrics[3] = self_metrics.link_quality;

  // Apply non-linear utility functions
  candidates[candidate_count].utility_values[0] =
      stellar_utility_battery(self_metrics.battery);
  candidates[candidate_count].utility_values[1] =
      stellar_utility_uptime(uptime_norm);
  candidates[candidate_count].utility_values[2] =
      stellar_utility_trust(self_metrics.trust);
  candidates[candidate_count].utility_values[3] =
      stellar_utility_linkq(self_metrics.link_quality);

  candidates[candidate_count].centrality =
      compute_centrality(neighbors, neighbor_count);
  candidates[candidate_count].is_self = true;
  candidates[candidate_count].on_pareto_frontier = false; // Will be computed
  candidates[candidate_count].pareto_rank = 0;
  candidate_count++;

  // Add neighbors
  for (size_t i = 0; i < neighbor_count; i++) {
    if (!neighbor_manager_is_in_cluster(&neighbors[i]) ||
        !neighbors[i].verified) {
      continue;
    }

    // Check trust threshold (trust-based filtering)
    if (neighbors[i].trust < TRUST_FLOOR) {
      ESP_LOGW(TAG, "[STELLAR] Excluding node_%lu: trust %.2f < threshold %.2f",
               neighbors[i].node_id, neighbors[i].trust, TRUST_FLOOR);
      continue;
    }

    uptime_norm =
        (float)neighbors[i].uptime_seconds / (UPTIME_MAX_DAYS * 86400.0f);
    if (uptime_norm > 1.0f)
      uptime_norm = 1.0f;

    candidates[candidate_count].node_id = neighbors[i].node_id;
    candidates[candidate_count].raw_metrics[0] = neighbors[i].battery;
    candidates[candidate_count].raw_metrics[1] = uptime_norm;
    candidates[candidate_count].raw_metrics[2] = neighbors[i].trust;
    candidates[candidate_count].raw_metrics[3] = neighbors[i].link_quality;

    // Apply utility functions
    candidates[candidate_count].utility_values[0] =
        stellar_utility_battery(neighbors[i].battery);
    candidates[candidate_count].utility_values[1] =
        stellar_utility_uptime(uptime_norm);
    candidates[candidate_count].utility_values[2] =
        stellar_utility_trust(neighbors[i].trust);
    candidates[candidate_count].utility_values[3] =
        stellar_utility_linkq(neighbors[i].link_quality);

    candidates[candidate_count].centrality = 0.8f; // Default for remote nodes
    // Note: Each node only knows its own RSSI variance, so we cannot compute
    // the true spatial centrality for remote nodes without a gossip protocol.
    // 0.8 is a conservative assumption (slightly penalises remote nodes).
    // This is a documented limitation — see senior_engineering_review_v2.md.
    candidates[candidate_count].is_self = false;
    candidates[candidate_count].on_pareto_frontier = false;
    candidates[candidate_count].pareto_rank = 0;
    candidate_count++;
  }

  if (candidate_count == 0) {
    ESP_LOGE(TAG, "[STELLAR] No candidates for election");
    return 0;
  }

  // Single candidate (self or only node): always elect as CH
  if (candidate_count == 1) {
    uint32_t sole_winner = candidates[0].node_id;
    ESP_LOGI(TAG, "[STELLAR] Single candidate: node_%lu elected as CH",
             sole_winner);
    return sole_winner;
  }

  // Phase 1: Log utility values
  ESP_LOGI(TAG, "[Phase 1] Computed utility values for %zu candidates",
           candidate_count);
  for (size_t i = 0; i < candidate_count && i < 5; i++) {
    ESP_LOGI(TAG, "  node_%lu: u=[%.3f, %.3f, %.3f, %.3f]",
             candidates[i].node_id, candidates[i].utility_values[0],
             candidates[i].utility_values[1], candidates[i].utility_values[2],
             candidates[i].utility_values[3]);
  }

  // Phase 2: Compute Pareto frontier
  ESP_LOGI(TAG, "[Phase 2] Computing Pareto frontier");
  compute_pareto_frontier(candidates, candidate_count);

  int pareto_count = 0;
  for (size_t i = 0; i < candidate_count; i++) {
    if (candidates[i].on_pareto_frontier) {
      pareto_count++;
      ESP_LOGI(TAG, "  Pareto member: node_%lu (dominates %d nodes)",
               candidates[i].node_id, candidates[i].pareto_rank);
    }
  }
  ESP_LOGI(TAG, "[Phase 2] Pareto frontier contains %d nodes", pareto_count);

  // Compute STELLAR scores for all candidates
  for (size_t i = 0; i < candidate_count; i++) {
    node_metrics_t tmp = {.battery = candidates[i].raw_metrics[0],
                          .uptime_seconds =
                              (uint64_t)(candidates[i].raw_metrics[1] *
                                         UPTIME_MAX_DAYS * 86400.0f),
                          .trust = candidates[i].raw_metrics[2],
                          .link_quality = candidates[i].raw_metrics[3]};
    candidates[i].stellar_score = metrics_compute_stellar_score(
        &tmp, candidates[i].pareto_rank, candidates[i].centrality);
  }

  // Phase 3: Nash Bargaining Selection from Pareto frontier
  ESP_LOGI(TAG, "[Phase 3] Nash Bargaining selection");
  uint32_t winner = nash_bargaining_selection(candidates, candidate_count);

  // Fallback: if Nash bargaining fails, use highest STELLAR score from Pareto
  // frontier
  if (winner == 0) {
    ESP_LOGW(TAG, "[Phase 3] Nash bargaining failed, using max STELLAR score "
                  "from Pareto");
    float max_score = -1e9f;
    for (size_t i = 0; i < candidate_count; i++) {
      if (candidates[i].on_pareto_frontier &&
          (candidates[i].stellar_score > max_score ||
           (fabsf(candidates[i].stellar_score - max_score) < 1e-6f &&
            (winner == 0 || candidates[i].node_id < winner)))) {
        max_score = candidates[i].stellar_score;
        winner = candidates[i].node_id;
      }
    }
  }

  // Final fallback: if still no winner, use highest overall score (tie-break:
  // lowest node_id)
  if (winner == 0) {
    ESP_LOGW(
        TAG,
        "[Phase 3] Pareto fallback failed, using overall max STELLAR score");
    float max_score = -1e9f;
    for (size_t i = 0; i < candidate_count; i++) {
      if (candidates[i].stellar_score > max_score ||
          (fabsf(candidates[i].stellar_score - max_score) < 1e-6f &&
           (winner == 0 || candidates[i].node_id < winner))) {
        max_score = candidates[i].stellar_score;
        winner = candidates[i].node_id;
      }
    }
  }

  // Log results
  bool self_won = (winner == g_node_id);
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "STELLAR Election Complete");
  ESP_LOGI(TAG, "Winner: node_%lu (self=%d)", winner, self_won);
  ESP_LOGI(TAG, "========================================");

  // Log all candidates with their scores
  for (size_t i = 0; i < candidate_count; i++) {
    ESP_LOGI(TAG, "  Candidate: node_%lu, Ψ=%.4f, Pareto=%d, Dom=%d, κ=%.2f",
             candidates[i].node_id, candidates[i].stellar_score,
             candidates[i].on_pareto_frontier, candidates[i].pareto_rank,
             candidates[i].centrality);
  }

  return winner;
}

// ============================================
// Legacy Election Function
// ============================================

static uint32_t election_run_legacy(void) {
  ESP_LOGI(TAG, "Starting Legacy CH election");

  // Get all neighbors within cluster radius
  neighbor_entry_t neighbors[MAX_NEIGHBORS];
  size_t neighbor_count = neighbor_manager_get_all(neighbors, MAX_NEIGHBORS);

  // Build candidate list (self + neighbors)
  candidate_t candidates[MAX_NEIGHBORS + 1];
  size_t candidate_count = 0;

  // Add self
  node_metrics_t self_metrics = metrics_get_current();
  extern uint32_t g_node_id;

  candidates[candidate_count].node_id = g_node_id;
  candidates[candidate_count].score = self_metrics.composite_score;
  candidates[candidate_count].link_quality = self_metrics.link_quality;
  candidates[candidate_count].battery = self_metrics.battery;
  candidates[candidate_count].trust = self_metrics.trust;
  candidates[candidate_count].is_self = true;
  candidate_count++;

  // Add neighbors within cluster radius
  for (size_t i = 0; i < neighbor_count; i++) {
    if (neighbor_manager_is_in_cluster(&neighbors[i]) &&
        neighbors[i].verified) {
      candidates[candidate_count].node_id = neighbors[i].node_id;
      candidates[candidate_count].score = neighbors[i].score;
      candidates[candidate_count].link_quality = neighbors[i].link_quality;
      candidates[candidate_count].battery = neighbors[i].battery;
      candidates[candidate_count].trust = neighbors[i].trust;
      candidates[candidate_count].is_self = false;
      candidate_count++;
    }
  }

  if (candidate_count == 0) {
    ESP_LOGE(TAG, "No candidates for election");
    return 0;
  }

  // Sort candidates (best first)
  qsort(candidates, candidate_count, sizeof(candidate_t), compare_candidates);

  // Winner is first candidate
  uint32_t winner = candidates[0].node_id;
  bool self_won = candidates[0].is_self;

  ESP_LOGI(TAG,
           "Legacy election complete: winner=node_%lu (self=%d), score=%.3f",
           winner, self_won, candidates[0].score);

  // Log top 3 candidates
  for (size_t i = 0; i < candidate_count && i < 3; i++) {
    ESP_LOGI(TAG, "  Candidate %zu: node_%lu, score=%.3f", i + 1,
             candidates[i].node_id, candidates[i].score);
  }

  return winner;
}

// ============================================
// Public Election API
// ============================================

uint32_t election_run(void) {
  if (s_election_mutex &&
      xSemaphoreTake(s_election_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    ESP_LOGW(TAG, "Election mutex timeout (another run in progress?)");
    return 0;
  }

  uint32_t winner;

#if USE_STELLAR_ALGORITHM
  winner = election_run_stellar();
#else
  winner = election_run_legacy();
#endif

  if (s_election_mutex)
    xSemaphoreGive(s_election_mutex);
  return winner;
}

bool election_check_reelection_needed(void) {
  // Check if we are the CH
  extern bool g_is_ch;
  extern uint32_t g_node_id;

  if (g_is_ch) {
    // We are the CH, check our own health
    node_metrics_t self_metrics = metrics_get_current();

    if (self_metrics.battery < BATTERY_LOW_THRESHOLD) {
      bool healthier_exists = false;
      neighbor_entry_t neighbors[MAX_NEIGHBORS];
      size_t count = neighbor_manager_get_all(neighbors, MAX_NEIGHBORS);
      for (size_t i = 0; i < count; i++) {
        if (neighbor_manager_is_in_cluster(&neighbors[i]) &&
            neighbors[i].verified) {
          if (neighbors[i].battery >= BATTERY_LOW_THRESHOLD ||
              neighbors[i].battery > self_metrics.battery + 0.05f) {
            healthier_exists = true;
            break;
          }
        }
      }

      if (healthier_exists) {
        ESP_LOGI(
            TAG,
            "Self (CH) battery low (%.2f), healthier neighbor found, yielding",
            self_metrics.battery);
        return true;
      }
    }

    if (self_metrics.trust < TRUST_FLOOR) {
      ESP_LOGI(TAG, "Self (CH) trust low (%.2f), re-election needed",
               self_metrics.trust);
      return true;
    }

    if (self_metrics.link_quality < LINK_QUALITY_FLOOR) {
      ESP_LOGI(TAG, "Self (CH) link quality low (%.2f), re-election needed",
               self_metrics.link_quality);
      return true;
    }

    // NEW: Check for conflicting CHs (multiple nodes claiming CH)
    neighbor_entry_t neighbors[MAX_NEIGHBORS];
    size_t count = neighbor_manager_get_all(neighbors, MAX_NEIGHBORS);

    for (size_t i = 0; i < count; i++) {
      if (neighbors[i].is_ch && neighbors[i].verified) {
        // Another node is also claiming to be CH!
        // Resolve conflict: Meritocracy First!
        // If the other CH has a BETTER score, we yield (algorithm takes
        // precedence) We use a small buffer (0.01) to treat nearly-equal scores
        // as ties
        float score_diff = neighbors[i].score - self_metrics.composite_score;

        if (score_diff > 0.01f) {
          ESP_LOGW(TAG,
                   "CH conflict: Neighbor %lu is better (Score %.2f vs My "
                   "%.2f). Yielding.",
                   neighbors[i].node_id, neighbors[i].score,
                   self_metrics.composite_score);
          return true;
        } else if (score_diff < -0.01f) {
          ESP_LOGI(TAG,
                   "CH conflict: I am better (Score %.2f vs Neighbor %.2f). "
                   "Staying.",
                   self_metrics.composite_score, neighbors[i].score);
          // Stay CH, neighbor should yield
        } else {
          // Scores are effectively equal (tie). Use Node ID to break tie.
          if (neighbors[i].node_id < g_node_id) {
            ESP_LOGW(
                TAG,
                "CH conflict: TIED Score, Neighbor %lu has lower ID. Yielding.",
                neighbors[i].node_id);
            return true;
          } else {
            ESP_LOGI(TAG, "CH conflict: TIED Score, I have lower ID. Staying.");
          }
        }
      }
    }

    return false;
  }

  // Check if current CH is still valid
  uint32_t current_ch = neighbor_manager_get_current_ch();
  if (current_ch == 0) {
    ESP_LOGI(TAG, "No valid CH, re-election needed");
    return true;
  }

  neighbor_entry_t ch_entry;
  if (!neighbor_manager_get(current_ch, &ch_entry)) {
    ESP_LOGI(TAG, "CH entry not found, re-election needed");
    return true;
  }

  if (ch_entry.battery < BATTERY_LOW_THRESHOLD) {
    bool healthier_exists = false;
    node_metrics_t self_m = metrics_get_current();

    // Am I healthier?
    if (self_m.battery >= BATTERY_LOW_THRESHOLD ||
        self_m.battery > ch_entry.battery + 0.05f) {
      healthier_exists = true;
    } else {
      // Is anyone else healthier?
      neighbor_entry_t neighbors[MAX_NEIGHBORS];
      size_t count = neighbor_manager_get_all(neighbors, MAX_NEIGHBORS);
      for (size_t i = 0; i < count; i++) {
        if (neighbor_manager_is_in_cluster(&neighbors[i]) &&
            neighbors[i].verified && neighbors[i].node_id != current_ch) {
          if (neighbors[i].battery >= BATTERY_LOW_THRESHOLD ||
              neighbors[i].battery > ch_entry.battery + 0.05f) {
            healthier_exists = true;
            break;
          }
        }
      }
    }

    if (healthier_exists) {
      ESP_LOGI(TAG,
               "CH battery low (%.2f) and healthier node available, triggering "
               "re-election",
               ch_entry.battery);
      return true;
    }
  }

  if (ch_entry.trust < TRUST_FLOOR) {
    ESP_LOGI(TAG, "CH trust low (%.2f), re-election needed", ch_entry.trust);
    return true;
  }

  if (ch_entry.link_quality < LINK_QUALITY_FLOOR) {
    ESP_LOGI(TAG, "CH link quality low (%.2f), re-election needed",
             ch_entry.link_quality);
    return true;
  }

  return false;
}

uint64_t election_get_window_start(void) { return election_window_start; }

void election_reset_window(void) {
  election_window_start = esp_timer_get_time() / 1000;
  ESP_LOGI(TAG, "Election window reset");
}

void election_init(void) {
  election_window_start = 0;
  s_election_mutex = xSemaphoreCreateMutex();
  if (s_election_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create election mutex");
  }
  ESP_LOGI(TAG, "Election system initialized (STELLAR=%d)",
           USE_STELLAR_ALGORITHM);
}
