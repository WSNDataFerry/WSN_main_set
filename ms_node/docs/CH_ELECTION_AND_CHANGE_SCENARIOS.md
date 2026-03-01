# CH Election and CH Change Scenarios

This document lists **every condition** under which the Cluster Head (CH) is chosen and under which the CH changes.

---

## 1. State flow (high level)

```
INIT (2s)
    → DISCOVER (5s)
        → MEMBER   (if existing valid CH found)
        → CANDIDATE (if no CH found)
            → CH      (if I won election)
            → MEMBER  (if someone else won)
            → DISCOVER (if no valid winner)
```

- **INIT:** After 2 seconds → always go to **DISCOVER**.
- **DISCOVER:** Run for 5 seconds. If a **valid CH** is seen (see Section 2) after ≥2s or at end of 5s → **MEMBER**. Else → **CANDIDATE**.
- **CANDIDATE:** After **ELECTION_WINDOW_MS** (10s) run **election_run()**.  
  - Winner = self → **CH**  
  - Winner = other → **MEMBER**  
  - Winner = 0 (no valid winner) → **DISCOVER**
- **CH** and **MEMBER** can later transition back to **CANDIDATE** or **MEMBER** when re-election or CH-loss conditions are met (Section 3).

---

## 2. When is a neighbor considered a “valid CH”?

Used by: **DISCOVER** (join as MEMBER), **MEMBER** (do I still have a CH?), **election_check_reelection_needed()**, **neighbor_manager_get_current_ch()**.

A neighbor is a **valid CH** only if **all** of the following hold:

| # | Condition | Config / meaning |
|---|-----------|-------------------|
| 1 | `neighbor.is_ch == true` | Node is advertising as CH. |
| 2 | `neighbor.verified == true` | HMAC-verified (same cluster key). |
| 3 | `neighbor.trust >= TRUST_FLOOR` | `TRUST_FLOOR = 0.2` |
| 4 | **CH beacon is timely** | `(now_ms - neighbor.ch_announce_timestamp) < CH_BEACON_TIMEOUT_MS` → **CH_BEACON_TIMEOUT_MS = 5000** (5 s). |

If several neighbors satisfy the above, **neighbor_manager_get_current_ch()** returns the one with **highest `neighbor.score`**. So “valid CH” = best such neighbor.

---

## 3. CH changing scenarios (when does the CH change?)

### 3.1 Current CH steps down (CH → MEMBER or CH → CANDIDATE)

These are checked **every state machine cycle** in **STATE_CH** via **election_check_reelection_needed()**. If it returns true, CH either **yields to another CH** or **goes to CANDIDATE**.

- **Yield allowed only if:**  
  `(s_current_phase == PHASE_STELLAR) || (other_ch != 0)`  
  So: during DATA phase we only yield if there is already another valid CH (conflict). During STELLAR we can yield for any re-election reason.

| # | Condition | Result |
|---|-----------|--------|
| 1 | **CH’s own battery low** and a **healthier** node exists | Re-election: CH → CANDIDATE. “Healthier” = battery ≥ BATTERY_LOW_THRESHOLD (0.2) or battery > CH + 0.05. |
| 2 | **CH’s own trust < TRUST_FLOOR** (0.2) | Re-election: CH → CANDIDATE. |
| 3 | **CH’s own link_quality < LINK_QUALITY_FLOOR** (0.2) | Re-election: CH → CANDIDATE. |
| 4 | **Another node also claims CH** (CH conflict): |  |
| 4a | Other CH’s **score > my score** (by > 0.01) | Yield: CH → MEMBER (follow other CH). |
| 4b | Other CH’s **score < my score** (by > 0.01) | Stay CH. |
| 4c | **Scores tied** (within 0.01) | **Lower node_id** keeps CH; the other yields → MEMBER. |

If we yield to an existing CH we go **MEMBER**. If re-election is needed but there is no other CH (e.g. low battery/trust/link), we go **CANDIDATE** and **election_reset_window()**.

### 3.2 Member loses CH (MEMBER → CANDIDATE)

Checked in **STATE_MEMBER**:

| # | Condition | Config |
|---|-----------|--------|
| 1 | **No valid CH** for **CH_MISS_THRESHOLD** consecutive cycles | CH_MISS_THRESHOLD = 5 (≈5 s at ~1 s/cycle). So “CH lost” after ~5 s of no valid CH. |
| 2 | **Re-election needed** (see Section 4) and we are in **STELLAR** phase | MEMBER → CANDIDATE, **election_reset_window()**. |

“No valid CH” means **neighbor_manager_get_current_ch() == 0** (no neighbor passes the valid-CH conditions in Section 2).

### 3.3 Discover finds CH (DISCOVER → MEMBER)

| # | Condition | When |
|---|-----------|------|
| 1 | **neighbor_manager_get_current_ch() != 0** and **≥ 2 s** in DISCOVER | Join as MEMBER immediately. |
| 2 | **neighbor_manager_get_current_ch() != 0** at **end of 5 s** DISCOVER | Join as MEMBER. |

### 3.4 Election result (CANDIDATE → CH or MEMBER or DISCOVER)

After **ELECTION_WINDOW_MS** (10 s) in CANDIDATE:

| election_run() result | Transition |
|------------------------|------------|
| winner == g_node_id    | **CANDIDATE → CH** |
| winner != 0 (other node)| **CANDIDATE → MEMBER** |
| winner == 0            | **CANDIDATE → DISCOVER** (no valid winner) |

### 3.5 UAV trigger (CH → UAV_ONBOARDING → CH)

- In **STATE_CH**, if **rf_receiver_check_trigger()** is true (RF trigger) or user sends **TRIGGER_UAV**, node goes **STATE_UAV_ONBOARDING**. When onboarding finishes (or times out), it goes back to **STATE_CH**.

---

## 4. When is “re-election needed”? (election_check_reelection_needed())

### 4.1 When **I am CH** (g_is_ch == true)

Re-election is needed if **any** of:

- My **battery < BATTERY_LOW_THRESHOLD** (0.2) **and** there exists a **healthier** in-cluster verified neighbor (battery ≥ 0.2 or > mine + 0.05).
- My **trust < TRUST_FLOOR** (0.2).
- My **link_quality < LINK_QUALITY_FLOOR** (0.2).
- **CH conflict:** Another verified neighbor has **is_ch == true** and:
  - its score > my score (by > 0.01) → yield; or
  - tied score and its **node_id < my node_id** → yield.

### 4.2 When **I am MEMBER** (g_is_ch == false)

Re-election is needed if **any** of:

- **neighbor_manager_get_current_ch() == 0** (no valid CH).
- CH entry **not found** in neighbor table (e.g. removed as stale).
- **CH’s battery** low **and** a healthier node exists (CH or another member with battery ≥ 0.2 or > CH + 0.05).
- **CH’s trust < TRUST_FLOOR** (0.2).
- **CH’s link_quality < LINK_QUALITY_FLOOR** (0.2).

Re-election is **only acted on** in MEMBER when **s_current_phase == PHASE_STELLAR** (so we don’t trigger re-election in the middle of DATA phase).

---

## 5. CH electing conditions (who can run and who wins)

### 5.1 Who can be a candidate (who gets into the election)

The election runs on a **candidate list**. Only nodes in this list can win.

- **Your own node (self)**  
  - Always included. Every node considers itself a candidate.

- **Each neighbor** is added only if **all** of these are true:

  | Condition | Meaning |
  |-----------|--------|
  | **In cluster** | `neighbor.rssi_ewma >= CLUSTER_RADIUS_RSSI_THRESHOLD` (−85 dBm). Strong enough link; we only elect CHs we can reliably talk to. |
  | **Verified** | `neighbor.verified == true`. BLE advertisement HMAC-verified with the same cluster key (same network). |
  | **Trust** | `neighbor.trust >= TRUST_FLOOR` (0.2). Not excluded by reputation. |

  So: **candidates = self + (neighbors that are in-cluster, verified, and trusted)**.  
  Legacy uses the same “in cluster” and “verified”; STELLAR also enforces the trust check.

- **Edge cases:**
  - **No candidates** (e.g. no neighbors, or none pass the filters): `election_run()` returns **0** → state machine sends CANDIDATE → **DISCOVER** (restart discovery).
  - **One candidate** (only self, or one neighbor that passed): that node wins immediately; no Pareto/Nash or sort needed.

---

### 5.2 Who wins — STELLAR algorithm (USE_STELLAR_ALGORITHM = 1)

Used when the build uses the STELLAR algorithm. Decision is in this order:

**Step 0: Count candidates**  
- If **candidate_count == 0** → return **0** (no winner; go to DISCOVER).  
- If **candidate_count == 1** → that single node wins; return its `node_id`.

**Step 1: Utilities**  
- For each candidate (self + qualifying neighbors), compute four **utility values** from raw metrics: battery, uptime, trust, link_quality.  
- These are non-linear (e.g. battery and link quality matter more when they’re low). So we get four numbers per candidate.

**Step 2: Pareto frontier**  
- **Pareto dominance:** Candidate A *dominates* B if A is at least as good as B on every dimension (battery, uptime, trust, link_quality) and strictly better on at least one.  
- **Pareto frontier** = set of candidates that are **not dominated** by anyone else.  
- Only these “non-dominated” candidates are allowed to become CH. Dominated nodes are dropped from the final choice.

**Step 3: Nash bargaining (on Pareto only)**  
- Among **only** the Pareto-frontier candidates, compute a **Nash product** per candidate: a weighted product of “surplus over a minimum” for each utility (using log for numerical stability).  
- Each candidate must be above a *disagreement point* on all four dimensions; otherwise it’s invalid for Nash.  
- **Winner** = Pareto candidate with **largest Nash product**.  
- Tie (Nash product equal within tolerance): **lower node_id** wins.

**Step 4: Fallback if Nash gives no winner**  
- If no Pareto candidate had a valid Nash product (e.g. all below disagreement on some dimension):  
  - Winner = Pareto candidate with **highest STELLAR score Ψ** (tie-break: **lower node_id**).  
- **STELLAR score** combines: battery, uptime, trust, link_quality, Pareto rank (how many others it dominates), and centrality (RSSI variance — more central in the network = better).

**Step 5: Second fallback**  
- If still no winner (e.g. empty Pareto):  
  - Winner = **any** candidate with **highest STELLAR score** (tie-break: **lower node_id**).

So in short: **Pareto filter → Nash on Pareto → if needed, max STELLAR on Pareto → if needed, max STELLAR over all**. Tie-break is always **lower node_id** so all nodes pick the same CH.

---

### 5.3 Who wins — Legacy algorithm (USE_STELLAR_ALGORITHM = 0)

Candidates are the same (self + in-cluster, verified neighbors; legacy does not filter by trust in the same way but uses “in cluster” and “verified”).

- **Sort** the candidate list with this **priority** (first criterion wins; if equal, next criterion):
  1. **Score** — **higher** is better (descending).
  2. **Link quality** — higher is better.
  3. **Battery** — higher is better.
  4. **Trust** — higher is better.
  5. **Node ID** — **lower** is better (so the same node wins on every device when everything else is tied).

- **Winner** = **first** element of this sorted list (i.e. the one with the best score, then best link_quality, then battery, then trust, then lowest node_id).

So: **Legacy = single composite score → sort by (score, link_quality, battery, trust, node_id) → winner = first.**

---

### 5.4 Tie-breaking (both algorithms)

Whenever two candidates are considered “equal” (same score, or same Nash product within tolerance):

- **Lower node_id** is chosen as winner.  
- This keeps the outcome **deterministic** across all nodes: everyone runs the same rules and picks the same CH.

---

### 5.5 Summary table

| Case | Result |
|------|--------|
| **No candidates** | `election_run()` returns **0** → CANDIDATE goes to **DISCOVER**. |
| **Single candidate** | That node wins; no further comparison. |
| **STELLAR, multiple** | Pareto → Nash on Pareto → (fallback) max Ψ on Pareto → (fallback) max Ψ over all; tie-break **lower node_id**. |
| **Legacy, multiple** | Sort by (score ↓, link_quality ↓, battery ↓, trust ↓, node_id ↑); **winner = first**. |

---

## 6. Config constants (reference)

| Constant | Value | Meaning |
|----------|--------|---------|
| TRUST_FLOOR | 0.2 | Min trust for valid CH and for election candidate. |
| BATTERY_LOW_THRESHOLD | 0.2 | Below this, CH may yield if someone healthier exists. |
| LINK_QUALITY_FLOOR | 0.2 | Below this, CH is considered bad; re-election. |
| CH_BEACON_TIMEOUT_MS | 5000 | CH beacon older than 5 s → not valid CH. |
| CH_MISS_THRESHOLD | 5 | Consecutive cycles with no valid CH before MEMBER declares “CH lost”. |
| ELECTION_WINDOW_MS | 10000 | CANDIDATE runs election after 10 s. |
| CLUSTER_RADIUS_RSSI_THRESHOLD | -85.0 | Neighbor with RSSI (ewma) better than -85 dBm is “in cluster”. |

---

## 7. Summary table: “When does CH change?”

| Scenario | From | To | Trigger |
|----------|------|-----|---------|
| Boot, no CH | INIT → DISCOVER → CANDIDATE | CH or MEMBER | Election after 10 s. |
| Boot, CH exists | INIT → DISCOVER → MEMBER | — | get_current_ch() ≠ 0. |
| CH yields (better CH) | CH | MEMBER | Other verified CH with higher score (or lower node_id if tied). |
| CH low battery/trust/link | CH | CANDIDATE | election_check_reelection_needed() and no other CH to yield to. |
| Member loses CH | MEMBER | CANDIDATE | No valid CH for 5 consecutive cycles. |
| Member sees bad CH | MEMBER | CANDIDATE | Re-election needed (CH battery/trust/link low) in STELLAR phase. |
| No election winner | CANDIDATE | DISCOVER | election_run() returns 0. |
| UAV trigger | CH | UAV_ONBOARDING | rf_receiver or TRIGGER_UAV; then back to CH. |

This covers every condition and transition in the code for CH election and CH changes.
