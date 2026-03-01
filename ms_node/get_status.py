#!/usr/bin/env python3
import serial
import time
import sys

# Node ports
nodes = [
    ('/dev/tty.usbmodem5ABA0603751', 'Node1'),
    ('/dev/tty.usbmodem5ABA0605221', 'Node2'),
    ('/dev/tty.usbmodem5ABA0607321', 'Node3'),
]

print("=== Real-Time Cluster Status ===\n")

for port, name in nodes:
    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
        time.sleep(0.3)
        ser.reset_input_buffer()
        
        print(f"{name} ({port}):")
        found_status = False
        start = time.time()
        
        while time.time() - start < 10 and not found_status:  # Wait up to 10 seconds
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if 'STATUS: State=' in line:
                    print(f"  {line}")
                    found_status = True
                elif 'panic' in line.lower() or 'guru meditation' in line.lower():
                    print(f"  ❌ CRASH: {line}")
                    found_status = True
                    
        if not found_status:
            print(f"  ⏳ Waiting for status...")
            
        ser.close()
    except Exception as e:
        print(f"  ❌ Error: {e}")
    print()

print("Done!")
