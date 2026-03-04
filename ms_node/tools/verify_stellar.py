#!/usr/bin/env python3
import sys
import re
import time
import argparse
import serial
from serial.tools import list_ports
import glob

# ==========================
# STELLAR Log Patterns
# ==========================
PATTERNS = {
    "lyapunov": re.compile(
        r"METRICS: \[STELLAR\] Lyapunov weights: "
        r"B=([\d\.]+) U=([\d\.]+) T=([\d\.]+) L=([\d\.]+), "
        r"V=([\d\.]+), Conv=(\d)"
    ),
    "pareto": re.compile(
        r"METRICS: \[STELLAR\] Pareto rank=(\d+)/(\d+), Centrality=([\d\.]+)"
    ),
    "score": re.compile(
        r"METRICS: \[STELLAR\] Score components: .* Ψ=([\d\.]+)"
    )
}

# ==========================
# Linux Serial Scan
# ==========================
def scan_ports():
    """
    Prefer /dev/ttyACM* (ESP32 / CDC ACM devices)
    Fallback to pyserial detected ports if needed
    """
    acm_ports = sorted(glob.glob("/dev/ttyACM*"))
    if acm_ports:
        return acm_ports

    # Fallback: pyserial enumeration
    ports = list_ports.comports()
    devices = []
    for p in ports:
        if "Bluetooth" not in p.device and "debug" not in p.device:
            devices.append(p.device)
    return devices

# ==========================
# Monitor STELLAR Output
# ==========================
def monitor_stellar(port, baudrate=115200, duration=30):
    print(f"Connecting to {port} @ {baudrate} baud")
    try:
        ser = serial.Serial(port, baudrate, timeout=1)
    except serial.SerialException as e:
        print(f"❌ Failed to open serial port: {e}")
        return

    start_time = time.time()
    stellar_data = {
        "weights_updates": 0,
        "pareto_updates": 0,
        "score_updates": 0,
        "converged": False,
        "last_psi": 0.0
    }

    print(f"Monitoring for {duration} seconds (Ctrl+C to stop)")
    print("-" * 60)

    try:
        while (time.time() - start_time) < duration:
            if ser.in_waiting:
                try:
                    line = ser.readline().decode("utf-8", errors="replace").strip()
                except Exception:
                    continue

                if not line:
                    continue

                # Lyapunov Weights
                m = PATTERNS["lyapunov"].search(line)
                if m:
                    b, u, t, l_q, v, conv = m.groups()
                    print(
                        f"[WEIGHTS] B={b} U={u} T={t} L={l_q} | "
                        f"V={v} Converged={'YES' if conv=='1' else 'NO'}"
                    )
                    stellar_data["weights_updates"] += 1
                    if conv == "1":
                        stellar_data["converged"] = True

                # Pareto Rank
                m = PATTERNS["pareto"].search(line)
                if m:
                    rank, total, cent = m.groups()
                    print(f"[PARETO]  Rank={rank}/{total} Centrality={cent}")
                    stellar_data["pareto_updates"] += 1

                # Score
                m = PATTERNS["score"].search(line)
                if m:
                    psi = float(m.group(1))
                    print(f"[SCORE]   Ψ (Psi) = {psi}")
                    stellar_data["score_updates"] += 1
                    stellar_data["last_psi"] = psi
            else:
                time.sleep(0.01)

    except KeyboardInterrupt:
        print("\n⏹ Monitoring stopped by user")
    finally:
        ser.close()

    # ==========================
    # Summary
    # ==========================
    print("-" * 60)
    print("STELLAR Verification Summary")
    print(f"  Lyapunov Updates : {stellar_data['weights_updates']}")
    print(f"  Pareto Updates   : {stellar_data['pareto_updates']}")
    print(f"  Score Updates    : {stellar_data['score_updates']}")
    print(f"  Converged        : {'YES' if stellar_data['converged'] else 'NO'}")
    print(f"  Last Ψ Score     : {stellar_data['last_psi']:.4f}")

    if stellar_data["weights_updates"] and stellar_data["score_updates"]:
        print("\n✅ STELLAR algorithm is ACTIVE")
    else:
        print("\n❌ STELLAR algorithm NOT detected")

# ==========================
# Entry Point
# ==========================
if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Linux STELLAR Serial Monitor"
    )
    parser.add_argument(
        "--port", help="Serial port (e.g. /dev/ttyACM0)"
    )
    parser.add_argument(
        "--duration", type=int, default=90,
        help="Monitor duration in seconds"
    )
    args = parser.parse_args()

    target_port = args.port
    if not target_port:
        ports = scan_ports()
        if not ports:
            print("❌ No serial ports found")
            sys.exit(1)

        print("Detected serial ports:")
        for i, p in enumerate(ports):
            print(f"  [{i}] {p}")

        if len(ports) == 1:
            target_port = ports[0]
            print(f"Auto-selected {target_port}")
        else:
            try:
                sel = int(input("Select port index: "))
                target_port = ports[sel]
            except (ValueError, IndexError):
                print("Invalid selection")
                sys.exit(1)

    monitor_stellar(target_port, duration=args.duration)