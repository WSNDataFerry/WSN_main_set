#!/usr/bin/env python3
import sys
import re
import time
import argparse
import serial
import serial
from serial.tools import list_ports
import glob

# STELLAR Log Patterns
PATTERNS = {
    "lyapunov": re.compile(r"METRICS: \[STELLAR\] Lyapunov weights: B=([\d\.]+) U=([\d\.]+) T=([\d\.]+) L=([\d\.]+), V=([\d\.]+), Conv=(\d)"),
    "pareto": re.compile(r"METRICS: \[STELLAR\] Pareto rank=(\d+)/(\d+), Centrality=([\d\.]+)"),
    "score": re.compile(r"METRICS: \[STELLAR\] Score components: .* Ψ=([\d\.]+)")
}

def scan_ports():
    ports = list(list_ports.comports())
    devices = []
    for p in ports:
        # Filter out common macOS virtual ports that are never ESP32s
        if sys.platform == 'darwin':
            if "usbmodem" in p.device:
                devices.append(p.device)
        else:
            # For other platforms, we keep standard behavior but still filter obvious noise
            if "Bluetooth" not in p.device and "debug-console" not in p.device:
                devices.append(p.device)
    return devices

def monitor_stellar(port, baudrate=115200, duration=30):
    print(f"Connecting to {port} at {baudrate} baud...")
    try:
        ser = serial.Serial(port, baudrate, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening port: {e}")
        return

    start_time = time.time()
    stellar_data = {
        "weights_updates": 0,
        "pareto_updates": 0,
        "score_updates": 0,
        "converged": False,
        "last_psi": 0.0
    }

    print(f"Monitoring for {duration} seconds... Press Ctrl+C to stop early.")
    print("-" * 60)
    
    try:
        while (time.time() - start_time) < duration:
            if ser.in_waiting:
                try:
                    line = ser.readline().decode('utf-8', errors='replace').strip()
                except Exception:
                    continue
                    
                if not line:
                    continue

                # Check Lyapunov Weights
                m_lyap = PATTERNS["lyapunov"].search(line)
                if m_lyap:
                    b, u, t, l_q, v, conv = m_lyap.groups()
                    print(f"[WEIGHTS] B={b} U={u} T={t} L={l_q} | V={v} Converged={'YES' if conv=='1' else 'NO'}")
                    stellar_data["weights_updates"] += 1
                    if conv == '1':
                        stellar_data["converged"] = True

                # Check Pareto
                m_par = PATTERNS["pareto"].search(line)
                if m_par:
                    rank, total, cent = m_par.groups()
                    print(f"[PARETO]  Rank={rank}/{total} Centrality={cent}")
                    stellar_data["pareto_updates"] += 1

                # Check Score
                m_score = PATTERNS["score"].search(line)
                if m_score:
                    psi = m_score.group(1)
                    print(f"[SCORE]   Ψ (Psi) = {psi}")
                    stellar_data["score_updates"] += 1
                    stellar_data["last_psi"] = float(psi)
            else:
                time.sleep(0.01)

    except KeyboardInterrupt:
        print("\nStopping monitor...")
    except Exception as e:
        print(f"Error reading serial: {e}")
    finally:
        ser.close()

    print("-" * 60)
    print("STELLAR Verification Summary:")
    print(f"  - Lyapunov Weight Updates: {stellar_data['weights_updates']}")
    print(f"  - Pareto Rank Updates:     {stellar_data['pareto_updates']}")
    print(f"  - Score Calculations:      {stellar_data['score_updates']}")
    print(f"  - Weight Convergence:      {'✅ YES' if stellar_data['converged'] else '❌ NO (or not yet)'}")
    print(f"  - Last STELLAR Score:      {stellar_data['last_psi']:.4f}")
    
    if stellar_data["weights_updates"] > 0 and stellar_data["score_updates"] > 0:
        print("\n✅ STELLAR Algorithm is ACTIVE and processing data.")
    else:
        print("\n❌ STELLAR Algorithm appears INACTIVE (no specific logs found).")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Verify STELLAR Algorithm Functionality")
    parser.add_argument("--port", help="Serial port to monitor", default=None)
    parser.add_argument("--duration", type=int, default=90, help="Duration to monitor in seconds (recommended >=60s for phase cycle)")
    args = parser.parse_args()

    target_port = args.port
    if not target_port:
        available_ports = scan_ports()
        if not available_ports:
            print("No serial ports found.")
            sys.exit(1)
        # Verify if one of them is likely the ESP32
        print("Available ports:")
        for i, p in enumerate(available_ports):
            print(f"  [{i}] {p}")
        
        if len(available_ports) == 1:
            target_port = available_ports[0]
            print(f"Auto-selecting {target_port}")
        else:
            try:
                sel = int(input("Select port index: "))
                target_port = available_ports[sel]
            except (ValueError, IndexError):
                print("Invalid selection.")
                sys.exit(1)

    monitor_stellar(target_port, duration=args.duration)
