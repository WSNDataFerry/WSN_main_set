#pragma once

// =============================================================================
// Algorithm robustness: single CH, hysteresis, score smoothing, tie-break by
// node_id, election stagger. Sensors/battery: real when hw connected, dummy
// when not (so cluster still runs and reports SENSORS_REAL/BATTERY_REAL).
// =============================================================================

// Cluster Config
#define CLUSTER_KEY_SIZE 32
#define MAX_NEIGHBORS 10
#define MAX_CLUSTER_SIZE 5
#define ELECTION_WINDOW_MS 10000
// Note: Deterministic tie-break by lowest node_id in election ensures single
// CH. No stagger needed — Nash Bargaining + node_id tie-break is sufficient.
#define SLOT_DURATION_SEC 10 // User Requested: 10s per node (Githmi Style)

// STELLAR / TDMA Superframe Phases
// One superframe = STELLAR phase + DATA phase.
// STELLAR phase: focus on exchanging metrics and running elections.
// DATA phase: fixed CH, TDMA schedule for sensor data to CH.
#define STELLAR_PHASE_MS 20000 // 20s STELLAR metrics/election window
#define DATA_PHASE_MS 20000    // 20s TDMA data window
#define PHASE_GUARD_MS 5000 // Guard before TDMA slots start inside DATA phase
#define STATE_MACHINE_TASK_STACK_SIZE                                          \
  12288 // Increased from 8192 to prevent stack overflow (with compression)
#define STATE_MACHINE_TASK_PRIORITY 5
#define METRICS_TASK_STACK_SIZE 4096
#define METRICS_TASK_PRIORITY 4

// Weights
#define WEIGHT_BATTERY 0.3f
#define WEIGHT_UPTIME 0.2f
#define WEIGHT_TRUST 0.3f
#define WEIGHT_LINK_QUALITY 0.2f
#define HSR_WEIGHT 0.4f
#define REPUTATION_WEIGHT 0.3f
#define PDR_WEIGHT 0.3f
#define RSSI_EWMA_ALPHA 0.2f
#define NEIGHBOR_TIMEOUT_MS                                                    \
  60000 // Increased to 60s to survive 20s Stellar + 20s Data phases
#define CLUSTER_RADIUS_RSSI_THRESHOLD -85.0f
// CRITICAL FIX: CH beacon timeout must survive the full DATA phase (20s) where
// BLE scanning is disabled. Set to 45s to cover DATA + partial STELLAR + buffer.
// The actual timeout check is now phase-aware (disabled during DATA phase).
#define CH_BEACON_TIMEOUT_MS                                                   \
  45000 // 45s: DATA(20s) + STELLAR(20s) + 5s buffer for stability
#define CH_MEMBER_HYSTERESIS_MS                                                \
  8000 // 8s hysteresis to prevent oscillation after beacon timeout
#define CH_MEMBER_MISSING_CONSECUTIVE                                          \
  15 // Consecutive runs with CH missing before starting hysteresis (stability)
// CH loss detection during STELLAR phase only. State machine runs ~1s/cycle,
// so 10 misses ≈ 10s to confirm CH loss (conservative to prevent false alarms).
#define CH_MISS_THRESHOLD 10 // Consecutive missed CH beacons before re-election

// STELLAR
// When 1: allow simulated sensors/battery in main loop when no hardware is
// available. Keep this enabled so the firmware produces dummy sensor values
// when real sensors are absent. Battery simulation is disabled separately in
// `main/ms_node.c` (we now use ADC for battery reads and do not synthesize
// battery percentages).
#define ENABLE_MOCK_SENSORS 1
#define USE_STELLAR_ALGORITHM 1
// Smooth STELLAR score to avoid re-election from brief dips (0.1 = slow, 0.3 =
// fast)
#define STELLAR_SCORE_EWMA_ALPHA                                               \
  0.25f // Tuned: faster reaction to events (was 0.15)
#define ENTROPY_GAMMA 1.0f
#define EWMA_VARIANCE_ALPHA 0.1f
#define MIN_WEIGHT_VALUE 0.05f
#define LYAPUNOV_BETA 0.1f
#define LYAPUNOV_ETA                                                           \
  0.05f // Tuned: faster convergence visible in plots (was 0.01)
#define LYAPUNOV_LAMBDA 0.01f
#define CONVERGENCE_THRESHOLD 0.001f
#define UTILITY_LAMBDA_B 2.0f
#define UTILITY_LAMBDA_U 1.0f
#define UTILITY_GAMMA_L 2.0f
#define UPTIME_MAX_DAYS 7.0f
#define PARETO_DELTA 0.1f
#define CENTRALITY_EPSILON                                                     \
  0.5f // κ(n) = 1/(1 + ε*(1-centrality)); ε=0.5 gives κ∈[0.67,1.0]
// Lyapunov equilibrium weights (w_eq): the initial/reference weight vector
// used as the second term in the Lyapunov gradient: ∂V/∂w = (w-w*) + β(w-w_eq)
#define W_EQ_BATTERY WEIGHT_BATTERY
#define W_EQ_UPTIME WEIGHT_UPTIME
#define W_EQ_TRUST WEIGHT_TRUST
#define W_EQ_LINK_QUALITY WEIGHT_LINK_QUALITY
#define DISAGREE_BATTERY 0.1f
#define DISAGREE_UPTIME 0.1f
#define DISAGREE_TRUST 0.1f
#define DISAGREE_LINKQ 0.1f
#define PDR_EWMA_ALPHA 0.1f
#define TRUST_FLOOR 0.2f
#define BATTERY_LOW_THRESHOLD 0.2f
#define LINK_QUALITY_FLOOR 0.2f

// ESP-NOW
#define ESP_NOW_CHANNEL 1
#define ESP_NOW_PMK "pmk1234567890123"
#define ESP_NOW_LMK "lmk1234567890123"

// Persistence
#define SPIFFS_BASE_PATH "/spiffs"

// BLE Configuration
#define BLE_DEVICE_NAME_PREFIX "MSN-"
#define BLE_SCAN_INTERVAL_MS 100 // Scan interval
#define BLE_SCAN_WINDOW_MS 50 // Scan window (50% duty cycle to allow CPU idle)
