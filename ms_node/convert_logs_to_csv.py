
import csv
import re
import os
from datetime import datetime

LOG_DIR = "test2_failure"
OUTPUT_DIR = "test2_failure"

NODES = ["Node1", "Node2", "Node3"]

def convert_log_to_csv(node_name):
    log_file = os.path.join(LOG_DIR, f"{node_name}_monitor.log")
    csv_file = os.path.join(OUTPUT_DIR, f"{node_name}_monitor.csv")
    
    if not os.path.exists(log_file):
        print(f"Skipping {node_name}: Log file not found.")
        return

    print(f"Converting {log_file} -> {csv_file}")
    
    with open(log_file, "r") as f_in, open(csv_file, "w", newline='') as f_out:
        writer = csv.writer(f_out)
        # Write header expected by generate_paper_plots.py
        writer.writerow(["Timestamp", "Raw_Line"])
        
        # Get today's date for full timestamp
        today_str = datetime.now().strftime("%Y-%m-%d")
        
        for line in f_in:
            line = line.strip()
            # Match [HH:MM:SS]
            m = re.match(r"^\[(\d{2}:\d{2}:\d{2})\] (.*)", line)
            if m:
                time_str = m.group(1)
                content = m.group(2)
                # Combine date and time
                full_ts = f"{today_str} {time_str}"
                writer.writerow([full_ts, content])
            else:
                # Handle lines without timestamp (rare) or append to previous?
                pass

if __name__ == "__main__":
    for node in NODES:
        convert_log_to_csv(node)
