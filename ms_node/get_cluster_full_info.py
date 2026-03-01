#!/usr/bin/env python3
import serial
import time
import sys
import json
from datetime import datetime

# Node configuration
NODES = [
    {'name': 'Node1', 'port': '/dev/tty.usbmodem5ABA0603751'},
    {'name': 'Node2', 'port': '/dev/tty.usbmodem5ABA0605221'},
    {'name': 'Node3', 'port': '/dev/tty.usbmodem5ABA0607321'},
]

def get_node_report(name, port):
    print(f"[{name}] Connecting to {port}...")
    try:
        # Open serial port
        ser = serial.Serial(port, 115200, timeout=2.0)
        time.sleep(3.0) # Wait even longer for connection to stabilize
        
        # Flush input
        ser.reset_input_buffer()
        
        # Send CLUSTER command
        print(f"[{name}] Sending CLUSTER command...")
        ser.write(b'\nCLUSTER\n')
        ser.flush()
        
        # Read response
        buffer = []
        started = False
        start_time = time.time()
        
        while time.time() - start_time < 15.0: # 15 second timeout
            if ser.in_waiting:
                try:
                    line = ser.readline().decode('utf-8', errors='ignore').strip()
                except:
                    continue
                    
                if line == "CLUSTER_REPORT_START":
                    started = True
                    buffer = [] # Clear buffer, start fresh
                    continue
                
                if line == "CLUSTER_REPORT_END":
                    ser.close()
                    return parse_buffer(buffer)
                
                if started:
                    buffer.append(line)
            else:
                time.sleep(0.05)
                
        ser.close()
        print(f"[{name}] Timeout waiting for report.")
        return None
        
    except serial.SerialException as e:
        print(f"[{name}] Connection Error: {e}")
        return None
    except Exception as e:
        print(f"[{name}] Error: {e}")
        return None

def parse_buffer(lines):
    data = {'neighbors': []}
    current_neighbor = {}
    
    # Simple state machine for parsing
    for line in lines:
        if '=' not in line:
            continue
            
        key, value = line.split('=', 1)
        key = key.strip()
        value = value.strip()
        
        if key.startswith('MEMBER_'):
            # Neighbor/Member data
            if key == 'MEMBER_COUNT':
                # This is a global field, not per-member
                data['MEMBER_COUNT'] = value
                continue
                
            if key == 'MEMBER_ID':
                # New member starting, save previous if exists
                if current_neighbor:
                    data['neighbors'].append(current_neighbor)
                current_neighbor = {'ID': value}
            else:
                k = key.replace('MEMBER_', '')
                current_neighbor[k] = value
        else:
            # Node (Self) data
            data[key] = value
            
    # Append last neighbor
    if current_neighbor:
        data['neighbors'].append(current_neighbor)
        
    return data

def print_summary(results):
    print("\n" + "="*80)
    print(f"CLUSTER FULL REPORT - {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("="*80)
    
    # 1. Cluster Overview Table
    print(f"\n{'NODE':<8} {'ROLE':<10} {'ID':<5} {'CH':<5} {'STELLAR':<8} {'BATTERY':<8} {'UPTIME_H':<8} {'TRUST':<6} {'LINK_Q':<8} {'MEMBERS':<8}")
    print("-" * 90)
    
    cluster_head = None
    
    for r in results:
        d = r['data']
        name = r['name']
        
        if not d:
            print(f"{name:<8} {'OFFLINE/ERROR':<50}")
            continue
            
        is_ch = (d.get('IS_CH') == '1')
        role = "CH" if is_ch else "MEMBER"
        
        if is_ch:
            cluster_head = f"Node {d.get('NODE_ID')} ({name})"
            
        # Extract values with safe defaults
        node_id = d.get('NODE_ID', '?')
        curr_ch = d.get('CURRENT_CH', '?')
        stellar = float(d.get('STELLAR_SCORE', 0))
        batt = float(d.get('BATTERY', 0))
        trust = float(d.get('TRUST', 0))
        linkq = float(d.get('LINK_QUALITY', 0))
        
        # Calculate Uptime in Hours (Approximate based on boot time if available, or just placeholder)
        # The C code doesn't output UPTIME directly in CLUSTER_REPORT, let's fix that or use a placeholder
        # For now, we'll use a placeholder '?' or 0 if not available
        uptime = float(d.get('UPTIME', 0)) / 3600.0 if d.get('UPTIME') else 0.0
        
        # Fix Member Count: Use the actual list length if > 0, else use reported
        reported_mem_count = int(d.get('MEMBER_COUNT', 0))
        actual_mem_count = len(d.get('neighbors', []))
        
        # If reported is 0 but we see neighbors, use the neighbor count (filtered by cluster radius)
        display_mem_count = max(reported_mem_count, actual_mem_count)

        if is_ch:
            print(f"{name:<8} {role:<10} {node_id:<5} {curr_ch:<5} {stellar:<8.4f} {batt:<8.2f} {uptime:<8.1f} {trust:<6.2f} {linkq:<8.2f} {display_mem_count:<8}")
        else:
             # For members, hide "MEMBERS" count (redundant/confusing)
            print(f"{name:<8} {role:<10} {node_id:<5} {curr_ch:<5} {stellar:<8.4f} {batt:<8.2f} {uptime:<8.1f} {trust:<6.2f} {linkq:<8.2f} {'-':<8}")

    print("-" * 90)
    if cluster_head:
        print(f"ACTUAL CLUSTER HEAD: {cluster_head}")
    else:
        print("ACTUAL CLUSTER HEAD: NONE / ELECTION IN PROGRESS")
    print("="*80)

    # 2. Detailed Node Views
    print("\nDETAILED NODE VIEWS")
    print("="*80)
    for r in results:
        d = r['data']
        name = r['name']
        if not d: continue
        
        node_id = d.get('NODE_ID', '?')
        role = d.get('ROLE', 'UNKNOWN')
        is_ch = (d.get('IS_CH') == '1')
        role_desc = "Cluster Head (LEADER)" if is_ch else f"Member (Follower)"
        
        # Recalculate uptime for detailed view
        uptime_h = float(d.get('UPTIME', 0)) / 3600.0 if d.get('UPTIME') else 0.0

        print(f"\n--- {name} (ID: {node_id}) ---")
        print(f"Role: {role} [{role_desc}]")
        print(f"MAC Address: {d.get('MAC')}")
        print(f"Scores: Stellar={d.get('STELLAR_SCORE')} | Composite={d.get('COMPOSITE_SCORE')}")
        
        # Parse Real/Dummy Flags
        sens_real = "REAL" if d.get('SENSORS_REAL') == '1' else "DUMMY"
        batt_real = "REAL" if d.get('BATTERY_REAL') == '1' else "DUMMY"
        
        print(f"Metrics: Battery={d.get('BATTERY')} | Trust={d.get('TRUST')} | LinkQ={d.get('LINK_QUALITY')} | Uptime={uptime_h:.2f}h")
        print(f"Status:  Sensors={sens_real} | Battery={batt_real}")
        
        neighbors = d.get('neighbors', [])
        if neighbors:
            print(f"Detected Members (In Cluster Radius): {len(neighbors)}")
            for n in neighbors:
                n_id = n.get('ID', '?')
                n_mac = n.get('MAC', '?')
                n_score = n.get('SCORE', '?')
                print(f"  - Node {n_id}: Score={n_score} | MAC={n_mac}")
        else:
            print("No members visible in cluster radius.")

def main():
    results = []
    
    # Run sequentially to avoid serial port conflicts/complexity
    for node in NODES:
        # Check if user wants to quit
        try:
            data = get_node_report(node['name'], node['port'])
            results.append({'name': node['name'], 'data': data})
        except KeyboardInterrupt:
            print("\nAborted by user.")
            sys.exit(0)
        
    print_summary(results)

if __name__ == "__main__":
    main()
