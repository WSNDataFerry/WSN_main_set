import pandas as pd
import re
import os

# Configuration
FILES = {
    "Node1": "research_data/Node1_20260218_StressTest.csv",
    "Node2": "research_data/Node2_20260218_StressTest.csv",
    "Node3": "research_data/Node3_20260218_StressTest.csv"
}

# Regex for C-code logs (contained in Raw_Line)
# Example: "[STELLAR] Final Score: 0.87 (MyID: 3125566642)"
RE_SCORE = re.compile(r"\[STELLAR\] Final Score: (\d+\.\d+)")
# Example: "[STELLAR] Lyapunov weights: B=0.30 U=0.20 T=0.30 L=0.20, V=0.00"
RE_WEIGHTS = re.compile(r"\[STELLAR\] Lyapunov weights: B=(\d+\.\d+) U=(\d+\.\d+) T=(\d+\.\d+) L=(\d+\.\d+)")
# Example: "STATUS: State=CH, Role=CH, CH=0, Size=2"
RE_STATUS = re.compile(r"STATUS: State=(\w+)")
# Example: "STATE: CH lost (current_ch=0)"
RE_CH_LOST = re.compile(r"STATE: CH lost")

def analyze_node(node_name, filepath):
    print(f"\n--- Analyzing {node_name} ---")
    if not os.path.exists(filepath):
        print(f"File not found: {filepath}")
        return

    try:
        df = pd.read_csv(filepath)
    except Exception as e:
        print(f"Error reading CSV: {e}")
        return

    events = []
    last_score = None
    last_weights = "N/A"
    current_state = "UNKNOWN"

    for index, row in df.iterrows():
        line = str(row.get('Raw_Line', ''))
        ts = row.get('Timestamp', 'Unknown')

        # 1. Capture Scores
        m_score = RE_SCORE.search(line)
        if m_score:
            score = float(m_score.group(1))
            last_score = score

        # 2. Capture Weights
        m_weights = RE_WEIGHTS.search(line)
        if m_weights:
            w_str = f"B={m_weights.group(1)} U={m_weights.group(2)} T={m_weights.group(3)} L={m_weights.group(4)}"
            if w_str != last_weights:
                 events.append(f"[{ts}] Alg Adaptation: Weights {w_str}")
                 last_weights = w_str

        # 3. Capture State Transitions (The real truth)
        m_status = RE_STATUS.search(line)
        if m_status:
            new_state = m_status.group(1)
            if new_state != current_state:
                events.append(f"[{ts}] EVENT: STATE CHANGE {current_state} -> {new_state}")
                if new_state == "CH":
                     events.append(f"       -> BECAME CLUSTER HEAD (Score: {last_score})")
                current_state = new_state

        # 4. Capture CH Lost (Explicit warning)
        if RE_CH_LOST.search(line):
             # Only log if we haven't just switched state to avoid duplicates
             events.append(f"[{ts}] EVENT: Detected CH Failure (CH Lost)")

    if not events:
        print("No algorithm events found.")
    else:
        for e in events:
            print(e)

def main():
    print("Starting Algorithm Audit...")
    for node, path in FILES.items():
        analyze_node(node, path)

if __name__ == "__main__":
    main()
