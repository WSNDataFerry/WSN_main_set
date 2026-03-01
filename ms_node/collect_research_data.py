import serial
import time
import csv
import threading
import re
import argparse
from datetime import datetime

# Configuration
NODES = [
    {"id": "Node1", "port": "/dev/tty.usbmodem5ABA0603751", "baud": 115200},
    {"id": "Node2", "port": "/dev/tty.usbmodem5ABA0605221", "baud": 115200},
    {"id": "Node3", "port": "/dev/tty.usbmodem5ABA0607321", "baud": 115200}
]

LOG_DIR = "research_data"
DURATION_SEC = 120  # 2 Minutes

# Regex Patterns
REGEX_STATUS = r"STATUS: State=(\w+), Role=(\w+), CH=(\d+)"
REGEX_STELLAR = r"STELLAR.*Ψ=(\d+\.\d+)"
REGEX_METRICS = r"METRICS.*PDR=(\d+\.\d+).*HSR=(\d+\.\d+)"
REGEX_BATTERY = r"PME batt=(\d+)%"

def read_serial(node_config, stop_event):
    node_id = node_config["id"]
    port = node_config["port"]
    baud = node_config["baud"]
    
    filename = f"{LOG_DIR}/{node_id}_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
    
    print(f"[{node_id}] Connecting to {port}...")
    
    try:
        with serial.Serial(port, baud, timeout=1) as ser, open(filename, 'w', newline='') as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow(["Timestamp", "Raw_Line", "State", "Role", "CH_ID", "Stellar_Score", "PDR", "Battery"])
            
            print(f"[{node_id}] Logging to {filename}")
            
            # State tracking
            current_state = "UNKNOWN"
            current_role = "UNKNOWN"
            current_ch = "0"
            current_score = "0.0"
            current_pdr = "0.0"
            current_battery = "0"

            while not stop_event.is_set():
                if ser.in_waiting:
                    try:
                        line = ser.readline().decode('utf-8', errors='ignore').strip()
                        if not line:
                            continue
                            
                        timestamp = datetime.now().isoformat()
                        
                        # Parse Metrics
                        m_status = re.search(REGEX_STATUS, line)
                        if m_status:
                            current_state = m_status.group(1)
                            current_role = m_status.group(2)
                            current_ch = m_status.group(3)
                            
                        m_stellar = re.search(REGEX_STELLAR, line)
                        if m_stellar:
                            current_score = m_stellar.group(1)
                            
                        m_metrics = re.search(REGEX_METRICS, line)
                        if m_metrics:
                            current_pdr = m_metrics.group(1)
                            
                        m_batt = re.search(REGEX_BATTERY, line)
                        if m_batt:
                            current_battery = m_batt.group(1)

                        # Write to CSV
                        writer.writerow([timestamp, line, current_state, current_role, current_ch, current_score, current_pdr, current_battery])
                        csvfile.flush()
                        
                        # Print meaningful updates to console
                        if "STATUS" in line or "ELECTION" in line or "MY NODE ID" in line:
                            print(f"[{node_id}] {line}")
                            
                    except Exception as e:
                        print(f"[{node_id}] Error reading line: {e}")
                else:
                    time.sleep(0.01)
                    
    except serial.SerialException as e:
        print(f"[{node_id}] Connection Failed: {e}")

def main():
    parser = argparse.ArgumentParser(description='Collect WSN Research Data')
    parser.add_argument('--duration', type=int, default=3600, help='Duration in seconds')
    args = parser.parse_args()
    
    duration = args.duration
    import os
    if not os.path.exists(LOG_DIR):
        os.makedirs(LOG_DIR)
        
    stop_event = threading.Event()
    threads = []
    
    print(f"Starting Data Collection for {duration} seconds...")
    
    for node in NODES:
        t = threading.Thread(target=read_serial, args=(node, stop_event))
        t.daemon = True
        t.start()
        threads.append(t)
        
    try:
        # Run for specified duration
        for _ in range(duration):
            time.sleep(1)
            # print(f"Time remaining: {duration - _}s", end='\r')
    except KeyboardInterrupt:
        print("\nStopping collection...")
    finally:
        stop_event.set()
        for t in threads:
            t.join()
        print("Data Collection Complete.")

if __name__ == "__main__":
    main()
