import pandas as pd
import json
import glob
import os
import re

LOG_DIR = "research_data"
OUTPUT_FILE = "experiment_1_results.txt"

def parse_json_from_line(line):
    # JSON is inside the raw line, typically starting with { and ending with }
    # It might be surrounded by log text.
    try:
        match = re.search(r'(\{.*\})', line)
        if match:
            return json.loads(match.group(1))
    except:
        pass
    return None

def analyze_file(filepath):
    print(f"Analyzing {filepath}...")
    try:
        df = pd.read_csv(filepath)
    except Exception as e:
        print(f"Error reading {filepath}: {e}")
        return None

    results = {}
    node_id = os.path.basename(filepath).split('_')[0]
    
    # 1. CH Stability Analysis
    if 'CH_ID' in df.columns:
        ch_changes = df['CH_ID'].ne(df['CH_ID'].shift()).sum() - 1 # -1 for initial
        unique_chs = df['CH_ID'].unique()
        # Filter 0 (no CH)
        valid_chs = [ch for ch in unique_chs if ch != 0]
        results['ch_changes'] = int(ch_changes) if ch_changes > 0 else 0
        results['unique_chs'] = [int(ch) for ch in valid_chs]
        
    # 2. PDR Analysis (Parsing JSON from Raw_Line)
    # We look for lines containing "RX Sensor Data" OR clean JSON lines
    # The script logs "RX Sensor Data..." logs from ESP log (which contains the JSON in logic but maybe not in text?)
    # Wait, esp_now_manager.c logs: "RX Sensor Data... Seq=%lu..." 
    # AND it writes JSON to storage: storage_manager_write(log_line) which MIGHT be printed if we dump storage?
    # NO, strictly looking at the "RX Sensor Data" log line for PDR from *other* nodes.
    
    # Let's verify what `collect_research_data.py` captured. 
    # It captured ALL serial output.
    # The log format in `esp_now_manager.c` is:
    # "RX Sensor Data from node_%lu: Seq=%lu, MAC=..."
    
    received_packets = []
    
    for _, row in df.iterrows():
        line = str(row.get('Raw_Line', ''))
        # Updated Regex to match the C code format
        # "RX Sensor Data from node_%lu: Seq=%lu, MAC=..."
        match = re.search(r"RX Sensor Data from node_(\d+): Seq=(\d+)", line)
        if match:
            src_node = int(match.group(1))
            seq_num = int(match.group(2))
            received_packets.append({'src': src_node, 'seq': seq_num})
            
    # Calculate PDR per source node
    pdr_stats = {}
    if received_packets:
        packet_df = pd.DataFrame(received_packets)
        for src in packet_df['src'].unique():
            src_pkts = packet_df[packet_df['src'] == src]
            # unexpected huge PDR > 100% was due to counting duplicates. Use unique seq.
            unique_seqs = src_pkts['seq'].unique()
            count = len(unique_seqs)
            min_seq = src_pkts['seq'].min()
            max_seq = src_pkts['seq'].max()
            expected = max_seq - min_seq + 1
            if expected > 0:
                pdr = (count / expected) * 100.0
            else:
                pdr = 0.0
            pdr_stats[int(src)] = {
                'count': int(count),
                'min_seq': int(min_seq),
                'max_seq': int(max_seq),
                'pdr_pct': round(pdr, 2)
            }
            
    results['pdr_stats'] = pdr_stats

    # 3. CH Recovery Time (Specific to Exp 2)
    # Looking for transition from CH!=0 -> CH=0 -> CH!=0 OR CH=A -> CH=0 -> CH=B
    if 'CH_ID' in df.columns:
        df['Timestamp'] = pd.to_datetime(df['Timestamp'])
        ch_changes = df[df['CH_ID'].ne(df['CH_ID'].shift())]
        
        recovery_events = []
        # Iterate through changes to find breakdown events
        # A breakdown is when CH goes to 0 (or disappears) then comes back
        # OR when CH changes ID directly
        
        # Simplified logic: Time between "Last seen old CH" and "First seen new CH"
        # Ideally we look for State transitions in Member nodes:
        # Member(CH=A) -> ... -> Member(CH=B)
        
        if len(unique_chs) > 1:
            # We have a CH change
            # Get timestamps of changes
            results['recovery_time_ms'] = "N/A (Complex)" 
            # For exact calculation we need more logic, but let's log the timestamps of change
            change_log = []
            for idx, row in ch_changes.iloc[1:].iterrows(): # Skip first row (initial)
                change_log.append({
                    'time': row['Timestamp'].strftime('%H:%M:%S.%f'),
                    'new_ch': int(row['CH_ID']),
                    'old_ch': int(df.loc[idx-1]['CH_ID']) if idx>0 else 0
                })
            results['ch_change_log'] = change_log

    return results

def main():
    # Only analyze the LATEST run (most recent files)
    all_files = glob.glob(os.path.join(LOG_DIR, "*.csv"))
    if not all_files:
        print("No CSV files found.")
        return
        
    # Group by timestamp in filename to find latest batch
    # Filename format: NodeX_YYYYMMDD_HHMMSS.csv
    # Extract timestamp part
    timestamps = set()
    for f in all_files:
        match = re.search(r'_(\d{8}_\d{6})\.csv', f)
        if match:
            timestamps.add(match.group(1))
            
    if not timestamps:
        print("Could not parse filenames.")
        return
        
    latest_ts = sorted(list(timestamps))[-1]
    print(f"Analyzing latest run: {latest_ts}")
    
    files = [f for f in all_files if latest_ts in f]

    all_results = {}
    for f in files:
        res = analyze_file(f)
        if res:
            all_results[os.path.basename(f)] = res

    # Generate Report
    report = []
    report.append("========================================")
    report.append(f"   ANALYSIS RESULTS (Run: {latest_ts})")
    report.append("========================================")
    
    for filename, res in all_results.items():
        node = filename.split('_')[0]
        report.append(f"\n[ {node} ]")
        report.append(f"  CH Changes Detected: {res.get('ch_changes', 'N/A')}")
        report.append(f"  Valid CHs Seen: {res.get('unique_chs', [])}")
        
        if 'ch_change_log' in res and res['ch_change_log']:
             report.append("  CH Change Events:")
             for event in res['ch_change_log']:
                 report.append(f"    - At {event['time']}: CH {event['old_ch']} -> {event['new_ch']}")

        pdr_stats = res.get('pdr_stats', {})
        if pdr_stats:
            report.append("  Packet Delivery Ratio (Data Received by this Node):")
            for src, stats in pdr_stats.items():
                report.append(f"    -> From Node {src}: {stats['pdr_pct']}% ({stats['count']} packets, Seq {stats['min_seq']}-{stats['max_seq']})")
        else:
            report.append("  No Sensor Data Packets Received (Expected if not CH)")

    report_str = "\n".join(report)
    print(report_str)
    
    # Save to specific report file for this run
    outfile = f"report_{latest_ts}.txt"
    with open(outfile, "w") as f:
        f.write(report_str)
    print(f"\nReport saved to {outfile}")

if __name__ == "__main__":
    main()
