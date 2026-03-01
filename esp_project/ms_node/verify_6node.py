import serial
import time
import glob
import re

ports = sorted(glob.glob('/dev/cu.usb*'))[:3]
sessions = []

ch_rx_regex = re.compile(r"Received Historical Payload string from Node:\s*(\d+)")
seen_nodes = set()

print(f"Connecting to {len(ports)} local nodes...")
for p in ports:
    try:
        s = serial.Serial(p, 115200, timeout=0.1)
        sessions.append({'port': p, 'serial': s})
    except Exception as e:
        pass

print("Monitoring for 90 seconds to allow full TDMA schedules to complete...")
start_time = time.time()

try:
    while time.time() - start_time < 90:
        for session in sessions:
            if session['serial'].in_waiting:
                line = session['serial'].readline().decode('utf-8', errors='ignore').strip()
                match = ch_rx_regex.search(line)
                if match:
                    node_id = match.group(1)
                    if node_id not in seen_nodes:
                        print(f"[SUCCESS] CH just received payload from Node ID: {node_id}")
                        seen_nodes.add(node_id)
except KeyboardInterrupt:
    pass

print("\n--- 6-NODE CLUSTER VERIFICATION REPORT ---")
if len(seen_nodes) == 0:
    print("❌ No data receptions detected. Is a local CH elected?")
else:
    print(f"✅ Your local CH successfully received data from {len(seen_nodes)} distinct nodes:")
    for n in seen_nodes:
        print(f"   - Node {n}")

for s in sessions:
    s['serial'].close()
