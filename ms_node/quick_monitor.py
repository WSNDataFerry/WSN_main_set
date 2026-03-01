#!/usr/bin/env python3
"""Quick serial monitor to check node status"""
import serial
import sys
import time

def monitor_node(port, duration=20):
    try:
        ser = serial.Serial(port, 115200, timeout=1)
        print(f"\n=== Monitoring {port} for {duration} seconds ===\n")
        
        start_time = time.time()
        line_count = 0
        max_lines = 200
        
        while (time.time() - start_time) < duration and line_count < max_lines:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    print(line)
                    line_count += 1
                    # Check for key indicators
                    if 'STORAGE' in line or 'MSLG' in line or 'State transition' in line or 'CH Stored' in line or 'Stored sensor data' in line:
                        print(f"  ⭐ Key event detected!")
            else:
                time.sleep(0.1)
        
        ser.close()
        print(f"\n=== Captured {line_count} lines ===\n")
        return True
    except Exception as e:
        print(f"Error monitoring {port}: {e}")
        return False

if __name__ == '__main__':
    port = sys.argv[1] if len(sys.argv) > 1 else '/dev/cu.usbmodem5ABA0603751'
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    monitor_node(port, duration)
