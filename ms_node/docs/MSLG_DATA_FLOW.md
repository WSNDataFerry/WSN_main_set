# MSLG Data Flow Analysis — Code-Verified Behaviour

> **Date:** March 2026  
> **Scope:** Verified against `state_machine.c`, `esp_now_manager.c`, `storage_manager.c`  
> **Cluster:** 3 nodes — CH (`3125565838`), MBR1 (`3125668638`), MBR2 (`3125683842`)

---

## Table of Contents

1. [Superframe Timeline](#1-superframe-timeline)
2. [Member Data Flow](#2-member-data-flow)
3. [CH Data Flow](#3-ch-data-flow)
4. [MSLG Growth Graphs](#4-mslg-growth-graphs)
5. [Deduplication Paths](#5-deduplication-paths)
6. [Code Path References](#6-code-path-references)
7. [Numerical Projections](#7-numerical-projections)

---

## 1. Superframe Timeline

One full superframe = **40 seconds** (configurable in `config.h`).

```
 Time (s)  0         10         20         30         40
           │          │          │          │          │
           ├──────────┼──────────┼──────────┼──────────┤
           │    STELLAR PHASE    │      DATA PHASE      │
           │     20 000 ms       │      20 000 ms       │
           │                     │                      │
           │  ┌───────────────┐  │  ┌────────────────┐  │
           │  │ BLE  = ON     │  │  │ BLE  = OFF     │  │
           │  │ NOW  = OFF    │  │  │ NOW  = ON      │  │
           │  │ Scan+Advertise│  │  │ TDMA scheduled │  │
           │  │ Election runs │  │  │ Sensor TX/RX   │  │
           │  │ Neighbour tbl │  │  │ MSLG store     │  │
           │  │ Score exchange│  │  │ MSLG drain     │  │
           │  └───────────────┘  │  └────────────────┘  │
           │                     │                      │
           │    No MSLG writes   │  All MSLG activity   │
           │    No ESP-NOW       │  All ESP-NOW          │
           └─────────────────────┴──────────────────────┘

           ◀───── Radio: BLE ────▶◀──── Radio: ESP-NOW ──▶
```

### DATA Phase Internal Timing

```
 DATA Phase Start (epoch_us)
 │
 ├─── Guard (5 s) ───── No TDMA, CH broadcasts schedule
 │
 ├─── Slot 0 (10 s) ─── Highest-priority member
 │    │
 │    ├── Store-first writes (+1 chunk / 2 s)
 │    ├── Live ESP-NOW sends (2 s gap enforcement)
 │    └── Burst drain (up to 8 pops, -N chunks)
 │
 ├─── Slot 1 (10 s) ─── Next member
 │    └── Same as Slot 0
 │
 └─── CH self-store ─── Every 5 s throughout entire DATA phase
                         (+1 chunk / 5 s, not slotted)
```

---

## 2. Member Data Flow

### Pipeline Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                        MEMBER NODE                              │
│                                                                 │
│  ┌──────────────┐                                               │
│  │  Main Loop   │  Every 2 s                                    │
│  │  (ms_node.c) │──────┐                                       │
│  │              │      ▼                                        │
│  │ seq_num++    │  metrics_set_sensor_data()                    │
│  │ mock/real    │  {temp, hum, press, eco2, tvoc, mag, audio}   │
│  └──────────────┘      │                                        │
│                        ▼                                        │
│  ┌──────────────────────────────────────────┐                   │
│  │  State Machine (100 ms tick)             │                   │
│  │  case STATE_MEMBER:                      │                   │
│  │                                          │                   │
│  │  ┌─── DATA Phase? ─── In TDMA Slot? ───┐│                   │
│  │  │         YES              YES         ││                   │
│  │  │                                      ││                   │
│  │  │  ┌─────────────────────────────┐     ││                   │
│  │  │  │ 1. STORE-FIRST             │     ││                   │
│  │  │  │    write_compressed(json)   │─────┼┼──▶ /spiffs/data.lz
│  │  │  │    +1 MSLG chunk           │     ││     [GROWS]        │
│  │  │  └─────────────────────────────┘     ││                   │
│  │  │                                      ││                   │
│  │  │  ┌─────────────────────────────┐     ││                   │
│  │  │  │ 2. LIVE SEND               │     ││                   │
│  │  │  │    esp_now_send(CH, payload)│─────┼┼──▶ CH (ESP-NOW)
│  │  │  │    2 s gap enforcement      │     ││                   │
│  │  │  │    ~3-5 sends per slot      │     ││                   │
│  │  │  └─────────────────────────────┘     ││                   │
│  │  │                                      ││                   │
│  │  │  ┌─────────────────────────────┐     ││                   │
│  │  │  │ 3. BURST DRAIN             │     ││                   │
│  │  │  │    pop_mslg_chunk() × 8    │◀────┼┼── /spiffs/data.lz
│  │  │  │    decompress if algo=1    │     ││    [SHRINKS]       │
│  │  │  │    send to CH via ESP-NOW  │─────┼┼──▶ CH (ESP-NOW)
│  │  │  │    requeue on failure      │     ││                   │
│  │  │  │    stop 500 ms before end  │     ││                   │
│  │  │  └─────────────────────────────┘     ││                   │
│  │  └──────────────────────────────────────┘│                   │
│  └──────────────────────────────────────────┘                   │
└─────────────────────────────────────────────────────────────────┘
```

### Member MSLG Timeline (One Superframe)

```
MSLG
Chunks
  │
 8┤                    ╭─╮
  │                   ╱   ╲
 6┤                  ╱     ╲
  │                 ╱       ╲         ← Burst drain kicks in
 5┤                ╱         ╲          (pops up to 8 chunks)
  │               ╱           ╲
 4┤              ╱             ╲
  │             ╱               ╲
 3┤            ╱                 ╲
  │           ╱  ← Store-first    ╲
 2┤          ╱   (+1 chunk/2s)     ╲
  │         ╱                       ╲
 1┤        ╱                         ╲
  │       ╱                           ╲
 0┤──────╱                             ╲──────────────
  └──────┬──────────┬──────────┬────────┬──────────────
         0s        10s        20s      30s        40s
         │          │          │        │          │
         │ STELLAR  │  STELLAR │ DATA   │  DATA    │
         │ (no MSLG)│          │ GUARD  │  SLOT    │
         │          │          │        │          │
                                  ◀──────────────▶
                                   TDMA Slot Window
```

### Sawtooth Over Multiple Superframes

```
MSLG
Chunks
  │
 8┤      ╱╲          ╱╲          ╱╲          ╱╲
  │     ╱  ╲        ╱  ╲        ╱  ╲        ╱  ╲
 6┤    ╱    ╲      ╱    ╲      ╱    ╲      ╱    ╲
  │   ╱      ╲    ╱      ╲    ╱      ╲    ╱      ╲
 4┤  ╱        ╲  ╱        ╲  ╱        ╲  ╱        ╲
  │ ╱          ╲╱          ╲╱          ╲╱          ╲
 2┤╱                                                ╲
  │
 0┤────────────────────────────────────────────────────
  └──┬──────────┬──────────┬──────────┬──────────┬────
     SF 1       SF 2       SF 3       SF 4       SF 5
     (40s)      (80s)      (120s)     (160s)     (200s)

  ▲ grow (+1/2s)   ▼ drain (-8 burst)   ── flat (STELLAR)

  Net effect: roughly flat or slowly declining per superframe.
  The drain sends OLD stored chunks while only NEW chunks are added.
```

---

## 3. CH Data Flow

### Pipeline Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                         CLUSTER HEAD (CH)                           │
│                                                                     │
│   ┌─── SOURCE 1: Own Sensor Data ───────────────────────────┐      │
│   │                                                         │      │
│   │  Main Loop (2 s) → metrics_set_sensor_data()            │      │
│   │  State Machine → case STATE_CH:                         │      │
│   │  Every 5 s during DATA phase:                           │      │
│   │    write_compressed(own_json) ──────────────────────────┼──▶ data.lz
│   │    +1 chunk per 5 s                                     │    [GROWS]
│   └─────────────────────────────────────────────────────────┘      │
│                                                                     │
│   ┌─── SOURCE 2: MBR1 Data (ESP-NOW RX) ───────────────────┐      │
│   │                                                         │      │
│   │  esp_now_recv_cb() → esp_now_manager.c:160              │      │
│   │    ├── Sensor payload received                          │      │
│   │    ├── dedup_should_store(node_id, seq_num) ← 4-entry   │      │
│   │    │   circular buffer per node                         │      │
│   │    ├── PASS (seq unique every 2 s) ✅                    │      │
│   │    └── write_compressed(member_json) ───────────────────┼──▶ data.lz
│   │        +1 chunk per received payload                    │    [GROWS]
│   └─────────────────────────────────────────────────────────┘      │
│                                                                     │
│   ┌─── SOURCE 3: MBR2 Data (ESP-NOW RX) ───────────────────┐      │
│   │                                                         │      │
│   │  (Same path as SOURCE 2)                                │      │
│   │  +1 chunk per received payload ─────────────────────────┼──▶ data.lz
│   │                                                         │    [GROWS]
│   └─────────────────────────────────────────────────────────┘      │
│                                                                     │
│   ┌─── DRAIN: NONE during normal operation ─────────────────┐      │
│   │                                                         │      │
│   │  The burst drain code lives in case STATE_MEMBER        │      │
│   │  (state_machine.c:995). CH never enters STATE_MEMBER.   │      │
│   │                                                         │      │
│   │  Only drain path: STATE_UAV_ONBOARDING (line 1135)      │      │
│   │  → HTTP POST to UAV server → storage_manager_clear()    │      │
│   │                                                         │      │
│   │  ⚠ If no UAV arrives → auto-purge at 90% SPIFFS full   │      │
│   └─────────────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────────────┘
```

### CH MSLG Growth per Superframe

```
                     One DATA Phase (20 s)
                ┌──────────────────────────────┐
                │                              │
   Source       │  Chunks Added                │  Rate
   ─────────── │ ──────────────────────────── │ ──────────
   Own data     │  ████ (4 chunks)             │  1 / 5 s
   MBR1 live    │  ██████████ (3-5 chunks)     │  1 / 2 s
   MBR1 burst   │  ████████████████ (up to 8)  │  burst
   MBR2 live    │  ██████████ (3-5 chunks)     │  1 / 2 s
   MBR2 burst   │  ████████████████ (up to 8)  │  burst
                │                              │
                │  TOTAL: ~14-24 new chunks    │
                └──────────────────────────────┘
```

### CH MSLG Growth Over Time (Monotonic)

```
MSLG
Chunks
  │
700┤                                                          ╱
   │                                                        ╱
600┤                                                      ╱
   │                                                    ╱
500┤                                                  ╱
   │                                                ╱
400┤                                              ╱         ← Linear growth
   │                                            ╱             (~14-24 chunks
300┤                                          ╱                per 40 s SF)
   │                                        ╱
200┤                                      ╱
   │                                    ╱
100┤                                  ╱
   │                  ╱─────────────╱
 50┤            ╱───╱
   │      ╱───╱
  0┤────╱
   └──┬──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────
      0     2.5     5     7.5    10    12.5    15    17.5    20
                         Time (minutes)

   Growth rate: ~18 chunks / 40 s ≈ 27 chunks/min ≈ 1 620 chunks/hour

   ⚠ At 90% SPIFFS capacity → auto-purge deletes ALL stored data
     and node continues fresh.
```

---

## 4. MSLG Growth Graphs

### Side-by-Side: Member vs CH

```
   MEMBER (MBR1)                          CLUSTER HEAD (CH)
   Sawtooth Pattern                       Monotonic Growth

   Chunks                                 Chunks
    │                                      │
   8┤  ╱╲   ╱╲   ╱╲   ╱╲                700┤                              ╱
    │ ╱  ╲ ╱  ╲ ╱  ╲ ╱  ╲                 │                            ╱
   6┤╱    ╲    ╲    ╲    ╲              500┤                          ╱
    │      ╲╱   ╲╱   ╲╱   ╲               │                        ╱
   4┤                       ╲            300┤                      ╱
    │                        ╲╱             │                    ╱
   2┤                                    100┤              ╱───╱
    │                                       │        ╱───╱
   0┤──────────────────────              0 ┤──────╱
    └──┬──┬──┬──┬──┬──┬──                  └──┬──┬──┬──┬──┬──┬──
       0  2  4  6  8 10 min                   0  2  4  6  8 10 min

   ● Grows during TDMA slot               ● Grows from 3 sources:
     (+1 chunk per 2 s send)                 - Own data (every 5 s)
   ● Drains during burst                    - MBR1 (live + burst)
     (-8 chunks max)                         - MBR2 (live + burst)
   ● Flat during STELLAR phase             ● NEVER drains
   ● Net: ~flat or slowly declining        ● Only drain: UAV offload
                                             or auto-purge at 90%
```

### What Happens When SPIFFS Fills Up

```
MSLG                                    ┌───────────────────────┐
Chunks                                  │  SPIFFS AUTO-PURGE    │
  │                                     │  Threshold: 90%       │
  │                                     │  Action: delete all   │
  │                                ╱    │  Files: data.lz       │
  │                              ╱      │         data.txt      │
  │                            ╱        │         queue.txt     │
  │                          ╱          └───────────────────────┘
  │                        ╱
  │                      ╱  ← 90% SPIFFS reached
  │                    ╱    ╔════════════════════════╗
  │                  ╱      ║ PURGE! Chunks → 0     ║
  │                ╱        ╚════════════════════════╝
  │              ╱                │
  │            ╱                  ▼  Start fresh
  │          ╱                    ╱
  │        ╱                    ╱
  │      ╱                    ╱     ← Grows again from 0
  │    ╱                    ╱
  │  ╱                    ╱
  │╱                    ╱
  └─────────────────────────────────────────────────────
    0    10    20    30    40    50    60    70 min

  The CH keeps collecting fresh data.
  Old data that no UAV collected is discarded.
  Fresh readings > stale readings in DTN.
```

---

## 5. Deduplication Paths

### Where Dedup Happens (and Doesn't)

```
┌──────────────────────────────────────────────────────────────────┐
│                    DEDUP DECISION TREE                           │
│                                                                  │
│  Member stores own data (state_machine.c:936)                    │
│  └─▶ NO dedup — stores every TDMA slot entry                    │
│      Reason: seq_num unique every 2 s, no duplicates possible   │
│                                                                  │
│  CH stores own data (state_machine.c:633)                        │
│  └─▶ NO dedup — stores every 5 s unconditionally                │
│      Reason: CH's own seq increments monotonically              │
│                                                                  │
│  CH stores MEMBER data (esp_now_manager.c:172)                   │
│  └─▶ YES — dedup_should_store(node_id, seq_num)                 │
│      │                                                           │
│      ├─ 4-entry circular buffer per node                         │
│      │  (DEDUP_HISTORY_PER_NODE = 4, DEDUP_MAX_NODES = 8)       │
│      │                                                           │
│      ├─ WHY: ESP-NOW may retransmit the same frame               │
│      │  If MAC-layer ACK is lost, sender retries with same seq   │
│      │  Without dedup → same payload stored 2-3x                 │
│      │                                                           │
│      └─ PASSES NOW: seq_num increments every 2 s on member       │
│         → each payload has unique seq → dedup allows storage ✅   │
│                                                                  │
│  Historical JSON data (esp_now_manager.c:227)                    │
│  └─▶ YES — same dedup_should_store() check                      │
│      (burst-drain chunks forwarded from member MSLG)             │
└──────────────────────────────────────────────────────────────────┘
```

### Why the Dedup Circular Buffer Exists

> **One sentence:** ESP-NOW has no transport-layer guarantee — if the
> MAC-layer ACK is lost, the sender retransmits the exact same frame
> (same `seq_num`), and without dedup the CH would store it 2-3× as
> duplicate MSLG chunks.

**The problem**

Without dedup, 1 sensor reading = 2 MSLG chunks. Over time this
doubles SPIFFS consumption and corrupts any analytics that assume
1 chunk = 1 unique reading.

```
  Member sends seq 42        CH receives twice (ACK lost)
  ┌──────────┐               ┌──────────────────────────┐
  │ seq = 42 │──ESP-NOW──▶   │ RX #1: store(seq=42) ✅  │
  │          │   ╳ ACK lost  │ RX #2: store(seq=42) ✅  │ ← DUPLICATE!
  └──────────┘               │ data.lz now has 2×seq42  │
                             └──────────────────────────┘
```

**The fix**

A tiny **4-slot ring buffer per node**. Before storing, the CH checks:
*"have I seen this `seq_num` from this `node_id` recently?"*

| Result | Action |
|--------|--------|
| **Not found** | Store chunk + insert seq into ring → ✅ |
| **Found** | Discard (it's a retransmit) → ❌ |

4 slots is enough because the member only sends ~3-5 unique seqs per
TDMA slot, and the buffer wraps naturally.

```
  With dedup:
  ┌──────────┐               ┌──────────────────────────┐
  │ seq = 42 │──ESP-NOW──▶   │ RX #1: dedup → new → ✅  │
  │          │   ╳ ACK lost  │ RX #2: dedup → dup → ❌  │
  └──────────┘               │ data.lz has exactly 1×42 │
                             └──────────────────────────┘
```

### Dedup Circular Buffer Visualisation

```
  Member MBR1 sends seq 101, 102, 103, 104, 105...
  CH's dedup buffer for MBR1 (4 slots):

  Time →  T1       T2       T3       T4       T5       T6
         ┌──┐     ┌──┐     ┌──┐     ┌──┐     ┌──┐     ┌──┐
  Slot 0 │101│    │101│    │101│    │101│    │105│    │105│ ← wraps
  Slot 1 │ - │    │102│    │102│    │102│    │102│    │106│
  Slot 2 │ - │    │ - │    │103│    │103│    │103│    │103│
  Slot 3 │ - │    │ - │    │ - │    │104│    │104│    │104│
         └──┘     └──┘     └──┘     └──┘     └──┘     └──┘

  Check: is incoming seq in any slot?
    seq=105 at T5: not in [101,102,103,104] → STORE ✅
    seq=103 at T5: found in slot 2 → REJECT (ESP-NOW retransmit)
```

---

## 6. Code Path References

### Member Store-First Path

```
state_machine.c : case STATE_MEMBER (line 645)
  │
  ├── can_send check (2 s gap) ─── line ~860
  │     │
  │     ├── metrics_get_sensor_data(&payload) ─── line ~870
  │     │
  │     ├── snprintf(json_payload, ...) ─── line ~920
  │     │
  │     ├── storage_manager_write_compressed(json, true) ─── line 936
  │     │   └── storage_manager.c → write_mslg_chunk()
  │     │       ├── storage_manager_purge_if_full()  ← NEW: auto-purge
  │     │       ├── fopen(data.lz, "ab")
  │     │       ├── fwrite(hdr + payload)
  │     │       └── retry on failure after purge
  │     │
  │     └── esp_now_manager_send_data(ch_mac, payload) ─── line 948
  │
  └── Burst drain (line 995)
        ├── storage_manager_pop_mslg_chunk() ─── line 1063
        ├── lz_decompress_miniz() if algo=1 ─── line 1082
        ├── esp_now_manager_send_data(ch_mac, chunk) ─── line 1101
        └── requeue via write_compressed() on send failure ─── line 1110
```

### CH Self-Store Path

```
state_machine.c : case STATE_CH (line 425)
  │
  ├── Every 5 s check ─── line ~595
  │     │
  │     ├── metrics_get_sensor_data(&payload) ─── line ~600
  │     │
  │     ├── snprintf(json_payload, ...) ─── line ~615
  │     │
  │     └── storage_manager_write_compressed(json, true) ─── line 633
  │         └── (same write_mslg_chunk path as above)
  │
  └── break; ─── line 643
      ▲
      │  CH state ends here. No drain code.
```

### CH Receive Path (Member Data Ingestion)

```
esp_now_manager.c : esp_now_recv_cb()
  │
  ├── Sensor payload detected ─── line 160
  │     │
  │     ├── dedup_should_store(node_id, seq_num) ─── line 172
  │     │     ├── Check 4-entry circular buffer for this node
  │     │     ├── If seq found → return false (duplicate)
  │     │     └── If seq NOT found → insert, return true
  │     │
  │     ├── (if passed) format json ─── line ~185
  │     │
  │     └── storage_manager_write_compressed(json, true) ─── line 197
  │
  └── Historical JSON data ─── line 213
        ├── dedup_should_store() ─── line 227
        └── storage_manager_write_compressed() ─── line 240
```

---

## 7. Numerical Projections

### Chunks Per Superframe (40 s)

| Source | Role | Chunks/SF | Calculation |
|--------|------|-----------|-------------|
| Member self-store | MBR | +3 to +5 | 1 chunk per 2 s send, ~6-10 s slot |
| Member burst drain | MBR | -3 to -8 | Pops stored chunks, up to 8 |
| **Member net** | **MBR** | **~0 to -3** | **Roughly flat or slowly declining** |
| CH self-store | CH | +4 | 1 chunk per 5 s × 20 s DATA phase |
| CH from MBR1 (live) | CH | +3 to +5 | ~3-5 live sends per slot |
| CH from MBR1 (burst) | CH | +3 to +8 | Burst drain forwarded chunks |
| CH from MBR2 (live) | CH | +3 to +5 | Same as MBR1 |
| CH from MBR2 (burst) | CH | +3 to +8 | Same as MBR1 |
| **CH net** | **CH** | **+16 to +30** | **Linear growth, never drains** |

### CH Projection Over Time

| Time | Superframes | Est. CH Chunks | SPIFFS % (est.) |
|------|-------------|----------------|------------------|
| 0 min | 0 | 0 | 0% |
| 5 min | 7.5 | ~135-225 | ~5-10% |
| 10 min | 15 | ~270-450 | ~10-20% |
| 20 min | 30 | ~540-900 | ~20-40% |
| 30 min | 45 | ~810-1 350 | ~30-60% |
| 45 min | 67 | ~1 200-2 000 | ~50-85% |
| **~50-60 min** | **~80** | **~1 440-2 400** | **≥90% → PURGE** |

> **Note:** Actual SPIFFS % depends on chunk size (typically ~200-300 bytes per MSLG chunk after header). With a 1.5 MB SPIFFS partition, ~5 000+ chunks fit before 90%.

### Growth Rate Formula

$$\text{CH chunks/min} = \frac{\text{CH\_own} + \sum_{m \in \text{members}} (\text{live}_m + \text{burst}_m)}{\text{superframe\_sec} / 60}$$

$$= \frac{4 + (5 + 5) + (5 + 5)}{40/60} \approx \frac{24}{0.667} \approx 36 \text{ chunks/min}$$

### Member Steady-State

$$\text{MBR net/SF} = \text{store\_first} - \text{burst\_drain} = 4 - 6 = -2$$

Members slowly drain their MSLG (the burst drain sends more old chunks than new ones are added). After a few superframes the member's MSLG reaches a low steady state of 0-3 chunks.

---

## Summary

```
┌───────────────────────────────────────────────────────────────┐
│                                                               │
│   MEMBER: Sawtooth ╱╲╱╲╱╲   →  roughly flat, self-draining  │
│                                                               │
│   CH:     Linear   ╱─────   →  grows ~36 chunks/min          │
│                     ╱            from 3 simultaneous sources   │
│                   ╱                                           │
│                 ╱              →  auto-purge at 90% SPIFFS    │
│               ╱                  OR UAV offload clears it     │
│                                                               │
│   ┌───────────────────────────────────────────────────┐       │
│   │  KEY INSIGHT: The CH's growth LOOKS exponential    │       │
│   │  compared to a member because it accumulates from  │       │
│   │  ALL nodes and NEVER drains. But it's actually     │       │
│   │  LINEAR growth from multiple constant-rate sources.│       │
│   └───────────────────────────────────────────────────┘       │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

---

*Generated from code analysis of `state_machine.c`, `esp_now_manager.c`, and `storage_manager.c` — March 2026*
