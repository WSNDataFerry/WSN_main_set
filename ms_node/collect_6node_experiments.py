#!/usr/bin/env python3
"""
Comprehensive 6-Node Experimental Data Collection
Collects all metrics needed for publication from 6 ESP32-S3 nodes
"""
import serial
import time
import csv
import threading
import re
import json
import os
import argparse
from datetime import datetime
from collections import defaultdict
import numpy as np

# ============================================
# CONFIGURATION
# ============================================

# Update these with your actual 6 node ports
NODES = [
    {"id": "Node1", "port": "/dev/cu.usbmodem5ABA0603751"},
    {"id": "Node2", "port": "/dev/cu.usbmodem5ABA0605221"},
    {"id": "Node3", "port": "/dev/cu.usbmodem5ABA0607321"},
    {"id": "Node4", "port": "/dev/cu.usbmodemXXXX"},  # TODO: Update
    {"id": "Node5", "port": "/dev/cu.usbmodemXXXX"},  # TODO: Update
    {"id": "Node6", "port": "/dev/cu.usbmodemXXXX"},  # TODO: Update
]

BAUD = 115200
OUTPUT_BASE_DIR = "experimental_data"

# Regex Patterns for Metric Extraction
PATTERNS = {
    "state_transition": re.compile(r"State transition: (\w+) -> (\w+)"),
    "stellar_weights": re.compile(r"\[STELLAR\] Lyapunov weights: B=([\d.]+) U=([\d.]+) T=([\d.]+) L=([\d.]+)"),
    "stellar_score": re.compile(r"\[STELLAR\] Score components.*Ψ=([\d.]+)"),
    "pareto_rank": re.compile(r"\[STELLAR\] Pareto rank=(\d+)/(\d+)"),
    "centrality": re.compile(r"Centrality=([\d.]+)"),
    "lyapunov_v": re.compile(r"V=([\d.]+)"),
    "lyapunov_conv": re.compile(r"Conv=(\d)"),
    "battery": re.compile(r"batt=(\d+)%"),
    "trust": re.compile(r"Trust=([\d.]+)"),
    "link_quality": re.compile(r"LinkQ=([\d.]+)"),
    "election_start": re.compile(r"Starting.*election|Running.*election"),
    "election_winner": re.compile(r"Selected CH: node_(\d+)|Winner: node_(\d+)"),
    "ch_beacon": re.compile(r"CH beacon|I am CH"),
    "packet_sent": re.compile(r"Sent sensor data|ESP_NOW.*TX|TX.*sensor"),
    "packet_received": re.compile(r"RX Sensor Data|ESP_NOW.*RX|RX.*sensor"),
    "hmac_success": re.compile(r"HMAC.*success|HMAC.*valid"),
    "hmac_fail": re.compile(r"HMAC.*fail|HMAC.*invalid"),
    "rssi": re.compile(r"RSSI[:\s]+(-?\d+)"),
    "pdr": re.compile(r"PDR[:\s]+([\d.]+)"),
    "hsr": re.compile(r"HSR[:\s]+([\d.]+)"),
    "status": re.compile(r"STATUS: State=(\w+), Role=(\w+), CH=(\d+), Size=(\d+)"),
    "neighbor_discovered": re.compile(r"Neighbor.*discovered|New neighbor"),
    "neighbor_lost": re.compile(r"Neighbor.*lost|Neighbor.*timeout"),
}

# ============================================
# DATA STRUCTURES
# ============================================

class NodeMetrics:
    def __init__(self, node_id):
        self.node_id = node_id
        self.start_time = None
        self.convergence_time = None
        self.elections = []
        self.ch_selections = []
        self.stellar_scores = []
        self.weights = {"B": [], "U": [], "T": [], "L": []}
        self.lyapunov_v = []
        self.lyapunov_conv = []
        self.pareto_ranks = []
        self.centrality = []
        self.battery = []
        self.trust = []
        self.link_quality = []
        self.state_changes = []
        self.packets_sent = 0
        self.packets_received = 0
        self.hmac_success = 0
        self.hmac_failures = 0
        self.rssi_values = []
        self.pdr_values = []
        self.hsr_values = []
        self.current_state = "UNKNOWN"
        self.current_role = "UNKNOWN"
        self.current_ch = "0"
        self.ch_count = 0
        self.member_count = 0

# Global metrics storage
metrics = {node["id"]: NodeMetrics(node["id"]) for node in NODES}
lock = threading.Lock()

# ============================================
# MONITORING FUNCTIONS
# ============================================

def monitor_node(node_config, start_time, duration, output_dir):
    """Monitor a single node and extract all metrics"""
    node_id = node_config["id"]
    port = node_config["port"]
    
    log_file = f"{output_dir}/{node_id}_raw.log"
    csv_file = f"{output_dir}/{node_id}_metrics.csv"
    
    node_metrics = metrics[node_id]
    node_metrics.start_time = start_time
    
    try:
        ser = serial.Serial(port, BAUD, timeout=1)
        print(f"[{node_id}] Connected to {port}")
        
        with open(log_file, 'w') as log, open(csv_file, 'w', newline='') as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow([
                "timestamp_s", "state", "role", "ch_id", "cluster_size",
                "stellar_score", "weight_B", "weight_U", "weight_T", "weight_L",
                "lyapunov_v", "lyapunov_conv", "pareto_rank", "centrality",
                "battery", "trust", "link_quality", "rssi", "pdr", "hsr",
                "event_type", "raw_line"
            ])
            
            while time.time() - start_time < duration:
                if ser.in_waiting:
                    try:
                        line = ser.readline().decode('utf-8', errors='ignore').strip()
                        if not line:
                            continue
                        
                        timestamp = time.time() - start_time
                        log.write(f"{timestamp:.3f}: {line}\n")
                        log.flush()
                        
                        # Initialize CSV row
                        row = [timestamp] + [""]*20 + [line]
                        
                        # Extract all metrics
                        extract_metrics(node_id, line, timestamp, row, writer)
                        
                    except Exception as e:
                        print(f"[{node_id}] Error: {e}")
                else:
                    time.sleep(0.01)
        
        ser.close()
        print(f"[{node_id}] Monitoring complete")
        
    except serial.SerialException as e:
        print(f"[{node_id}] Connection failed: {e}")
    except Exception as e:
        print(f"[{node_id}] Error: {e}")

def extract_metrics(node_id, line, timestamp, row, writer):
    """Extract all metrics from a log line"""
    node_metrics = metrics[node_id]
    
    # State transitions
    m = PATTERNS["state_transition"].search(line)
    if m:
        old_state, new_state = m.groups()
        node_metrics.state_changes.append({
            "time": timestamp,
            "from": old_state,
            "to": new_state
        })
        node_metrics.current_state = new_state
        row[1] = new_state
        
        # Track convergence time (first CH or MEMBER state)
        if node_metrics.convergence_time is None:
            if new_state in ["CH", "MEMBER"]:
                node_metrics.convergence_time = timestamp
        
        # Update role
        if new_state == "CH":
            node_metrics.current_role = "CH"
            node_metrics.ch_count += 1
            row[2] = "CH"
        elif new_state == "MEMBER":
            node_metrics.current_role = "MEMBER"
            node_metrics.member_count += 1
            row[2] = "MEMBER"
    
    # STATUS line (comprehensive state info)
    m = PATTERNS["status"].search(line)
    if m:
        state, role, ch_id, size = m.groups()
        node_metrics.current_state = state
        node_metrics.current_role = role
        node_metrics.current_ch = ch_id
        row[1] = state
        row[2] = role
        row[3] = ch_id
        row[4] = size
    
    # STELLAR weights
    m = PATTERNS["stellar_weights"].search(line)
    if m:
        b, u, t, l = map(float, m.groups())
        node_metrics.weights["B"].append((timestamp, b))
        node_metrics.weights["U"].append((timestamp, u))
        node_metrics.weights["T"].append((timestamp, t))
        node_metrics.weights["L"].append((timestamp, l))
        row[6:10] = [b, u, t, l]
    
    # STELLAR score
    m = PATTERNS["stellar_score"].search(line)
    if m:
        score = float(m.group(1))
        node_metrics.stellar_scores.append((timestamp, score))
        row[5] = score
    
    # Lyapunov value
    m = PATTERNS["lyapunov_v"].search(line)
    if m:
        v = float(m.group(1))
        node_metrics.lyapunov_v.append((timestamp, v))
        row[11] = v
    
    # Lyapunov convergence
    m = PATTERNS["lyapunov_conv"].search(line)
    if m:
        conv = int(m.group(1))
        node_metrics.lyapunov_conv.append((timestamp, conv))
        row[12] = conv
    
    # Pareto rank
    m = PATTERNS["pareto_rank"].search(line)
    if m:
        rank = int(m.group(1))
        node_metrics.pareto_ranks.append((timestamp, rank))
        row[13] = rank
    
    # Centrality
    m = PATTERNS["centrality"].search(line)
    if m:
        cent = float(m.group(1))
        node_metrics.centrality.append((timestamp, cent))
        row[14] = cent
    
    # Battery
    m = PATTERNS["battery"].search(line)
    if m:
        batt = int(m.group(1))
        node_metrics.battery.append((timestamp, batt))
        row[15] = batt
    
    # Trust
    m = PATTERNS["trust"].search(line)
    if m:
        trust = float(m.group(1))
        node_metrics.trust.append((timestamp, trust))
        row[16] = trust
    
    # Link Quality
    m = PATTERNS["link_quality"].search(line)
    if m:
        lq = float(m.group(1))
        node_metrics.link_quality.append((timestamp, lq))
        row[17] = lq
    
    # RSSI
    m = PATTERNS["rssi"].search(line)
    if m:
        rssi = int(m.group(1))
        node_metrics.rssi_values.append((timestamp, rssi))
        row[18] = rssi
    
    # PDR
    m = PATTERNS["pdr"].search(line)
    if m:
        pdr = float(m.group(1))
        node_metrics.pdr_values.append((timestamp, pdr))
        row[19] = pdr
    
    # HSR
    m = PATTERNS["hsr"].search(line)
    if m:
        hsr = float(m.group(1))
        node_metrics.hsr_values.append((timestamp, hsr))
        row[20] = hsr
    
    # Elections
    if PATTERNS["election_start"].search(line):
        node_metrics.elections.append(timestamp)
        row[21] = "ELECTION_START"
    
    m = PATTERNS["election_winner"].search(line)
    if m:
        winner = m.group(1) or m.group(2)
        node_metrics.ch_selections.append({
            "time": timestamp,
            "winner": int(winner)
        })
        row[21] = "CH_SELECTED"
    
    # Communication events
    if PATTERNS["packet_sent"].search(line):
        node_metrics.packets_sent += 1
        row[21] = "PACKET_SENT"
    
    if PATTERNS["packet_received"].search(line):
        node_metrics.packets_received += 1
        row[21] = "PACKET_RECEIVED"
    
    if PATTERNS["hmac_success"].search(line):
        node_metrics.hmac_success += 1
        row[21] = "HMAC_SUCCESS"
    
    if PATTERNS["hmac_fail"].search(line):
        node_metrics.hmac_failures += 1
        row[21] = "HMAC_FAIL"
    
    # Write row if any metric was extracted
    if any(row[1:22]):  # Check if any metric field is filled
        writer.writerow(row)
        csvfile.flush()

# ============================================
# ANALYSIS FUNCTIONS
# ============================================

def calculate_convergence_statistics():
    """Calculate convergence time statistics"""
    convergence_times = []
    for node_id, m in metrics.items():
        if m.convergence_time is not None:
            convergence_times.append(m.convergence_time)
    
    if not convergence_times:
        return None
    
    return {
        "mean": np.mean(convergence_times),
        "std": np.std(convergence_times),
        "min": np.min(convergence_times),
        "max": np.max(convergence_times),
        "median": np.median(convergence_times),
        "n": len(convergence_times),
        "ci_95": (
            np.mean(convergence_times) - 1.96 * np.std(convergence_times) / np.sqrt(len(convergence_times)),
            np.mean(convergence_times) + 1.96 * np.std(convergence_times) / np.sqrt(len(convergence_times))
        )
    }

def calculate_fairness():
    """Calculate Gini coefficient from CH selections"""
    ch_counts = {}
    total_ch_selections = 0
    
    for node_id, m in metrics.items():
        ch_count = m.ch_count
        ch_counts[node_id] = ch_count
        total_ch_selections += ch_count
    
    if total_ch_selections == 0:
        return None
    
    # Calculate Gini coefficient
    sorted_counts = sorted(ch_counts.values())
    n = len(sorted_counts)
    cumsum = np.cumsum(sorted_counts)
    
    gini = (2 * np.sum((np.arange(1, n+1)) * sorted_counts)) / (n * total_ch_selections) - (n + 1) / n
    
    return {
        "gini": gini,
        "ch_distribution": ch_counts,
        "total_selections": total_ch_selections,
        "n": n
    }

def calculate_stellar_statistics():
    """Calculate STELLAR algorithm statistics"""
    all_scores = []
    all_weights = {"B": [], "U": [], "T": [], "L": []}
    all_lyapunov_v = []
    
    for node_id, m in metrics.items():
        all_scores.extend([s[1] for s in m.stellar_scores])
        all_weights["B"].extend([w[1] for w in m.weights["B"]])
        all_weights["U"].extend([w[1] for w in m.weights["U"]])
        all_weights["T"].extend([w[1] for w in m.weights["T"]])
        all_weights["L"].extend([w[1] for w in m.weights["L"]])
        all_lyapunov_v.extend([v[1] for v in m.lyapunov_v])
    
    stats = {}
    if all_scores:
        stats["stellar_score"] = {
            "mean": np.mean(all_scores),
            "std": np.std(all_scores),
            "min": np.min(all_scores),
            "max": np.max(all_scores)
        }
    
    for metric in ["B", "U", "T", "L"]:
        if all_weights[metric]:
            stats[f"weight_{metric}"] = {
                "mean": np.mean(all_weights[metric]),
                "std": np.std(all_weights[metric]),
                "final": all_weights[metric][-1] if all_weights[metric] else None
            }
    
    if all_lyapunov_v:
        stats["lyapunov_v"] = {
            "mean": np.mean(all_lyapunov_v),
            "std": np.std(all_lyapunov_v),
            "final": all_lyapunov_v[-1] if all_lyapunov_v else None
        }
    
    return stats

def generate_summary_report(output_dir, scenario_name, duration):
    """Generate comprehensive summary report"""
    report = {
        "scenario": scenario_name,
        "duration_seconds": duration,
        "nodes": len(NODES),
        "timestamp": datetime.now().isoformat(),
        "convergence": calculate_convergence_statistics(),
        "fairness": calculate_fairness(),
        "stellar": calculate_stellar_statistics(),
        "communication": {},
        "per_node": {}
    }
    
    # Communication statistics
    total_sent = sum(m.packets_sent for m in metrics.values())
    total_received = sum(m.packets_received for m in metrics.values())
    total_hmac_success = sum(m.hmac_success for m in metrics.values())
    total_hmac_fail = sum(m.hmac_failures for m in metrics.values())
    
    report["communication"] = {
        "packets_sent": total_sent,
        "packets_received": total_received,
        "hmac_success": total_hmac_success,
        "hmac_failures": total_hmac_fail,
        "hmac_success_rate": total_hmac_success / (total_hmac_success + total_hmac_fail) if (total_hmac_success + total_hmac_fail) > 0 else 0,
        "pdr": total_received / total_sent if total_sent > 0 else 0
    }
    
    # Per-node statistics
    for node_id, m in metrics.items():
        report["per_node"][node_id] = {
            "convergence_time": m.convergence_time,
            "ch_count": m.ch_count,
            "member_count": m.member_count,
            "elections": len(m.elections),
            "state_changes": len(m.state_changes),
            "packets_sent": m.packets_sent,
            "packets_received": m.packets_received,
            "final_battery": m.battery[-1][1] if m.battery else None,
            "final_stellar_score": m.stellar_scores[-1][1] if m.stellar_scores else None
        }
    
    # Save report
    report_file = f"{output_dir}/summary.json"
    with open(report_file, 'w') as f:
        json.dump(report, f, indent=2)
    
    print(f"\n{'='*60}")
    print("SUMMARY REPORT")
    print(f"{'='*60}")
    print(f"Scenario: {scenario_name}")
    print(f"Duration: {duration} seconds")
    print(f"\nConvergence:")
    if report["convergence"]:
        c = report["convergence"]
        print(f"  Mean: {c['mean']:.3f} ± {c['std']:.3f} seconds")
        print(f"  95% CI: [{c['ci_95'][0]:.3f}, {c['ci_95'][1]:.3f}]")
        print(f"  Range: [{c['min']:.3f}, {c['max']:.3f}]")
        print(f"  n = {c['n']}")
    
    print(f"\nFairness:")
    if report["fairness"]:
        f = report["fairness"]
        print(f"  Gini Coefficient: {f['gini']:.3f}")
        print(f"  CH Distribution: {f['ch_distribution']}")
    
    print(f"\nCommunication:")
    comm = report["communication"]
    print(f"  Packets Sent: {comm['packets_sent']}")
    print(f"  Packets Received: {comm['packets_received']}")
    print(f"  HMAC Success Rate: {comm['hmac_success_rate']:.3f}")
    print(f"  PDR: {comm['pdr']:.3f}")
    
    print(f"\nReport saved to: {report_file}")
    print(f"{'='*60}\n")
    
    return report

# ============================================
# MAIN FUNCTION
# ============================================

def main():
    parser = argparse.ArgumentParser(description='6-Node WSN Experimental Data Collection')
    parser.add_argument('--scenario', type=str, required=True,
                       help='Scenario name (e.g., baseline_normal, sparse, battery_depletion)')
    parser.add_argument('--duration', type=int, default=1800,
                       help='Duration in seconds (default: 1800 = 30 minutes)')
    parser.add_argument('--run', type=int, default=1,
                       help='Run number (for multiple runs of same scenario)')
    
    args = parser.parse_args()
    
    scenario_name = args.scenario
    duration = args.duration
    run_num = args.run
    
    # Create output directory
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    output_dir = f"{OUTPUT_BASE_DIR}/{scenario_name}_run{run_num:02d}_{timestamp}"
    os.makedirs(output_dir, exist_ok=True)
    
    print(f"{'='*60}")
    print(f"6-NODE EXPERIMENTAL DATA COLLECTION")
    print(f"{'='*60}")
    print(f"Scenario: {scenario_name}")
    print(f"Run: {run_num}")
    print(f"Duration: {duration} seconds ({duration/60:.1f} minutes)")
    print(f"Nodes: {len(NODES)}")
    print(f"Output: {output_dir}")
    print(f"{'='*60}\n")
    
    # Verify ports
    print("Verifying node connections...")
    for node in NODES:
        if "XXXX" in node["port"]:
            print(f"⚠️  WARNING: {node['id']} port not configured: {node['port']}")
    
    input("Press ENTER to start data collection (ensure all nodes are powered on)...")
    
    start_time = time.time()
    threads = []
    
    # Start monitoring all nodes
    for node in NODES:
        t = threading.Thread(
            target=monitor_node,
            args=(node, start_time, duration, output_dir),
            daemon=True
        )
        t.start()
        threads.append(t)
        time.sleep(0.5)  # Stagger connections
    
    print(f"\nMonitoring started at {datetime.now().strftime('%H:%M:%S')}")
    print(f"Collection will run for {duration} seconds...")
    print("Press Ctrl+C to stop early\n")
    
    try:
        # Wait for completion
        elapsed = 0
        while elapsed < duration:
            time.sleep(10)
            elapsed = time.time() - start_time
            remaining = duration - elapsed
            if int(elapsed) % 60 == 0:  # Print every minute
                print(f"[{int(elapsed/60)}m] Elapsed: {int(elapsed)}s, Remaining: {int(remaining)}s")
    
    except KeyboardInterrupt:
        print("\n\nStopping collection early...")
    
    finally:
        # Wait for threads to finish
        print("Waiting for threads to finish...")
        for t in threads:
            t.join(timeout=5)
        
        # Generate summary report
        print("\nGenerating summary report...")
        generate_summary_report(output_dir, scenario_name, duration)
        
        print("Data collection complete!")
        print(f"Data saved to: {output_dir}")

if __name__ == "__main__":
    main()
