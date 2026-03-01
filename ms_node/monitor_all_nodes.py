#!/usr/bin/env python3
"""Monitor all nodes simultaneously and save logs to files"""
import serial
import sys
import time
import threading
from datetime import datetime
import os

# Ports from devices.yaml
NODES = [
    {"name": "Node1", "port": "/dev/cu.usbmodem5ABA0603751"},
    {"name": "Node2", "port": "/dev/cu.usbmodem5ABA0605221"},
    {"name": "Node3", "port": "/dev/cu.usbmodem5ABA0607321"},
]

def monitor_node(node_config, duration, output_dir, stop_event):
    """Monitor a single node and save to file"""
    name = node_config["name"]
    port = node_config["port"]
    
    log_file = os.path.join(output_dir, f"{name}.log")
    
    try:
        ser = serial.Serial(port, 115200, timeout=1)
        print(f"[{name}] Connected to {port}")
        
        with open(log_file, 'w', encoding='utf-8') as f:
            f.write(f"=== Monitoring {name} ({port}) ===\n")
            f.write(f"Started: {datetime.now().isoformat()}\n")
            f.write(f"Duration: {duration} seconds\n")
            f.write("=" * 60 + "\n\n")
            
            start_time = time.time()
            line_count = 0
            
            while not stop_event.is_set() and (time.time() - start_time) < duration:
                if ser.in_waiting:
                    try:
                        line = ser.readline().decode('utf-8', errors='ignore').strip()
                        if line:
                            timestamp = datetime.now().strftime('%H:%M:%S')
                            log_line = f"[{timestamp}] {line}\n"
                            f.write(log_line)
                            f.flush()  # Ensure immediate write
                            line_count += 1
                            
                            # Print key events to console
                            if any(keyword in line for keyword in ['MSLG', 'CH Stored', 'Stored sensor data', 
                                                                   'State transition', 'STELLAR', 'ERROR']):
                                print(f"[{name}] {line[:80]}")
                    except Exception as e:
                        print(f"[{name}] Error reading line: {e}")
                else:
                    time.sleep(0.1)
            
            elapsed = time.time() - start_time
            f.write(f"\n{'=' * 60}\n")
            f.write(f"Ended: {datetime.now().isoformat()}\n")
            f.write(f"Duration: {elapsed:.1f} seconds\n")
            f.write(f"Lines captured: {line_count}\n")
        
        ser.close()
        print(f"[{name}] Monitoring complete: {line_count} lines saved to {log_file}")
        return True
        
    except serial.SerialException as e:
        print(f"[{name}] Serial error: {e}")
        return False
    except Exception as e:
        print(f"[{name}] Error: {e}")
        return False

def main():
    duration = 300  # 5 minutes
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    output_dir = f"monitoring_{timestamp}"

    # Create output directory
    os.makedirs(output_dir, exist_ok=True)
    print(f"Created output directory: {output_dir}")
    print(f"Monitoring {len(NODES)} nodes for {duration} seconds (5 minutes)...\n")
    
    # Create stop event for graceful shutdown
    stop_event = threading.Event()
    
    # Start monitoring threads
    threads = []
    for node in NODES:
        if not os.path.exists(node["port"]):
            print(f"[{node['name']}] WARNING: Port {node['port']} not found, skipping")
            continue
        
        t = threading.Thread(
            target=monitor_node,
            args=(node, duration, output_dir, stop_event),
            daemon=True
        )
        t.start()
        threads.append(t)
        time.sleep(0.5)  # Stagger connections
    
    if not threads:
        print("ERROR: No nodes available to monitor")
        return 1
    
    # Wait for duration
    print(f"\nMonitoring started at {datetime.now().strftime('%H:%M:%S')}")
    print("Press Ctrl+C to stop early\n")
    
    try:
        time.sleep(duration)
    except KeyboardInterrupt:
        print("\n\nStopping monitoring early...")
        stop_event.set()
    
    # Wait for threads to finish
    for t in threads:
        t.join(timeout=5)
    
    print(f"\n=== Monitoring Complete ===")
    print(f"Logs saved to: {output_dir}/")
    print(f"Files:")
    for node in NODES:
        log_file = os.path.join(output_dir, f"{node['name']}.log")
        if os.path.exists(log_file):
            size = os.path.getsize(log_file)
            print(f"  - {node['name']}.log ({size:,} bytes)")
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
