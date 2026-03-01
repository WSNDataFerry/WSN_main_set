#!/usr/bin/env python3
"""
raw_serial_capture.py — Capture 60s of raw serial output from all 3 nodes.
Print all lines verbatim; look for: Send, CH, RX, Fallback, Cache, Phase
"""
import serial
import threading
import time

PORTS = [
    "/dev/cu.usbmodem5ABA0603751",
    "/dev/cu.usbmodem5ABA0605221",
    "/dev/cu.usbmodem5ABA0607321",
]
BAUD = 115200
DURATION = 60

stop_event = threading.Event()

# Keywords to include in output (everything else is suppressed to reduce noise)
KEYWORDS = [
    "STATE", "state", "MEMBER", "CH:", "->", "phase", "PHASE",
    "FALLBACK", "Sent sensor", "SEND FAIL", "RX Sensor", "RX Compress",
    "cache", "Cache", "can_send", "current_ch", "timestamp", "node_id=",
    "Send skip", "CH MAC", "cached", "SCHED", "DATA", "STELLAR",
    "Slot", "slot", "beacon", "Beacon",
]

def monitor_node(idx, port):
    try:
        ser = serial.Serial(port, BAUD, timeout=0.5)
    except Exception as e:
        print(f"[P{idx+1}] OPEN FAILED: {e}")
        return

    print(f"[P{idx+1}] {port[-22:]} connected")
    while not stop_event.is_set():
        try:
            line = ser.readline().decode("utf-8", errors="replace").strip()
        except Exception:
            break
        if not line:
            continue
        # Only print lines matching keywords
        if any(kw in line for kw in KEYWORDS):
            ts = time.strftime("%H:%M:%S")
            print(f"[{ts}][N{idx+1}] {line}")
    ser.close()

threads = []
for i, port in enumerate(PORTS):
    t = threading.Thread(target=monitor_node, args=(i, port), daemon=True)
    t.start()
    threads.append(t)

print(f"\n=== Raw capture: {DURATION}s ===\n")
start = time.time()
while time.time() - start < DURATION:
    time.sleep(1)

stop_event.set()
for t in threads:
    t.join(timeout=2)
print("\n=== Capture complete ===\n")
