import serial
import time
import threading
import os
import sys

# Configuration
DURATION_SEC = 300  # 10 Minutes
LOG_DIR = "TestCurrent"
PORTS = {
    "/dev/cu.usbmodem5ABA0603751": "Node1",
    "/dev/cu.usbmodem5ABA0605221": "Node2",
    "/dev/cu.usbmodem5ABA0607321": "Node3",
}

def monitor_node(port, node_name, stop_event):
    log_file_path = os.path.join(LOG_DIR, f"{node_name}.log")
    try:
        ser = serial.Serial(port, 115200, timeout=1)
        print(f"Connected to {node_name} on {port}")
    except Exception as e:
        print(f"Failed to connect to {node_name}: {e}")
        return

    with open(log_file_path, "w") as f:
        while not stop_event.is_set():
            try:
                if ser.in_waiting:
                    line = ser.readline().decode("utf-8", errors="replace").strip()
                    timestamp = time.strftime("%H:%M:%S")
                    log_entry = f"[{timestamp}] {line}"
                    # Print critical events to console so we can see them live
                    if "task_wdt" in line or "rst:" in line or "Guru" in line:
                         print(f"[{node_name}] \U0001f6a8 {line}")
                    elif "ELECTION" in line or "SENT OK" in line:
                         print(f"[{node_name}] {line}")
                    
                    f.write(log_entry + "\n")
                    f.flush()
                else:
                    time.sleep(0.01)
            except Exception as e:
                f.write(f"[ERROR] Serial read failed: {e}\n")
                break
    ser.close()
    print(f"Finished monitoring {node_name}")

def main():
    if not os.path.exists(LOG_DIR):
        os.makedirs(LOG_DIR)

    print(f"Starting {DURATION_SEC}s monitor session. Logging to {LOG_DIR}...")
    
    stop_event = threading.Event()
    threads = []

    for port, name in PORTS.items():
        t = threading.Thread(target=monitor_node, args=(port, name, stop_event))
        t.start()
        threads.append(t)

    try:
        # Update progress
        for i in range(DURATION_SEC):
            time.sleep(1)
            if i % 60 == 0 and i > 0:
                print(f"--- {i // 60} minutes elapsed ---")
    except KeyboardInterrupt:
        print("\nStopping monitor early...")

    stop_event.set()
    for t in threads:
        t.join()

    print("Monitor session complete.")

if __name__ == "__main__":
    main()
