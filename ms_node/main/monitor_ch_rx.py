#!/usr/bin/env python3
"""
monitor_ch_rx.py — 2-minute monitor to verify CH receives sensor data from members.
Tracks: RX Sensor Data, NO-SCHED FALLBACK sends, SEND FAILED errors.
"""
import serial
import threading
import time
import sys
import re
from datetime import datetime

PORTS = [
    "/dev/ttyACM0",
    "/dev/ttyACM1",
    "/dev/ttyACM2",
]
BAUD = 115200
DURATION = 120  # seconds

# Per-node counters
stats = [{
    "port": p,
    "node_id": None,
    "role": "?",
    "rx_sensor_data": 0,
    "rx_compressed": 0,
    "nosched_fallback": 0,
    "send_ok": 0,
    "send_failed": 0,
    "send_failed_not_found": 0,
    "timestamp0_warn": 0,
    "ch_mac_not_found": 0,
    "phase_data_enters": 0,
} for p in PORTS]

stop_event = threading.Event()

def monitor_node(idx, port):
    s = stats[idx]
    try:
        ser = serial.Serial(port, BAUD, timeout=0.5)
    except Exception as e:
        print(f"[{port}] OPEN FAILED: {e}")
        return

    print(f"[{port}] Connected")
    while not stop_event.is_set():
        try:
            line = ser.readline().decode("utf-8", errors="replace").strip()
        except Exception:
            break
        if not line:
            continue

        # Role detection (case-insensitive)
        l = line.lower()
        if "state_ch" in l or ": ch:" in l or " ch:" in l:
            s["role"] = "CH"
        elif "state_member" in l or "member:" in l:
            s["role"] = "NODE"

        # Node ID
        m = re.search(r"node_id=(\d+)", line)
        if m and s["node_id"] is None:
            s["node_id"] = int(m.group(1))

        # CH-side RX (case-insensitive)
        if "rx sensor data" in l:
            s["rx_sensor_data"] += 1
            print(f"[N{s['node_id'] or idx+1} CH]  🟢 RX SENSOR DATA: {line[-120:]}")
        if "rx compressed" in l:
            s["rx_compressed"] += 1
            print(f"[N{s['node_id'] or idx+1} CH]  🟢 RX COMPRESSED: {line[-120:]}")

        # Member-side send events
        # Member-side send events (case-insensitive, match actual log variants)
        if "no-sched fallback" in l or "no_sched fallback" in l:
            s["nosched_fallback"] += 1
            print(f"[N{s['node_id'] or idx+1} MBR] 📤 {line[-120:]}")
        if "sent sensor data" in l:
            s["send_ok"] += 1
            print(f"[N{s['node_id'] or idx+1} MBR] ✅ SENT OK: {line[-120:]}")
        # match send failures such as "ESP-NOW send failed, status: 1"
        if "send failed" in l:
            s["send_failed"] += 1
            if "not_found" in l or "not found" in l:
                s["send_failed_not_found"] += 1
            print(f"[N{s['node_id'] or idx+1} MBR] ❌ SEND FAILED: {line[-160:]}")
        if "timestamp=0" in l or "timestamp_ms=0" in l:
            s["timestamp0_warn"] += 1
        if "ch mac not found" in l or "ch_mac not found" in l:
            s["ch_mac_not_found"] += 1
            print(f"[N{s['node_id'] or idx+1} MBR] ⚠️  CH MAC NOT FOUND")
        if "Phase transition" in line and "DATA" in line:
            s["phase_data_enters"] += 1

    ser.close()

threads = []
for i, port in enumerate(PORTS):
    t = threading.Thread(target=monitor_node, args=(i, port), daemon=True)
    t.start()
    threads.append(t)

print(f"\n{'='*60}")
print(f"Monitor started — running for {DURATION}s")
print(f"{'='*60}\n")

start = time.time()
while time.time() - start < DURATION:
    elapsed = int(time.time() - start)
    remaining = DURATION - elapsed
    if elapsed % 30 == 0 and elapsed > 0:
        print(f"\n[{elapsed}s] --- Interim summary ---")
        for s in stats:
            nid = s["node_id"] or "?"
            print(f"  Node {nid} ({s['role']}@{s['port'][-16:]}): "
                  f"RX_data={s['rx_sensor_data']} SENT_OK={s['send_ok']} "
                  f"FAILED={s['send_failed']} FALLBACK={s['nosched_fallback']}")
        print()
    time.sleep(1)

stop_event.set()
for t in threads:
    t.join(timeout=2)

print(f"\n{'='*60}")
print("FINAL RESULTS (2-minute run)")
print(f"{'='*60}")
for s in stats:
    nid = s["node_id"] or "?"
    print(f"\nNode {nid} ({s['role']}) @ {s['port']}")
    print(f"  RX Sensor Data packets  : {s['rx_sensor_data']}")
    print(f"  RX Compressed packets   : {s['rx_compressed']}")
    print(f"  No-Sched Fallback fires  : {s['nosched_fallback']}")
    print(f"  Sends OK                : {s['send_ok']}")
    print(f"  Send FAILED (total)     : {s['send_failed']}")
    print(f"  Send FAILED NOT_FOUND   : {s['send_failed_not_found']}")
    print(f"  timestamp=0 warnings    : {s['timestamp0_warn']}")
    print(f"  CH MAC not found        : {s['ch_mac_not_found']}")
    print(f"  PHASE_DATA entries      : {s['phase_data_enters']}")

total_ch_rx = sum(s["rx_sensor_data"] + s["rx_compressed"] for s in stats if s["role"] == "CH")
mbr_sends = sum(s["send_ok"] for s in stats)
print(f"\n{'='*60}")
if total_ch_rx > 0:
    print(f"✅ SUCCESS: CH(s) received {total_ch_rx} data packets from {mbr_sends} sends")
else:
    print(f"❌ FAILURE: CH received 0 data packets. Sends seen={mbr_sends}")
    for s in stats:
        if s["send_failed"] > 0:
            print(f"  ⚠ Node {s['node_id']} had {s['send_failed']} SEND FAILED "
                  f"(NOT_FOUND={s['send_failed_not_found']})")
        if s["ch_mac_not_found"] > 0:
            print(f"  ⚠ Node {s['node_id']} had {s['ch_mac_not_found']} CH_MAC_NOT_FOUND")
print(f"{'='*60}")
