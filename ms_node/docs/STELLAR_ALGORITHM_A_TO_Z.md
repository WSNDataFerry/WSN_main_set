# STELLAR Algorithm — A to Z (Concepts, Formulas, and Code)

This document explains the **STELLAR** (Stochastic Trust-Enhanced Lyapunov-stable Leader Allocation and Ranking) algorithm from theory to implementation, with every formula and the exact code that implements it.

**Files involved:** `main/election.c`, `main/metrics.c`, `main/metrics.h`, `main/config.h`.

---

## 1. What STELLAR Is and When It Runs

- **Purpose:** Elect one **Cluster Head (CH)** from a set of candidates (self + neighbors) in a fair, multi-metric way. STELLAR uses four dimensions (battery, uptime, trust, link quality), turns them into **utilities**, finds the **Pareto frontier**, then picks one candidate via **Nash Bargaining**. Weights that combine these metrics are adapted over time using a **Lyapunov-stable** update.
- **When it runs:** Only when `USE_STELLAR_ALGORITHM` is 1 (`config.h` line 59). The election entry point is `election_run()` in `election.c`, which calls `election_run_stellar()` (lines 517–535).

**Code reference:**

```c
// election.c, lines 517–535
uint32_t election_run(void) {
  if (s_election_mutex && xSemaphoreTake(s_election_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    ESP_LOGW(TAG, "Election mutex timeout (another run in progress?)");
    return 0;
  }
  uint32_t winner;
#if USE_STELLAR_ALGORITHM
  winner = election_run_stellar();
#else
  winner = election_run_legacy();
#endif
  if (s_election_mutex) xSemaphoreGive(s_election_mutex);
  return winner;
}
```

---

## 2. Data Structures

### 2.1 Candidate Structure (Election)

**File:** `election.c` lines 21–31

```c
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
```

- **raw_metrics[0..3]:** Battery (0–1), uptime normalized to [0,1], trust (0–1), link_quality (0–1).
- **utility_values[0..3]:** Same four after applying the non-linear utility functions φ_battery, φ_uptime, φ_trust, φ_linkq.
- **stellar_score:** Ψ(n) = weighted sum × centrality factor + dominance bonus (used for fallbacks and logging).
- **pareto_rank:** Count of other candidates this candidate Pareto-dominates.
- **centrality:** κ(n); for self it is computed from RSSI variance; for remote nodes it is set to 0.8.
- **on_pareto_frontier:** True if no other candidate Pareto-dominates this one.

### 2.2 Lyapunov Weight Structure (Metrics)

**File:** `metrics.h` lines 26–31

```c
typedef struct {
  float weights[4];       // [battery, uptime, trust, linkq] — current adaptive weights
  float target_weights[4]; // w*(t) — entropy-derived target
  float lyapunov_value;   // V(w,t)
  bool converged;         // V < ε
} stellar_weights_t;
```

**File:** `metrics.c` lines 525–531 — global instance `g_stellar_weights` initialized with `WEIGHT_*` from config.

---

## 3. Constants (config.h)

All STELLAR-related constants and their use:

| Constant | Value | Meaning |
|----------|--------|--------|
| `WEIGHT_BATTERY` | 0.3f | Base weight for battery. |
| `WEIGHT_UPTIME` | 0.2f | Base weight for uptime. |
| `WEIGHT_TRUST` | 0.3f | Base weight for trust. |
| `WEIGHT_LINK_QUALITY` | 0.2f | Base weight for link quality. |
| `UTILITY_LAMBDA_B` | 2.0f | λ in battery utility φ_battery. |
| `UTILITY_LAMBDA_U` | 1.0f | λ in uptime utility φ_uptime. |
| `UTILITY_GAMMA_L` | 2.0f | γ in link quality utility φ_linkq. |
| `UPTIME_MAX_DAYS` | 7.0f | Uptime normalized by this many days (seconds = days×86400). |
| `PARETO_DELTA` | 0.1f | δ in dominance bonus ρ(n). |
| `CENTRALITY_EPSILON` | 0.5f | ε in centrality factor κ(n). |
| `DISAGREE_BATTERY` | 0.1f | Disagreement point d_i for Nash (battery). |
| `DISAGREE_UPTIME` | 0.1f | Disagreement point (uptime). |
| `DISAGREE_TRUST` | 0.1f | Disagreement point (trust). |
| `DISAGREE_LINKQ` | 0.1f | Disagreement point (link quality). |
| `MIN_WEIGHT_VALUE` | 0.05f | Minimum allowed weight after projection. |
| `ENTROPY_GAMMA` | 1.0f | γ in softmax confidence exp(-γ H_i). |
| `EWMA_VARIANCE_ALPHA` | 0.1f | α for variance EWMA. |
| `LYAPUNOV_BETA` | 0.1f | β in Lyapunov gradient (regularisation toward w_eq). |
| `LYAPUNOV_ETA` | 0.05f | Step size for weight gradient descent. |
| `LYAPUNOV_LAMBDA` | 0.01f | λ in Lyapunov function V (gradient norm term). |
| `CONVERGENCE_THRESHOLD` | 0.001f | V < this ⇒ converged. |
| `W_EQ_*` | WEIGHT_* | Equilibrium weights w_eq for Lyapunov. |
| `STELLAR_SCORE_EWMA_ALPHA` | 0.25f | Smoothing of raw STELLAR score in metrics_update. |
| `MAX_NEIGHBORS` | 10 | Max candidates; used in dominance bonus denominator. |
| `TRUST_FLOOR` | 0.2f | Neighbors with trust < this are excluded from election. |

---

## 3.5 How Each Raw Metric Is Measured (Before STELLAR)

STELLAR uses four **raw** metrics (battery, uptime, trust, link_quality) in [0,1]. This section describes **how each of these values is obtained** in the system — i.e. the measurement and calculation, not the utility transform φ_i.

### Trust (own node)

**Definition:** A composite of (1) **HMAC success rate** (did we verify incoming BLE packets?), (2) **packet delivery rate** (PDR) from BLE sequence numbers, and (3) **reputation** (trust values advertised by neighbors).

**Step 1 — HMAC Success Rate (HSR)**  
Every time we verify a BLE advertisement (HMAC check), we record success or failure:

- **Code:** `metrics.c` lines 194–205 — `metrics_record_hmac_success(bool success)`
- **Formula:** `hsr_ewma = HSR_WEIGHT * (success ? 1.0f : 0.0f) + (1.0f - HSR_WEIGHT) * hsr_ewma`  
  with `HSR_WEIGHT` = 0.4 (`config.h` line 36).  
- **Called from:** `ble_manager.c` after HMAC verify: success → `metrics_record_hmac_success(true)`, failure → `metrics_record_hmac_success(false)`.

**Step 2 — Packet delivery rate (PDR)**  
PDR is derived from **BLE packet loss**. When we receive a BLE packet from a neighbor, we compare its sequence number with the last one we saw:

- **Missed packets:** `diff = (curr_seq - last_seq)` (with 0..255 wrap); `missed = max(0, diff - 1)`; if `missed > 20` we treat as reboot and set `missed = 0`.
- **Code (missed count):** `neighbor_manager.c` lines 60–76; then `metrics_record_ble_reception(1, missed)`.
- **PER (Packet Error Rate):** `batch_per = failures / (successes + failures)`; then `per_ewma = PDR_EWMA_ALPHA * batch_per + (1 - PDR_EWMA_ALPHA) * per_ewma` (`metrics.c` lines 266–283). `PDR_EWMA_ALPHA` = 0.1.
- **PDR:** `pdr = 1.0f - per_ewma`; then `pdr_ewma = PDR_WEIGHT * pdr + (1 - PDR_WEIGHT) * pdr_ewma` with `PDR_WEIGHT` = 0.3.

**Step 3 — Reputation (from neighbors)**  
When we receive a **valid** BLE advertisement, we use the sender’s advertised trust as “reputation”:

- **Code:** `ble_manager.c`: `trust_f = (float)pkt->trust / 100.0f`; then `metrics_update_trust(trust_f)`.
- **Formula (reputation EWMA):** `reputation_ewma = REPUTATION_WEIGHT * trust_f + (1 - REPUTATION_WEIGHT) * reputation_ewma` with `REPUTATION_WEIGHT` = 0.3.

**Step 4 — Composite trust**  
Own-node trust is a weighted sum of the three components:

- **Formula:** `current_metrics.trust = HSR_WEIGHT * hsr_ewma + PDR_WEIGHT * pdr_ewma + REPUTATION_WEIGHT * reputation_ewma`  
  then clamped to [0, 1].  
- **Code:** `metrics.c` lines 207–236 — `metrics_update_trust(float reputation)`.

**Persistence:** Trust is saved to NVS every 60 s (`metrics_persist_trust`) and restored after reboot (`metrics_get_trust_nvs()`), so it survives restarts.

### Trust (neighbor)

- **Initial value:** From BLE advertisement (`pkt->trust / 100.0f`) when the neighbor is first seen, or from `persistence_get_initial_trust(node_id)` if we have a saved reputation.
- **Ongoing updates:** When we **receive** data from that neighbor (e.g. ESP-NOW sensor payload), we call `neighbor_manager_update_trust(node_id, true)`.  
- **Formula (neighbor):** `entry->trust = 0.9f * entry->trust + 0.1f * (success ? 1.0f : 0.0f)`  
  (`neighbor_manager.c` lines 321–339 — `neighbor_manager_update_trust`). So each successful interaction pushes trust up, each failure would push it down (success=false is not currently used in the codebase but the API supports it).

### Battery (own node)

- **Hardware path:** ADC on GPIO4 with voltage divider (e.g. 220k / 100k). `battery_init()` and `battery_read(&vadc_mv, &vbat_mv, &batt_pct)` in `ms_node.c` (e.g. lines 469, 631).
- **Valid range:** If `vbat_mv` is in (2000, 5000] mV, we use real battery: `pme_set_batt_pct(batt_pct)` and then `metrics_read_battery()` returns `pme_get_batt_pct() / 100.0f`.
- **Fallbacks:** If ADC invalid: with `ENABLE_MOCK_BATTERY`, use 75% and vbat_mv = 3975; otherwise assume USB and use 100%.
- **Code:** `metrics.c` lines 91–112 — `metrics_read_battery()` returns `(float)pct / 100.0f` (or 1.0f when pct==0 for USB/no-battery).

### Uptime (own node)

- **Definition:** Total seconds this node has been running, including across reboots (persisted).
- **Formula:** `uptime_seconds = base_uptime + boot_time`  
  where `base_uptime` = value loaded from NVS (`metrics_get_uptime()`), and `boot_time = esp_timer_get_time() / 1_000_000` (seconds since boot).
- **Code:** `metrics.c` lines 369–372 (in `metrics_update()`): `base_uptime = metrics_get_uptime(); boot_time = esp_timer_get_time()/1000000; current_metrics.uptime_seconds = base_uptime + boot_time`.
- **Persistence:** Uptime is saved to NVS every 60 s (`metrics_persist_uptime()`). On first boot, `metrics_get_uptime()` returns 0.

### Link quality (own node)

- **Definition:** Combination of **RSSI** (signal strength) and **PER** (packet error rate) so that good link = strong signal and few losses.
- **RSSI quality:** `rssi_quality = (rssi_ewma + 100.0f) / 50.0f` clamped to [0,1] (so -100 dBm → 0, -50 dBm → 1). RSSI is updated from BLE scan: `metrics_update_rssi(rssi)` → `rssi_ewma = 0.1f * rssi + 0.9f * rssi_ewma` (`metrics.c` 296–302, 246).
- **PER quality:** `per_quality = 1.0f - per_ewma` (same PER as used for trust/PDR).
- **Composite:** `link_quality = 0.7f * rssi_quality + 0.3f * per_quality` then clamped to [0,1].
- **Code:** `metrics.c` lines 239–263 — `metrics_recompute_link_quality()`. It is called after every PER/RSSI update (`metrics_record_ble_reception`, `metrics_update_rssi`, `metrics_update_per`).

### Battery, uptime, link_quality (neighbor)

- **Source:** Received from BLE advertisements. Each neighbor’s `battery`, `uptime_seconds`, and `link_quality` are sent in the BLE payload (e.g. `pkt->battery/100`, `pkt->link_quality/100`; uptime can be sent in the same packet or as 0 and updated later). Stored in `neighbor_table[i]` in `neighbor_manager_update()` (`neighbor_manager.c` lines 39–154).

---

## 4. Utility Functions φ_i (Non-Linear Transformations)

Concept: Raw metrics are in [0,1] (or uptime in [0,1] after normalization). We map them with **non-linear** functions so that low values are penalized more and the shape reflects “diminishing returns” or “saturation” where appropriate. All four utilities are implemented in `metrics.c` and used both in **metrics** (for continuous score) and in **election** (for Pareto and Nash).

### 4.1 Battery: φ_battery(b)

**Formula:**  
φ_battery(b) = (1 − e^{−λb}) / (1 − e^{−λ})  
with λ = UTILITY_LAMBDA_B (2.0).

**Properties:** φ(0)=0, φ(1)=1; concave (diminishing marginal utility); penalizes low battery heavily.

**Code:** `metrics.c` lines 737–744

```c
float stellar_utility_battery(float battery) {
  float lambda = UTILITY_LAMBDA_B;
  float numerator = 1.0f - expf(-lambda * battery);
  float denominator = 1.0f - expf(-lambda);
  if (denominator < 1e-6f) denominator = 1e-6f;
  return numerator / denominator;
}
```

### 4.2 Uptime: φ_uptime(u)

**Formula:**  
φ_uptime(u) = tanh(λ × u)  
with λ = UTILITY_LAMBDA_U (1.0).  
u is **uptime_normalized** = uptime_seconds / (UPTIME_MAX_DAYS × 86400), capped at 1.0.

**Properties:** φ(0)=0, saturates toward 1 for large u; prevents “zombie leader” (very old nodes don’t dominate indefinitely).

**Code:** `metrics.c` lines 755–757

```c
float stellar_utility_uptime(float uptime_normalized) {
  return tanhf(UTILITY_LAMBDA_U * uptime_normalized);
}
```

### 4.3 Trust: φ_trust(t)

**Formula:**  
φ_trust(t) = t² × (3 − 2t)  
(Hermite smooth-step). t is clamped to [0, 1].

**Properties:** φ(0)=0, φ(0.5)=0.5, φ(1)=1; S-shaped; emphasizes differentiation in the mid-range.

**Code:** `metrics.c` lines 769–775

```c
float stellar_utility_trust(float trust) {
  if (trust < 0.0f) trust = 0.0f;
  if (trust > 1.0f) trust = 1.0f;
  return trust * trust * (3.0f - 2.0f * trust);
}
```

### 4.4 Link Quality: φ_linkq(l)

**Formula:**  
φ_linkq(l) = l^{1/γ}  
with γ = UTILITY_GAMMA_L (2.0). l is clamped to [0, 1].

**Properties:** φ(0)=0, φ(1)=1; concave for γ>1; higher sensitivity at low link quality.

**Code:** `metrics.c` lines 786–792

```c
float stellar_utility_linkq(float link_quality) {
  if (link_quality < 0.0f) link_quality = 0.0f;
  if (link_quality > 1.0f) link_quality = 1.0f;
  return powf(link_quality, 1.0f / UTILITY_GAMMA_L);
}
```

---

## 5. Lyapunov Weight Adaptation (metrics.c)

Concept: The four weights w_i used in the STELLAR score and in Nash are **not** fixed. They are updated every time `metrics_update_stellar_weights()` is called (from `metrics_update()` and from `election_run_stellar()` at the start of an election). The update is a **Lyapunov-stable** gradient descent so that:
- Weights move toward an **entropy-derived target** w*(t) (higher confidence in a metric → higher target weight for that metric).
- A **regularisation** term pulls toward the equilibrium weights w_eq (the base WEIGHT_*).
- The weight vector stays on the **probability simplex** (non-negative, sum to 1).

### 5.1 Variance Estimates (EWMA)

**Formula:**  
σ²_i ← α (x_t − x_{t−1})² + (1−α) σ²_i  
with α = EWMA_VARIANCE_ALPHA (0.1). Applied for battery, trust, link_quality. Uptime variance is set to 0.001 (near-deterministic).

**Code:** `metrics.c` lines 583–608 — `metrics_update_variance_estimates()`

```c
void metrics_update_variance_estimates(void) {
  float diff;
  diff = current_metrics.battery - prev_battery;
  battery_variance_ewma = EWMA_VARIANCE_ALPHA * (diff * diff) +
                          (1.0f - EWMA_VARIANCE_ALPHA) * battery_variance_ewma;
  prev_battery = current_metrics.battery;
  // ... same for trust and link_quality ...
  current_metrics.battery_variance = battery_variance_ewma;
  current_metrics.trust_variance = trust_variance_ewma;
  current_metrics.linkq_variance = linkq_variance_ewma;
}
```

### 5.2 Differential Entropy H(m)

**Formula:**  
H(m) = 0.5 × ln(2πe × σ²)  
with 2πe ≈ 17.0794684. Used for each metric’s variance (uptime uses 0.001).

**Code:** `metrics.c` lines 541–546

```c
static float compute_differential_entropy(float variance) {
  const float TWO_PI_E = 17.0794684f;
  if (variance < 1e-6f) variance = 1e-6f;
  return 0.5f * logf(TWO_PI_E * variance);
}
```

### 5.3 Entropy-Based Confidence c_i

**Formula:**  
c_i = exp(−γ H_i) / Σ_j exp(−γ H_j)  
with γ = ENTROPY_GAMMA (1.0). So high entropy → low confidence; result is normalized to sum to 1 (probability simplex).

**Code:** `metrics.c` lines 549–574 — `metrics_compute_entropy_confidence()`

```c
void metrics_compute_entropy_confidence(void) {
  float entropies[4];
  entropies[0] = compute_differential_entropy(battery_variance_ewma);
  entropies[1] = compute_differential_entropy(0.001f);
  entropies[2] = compute_differential_entropy(trust_variance_ewma);
  entropies[3] = compute_differential_entropy(linkq_variance_ewma);
  float sum_exp = 0.0f;
  for (int i = 0; i < 4; i++) {
    float exp_val = expf(-ENTROPY_GAMMA * entropies[i]);
    sum_exp += exp_val;
    current_metrics.entropy_confidence[i] = exp_val;
  }
  if (sum_exp > 0.0f) {
    for (int i = 0; i < 4; i++)
      current_metrics.entropy_confidence[i] /= sum_exp;
  }
}
```

### 5.4 Target Weights w*(t)

**Formula:**  
w*_i = base_weight_i × (1 + 0.5 × (confidence_i − 0.25))  
then clamp each to at least MIN_WEIGHT_VALUE, then **normalize** so Σ w*_i = 1.

**Code:** `metrics.c` lines 641–661 (inside `metrics_update_stellar_weights()`)

```c
float base_weights[4] = {WEIGHT_BATTERY, WEIGHT_UPTIME, WEIGHT_TRUST, WEIGHT_LINK_QUALITY};
float sum = 0.0f;
for (int i = 0; i < 4; i++) {
  float confidence_adjustment = current_metrics.entropy_confidence[i] - 0.25f;
  g_stellar_weights.target_weights[i] =
      base_weights[i] * (1.0f + 0.5f * confidence_adjustment);
  if (g_stellar_weights.target_weights[i] < MIN_WEIGHT_VALUE)
    g_stellar_weights.target_weights[i] = MIN_WEIGHT_VALUE;
  sum += g_stellar_weights.target_weights[i];
}
for (int i = 0; i < 4; i++)
  g_stellar_weights.target_weights[i] /= sum;
```

### 5.5 Lyapunov Gradient and Weight Update

**Formula:**  
Gradient: ∂V/∂w_i = (w_i − w*_i) + β (w_i − w_eq,i)  
Update: w_i ← w_i − η × gradient  
Then **project onto simplex**: clamp each w_i to ≥ MIN_WEIGHT_VALUE, then divide by sum so Σ w_i = 1.

**Code:** `metrics.c` lines 663–687

```c
static const float w_eq[4] = {W_EQ_BATTERY, W_EQ_UPTIME, W_EQ_TRUST, W_EQ_LINK_QUALITY};
float grad_norm_sq = 0.0f;
for (int i = 0; i < 4; i++) {
  float diff_target = g_stellar_weights.weights[i] - g_stellar_weights.target_weights[i];
  float diff_eq = g_stellar_weights.weights[i] - w_eq[i];
  float gradient = diff_target + LYAPUNOV_BETA * diff_eq;
  g_stellar_weights.weights[i] -= LYAPUNOV_ETA * gradient;
  grad_norm_sq += gradient * gradient;
}
project_onto_simplex(g_stellar_weights.weights, 4);
```

### 5.6 Lyapunov Function V(w,t) and Convergence

**Formula:**  
V(w,t) = ½ Σ_i (w_i − w*_i)² + λ ‖∇‖²  
Converged if V < CONVERGENCE_THRESHOLD.

**Code:** `metrics.c` lines 688–700

```c
g_stellar_weights.lyapunov_value = 0.0f;
for (int i = 0; i < 4; i++) {
  float diff = g_stellar_weights.weights[i] - g_stellar_weights.target_weights[i];
  g_stellar_weights.lyapunov_value += 0.5f * diff * diff;
}
g_stellar_weights.lyapunov_value += LYAPUNOV_LAMBDA * grad_norm_sq;
g_stellar_weights.converged = (g_stellar_weights.lyapunov_value < CONVERGENCE_THRESHOLD);
```

### 5.7 project_onto_simplex

**Code:** `metrics.c` lines 611–628

```c
static void project_onto_simplex(float *weights, int n) {
  float sum = 0.0f;
  for (int i = 0; i < n; i++) {
    if (weights[i] < MIN_WEIGHT_VALUE) weights[i] = MIN_WEIGHT_VALUE;
    sum += weights[i];
  }
  if (sum > 0.0f) {
    for (int i = 0; i < n; i++) weights[i] /= sum;
  }
}
```

---

## 6. STELLAR Score Ψ(n) (metrics.c)

**Formula:**  
Ψ(n) = (Σ_i w_i × φ_i(m_i)) × κ(n) + ρ(n)  
- Base: weighted sum of the four utilities with current Lyapunov weights.  
- κ(n) = 1 / (1 + ε × (1 − centrality)); ε = CENTRALITY_EPSILON (0.5).  
- ρ(n) = PARETO_DELTA × (pareto_rank / MAX_NEIGHBORS).

**Code:** `metrics.c` lines 805–848 — `metrics_compute_stellar_score()`

```c
float metrics_compute_stellar_score(const node_metrics_t *metrics,
                                    int pareto_rank, float centrality) {
  stellar_weights_t sw = g_stellar_weights;
  float uptime_norm = (float)metrics->uptime_seconds / (UPTIME_MAX_DAYS * 86400.0f);
  if (uptime_norm > 1.0f) uptime_norm = 1.0f;

  float u_battery = stellar_utility_battery(metrics->battery);
  float u_uptime = stellar_utility_uptime(uptime_norm);
  float u_trust = stellar_utility_trust(metrics->trust);
  float u_linkq = stellar_utility_linkq(metrics->link_quality);

  float base_score = sw.weights[0] * u_battery + sw.weights[1] * u_uptime +
                     sw.weights[2] * u_trust + sw.weights[3] * u_linkq;

  float dominance_bonus = PARETO_DELTA * ((float)pareto_rank / (float)MAX_NEIGHBORS);
  float centrality_factor = 1.0f / (1.0f + CENTRALITY_EPSILON * (1.0f - centrality));
  float stellar_score = base_score * centrality_factor + dominance_bonus;
  return stellar_score;
}
```

This function is used:
- In **metrics_update()** (lines 427–429) to compute the node’s own continuous score (with Pareto rank and centrality from neighbor comparison).
- In **election_run_stellar()** (lines 378–388) to compute Ψ for every candidate after the Pareto frontier is computed (for fallback selection and logging).

---

## 7. Centrality κ(n) in Election (election.c)

**Concept:** For **self**, we compute a spatial centrality from the **RSSI variance** of neighbors: nodes with more similar RSSIs to all neighbors are “more central”. For **remote** nodes we don’t have their RSSI variance, so we use a default 0.8 (slightly penalizes them).

**Formula (self):**  
Mean RSSI = (1/N) Σ rssi_i.  
Variance = (1/N) Σ (rssi_i − mean)².  
Normalized variance = variance / 400 (cap at 1).  
κ = 1 − normalized_variance.  
So **lower variance ⇒ higher κ** (more central).

**Code:** `election.c` lines 207–234 — `compute_centrality()`

```c
static float compute_centrality(const neighbor_entry_t *neighbors, size_t count) {
  if (count == 0) return 1.0f;
  float mean_rssi = 0.0f;
  for (size_t i = 0; i < count; i++) mean_rssi += neighbors[i].rssi_ewma;
  mean_rssi /= (float)count;
  float variance = 0.0f;
  for (size_t i = 0; i < count; i++) {
    float diff = neighbors[i].rssi_ewma - mean_rssi;
    variance += diff * diff;
  }
  variance /= (float)count;
  float normalized_variance = variance / 400.0f;
  if (normalized_variance > 1.0f) normalized_variance = 1.0f;
  return 1.0f - normalized_variance;
}
```

- **Self:** `candidates[...].centrality = compute_centrality(neighbors, neighbor_count);` (election.c line 289).  
- **Neighbors:** `candidates[...].centrality = 0.8f;` (election.c line 331).

---

## 8. Pareto Frontier (election.c)

**Concept:** Candidate a **Pareto-dominates** b iff a is ≥ b on **all** four utility dimensions and **strictly better** on at least one. The **Pareto frontier** P is the set of candidates that are **not** dominated by anyone. Only these are eligible for Nash Bargaining.

**Definition:**  
a ≻ b  ⇔  ∀i: u_i(a) ≥ u_i(b)  and  ∃j: u_j(a) > u_j(b).

**Code — dominance check:** `election.c` lines 90–105

```c
static bool pareto_dominates(const stellar_candidate_t *a, const stellar_candidate_t *b) {
  bool at_least_one_greater = false;
  for (int i = 0; i < 4; i++) {
    if (a->utility_values[i] < b->utility_values[i]) return false;
    if (a->utility_values[i] > b->utility_values[i]) at_least_one_greater = true;
  }
  return at_least_one_greater;
}
```

**Code — frontier and ranks:** `election.c` lines 110–130

```c
static void compute_pareto_frontier(stellar_candidate_t *candidates, size_t count) {
  for (size_t i = 0; i < count; i++) {
    candidates[i].pareto_rank = 0;
    candidates[i].on_pareto_frontier = true;
    for (size_t j = 0; j < count; j++) {
      if (i == j) continue;
      if (pareto_dominates(&candidates[i], &candidates[j]))
        candidates[i].pareto_rank++;
      if (pareto_dominates(&candidates[j], &candidates[i]))
        candidates[i].on_pareto_frontier = false;
    }
  }
}
```

---

## 9. Nash Bargaining Selection (election.c)

**Concept:** Among **only** Pareto-optimal candidates, we pick the one that maximizes the **Nash product** (in log form for numerical stability). Disagreement points d_i ensure we don’t pick a node that is below minimum acceptable utility on any dimension.

**Formula:**  
n* = argmax_{n ∈ P}  Σ_i α_i × log(u_i(n) − d_i)  
where P = Pareto frontier, α_i = current Lyapunov weights, d_i = DISAGREE_* (0.1 for all four).  
If for any i we have u_i(n) − d_i ≤ 0, that candidate is **invalid** for Nash.

**Code:** `election.c` lines 143–191

```c
static uint32_t nash_bargaining_selection(stellar_candidate_t *candidates, size_t count) {
  float disagreement[4] = {DISAGREE_BATTERY, DISAGREE_UPTIME, DISAGREE_TRUST, DISAGREE_LINKQ};
  stellar_weights_t sw = metrics_get_stellar_weights();

  float max_nash_product = -1e9f;
  uint32_t winner = 0;
  for (size_t i = 0; i < count; i++) {
    if (!candidates[i].on_pareto_frontier) continue;

    float nash_product = 0.0f;
    bool valid = true;
    for (int j = 0; j < 4; j++) {
      float surplus = candidates[i].utility_values[j] - disagreement[j];
      if (surplus <= 0.0f) { valid = false; break; }
      nash_product += sw.weights[j] * logf(surplus);
    }

    if (valid && (nash_product > max_nash_product ||
                  (fabsf(nash_product - max_nash_product) < 1e-6f &&
                   (winner == 0 || candidates[i].node_id < winner)))) {
      max_nash_product = nash_product;
      winner = candidates[i].node_id;
    }
  }
  return winner;
}
```

Tie-break: **lowest node_id** (so all nodes agree on the same winner).

---

## 10. Full STELLAR Election Flow (election_run_stellar)

**File:** `election.c` lines 248–444. Step-by-step with line references:

1. **Update weights** (254): `metrics_update_stellar_weights()` — variance, entropy confidence, target weights, Lyapunov step, simplex projection, V(w,t).
2. **Get neighbors** (257–258): `neighbor_manager_get_all(neighbors, MAX_NEIGHBORS)`.
3. **Build candidate list** (260–339):  
   - **Self** (264–294): raw_metrics from `metrics_get_current()`, uptime_norm = uptime_seconds / (UPTIME_MAX_DAYS×86400), utility_values from the four φ_* functions, centrality from `compute_centrality(neighbors, neighbor_count)`.  
   - **Neighbors** (296–339): only if `neighbor_manager_is_in_cluster` and `verified` and `trust >= TRUST_FLOOR`. Same four utilities; centrality = 0.8f.
4. **Edge cases** (341–352): 0 candidates → return 0; 1 candidate → return that node_id.
5. **Phase 2 — Pareto** (364–367): `compute_pareto_frontier(candidates, candidate_count)`.
6. **STELLAR score for all** (378–388): For each candidate, build a temporary `node_metrics_t` from raw_metrics and call `metrics_compute_stellar_score(&tmp, pareto_rank, centrality)`; store in `candidates[i].stellar_score`.
7. **Phase 3 — Nash** (391–393): `winner = nash_bargaining_selection(candidates, candidate_count)`.
8. **Fallback 1** (396–409): If winner==0, choose Pareto candidate with **maximum stellar_score** (tie-break: lowest node_id).
9. **Fallback 2** (414–426): If still 0, choose **any** candidate with maximum stellar_score (tie-break: lowest node_id).
10. **Return** (444): winner node_id.

---

## 11. Where STELLAR Score Is Used Outside Election

- **metrics_update()** (`metrics.c` 391–441): When `USE_STELLAR_ALGORITHM` is 1, each tick we call `metrics_update_stellar_weights()`, compute Pareto rank (raw metric dominance over neighbors) and a connectivity-based centrality (n_count/MAX_NEIGHBORS), then `raw_stellar = metrics_compute_stellar_score(...)`. We then **smooth** it:  
  `stellar_score_smoothed = STELLAR_SCORE_EWMA_ALPHA * raw_stellar + (1 − STELLAR_SCORE_EWMA_ALPHA) * stellar_score_smoothed`  
  and set `current_metrics.stellar_score` and `composite_score` to this smoothed value. This is what re-election checks and BLE “score” use, so brief dips don’t trigger flapping.
- **state_machine.c** (e.g. CH yield): When checking re-election we compare `neighbors[i].score` vs `self_metrics.composite_score`; that composite is the smoothed STELLAR score when STELLAR is enabled.

---

## 12. Summary: Formula and Code Reference Table

| Item | Formula / Role | Code location |
|------|----------------|----------------|
| φ_battery(b) | (1−e^{−λb})/(1−e^{−λ}), λ=2 | metrics.c 737–744 |
| φ_uptime(u) | tanh(λu), λ=1 | metrics.c 755–757 |
| φ_trust(t) | t²(3−2t) | metrics.c 769–775 |
| φ_linkq(l) | l^{1/γ}, γ=2 | metrics.c 786–792 |
| Variance EWMA | σ²←α Δ²+(1−α)σ² | metrics.c 583–608 |
| H(σ²) | 0.5 ln(2πe σ²) | metrics.c 541–546 |
| c_i | exp(−γ H_i)/Σexp(−γ H_j) | metrics.c 549–574 |
| w*_i | base_i×(1+0.5(c_i−0.25)), then normalize | metrics.c 641–661 |
| Lyapunov gradient | (w−w*)+β(w−w_eq) | metrics.c 673–682 |
| Project simplex | clamp min, then sum-norm | metrics.c 611–628 |
| V(w,t) | ½‖w−w*‖² + λ‖∇‖² | metrics.c 688–696 |
| Ψ(n) | (Σ w_i φ_i)×κ + ρ | metrics.c 805–848 |
| κ (election, self) | 1 − var(RSSI)/400 | election.c 208–234 |
| Pareto a≻b | ∀i u_i(a)≥u_i(b) ∧ ∃j u_j(a)>u_j(b) | election.c 90–105 |
| Nash | argmax Σ α_i log(u_i−d_i) over P | election.c 143–191 |
| Fallbacks | max Ψ on Pareto, then max Ψ overall | election.c 396–426 |

This completes the A-to-Z of the STELLAR algorithm and its implementation in this codebase.
