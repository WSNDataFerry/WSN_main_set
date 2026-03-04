#!/usr/bin/env python3
"""
WSN Sensor Data Sequence Checker
==================================
Parses ESP-IDF terminal logs and checks that sensor data payload seq numbers
are in correct order for each node. No BLE — only sensor data payloads.

Checks:
  1. Per-node seq order is monotonically increasing (0, 1, 2, 3, ...)
  2. Detects seq gaps (e.g. 0 → 2, missing 1)
  3. Detects seq regressions (e.g. 3 → 1)
  4. Counts retransmissions (same seq sent multiple times — expected)
  5. Cross-correlates: what members sent vs what CH received

Log lines matched:
  CH side:
    "ESP_NOW: RX Sensor Data from node_XXXX: Seq=N, ..."
    "STATE: CH Stored own sensor data: seq=N"
  Member side:
    "STATE: Sent sensor data via ESP-NOW to CH: seq=N, node_id=XXXX"
    "STATE: Stored sensor data (Store-First): seq=N"

Usage:
  python check_seq_order.py logs/out.log logs/out1.log logs/out2.log
  python check_seq_order.py logs/
"""

import re
import sys
import os
from collections import defaultdict
from dataclasses import dataclass, field

# ─── Colors ──────────────────────────────────────────────────────────────────
GREEN  = "\033[92m"
RED    = "\033[91m"
YELLOW = "\033[93m"
CYAN   = "\033[96m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
RESET  = "\033[0m"

# ─── Known nodes ─────────────────────────────────────────────────────────────
NODE_NAMES = {
    3125565838: "CH  (1020ba4c598e)",
    3125668638: "MBR1(1020ba4deb1e)",
    3125683842: "MBR2(1020ba4e2682)",
}

def node_label(nid):
    return NODE_NAMES.get(nid, f"node_{nid}")

# ─── Data ────────────────────────────────────────────────────────────────────
@dataclass
class SeqEvent:
    timestamp_ms: int
    seq: int
    source: str        # CH_RX, CH_SELF, MEMBER_TX, MEMBER_STORE
    log_file: str
    line_num: int
    extra: str = ""    # sensor values snippet

@dataclass
class NodeStats:
    events: list = field(default_factory=list)
    unique_seqs: set = field(default_factory=set)
    seq_order: list = field(default_factory=list)
    gaps: list = field(default_factory=list)
    regressions: list = field(default_factory=list)
    duplicates: int = 0
    first_seen: dict = field(default_factory=dict)

# ─── Regex ───────────────────────────────────────────────────────────────────
ANSI_RE = re.compile(r'\x1b\[[0-9;]*m|\r')
TS_RE   = re.compile(r'I \((\d+)\)')

# CH receiving member sensor data
CH_RX_RE = re.compile(
    r'ESP_NOW: RX Sensor Data from node_(\d+): Seq=(\d+),\s*(.*)'
)
# CH storing own data
CH_SELF_RE = re.compile(
    r'STATE: CH Stored own sensor data: seq=(\d+)'
)
# Member sending to CH
MEMBER_TX_RE = re.compile(
    r'STATE: Sent sensor data via ESP-NOW to CH: seq=(\d+), node_id=(\d+)'
)
# Member local store
MEMBER_STORE_RE = re.compile(
    r'STATE: Stored sensor data \(Store-First\): seq=(\d+)'
)
# Detect node owner from BLE adv or member TX (to attribute MEMBER_STORE)
OWNER_RE = re.compile(
    r'BLE: Advertising packet: node_id=(\d+)'
)

# ─── Parse ───────────────────────────────────────────────────────────────────
def parse_log_file(filepath):
    nodes = defaultdict(lambda: NodeStats())
    fname = os.path.basename(filepath)
    owner_id = None

    with open(filepath, 'r', errors='replace') as f:
        for ln, raw in enumerate(f, 1):
            line = ANSI_RE.sub('', raw).strip()
            if not line:
                continue
            ts_m = TS_RE.search(line)
            if not ts_m:
                continue
            ts = int(ts_m.group(1))

            # CH RX sensor data
            m = CH_RX_RE.search(line)
            if m:
                nid, seq, extra = int(m.group(1)), int(m.group(2)), m.group(3)
                nodes[nid].events.append(SeqEvent(ts, seq, "CH_RX", fname, ln, extra))
                continue

            # CH self store
            m = CH_SELF_RE.search(line)
            if m:
                seq = int(m.group(1))
                nodes[3125565838].events.append(SeqEvent(ts, seq, "CH_SELF", fname, ln))
                continue

            # Member TX
            m = MEMBER_TX_RE.search(line)
            if m:
                seq, nid = int(m.group(1)), int(m.group(2))
                owner_id = nid
                nodes[nid].events.append(SeqEvent(ts, seq, "MEMBER_TX", fname, ln))
                continue

            # Member store
            m = MEMBER_STORE_RE.search(line)
            if m:
                seq = int(m.group(1))
                nid = owner_id or 0
                nodes[nid].events.append(SeqEvent(ts, seq, "MEMBER_STORE", fname, ln))
                continue

            # Detect owner (just for attribution)
            m = OWNER_RE.search(line)
            if m:
                owner_id = int(m.group(1))

    return dict(nodes)

# ─── Analyze ─────────────────────────────────────────────────────────────────
def analyze(events):
    stats = NodeStats()
    stats.events = events
    evts = sorted(events, key=lambda e: e.timestamp_ms)

    prev_seq = None
    max_seq = None

    for evt in evts:
        seq = evt.seq
        stats.seq_order.append((evt.timestamp_ms, seq, evt.source, evt.extra))
        stats.unique_seqs.add(seq)

        if seq not in stats.first_seen:
            stats.first_seen[seq] = evt.timestamp_ms

        if prev_seq is not None:
            if seq == prev_seq:
                stats.duplicates += 1
            elif seq < max_seq:
                stats.regressions.append((max_seq, seq, evt.timestamp_ms, evt.source))

        if max_seq is None or seq > max_seq:
            if max_seq is not None and seq > max_seq + 1:
                stats.gaps.append((max_seq, seq, evt.timestamp_ms, evt.source))
            max_seq = seq

        prev_seq = seq

    return stats

# ─── Printing ────────────────────────────────────────────────────────────────
def divider(c='─', w=80):
    print(f"{DIM}{c * w}{RESET}")

def header(title):
    print()
    divider('═')
    print(f"{BOLD}{CYAN}  {title}{RESET}")
    divider('═')

def print_report(nid, stats, event_types):
    label = node_label(nid)
    print()
    divider()
    print(f"{BOLD}  Node: {label} (id={nid}){RESET}")
    print(f"  Sources: {', '.join(sorted(event_types))}")
    divider()

    total = len(stats.events)
    unique = len(stats.unique_seqs)

    if unique > 0:
        smin = min(stats.unique_seqs)
        smax = max(stats.unique_seqs)
        expected = smax - smin + 1
        missing = expected - unique
    else:
        smin = smax = expected = missing = 0

    print(f"  Total events:      {total}")
    print(f"  Unique seqs:       {unique}")
    print(f"  Seq range:         {smin} → {smax}")
    print(f"  Expected (contiguous): {expected}")
    if missing == 0:
        print(f"  Missing seqs:      {GREEN}0 ✓{RESET}")
    else:
        print(f"  Missing seqs:      {RED}{missing} ✗{RESET}")
        all_expected = set(range(smin, smax + 1))
        actually_missing = sorted(all_expected - stats.unique_seqs)
        print(f"  Missing values:    {RED}{actually_missing}{RESET}")
    print(f"  Retransmissions:   {stats.duplicates}")

    # Timeline of unique seq transitions with sensor data
    if unique > 0:
        print()
        print(f"  {BOLD}Seq Timeline:{RESET}")
        seen = set()
        transitions = []
        for ts, seq, src, extra in stats.seq_order:
            if seq not in seen:
                seen.add(seq)
                transitions.append((ts, seq, src, extra))

        for i, (ts, seq, src, extra) in enumerate(transitions):
            dt = ""
            if i > 0:
                delta = (ts - transitions[i-1][0]) / 1000.0
                dt = f" (+{delta:.1f}s)"
            snippet = ""
            if extra:
                snippet = f"  {DIM}{extra[:70]}{RESET}"
            print(f"    {GREEN}✓{RESET} seq={seq:<5d} @ {ts/1000:>9.1f}s  [{src}]{dt}{snippet}")

    # Gaps
    if stats.gaps:
        print()
        print(f"  {BOLD}{YELLOW}⚠ Seq Gaps:{RESET}")
        for prev, cur, ts, src in stats.gaps:
            miss = list(range(prev + 1, cur))
            print(f"    {YELLOW}seq {prev} → {cur}  (missing: {miss})  @ {ts/1000:.1f}s [{src}]{RESET}")
    else:
        print(f"\n  {GREEN}✓ No seq gaps{RESET}")

    # Regressions
    if stats.regressions:
        print()
        print(f"  {BOLD}{RED}✗ Seq Regressions:{RESET}")
        for prev, cur, ts, src in stats.regressions:
            print(f"    {RED}{prev} → {cur}  @ {ts/1000:.1f}s [{src}]{RESET}")
    else:
        print(f"  {GREEN}✓ No seq regressions{RESET}")

    # Retransmission counts
    if stats.duplicates > 0:
        print()
        print(f"  {BOLD}Retransmissions per Seq:{RESET}")
        counts = defaultdict(int)
        for e in stats.events:
            counts[e.seq] += 1
        for seq in sorted(counts):
            cnt = counts[seq]
            bar = "█" * min(cnt, 50)
            c = YELLOW if cnt > 5 else ""
            r = RESET if c else ""
            print(f"    seq={seq:<5d}: {c}{cnt:>4d}x{r} {DIM}{bar}{RESET}")


def cross_correlate(all_nodes, ch_nodes):
    header("CROSS-CORRELATION: Member TX → CH RX")

    ch_rx = defaultdict(lambda: defaultdict(int))
    for nid, stats in ch_nodes.items():
        for e in stats.events:
            if e.source == "CH_RX":
                ch_rx[nid][e.seq] += 1

    mbr_tx = defaultdict(lambda: defaultdict(int))
    for nid, stats in all_nodes.items():
        for e in stats.events:
            if e.source == "MEMBER_TX":
                mbr_tx[nid][e.seq] += 1

    for nid in sorted(set(list(ch_rx.keys()) + list(mbr_tx.keys()))):
        if nid == 3125565838:
            continue
        label = node_label(nid)
        print(f"\n  {BOLD}{label}:{RESET}")

        tx_seqs = set(mbr_tx[nid].keys())
        rx_seqs = set(ch_rx[nid].keys())
        all_seqs = sorted(tx_seqs | rx_seqs)

        if not all_seqs:
            print(f"    {DIM}(no data){RESET}")
            continue

        print(f"    {'Seq':>6s}  {'MBR TX':>8s}  {'CH RX':>8s}  {'Status'}")
        print(f"    {'───':>6s}  {'──────':>8s}  {'─────':>8s}  {'──────'}")

        for seq in all_seqs:
            tc = mbr_tx[nid].get(seq, 0)
            rc = ch_rx[nid].get(seq, 0)
            if tc > 0 and rc > 0:
                st = f"{GREEN}✓ OK{RESET}"
            elif tc > 0 and rc == 0:
                st = f"{RED}✗ LOST{RESET}"
            elif tc == 0 and rc > 0:
                st = f"{YELLOW}? RX only{RESET}"
            else:
                st = "—"
            print(f"    {seq:>6d}  {tc:>8d}  {rc:>8d}  {st}")


def print_verdict(analysis):
    header("OVERALL VERDICT")

    total_gaps = total_reg = 0
    for nid, stats in sorted(analysis.items()):
        if not stats.unique_seqs:
            continue
        total_gaps += len(stats.gaps)
        total_reg += len(stats.regressions)

        label = node_label(nid)
        u = len(stats.unique_seqs)
        smin, smax = min(stats.unique_seqs), max(stats.unique_seqs)

        if not stats.gaps and not stats.regressions:
            icon = f"{GREEN}✓{RESET}"
        elif stats.regressions:
            icon = f"{RED}✗{RESET}"
        else:
            icon = f"{YELLOW}⚠{RESET}"

        print(f"  {icon} {label}: seq {smin}→{smax} ({u} unique, "
              f"{len(stats.gaps)} gaps, {len(stats.regressions)} regressions, "
              f"{stats.duplicates} retx)")

    print()
    if total_gaps == 0 and total_reg == 0:
        print(f"  {BOLD}{GREEN}═══════════════════════════════════════════════{RESET}")
        print(f"  {BOLD}{GREEN}  ✓ ALL NODES: SENSOR DATA SEQUENCING CORRECT {RESET}")
        print(f"  {BOLD}{GREEN}═══════════════════════════════════════════════{RESET}")
    elif total_reg > 0:
        print(f"  {BOLD}{RED}═══════════════════════════════════════════════{RESET}")
        print(f"  {BOLD}{RED}  ✗ SEQUENCING ERRORS: {total_gaps} gaps, {total_reg} regressions{RESET}")
        print(f"  {BOLD}{RED}═══════════════════════════════════════════════{RESET}")
    else:
        print(f"  {BOLD}{YELLOW}═══════════════════════════════════════════════{RESET}")
        print(f"  {BOLD}{YELLOW}  ⚠ SEQ GAPS DETECTED: {total_gaps} total              {RESET}")
        print(f"  {BOLD}{YELLOW}═══════════════════════════════════════════════{RESET}")


# ─── Main ────────────────────────────────────────────────────────────────────
def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <log_file_or_dir> [log2] [log3] ...")
        print(f"\nExamples:")
        print(f"  {sys.argv[0]} logs/out.log logs/out1.log logs/out2.log")
        print(f"  {sys.argv[0]} logs/")
        sys.exit(1)

    log_files = []
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            for f in sorted(os.listdir(arg)):
                if f.endswith('.log'):
                    log_files.append(os.path.join(arg, f))
        elif os.path.isfile(arg):
            log_files.append(arg)
        else:
            print(f"{RED}Warning: '{arg}' not found{RESET}")

    if not log_files:
        print(f"{RED}No log files found!{RESET}")
        sys.exit(1)

    header("WSN Sensor Data Sequence Checker")
    print(f"  Analyzing {len(log_files)} log file(s):")
    for f in log_files:
        print(f"    • {f}")

    all_nodes = defaultdict(lambda: NodeStats())
    ch_nodes  = defaultdict(lambda: NodeStats())

    for fp in log_files:
        fname = os.path.basename(fp)
        print(f"\n  Parsing {fname}...", end=" ")
        file_nodes = parse_log_file(fp)
        cnt = sum(len(s.events) for s in file_nodes.values())
        print(f"{cnt} sensor data events across {len(file_nodes)} node(s)")

        for nid, stats in file_nodes.items():
            all_nodes[nid].events.extend(stats.events)
            for e in stats.events:
                if e.source in ("CH_RX", "CH_SELF"):
                    ch_nodes[nid].events.append(e)

    # ── CH perspective ──
    header("CH PERSPECTIVE (sensor data received at cluster head)")
    ch_analysis = {}
    for nid in sorted(ch_nodes):
        evts = ch_nodes[nid].events
        if not evts:
            continue
        stats = analyze(evts)
        ch_analysis[nid] = stats
        print_report(nid, stats, {e.source for e in evts})

    # ── Member perspective ──
    has_mbr = any(
        any(e.source in ("MEMBER_TX", "MEMBER_STORE") for e in all_nodes[nid].events)
        for nid in all_nodes
    )
    if has_mbr:
        header("MEMBER PERSPECTIVE (sensor data sent by each node)")
        for nid in sorted(all_nodes):
            evts = [e for e in all_nodes[nid].events if e.source in ("MEMBER_TX", "MEMBER_STORE")]
            if not evts:
                continue
            stats = analyze(evts)
            print_report(nid, stats, {e.source for e in evts})

    # ── Cross-correlation ──
    if has_mbr:
        cross_correlate(dict(all_nodes), dict(ch_nodes))

    # ── Verdict ──
    print_verdict(ch_analysis)


if __name__ == "__main__":
    main()
