#!/usr/bin/env python3
"""
Parse ms_node/out.log, out1.log, out2.log for "Sent sensor data" sends and "RX Sensor Data" receives.
Produce CSV mapping send -> matching RX (by node_id and seq).
"""
import re
import csv
from pathlib import Path

LOG_DIR = Path(__file__).resolve().parents[1]
FILES = [LOG_DIR / 'out.log', LOG_DIR / 'out1.log', LOG_DIR / 'out2.log']

send_re = re.compile(r"sent sensor data.*seq=(\d+),\s*node_id=(\d+)", re.IGNORECASE)
rx_re = re.compile(r"rx sensor data.*node_(\d+).*seq=(\d+)", re.IGNORECASE)
# timestamp like: I (123456) ... or W (123456)
time_re = re.compile(r"[IW]\s*\((\d+)\)")

sends = []
rxs = []

for fp in FILES:
    if not fp.exists():
        continue
    with open(fp, 'r', errors='replace') as f:
        for i, line in enumerate(f, start=1):
            l = line.strip()
            m_send = send_re.search(l)
            m_rx = rx_re.search(l)
            m_time = time_re.search(l)
            tms = int(m_time.group(1)) if m_time else None
            if m_send:
                seq = int(m_send.group(1))
                node = int(m_send.group(2))
                sends.append({'file': fp.name, 'line': i, 'time': tms, 'node': node, 'seq': seq, 'raw': l})
            if m_rx:
                node = int(m_rx.group(1))
                seq = int(m_rx.group(2))
                rxs.append({'file': fp.name, 'line': i, 'time': tms, 'node': node, 'seq': seq, 'raw': l})

# Build an index of rx by (node,seq) -> list of rx entries
rx_index = {}
for r in rxs:
    key = (r['node'], r['seq'])
    rx_index.setdefault(key, []).append(r)

# For each send, try to find the earliest rx with same (node,seq) that occurs at/after send time (if time present)
rows = []
matched = 0
for s in sends:
    key = (s['node'], s['seq'])
    possible = rx_index.get(key, [])
    chosen = None
    if possible:
        # If send has time, prefer rx with time >= send.time, else pick first
        if s['time'] is not None:
            for r in sorted(possible, key=lambda x: (x['time'] if x['time'] is not None else 0, x['line'])):
                if r['time'] is None or s['time'] <= r['time']:
                    chosen = r
                    break
            if chosen is None:
                chosen = possible[0]
        else:
            chosen = possible[0]
    if chosen:
        matched += 1
        rows.append({
            'send_file': s['file'], 'send_line': s['line'], 'send_time_ms': s['time'], 'node_id': s['node'], 'seq': s['seq'], 'rx_file': chosen['file'], 'rx_line': chosen['line'], 'rx_time_ms': chosen['time']
        })
    else:
        rows.append({
            'send_file': s['file'], 'send_line': s['line'], 'send_time_ms': s['time'], 'node_id': s['node'], 'seq': s['seq'], 'rx_file': '', 'rx_line': '', 'rx_time_ms': ''
        })

# Print CSV to stdout and a short summary
writer = csv.DictWriter(__import__('sys').stdout, fieldnames=['send_file','send_line','send_time_ms','node_id','seq','rx_file','rx_line','rx_time_ms'])
writer.writeheader()
for r in rows:
    writer.writerow(r)

print('\n# Summary')
print(f"Total sends found: {len(sends)}")
print(f"Total RX entries found: {len(rxs)}")
print(f"Matched sends -> RX: {matched}")

# Per-node stats
from collections import defaultdict
node_stats = defaultdict(lambda: {'sends':0,'matched':0,'rxs':0})
for s in sends:
    node_stats[s['node']]['sends'] += 1
for r in rxs:
    node_stats[r['node']]['rxs'] += 1
for row in rows:
    if row['rx_file']:
        node_stats[row['node_id']]['matched'] += 1

print('\nPer-node:')
for node, st in sorted(node_stats.items()):
    print(f" node {node}: sends={st['sends']} matched={st['matched']} rx_entries={st['rxs']}")
