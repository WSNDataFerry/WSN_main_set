#!/usr/bin/env python3
import serial
import threading
import time
import sys

# Configuration
DURATION_SEC = 300  # Debug capture
NODES = [
    {"name": "Node1", "port": "/dev/tty.usbmodem5ABA0603751", "baud": 115200},
    {"name": "Node2", "port": "/dev/tty.usbmodem5ABA0605221", "baud": 115200},
    {"name": "Node3", "port": "/dev/tty.usbmodem5ABA0607321", "baud": 115200},
]

def monitor_node(node):
    filename = f"{node['name']}_monitor.log"
    print(f"[{node['name']}] Connecting to {node['port']} -> {filename}")
    
    try:
        with serial.Serial(node['port'], node['baud'], timeout=1) as ser, open(filename, 'w') as f:
            start_time = time.time()
            while time.time() - start_time < DURATION_SEC:
                if ser.in_waiting:
                    line = ser.readline()
                    try:
                        decoded = line.decode('utf-8', errors='replace')
                        timestamp = time.strftime("%H:%M:%S")
                        f.write(f"[{timestamp}] {decoded}")
                        f.flush()
                    except Exception as e:
                        print(f"[{node['name']}] Decode error: {e}")
                else:
                    time.sleep(0.01)
    except Exception as e:
        print(f"[{node['name']}] Connection failed: {e}")

def main():
    print(f"Starting network monitor for {DURATION_SEC} seconds...")
    threads = []
    
    # Clear any previous locks/issues by waiting a sec? No, just run.
    
    for node in NODES:
        t = threading.Thread(target=monitor_node, args=(node,))
        t.start()
        threads.append(t)
        
    for t in threads:
        t.join()
        
    print("Monitoring complete.")

if __name__ == "__main__":
    main()
