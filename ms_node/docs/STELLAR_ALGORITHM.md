# STELLAR Algorithm: Multi-Metric Distributed Cluster Head Election

## Overview

**STELLAR** (Secure Trust-Enhanced Lyapunov-optimized Leader Allocation for Resilient networks) is a distributed multi-metric algorithm for autonomous cluster head (CH) election in wireless sensor networks. It enables sensor nodes to self-organize into clusters without centralized coordination, selecting the most suitable node as the cluster head based on battery health, system uptime, trustworthiness, and link quality.

**Key Innovation:** Combines **Pareto dominance** (multi-objective optimization), **Lyapunov adaptive weights**, and **deterministic tie-breaking** to produce stable, resilient clusters in delay-tolerant networks.

---

## Table of Contents

1. [Core Concepts](#1-core-concepts)
2. [Mathematical Formulation](#2-mathematical-formulation)
3. [Implementation Details](#3-implementation-details)
4. [Code Architecture](#4-code-architecture)
5. [Metric Calculations](#5-metric-calculations)
6. [Pareto Frontier & Dominance](#6-pareto-frontier--dominance)
7. [Election Process](#7-election-process)
8. [Adaptive Weight Mechanism (Lyapunov)](#8-adaptive-weight-mechanism-lyapunov)
9. [Tuning & Configuration](#9-tuning--configuration)
10. [Examples & Interpretation](#10-examples--interpretation)
11. [Troubleshooting](#11-troubleshooting)

---

## 1. Core Concepts

### What is Cluster Head Election?

In a wireless sensor network, nodes self-organize into **clusters**. Each cluster has a **cluster head (CH)**, which:
- **Receives** sensor data from member nodes via TDMA slots
- **Aggregates & stores** data locally (MSLG format)
- **Uploads** accumulated data to the UAV/base station when available
- **Broadcasts** TDMA schedule so members know when to transmit

Without a CH, every member would transmit independently → radio collisions, wasted energy, poor reliability.

### Why Distributed?

In **remote deployments** (e.g., wildlife monitoring in forests), there is **no centralized coordinator**. Nodes must:
- Discover their neighbors (via BLE scanning)
- Exchange metrics autonomously (via BLE advertising)
- Vote for the best CH without communication overhead
- Accept the outcome deterministically (no arguments!)

### STELLAR's Approach

STELLAR evaluates **4 metrics per node**:

| Metric | Purpose | Example |
|--------|---------|---------|
| **Battery %** | Node with fresh batteries → less risk of sudden failure | 80% vs 20% |
| **Uptime** | Long-running node → proven stability | 7 days vs 1 hour |
| **Trust** | Reliable communication history → dependable data forwarding | 95% delivery vs 50% |
| **Link Quality** | Strong radio signal → lower re-transmissions needed | RSSI -50 dBm vs -85 dBm |

**Result:** Probabilistic guarantee that the election converges to a **near-optimal CH** that balances all 4 criteria.

---

## 2. Mathematical Formulation

### STELLAR Utility Score

Each node computes a **utility score** Ψ(n) using the formula:

$$\Psi(n) = \kappa(n) \cdot \sum_{i} w_i(t) \cdot \varphi_i(m_i) + \alpha \cdot \text{ParetoRank}(n)$$

Where:

| Symbol | Meaning | Range | Notes |
|--------|---------|-------|-------|
| **Ψ(n)** | Total utility of node n | [0, 100+] | Higher = better CH candidate |
| **κ(n)** | Centrality (neighbor influence) | [0.67, 1.0] | More neighbors → higher score |
| **wᵢ(t)** | Dynamic weight for metric i | [0.0, 1.0] | Adapts via Lyapunov feedback; Σ wᵢ = 1.0 |
| **φᵢ(mᵢ)** | Utility transform of metric i | [0.0, 1.0] | Non-linear mapping (e.g., sigmoid) |
| **mᵢ** | Raw metric value i | Variable | Battery %, uptime, trust score, RSSI |
| **ParetoRank(n)** | Count of nodes n dominates | 0 to N-1 | Tiebreaker when utility scores tied |
| **α** | Pareto weight | 0.001 | Small: Pareto is secondary tiebreaker |

### Component Details

#### Centrality κ(n)

$$\kappa(n) = \frac{1}{1 + \varepsilon(1 - c_n)}$$

Where:
- **cₙ** = normalized neighbor count = current_neighbors / MAX_NEIGHBORS
- **ε** = 0.5 (tuning constant)
- **κ(n)** ∈ [0.67, 1.0]

**Intuition:** Nodes with more neighbors (more central in the cluster) get a multiplier boost (up to 1.5x their utility).

**Example:** 
- Node with 0 neighbors: κ = 1 / (1 + 0.5×1) = 0.67
- Node with 10 neighbors (MAX): κ = 1 / (1 + 0.5×0) = 1.0

#### Utility Transforms φᵢ(mᵢ)

Each metric is transformed into [0, 1] range via **non-linear curves** that prevent extreme values from dominating:

**Battery φ_battery:**
$$\varphi_{\text{batt}}(b) = \frac{1 - e^{-\lambda b}}{1 - e^{-\lambda}}$$

- λ = 0.1 (tuning constant)
- b ∈ [0, 100] (battery %)
- **Characteristic:** Concave curve; 100% → 1.0, 50% → 0.73, 0% → 0.0

**Uptime φ_uptime:**
$$\varphi_{\text{uptime}}(u) = \tanh(\lambda u)$$

- λ = 0.0001 (very gentle slope)
- u ∈ [0, 7 days] (seconds, normalized to 7-day saturate point)
- **Characteristic:** Smooth S-curve; saturates at 1.0 after 7 days

**Trust φ_trust:**
$$\varphi_{\text{trust}}(t) = t^2 (3 - 2t)$$

- t ∈ [0.0, 1.0] (composite trust score)
- **Characteristic:** Smooth step function; 0.5 → 0.25, 1.0 → 1.0; penalizes low trust heavily

**Link Quality φ_linkq:**
$$\varphi_{\text{linkq}}(l) = l^{1/\gamma}$$

- l = RSSI_dBm × PER (product of RSSI EWMA and packet error rate EWMA)
- γ = 2.0 (power exponent; makes link quality less noisy)
- **Characteristic:** Emphasizes strong links; weak links below threshold → near 0

#### Lyapunov Adaptive Weights wᵢ(t)

The **weight vector w(t) = [w_battery, w_uptime, w_trust, w_linkq]** adapts over time to balance conflicting objectives:

$$\frac{dw}{dt} = -\eta \nabla V(w) - \beta(w - w_{\text{eq}})$$

Where:
- **V(w)** = variance of weighted utilities (Lyapunov function)
- **η** = 0.01 (learning rate)
- **β** = 0.02 (equilibrium restoring force)
- **w_eq** = [0.30, 0.20, 0.30, 0.20] (target default weights)

**Intuition:** If one metric dominates (e.g., battery always highest), the algorithm **automatically reduces its weight** and **increases weights of underutilized metrics**. Over time, w(t) converges to a stable point where all metrics contribute.

**In Practice:** Default weights used unless entropy trigger occurs. See [Adaptive Weight Mechanism](#8-adaptive-weight-mechanism-lyapunov).

---

## 3. Implementation Details

### File Structure

| File | Purpose | Key Functions |
|------|---------|----------------|
| [election.c](../main/election.c) | Core STELLAR algorithm | `election_stellar_score()` `pareto_dominates()` |
| [metrics.c](../main/metrics.c) | Metric computation & EWMA tracking | `metrics_compute_stellar_score()` `metrics_update_pdrf()` |
| [config.h](../main/config.h) | Algorithm tuning constants | `STELLAR_WEIGHT_*`, `LAMBDA_*`, `EPSILON_CENTRALITY` |
| [neighbor_manager.c](../main/neighbor_manager.c) | Neighbor table & RSSI/trust tracking | `neighbor_update_rssi()` `neighbor_get_all()` |

### Core Algorithm Flow

```c
// 1. STELLAR Phase (0-20s): Neighbors exchange metrics via BLE
for (20 seconds) {
    ble_scan();                    // Receive neighbor beacons
    ble_advertise();               // Send own score packet
    neighbor_table_update();       // Update from received metrics
}

// 2. Election Window (10-20s from start): Compute scores
if (time_ms % STELLAR_PHASE_MS > ELECTION_WINDOW_MS) {
    my_score = election_stellar_score();        // My utility
    
    for each neighbor {
        neighbor_score = neighbor.cached_score; // From BLE beacon
        if (neighbor_score > my_score) {
            neighbor_is_better = true;
        }
    }
    
    if (neighbor_is_better) {
        elected_ch = best_neighbor;             // Someone else is better
    } else {
        elected_ch = me;                        // I am best!
    }
}

// 3. State Transition (at 20s boundary)
if (elected_ch == me) {
    state = STATE_CH;
} else {
    state = STATE_MEMBER;
}
```

---

## 4. Code Architecture

### election.c: Core Scoring

```c
// Main entry point
float election_stellar_score(void) {
    // 1. Get current metrics
    float battery = metrics_get_battery_pct();
    float uptime = metrics_get_uptime_secs();
    float trust = metrics_get_trust_score();
    float link_quality = metrics_get_link_quality();
    
    // 2. Apply utility transforms
    float phi_battery = utility_exponential(battery, LAMBDA_BATTERY);
    float phi_uptime = utility_tanh(uptime, UPTIME_MAX_SECS);
    float phi_trust = utility_smooth_step(trust);
    float phi_linkq = utility_power_law(link_quality, GAMMA_LINKQ);
    
    // 3. Compute weighted sum
    float weighted_utility = 
        STELLAR_WEIGHT_BATTERY   * phi_battery +
        STELLAR_WEIGHT_UPTIME    * phi_uptime +
        STELLAR_WEIGHT_TRUST     * phi_trust +
        STELLAR_WEIGHT_LINKQ     * phi_linkq;
    
    // 4. Apply centrality multiplier
    uint8_t neighbor_count = neighbor_table_count();
    float centrality = 1.0 / (1.0 + EPSILON_CENTRALITY * (1.0 - neighbor_count / (float)MAX_NEIGHBORS));
    float score = centrality * weighted_utility;
    
    // 5. Add Pareto tiebreaker (small boost for less-dominated nodes)
    uint8_t pareto_rank = 0;
    for each neighbor {
        if (pareto_dominates(me, neighbor)) {
            pareto_rank++;
        }
    }
    score += ALPHA_PARETO * pareto_rank;
    
    return score;
}

// Pareto dominance: node A dominates B if A is ≥ on all metrics and > on at least one
bool pareto_dominates(node_t *a, node_t *b) {
    bool better_on_at_least_one = false;
    
    // Check all 4 metrics
    if (a->battery < b->battery) return false;
    if (a->battery > b->battery) better_on_at_least_one = true;
    
    if (a->uptime < b->uptime) return false;
    if (a->uptime > b->uptime) better_on_at_least_one = true;
    
    if (a->trust < b->trust) return false;
    if (a->trust > b->trust) better_on_at_least_one = true;
    
    if (a->link_quality < b->link_quality) return false;
    if (a->link_quality > b->link_quality) better_on_at_least_one = true;
    
    return better_on_at_least_one;  // A is better on at least 1 metric, not worse on any
}
```

### metrics.c: Metric Updates

```c
// Called every 1 second by metrics_task
void metrics_update_ewma(void) {
    // RSSI EWMA: 20% new, 80% old (α = 0.2)
    rssi_ewma = ALPHA_RSSI * latest_rssi + (1.0 - ALPHA_RSSI) * rssi_ewma;
    
    // PER (Packet Error Rate) EWMA
    uint8_t missed = expected_packets - delivered_packets;
    float per = (float)missed / (expected_packets);
    per_ewma = ALPHA_PER * per + (1.0 - ALPHA_PER) * per_ewma;
    
    // Link Quality = product of normalized RSSI and (1 - PER)
    link_quality = normalized_rssi(rssi_ewma) * (1.0 - per_ewma);
    
    // Trust Score (composite)
    float hmac_success_rate = auth_get_hmac_success_rate();
    float host_switch_rate = pdr_get_hsr();
    float pdr = pdr_get_pdr();
    trust_score = 0.4 * hmac_success_rate + 0.3 * host_switch_rate + 0.3 * pdr;
}

// Battery %: Read from INA219 power monitor
float metrics_get_battery_pct(void) {
    float voltage = ina219_read_bus_voltage();
    
    // Linear mapping: 2.8V (0%) to 4.2V (100%)
    float percent = (voltage - 2.8) / (4.2 - 2.8) * 100.0;
    
    // Clamp 0-100%
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    
    // Floor at 20% to prevent re-election storms
    if (percent < 20.0 && percent > 0.0) percent = 20.0;
    
    return percent;
}

// Uptime: Normalized to 7-day saturation point
float metrics_get_uptime_secs(void) {
    uint64_t uptime = esp_timer_get_time() / 1_000_000;  // seconds since boot
    uint64_t max_uptime = 7 * 24 * 3600;  // 7 days in seconds
    
    if (uptime > max_uptime) {
        return 1.0;  // Saturate at 1.0
    }
    
    return (float)uptime / max_uptime;
}
```

---

## 5. Metric Calculations

### Battery Percentage

**Raw Input:** INA219 bus voltage (V)

**Processing:**
1. Read voltage from power monitor chip: 2.8V (empty) to 4.2V (full)
2. Linear interpolation: 100% = 4.2V, 0% = 2.8V
3. Clamp to [0, 100]%
4. **Floor at 20%:** If battery drops below 20% but is still positive, artificially set to 20% to prevent rapid re-election

**Utility Transform:**
$$\varphi_{\text{batt}}(b) = \frac{1 - e^{-0.1b}}{1 - e^{-10}}$$

**Shape:** 
- 100% → 1.0
- 50% → 0.73
- 20% (min usable) → 0.22
- 0% (dead) → 0.0

**Why floor at 20%?** Prevents "flapping" where a dying node briefly has high score, is elected CH, then dies, triggering re-election.

### Uptime

**Raw Input:** ESP timer (microseconds since boot)

**Processing:**
1. Convert microseconds to seconds
2. Normalize to 7-day maximum: `uptime_normalized = min(uptime_secs, 604800) / 604800`
3. Saturate at 1.0 (after 7 days, uptime contributes equally as a freshly-booted node — prevents old nodes from dominating forever)

**Utility Transform:**
$$\varphi_{\text{uptime}}(u) = \tanh(0.0001 \times u)$$

**Shape:**
- 0 days → 0.0
- 1 day → 0.084
- 3 days → 0.24
- 5 days → 0.38
- 7 days → 0.55 (saturates)

**Why saturation?** Prevents old nodes from permanently dominating. Balance with battery: old + low battery → lower score.

### Trust Score

**Raw Input:** Composite of 3 sub-metrics (each [0, 1]):

| Sub-metric | Weight | Meaning |
|------------|--------|---------|
| HMAC Success Rate | 40% | Fraction of packets with valid HMAC signature (auth.c) |
| Host Switch Rate (HSR) | 30% | Rate of successful packet deliveries (PDR module) |
| Packet Delivery Ratio (PDR) | 30% | Fraction of expected packets received |

**Processing:**
```
trust_score = 0.4 × hmac_success_rate 
            + 0.3 × host_switch_rate 
            + 0.3 × pdr
```

**Utility Transform:**
$$\varphi_{\text{trust}}(t) = t^2(3 - 2t)$$

**Shape:**
- 0.0 → 0.0 (untrusted node = 0 contribution)
- 0.25 → 0.039 (low trust heavily penalized)
- 0.5 → 0.25
- 0.75 → 0.62
- 1.0 → 1.0 (fully trusted = full contribution)

**Why non-linear?** Low trust is dangerous (data loss); high trust is critical. The smooth-step curve amplifies the difference between 0.5 and 0.9 trust.

### Link Quality

**Raw Input:** RSSI (dBm) and PER (packet error rate)

**Processing:**
1. **RSSI EWMA:** Exponential moving average (α = 0.2)
   - Recent: -75 dBm, History avg: -70 dBm
   - RSSI_EWMA = 0.2×(-75) + 0.8×(-70) = -71 dBm
2. **Normalize RSSI:** `-75 dBm → 0.0`, `-40 dBm → 1.0` (floor threshold)
3. **PER EWMA:** Exponential moving average of packet loss (α = 0.05)
4. **Link Quality:** `(1.0 - PER) × normalized_RSSI`

**Utility Transform:**
$$\varphi_{\text{linkq}}(l) = l^{1/\gamma}, \quad \gamma = 2.0$$

**Shape:**
- Weak link (0.2) → 0.45
- Medium link (0.5) → 0.71
- Strong link (0.9) → 0.95

**Why power law?** RSSI is noisy; this smooths out random fluctuations while maintaining discrimination.

---

## 6. Pareto Frontier & Dominance

### What is Pareto Dominance?

Node **A dominates node B** if:
- A's battery ≥ B's battery, **AND**
- A's uptime ≥ B's uptime, **AND**
- A's trust ≥ B's trust, **AND**
- A's link quality ≥ B's link quality, **AND**
- A is strictly better on **at least 1** metric

**Example:**
- A: [90%, 5 days, 0.95 trust, -50 dBm]
- B: [80%, 3 days, 0.90 trust, -55 dBm]
→ **A dominates B** (better on all 4)

- C: [95%, 2 days, 0.85 trust, -60 dBm]
- D: [90%, 5 days, 0.90 trust, -50 dBm]
→ **Neither dominates** (C better on battery, D better on uptime & link quality)

### Pareto Rank (Tiebreaker)

When two nodes have **nearly identical STELLAR scores**, the **Pareto Rank** breaks ties:

```
Pareto_Rank(n) = count of nodes that n dominates
```

**Higher Pareto Rank** → less dominated by peers → slightly better CH choice.

**In Code:**
```c
uint8_t pareto_rank = 0;
for (int i = 0; i < neighbor_count; i++) {
    if (pareto_dominates(self, neighbors[i])) {
        pareto_rank++;
    }
}
// Final score += (0.001 * pareto_rank)  // Small boost
```

---

## 7. Election Process

### Timeline Within STELLAR Phase (0-20 seconds)

```
Time 0-10s:    ELECTION_WINDOW_MS
               ├── BLE scanning ENABLED
               ├── Receive neighbor beacons with cached scores
               ├── Update neighbor table with RSSI, battery, uptime, trust
               └── NO election computations yet

Time 10-20s:   ELECTION DECISION
               ├── Compute my own STELLAR score
               ├── Compare against cached neighbor scores
               └── Decide: Am I best? → I am CH
                   Otherwise: → I am MEMBER of best neighbor

Time 20s:      STATE TRANSITION
               ├── Cache elected CH node_id
               ├── Enter DATA phase
               └── If I am CH: Enable ESP-NOW RX, prepare TDMA schedule
                   If I am MEMBER: Enable ESP-NOW TX, wait for schedule
```

### Deterministic Tie-Breaking

**Rule:** If two nodes have identical STELLAR scores, the node with the **lower node_id wins**.

**Why deterministic?** Prevents:
- **Oscillation:** No "election forever" loops
- **Ambiguity:** Both nodes agree on the same CH
- **Overhead:** Single callback, no re-voting

**In Code:**
```c
if (fabs(my_score - best_neighbor_score) < EPSILON_SCORE_TIE) {
    // Tied scores
    if (my_node_id < best_neighbor_node_id) {
        elected_ch = ME;
    } else {
        elected_ch = best_neighbor;
    }
}
```

---

## 8. Adaptive Weight Mechanism (Lyapunov)

### Why Adaptive Weights?

In a diverse network, one metric may initially dominate:
- **Scenario 1:** All nodes have similar battery → battery weight should decrease
- **Scenario 2:** Network is very reliable → trust weight should decrease
- **Scenario 3:** All nodes near each other → link quality weight should decrease

**Lyapunov Gradient Descent** detects these imbalances and adapts weights to maintain **information diversity**.

### Algorithm

Every 10 seconds (in `metrics_task`):

1. **Compute weighted utility for each node:**
   ```
   Ψ(n) = κ(n) × Σ wᵢ φᵢ(mᵢ)
   ```

2. **Compute variance V(w) across all nodes' utilities:**
   ```
   V(w) = Σ(Ψ(n) - mean(Ψ))²
   ```

3. **Compute gradient ∇V:**
   ```
   ∇V = [∂V/∂w_battery, ∂V/∂w_uptime, ∂V/∂w_trust, ∂V/∂w_linkq]
   ```

4. **Update weights via gradient descent + equilibrium restoring:**
   ```
   w_new = w_old - η × ∇V(w) - β × (w_old - w_eq)
   
   η = 0.01 (learning rate)
   β = 0.02 (restoring force)
   w_eq = [0.30, 0.20, 0.30, 0.20] (default)
   ```

5. **Normalize:** Ensure Σ wᵢ = 1.0

6. **Clamp:** Each wᵢ ∈ [0.1, 0.5] (prevent extreme weights)

### Practical Effect

**Example:** If battery variance is very low:
- Weight on battery decreases: 0.30 → 0.25 → 0.20
- Weights on uptime/trust/linkq increase to compensate
- **Result:** Now uptime/trust differentiate nodes more

**Default Behavior:** If adaptive weights are disabled (entropy-based trigger), use fixed defaults:
- **Battery:** 0.30 (recharge risk is real)
- **Uptime:** 0.20 (stability matters less than equipment health)
- **Trust:** 0.30 (reliable forwarding is critical)
- **Link Quality:** 0.20 (local geometry is important but secondary)

---

## 9. Tuning & Configuration

### Key Parameters (config.h)

```c
// ===== STELLAR Weights (fixed, not adaptive) =====
#define STELLAR_WEIGHT_BATTERY   0.30f
#define STELLAR_WEIGHT_UPTIME    0.20f
#define STELLAR_WEIGHT_TRUST     0.30f
#define STELLAR_WEIGHT_LINKQ     0.20f

// ===== Utility Transform Parameters =====
#define LAMBDA_BATTERY          0.10f   // Exponential curve sharpness
#define UPTIME_MAX_SECS        604800   // 7 days saturation
#define GAMMA_LINKQ             2.0f    // Power law exponent

// ===== Centrality & Pareto =====
#define EPSILON_CENTRALITY      0.5f    // κ(n) = 1 / (1 + ε(1 - cₙ))
#define ALPHA_PARETO            0.001f  // Pareto rank weight

// ===== EWMA Smoothing (moving averages) =====
#define ALPHA_RSSI              0.20f   // RSSI: 20% new, 80% history
#define ALPHA_PER               0.05f   // PER: 5% new, 95% history
#define ALPHA_TRUST             0.10f   // Trust: 10% new, 90% history

// ===== Election Timing =====
#define STELLAR_PHASE_MS       20000    // BLE discovery phase
#define ELECTION_WINDOW_MS     10000    // Last 10s: compute scores
#define EPSILON_SCORE_TIE      0.001f   // Tied score threshold

// ===== Lyapunov (adaptive weight updates) =====
#define LYAPUNOV_LEARNING_RATE  0.01f   // η
#define LYAPUNOV_RESTORING_BETA 0.02f   // β
#define WEIGHT_MIN              0.10f   // Lower clamp
#define WEIGHT_MAX              0.50f   // Upper clamp
#define ENTROPY_TRIGGER_RATE    0.05f   // Activate adapt if entropy drop > 5%

// ===== Hardware Thresholds =====
#define BATTERY_MIN_USABLE_PCT  20.0f   // Floor battery at 20%
#define RSSI_FLOOR_DBM         -75      // Weak link threshold
#define MAX_NEIGHBORS           10      // Neighbor table size
#define CH_BEACON_TIMEOUT_MS   45000    // Member waits 45s for CH beacon
```

### Tuning Guide

| Goal | Parameter | Adjust |
|------|-----------|--------|
| Prefer long-lived nodes | ↑ `STELLAR_WEIGHT_UPTIME` (0.30) | Increase from 0.20 |
| Prefer reliable nodes | ↑ `STELLAR_WEIGHT_TRUST` (0.30) | Increase from 0.20 |
| Penalize low battery | ↑ `LAMBDA_BATTERY` (0.10) | Try 0.15 for sharper curve |
| Increase stability | ↑ `EPSILON_CENTRALITY` (0.5) | Try 0.3 to reduce neighbor influence |
| Aggressive weight adaptation | ↑ `LYAPUNOV_LEARNING_RATE` (0.01) | Try 0.05 |
| Fast RSSI response | ↑ `ALPHA_RSSI` (0.20) | Try 0.30 (less history) |
| Dampen noisy RSSI | ↓ `ALPHA_RSSI` (0.20) | Try 0.10 (more history) |

---

## 10. Examples & Interpretation

### Scenario 1: Simple 3-Node Cluster

**Nodes:**
```
Node A: battery=90%, uptime=3 days, trust=0.95, RSSI=-50 dBm
Node B: battery=70%, uptime=5 days, trust=0.90, RSSI=-60 dBm
Node C: battery=60%, uptime=1 day,  trust=0.85, RSSI=-70 dBm
```

**Step 1: Compute utilities**
```
φ_battery(90%)  = 1.0      φ_battery(70%)  = 0.88       φ_battery(60%)  = 0.80
φ_uptime(3d)    = 0.24     φ_uptime(5d)    = 0.38       φ_uptime(1d)    = 0.084
φ_trust(0.95)   = 0.92     φ_trust(0.90)   = 0.87       φ_trust(0.85)   = 0.81
φ_linkq(-50)    = 0.95     φ_linkq(-60)    = 0.75       φ_linkq(-70)    = 0.50
```

**Step 2: Weighted sum (weights: 0.30/0.20/0.30/0.20)**
```
Node A: 0.30×1.0 + 0.20×0.24 + 0.30×0.92 + 0.20×0.95 
      = 0.30 + 0.048 + 0.276 + 0.19 = 0.814

Node B: 0.30×0.88 + 0.20×0.38 + 0.30×0.87 + 0.20×0.75
      = 0.264 + 0.076 + 0.261 + 0.15 = 0.751

Node C: 0.30×0.80 + 0.20×0.084 + 0.30×0.81 + 0.20×0.50
      = 0.24 + 0.017 + 0.243 + 0.10 = 0.600
```

**Step 3: Apply centrality (assume 2 neighbors per node, MAX=10)**
```
κ = 1 / (1 + 0.5 × (1 - 2/10)) = 1 / (1 + 0.5×0.8) = 1 / 1.4 = 0.714
```

**Step 4: Final scores (all multiply by κ, then add Pareto)**
```
Node A: 0.714 × 0.814 + 0.001 × 2 = 0.581 + 0.002 = 0.583
Node B: 0.714 × 0.751 + 0.001 × 0 = 0.536 + 0.000 = 0.536
Node C: 0.714 × 0.600 + 0.001 × 0 = 0.428 + 0.000 = 0.428

Result: Node A is elected CH (highest score)
```

**Interpretation:** Node A has the best balance of fresh battery (90%) + reasonable uptime (3d) + excellent trust (0.95) + strong signal (-50 dBm). Despite B having longer uptime (5d), A's battery & trust compensate.

---

### Scenario 2: Low Battery Detection

**Same cluster, but Node A loses power:**
```
Node A: battery=15%, uptime=3 days, trust=0.95, RSSI=-50 dBm (floored at 20%)
```

**Recalculation:**
```
φ_battery(20%) = 0.22  (drops from 1.0)

Node A: 0.30×0.22 + 0.20×0.24 + 0.30×0.92 + 0.20×0.95 
      = 0.066 + 0.048 + 0.276 + 0.19 = 0.580

Node A (with κ): 0.714 × 0.580 = 0.414

Now Node B scores: 0.536 → B is elected CH!
```

**Interpretation:** Even though A still has good uptime/trust, low battery is disqualifying. **Battery acts as a safety veto**, preventing failing hardware from taking CH responsibility.

---

## 11. Troubleshooting

### Issue: Same node always elected CH

**Cause:** Metrics are static (all nodes have similar battery, uptime, trust).

**Solution:**
1. Check if nodes are running long enough to accumulate uptime variance
2. Verify RSSI measurements are working (BLE RSSI should vary by position)
3. Enable adaptive weight adjustment: increase `LYAPUNOV_LEARNING_RATE` to 0.05

### Issue: Rapid CH flapping (re-election every superframe)

**Cause:** Scores tied or very close; noise in RSSI/trust metrics.

**Solution:**
1. Increase hysteresis: `#define HYSTERESIS_ELECTION_MS 8000` (wait 8s before re-election)
2. Increase `CH_BEACON_TIMEOUT_MS` from 45s to 60s
3. Smooth RSSI more: decrease `ALPHA_RSSI` from 0.20 to 0.10
4. Check: Is current CH broadcasting beacons every second?

### Issue: Cluster fragmented (nodes elect different CHs)

**Cause:** Neighbor discovery not synchronized; some nodes miss BLE broadcasts.

**Solution:**
1. Increase `STELLAR_PHASE_MS` from 20s to 30s (longer discovery window)
2. Increase BLE advertising power: check NimBLE TX power setting
3. Verify neighbor table is populated: connect via serial, check `neighbor_count`

### Issue: Energy consumption too high (battery draining fast)

**Cause:** STELLAR phase (BLE scanning) is power-hungry.

**Solution:**
1. Increase `STELLAR_PHASE_MS` → longer periods, fewer scans per day
2. Reduce BLE scan window: set `scan_window < scan_interval`
3. Enable power-save mode for members: only CH runs full BLE

---

## References

- [STELLAR Protocol Paper](https://example.com) *(hypothetical citation)*
- IEEE 802.15.4 Clustering Standards
- BLE Scanning Best Practices (Bluetooth SIG)
- Lyapunov Stability Theory (Wikipedia: Lyapunov function)

---

**Document Version:** 1.0 (March 2026)  
**Last Updated:** March 12, 2026  
**Platform:** ESP32-S3, ESP-IDF v5.3, FreeRTOS SMP  
**Author:** Wireless Sensor Network Dev Team
