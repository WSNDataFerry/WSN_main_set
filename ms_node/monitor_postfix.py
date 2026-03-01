#!/usr/bin/env python3
"""2-minute post-fix monitor — verifies zero WDT panics after send-semaphore fix."""
import serial, threading, time, os
from datetime import datetime

PORTS = {
    "Node1": "/dev/cu.usbmodem5ABA0603751",
    "Node2": "/dev/cu.usbmodem5ABA0605221",
    "Node3": "/dev/cu.usbmodem5ABA0607321",
}
BAUD = 115200
DURATION = 120
OUT_DIR = "monitor_postfix_run"
os.makedirs(OUT_DIR, exist_ok=True)

stop_event = threading.Event()
lock = threading.Lock()

stats = {n: {
    "lines": 0, "state_changes": [], "elections": [],
    "ch_misses": 0, "ch_lost": 0, "panics": 0,
    "psi": [], "lyapunov_v": [],
    "trust_restored": [], "sem_timeout": 0,
    "status_snapshots": [], "last_status": "",
    "errors": [],
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
                    if "State transition:" in line:
                        s["state_changes"].append(f"[{ts}] {line.split('] ')[-1]}")
                    if "Election" in line or "Winner" in line:
                        s["elections"].append(f"[{ts}] {line.split('] ')[-1]}")
                    if "CH beacon missed" in line:
                        s["ch_misses"] += 1
                    if "CH lost" in line:
                        s["ch_lost"] += 1
                    if "Guru Meditation" in line or "panic'ed" in line:
                        s["panics"] += 1
                        s["errors"].append(f"[{ts}] {line}")
                    if "semaphore timeout" in line.lower():
                        s["sem_timeout"] += 1
                    if "Restored trust from NVS" in line:
                        s["trust_restored"].append(f"[{ts}] {line.split('] ')[-1]}")
                    if "Psi=" in line:
                        try:
                            s["psi"].append(float(line.split("Psi=")[1].strip()))
                        except:
                            pass
                    if "V=" in line and "Lyapunov" in line:
                        try:
                            s["lyapunov_v"].append(float(line.split("V=")[1].split(",")[0]))
                        except:
                            pass
                    if "STATUS:" in line:
                        s["last_status"] = f"[{ts}] {line}"
                        s["status_snapshots"].append(f"[{ts}] {line.split('STATUS: ')[-1]}")
            except:
                pass
    ser.close()
    print(f"  [{name}] Done. {stats[name]['lines']} lines.")


threads = []
print(f"\n{'='*60}")
print(f"  STELLAR WSN — 2-Minute Post-Fix Monitor")
print(f"  Start: {datetime.now().strftime('%H:%M:%S')}")
print(f"{'='*60}")
for name, port in PORTS.items():
    t = threading.Thread(target=monitor_node, args=(name, port), daemon=True)
    t.start()
    threads.append(t)
    print(f"  [{name}] Monitoring {port}")

start = time.time()
try:
    while time.time() - start < DURATION:
        elapsed = int(time.time() - start)
        with lock:
            status_line = " | ".join(
                f"{n}: {stats[n]['last_status'].split('State=')[-1].split(',')[0] if stats[n]['last_status'] else '?'}"
                for n in PORTS)
        print(f"\r  [{elapsed:3d}s / {DURATION}s] {status_line}   ", end="", flush=True)
        time.sleep(5)
except KeyboardInterrupt:
    print("\n  [!] Interrupted")

stop_event.set()
print(f"\n\n  Stop: {datetime.now().strftime('%H:%M:%S')}")
for t in threads:
    t.join(timeout=3)

# ── Analysis ──────────────────────────────────────────────────────────────────
print(f"\n{'='*60}")
print(f"  POST-FIX ANALYSIS SUMMARY")
print(f"{'='*60}")

for name in PORTS:
    s = stats[name]
    psi_vals = s["psi"]
    v_vals = s["lyapunov_v"]
    panic_flag = "✅ ZERO" if s["panics"] == 0 else f"🔴 {s['panics']}"
    print(f"\n+-- {name} {'-'*(48-len(name))}")
    print(f"|  Lines captured     : {s['lines']}")
    print(f"|  State transitions  : {len(s['state_changes'])}")
    print(f"|  Elections          : {len(s['elections'])}")
    print(f"|  CH beacon misses   : {s['ch_misses']}")
    print(f"|  CH lost events     : {s['ch_lost']}")
    print(f"|  PANICS (WDT)       : {panic_flag}  <-- KEY METRIC")
    print(f"|  Send sem timeouts  : {s['sem_timeout']}")
    print(f"|  Trust restored NVS : {len(s['trust_restored'])}")
    if psi_vals:
        avg = sum(psi_vals) / len(psi_vals)
        spread = max(psi_vals) - min(psi_vals)
        print(f"|  Psi min/avg/max    : {min(psi_vals):.4f} / {avg:.4f} / {max(psi_vals):.4f}  (spread={spread:.4f})")
    if v_vals:
        print(f"|  Lyapunov V min/max : {min(v_vals):.6f} / {max(v_vals):.6f}")
    for sc in s["state_changes"]:
        print(f"|  TRANSITION: {sc}")
    for tr in s["trust_restored"]:
        print(f"|  TRUST RESTORE: {tr}")
    if s["errors"]:
        for err in s["errors"][:3]:
            print(f"|  ERROR: {err}")
    snaps = s["status_snapshots"]
    if snaps:
        print(f"|  Last status: {snaps[-1]}")
    print(f"+{'-'*52}")

print(f"\n  Logs saved to: {OUT_DIR}/")
print(f"{'='*60}\n")
