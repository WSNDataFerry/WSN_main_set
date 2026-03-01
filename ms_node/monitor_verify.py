#!/usr/bin/env python3
import serial
import threading
import time
import sys
import re

# Configuration
DURATION_SEC = 120
NODES = [
    {"name": "Node1", "port": "/dev/tty.usbmodem5ABA0603751", "baud": 115200},
    {"name": "Node2", "port": "/dev/tty.usbmodem5ABA0605221", "baud": 115200},
    {"name": "Node3", "port": "/dev/tty.usbmodem5ABA0607321", "baud": 115200},
]

def monitor_node(node):
    filename = f"{node['name']}_verify.log"
    print(f"[{node['name']}] Connecting to {node['port']} -> {filename}")
    
    try:
        with serial.Serial(node['port'], node['baud'], timeout=1) as ser:
            with open(filename, 'w') as f:
                start_time = time.time()
                while time.time() - start_time < DURATION_SEC:
                    if ser.in_waiting:
                        try:
                            line_bytes = ser.readline()
                            line = line_bytes.decode('utf-8', errors='replace').strip()
                            if not line:
                                continue
                                
                            f.write(line + '\n')
                            f.flush()

                            # Verification Logic
                            if "SCHED:" in line:
                                print(f"✅ [{node['name']}] RX SCHEDULE: {line}")
                            elif "TIME SLICING:" in line:
                                print(f"✅ [{node['name']}] SLOT ACTIVE: {line}")
                            elif "Compressed:" in line:
                                print(f"✅ [{node['name']}] COMPRESSION: {line}")
                            elif "RX Sensor Data from node_" in line:
                                print(f"✅ [{node['name']}] RX DATA: {line}")
                                # Extract source
                                m = re.search(r"node_([0-9]+)", line)
                                if m:
                                    print(f"   -> From Node ID: {m.group(1)}")
                            elif "Sending Compressed:" in line:
                                print(f"✅ [{node['name']}] TX DATA: {line}")
                            elif "Packet type: 0xCF" in line:
                                print(f"✅ [{node['name']}] RX MAGIC 0xCF (Data): {line}")

                        except Exception as e:
                            print(f"Error reading line: {e}")
                    else:
                        time.sleep(0.01)
    except Exception as e:
        print(f"[{node['name']}] Connection failed: {e}")

def main():
    print(f"Starting verification monitor for {DURATION_SEC} seconds...")
    threads = []
    for node in NODES:
        t = threading.Thread(target=monitor_node, args=(node,))
        t.start()
        threads.append(t)
        
    for t in threads:
        t.join()
        
    print("Verification complete.")

if __name__ == "__main__":
    main()
