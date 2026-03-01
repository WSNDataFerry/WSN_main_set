#!/usr/bin/env python3
"""
5-Minute Multi-Node Monitor
Captures all serial output from 3 nodes simultaneously for 300 seconds.
Saves per-node log files for analysis.
"""
import serial
import threading
import time
import os
import sys
from datetime import datetime

PORTS = {
    "Node1": "/dev/cu.usbmodem5ABA0603751",
    "Node2": "/dev/cu.usbmodem5ABA0605221",
    "Node3": "/dev/cu.usbmodem5ABA0607321",
}
BAUD = 115200
DURATION = 300  # 5 minutes
OUT_DIR = "monitor_5min_run"

os.makedirs(OUT_DIR, exist_ok=True)

stop_event = threading.Event()
lock = threading.Lock()

# Shared stats per node
stats = {n: {
    "lines": 0,
    "state_changes": [],
    "elections": [],
    "ch_misses": 0,
    "ch_lost": 0,
    "lyapunov_v": [],
    "psi": [],
    "pareto": [],
    "centrality": [],
    "status_snapshots": [],
    "errors": [],
    "last_status": "",
} for n in PORTS}


def monitor_node(name, port):
    log_path = f"{OUT_DIR}/{name}_monitor.log"
    try:
        ser = serial.Serial(port, BAUD, timeout=1)
    except Exception as e:
        with lock:
            stats[name]["errors"].append(f"OPEN FAILED: {e}")
        return

    with open(log_path, "w") as f:
        while not stop_event.is_set():
            try:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="ignore").strip()
                if not line:
                    continue

                ts = datetime.now().strftime("%H:%M:%S")
                tagged = f"[{ts}] {line}"
                f.write(tagged + "\n")
                f.flush()

                with lock:
                    s = stats[name]
                    s["lines"] += 1

                    # State transitions
                    if "State transition:" in line:
                        s["state_changes"].append(f"[{ts}] {line.split('] ')[-1]}")

                    # Election events
                    if "Election" in line or "Winner" in line or "STELLAR Election" in line:
                        s["elections"].append(f"[{ts}] {line.split('] ')[-1]}")

                    # CH miss / lost
                    if "CH beacon missed" in line:
                        s["ch_misses"] += 1
                    if "CH lost" in line:
                        s["ch_lost"] += 1

                    # Lyapunov V value
                    if "Lyapunov weights:" in line and "V=" in line:
                        try:
                            v = float(line.split("V=")[1].split(",")[0])
                            s["lyapunov_v"].append(v)
                        except:
                            pass

                    # STELLAR score Ψ
                    if "Score components:" in line and "Ψ=" in line:
                        try:
                            psi = float(line.split("Ψ=")[1].strip())
                            s["psi"].append(psi)
                        except:
                            pass
                        try:
                            pareto_str = line.split("Pareto rank=")[0] if "Pareto rank=" in line else ""
                        except:
                            pass

                    # Pareto rank
                    if "Pareto rank=" in line:
                        try:
                            rank = int(line.split("Pareto rank=")[1].split("/")[0])
                            s["pareto"].append(rank)
                        except:
                            pass
                        try:
                            cent = float(line.split("Centrality=")[1].strip())
                            s["centrality"].append(cent)
                        except:
                            pass

                    # STATUS snapshot
                    if "STATUS:" in line:
                        s["last_status"] = f"[{ts}] {line}"
                        s["status_snapshots"].append(f"[{ts}] {line.split('STATUS: ')[-1]}")

                    # Errors / warnings
                    if any(k in line for k in ["ERROR", "FAILED", "panic", "abort", "Guru Meditation"]):
                        s["errors"].append(f"[{ts}] {line}")

            except serial.SerialException as e:
                with lock:
                    stats[name]["errors"].append(f"[{datetime.now().strftime('%H:%M:%S')}] SerialException: {e}")
                time.sleep(1)
            except Exception as e:
                pass

    ser.close()
    print(f"  [{name}] Monitor thread done. {stats[name]['lines']} lines captured.")


# ── Start threads ──────────────────────────────────────────────────────────────
threads = []
print(f"\n{'='*60}")
print(f"  STELLAR WSN — 5-Minute Live Monitor")
print(f"  Start: {datetime.now().strftime('%H:%M:%S')}")
print(f"  Duration: {DURATION}s")
print(f"{'='*60}")

for name, port in PORTS.items():
    t = threading.Thread(target=monitor_node, args=(name, port), daemon=True)
    t.start()
    threads.append(t)
    print(f"  [{name}] Monitoring {port}")

# ── Progress ticker ────────────────────────────────────────────────────────────
start = time.time()
try:
    while time.time() - start < DURATION:
        elapsed = int(time.time() - start)
        remaining = DURATION - elapsed
        with lock:
            status_line = " | ".join(
                f"{n}: {stats[n]['last_status'].split('State=')[-1].split(',')[0] if stats[n]['last_status'] else '?'}"
                for n in PORTS
            )
        print(f"\r  [{elapsed:3d}s / {DURATION}s] {status_line}   ", end="", flush=True)
        time.sleep(5)
except KeyboardInterrupt:
    print("\n  [!] Interrupted by user")

stop_event.set()
print(f"\n\n  Stop: {datetime.now().strftime('%H:%M:%S')}")
for t in threads:
    t.join(timeout=3)

# ── Print summary ──────────────────────────────────────────────────────────────
print(f"\n{'='*60}")
print(f"  ANALYSIS SUMMARY")
print(f"{'='*60}")

for name in PORTS:
    s = stats[name]
    psi_vals = s["psi"]
    v_vals = s["lyapunov_v"]
    pareto_vals = s["pareto"]
    cent_vals = s["centrality"]

    print(f"\n┌─ {name} {'─'*(50-len(name))}")
    print(f"│  Lines captured     : {s['lines']}")
    print(f"│  State transitions  : {len(s['state_changes'])}")
    print(f"│  Elections          : {len(s['elections'])}")
    print(f"│  CH beacon misses   : {s['ch_misses']}")
    print(f"│  CH lost events     : {s['ch_lost']}")
    print(f"│  Errors/Panics      : {len(s['errors'])}")

    if psi_vals:
        print(f"│  Ψ score  min/avg/max: {min(psi_vals):.4f} / {sum(psi_vals)/len(psi_vals):.4f} / {max(psi_vals):.4f}")
    if v_vals:
        print(f"│  Lyapunov V min/max : {min(v_vals):.6f} / {max(v_vals):.6f}")
    if pareto_vals:
        print(f"│  Pareto rank avg    : {sum(pareto_vals)/len(pareto_vals):.2f}")
    if cent_vals:
        print(f"│  Centrality avg     : {sum(cent_vals)/len(cent_vals):.3f}")

    if s["state_changes"]:
        print(f"│  State transitions:")
        for sc in s["state_changes"]:
            print(f"│    {sc}")

    if s["elections"]:
        print(f"│  Election events:")
        for ev in s["elections"][:5]:
            print(f"│    {ev}")

    if s["errors"]:
        print(f"│  ⚠ ERRORS:")
        for err in s["errors"][:5]:
            print(f"│    {err}")

    # Last 3 status snapshots
    snaps = s["status_snapshots"]
    if snaps:
        print(f"│  Last status: {snaps[-1]}")
    print(f"└{'─'*52}")

print(f"\n  Log files saved to: {OUT_DIR}/")
print(f"{'='*60}\n")
