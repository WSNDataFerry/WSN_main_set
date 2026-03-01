#!/usr/bin/env python3
"""
Check if WSN nodes discover each other: run serial monitor on all devices
from devices.yaml, then inspect logs for cluster states.

Usage:
  python check_cluster.py              # use devices.yaml
  python check_cluster.py --duration 35
"""
import subprocess
import sys
import threading
import time
from pathlib import Path

try:
    import yaml
except ImportError:
    yaml = None

NODES = [
    {"port": "/dev/tty.usbmodem5ABA0607321", "name": "Node3", "mac": "10:20:ba:4c:5a:98"},
    {"port": "/dev/tty.usbmodem5ABA0605221", "name": "Node2", "mac": "10:20:ba:4c:5c:b0"},
    {"port": "/dev/tty.usbmodem5ABA0603751", "name": "Node1", "mac": "10:20:ba:4b:dd:c0"},
]

MS_NODE_ROOT = Path(__file__).resolve().parent
DEVICES_FILE = MS_NODE_ROOT / "devices.yaml"


def load_nodes():
    if yaml and DEVICES_FILE.exists():
        with open(DEVICES_FILE) as f:
            data = yaml.safe_load(f) or {}
        return data.get("devices", NODES)
    return NODES


def monitor_node(node, duration=30):
    import serial
    log_file = f"/tmp/{node['name']}.log"
    print(f"[{node['name']}] Starting monitor on {node['port']} -> {log_file}")
    
    try:
        with serial.Serial(node["port"], 115200, timeout=0.1) as ser, open(log_file, "w") as f:
            start_time = time.time()
            while time.time() - start_time < duration:
                line = ser.readline()
                if line:
                    try:
                        decoded_line = line.decode('utf-8', errors='replace')
                        f.write(decoded_line)
                        f.flush()
                    except Exception:
                        pass
    except Exception as e:
        print(f"[{node['name']}] Error: {e}")


def check_logs(nodes, wait_s=35):
    time.sleep(wait_s)
    print("\n=== Checking discovery logs ===")
    for node in nodes:
        log_file = f"/tmp/{node['name']}.log"
        try:
            with open(log_file) as f:
                content = f.read()
            if "STATE_DISCOVER" in content:
                print(f"  ✓ {node['name']}: Entered DISCOVER state")
            if "STATE_CANDIDATE" in content:
                print(f"  ✓ {node['name']}: Entered CANDIDATE state")
            if "STATE_CH" in content:
                print(f"  ✓ {node['name']}: ELECTED AS CLUSTER HEAD")
            if "STATE_MEMBER" in content:
                print(f"  ✓ {node['name']}: Became cluster member")
            if "Peer discovered" in content or "BLE scan" in content:
                print(f"    {node['name']}: Saw BLE activity")
        except FileNotFoundError:
            print(f"  ✗ {node['name']}: No log file")


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=int, default=30, help="Monitor duration seconds")
    ap.add_argument("--wait", type=int, default=35, help="Seconds before checking logs")
    args = ap.parse_args()
    nodes = load_nodes()
    if not nodes:
        print("No devices in devices.yaml (or use default NODES)", file=sys.stderr)
        sys.exit(1)
    print("Starting multi-node monitor (use devices.yaml if present)...")
    print(f"Will run {args.duration}s then check logs.\n")
    threads = []
    for node in nodes:
        t = threading.Thread(target=monitor_node, args=(node, args.duration))
        t.start()
        threads.append(t)
        time.sleep(1)
    check_logs(nodes, args.wait)
    check_logs(nodes, args.wait)
    print("\nMonitoring complete. Check /tmp/Node*.log for full logs.")


if __name__ == "__main__":
    main()
