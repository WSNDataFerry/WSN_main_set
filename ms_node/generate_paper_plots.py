import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import re
import os
from datetime import datetime

# Configuration
DATA_DIR = "research_data"
OUTPUT_DIR = "test2_failure/plots" # Separate folder for Normal Operation
if not os.path.exists(OUTPUT_DIR):
    os.makedirs(OUTPUT_DIR)

# File Mappings (Experiment 2: Failure & Recovery)
FILES = {
    "Node1": "test2_failure/Node1_monitor.csv", 
    "Node2": "test2_failure/Node2_monitor.csv", 
    "Node3": "test2_failure/Node3_monitor.csv"
}

# Regex Patterns
Re_TRUST = re.compile(r"Trust Update Input: Rep=(\d+\.\d+), Current PDR=(\d+\.\d+), Current HSR=(\d+\.\d+)")
Re_NEIGHBOR_SCORE = re.compile(r"Discovered neighbor: node_id=(\d+), score=(\d+\.\d+)")
Re_ROLE = re.compile(r"STATUS: State=(\w+), Role=(\w+)")
Re_BATTERY = re.compile(r"PME batt=(\d+)%")
Re_STELLAR_WEIGHTS = re.compile(r"\[STELLAR\] Lyapunov weights: B=(\d+\.\d+) U=(\d+\.\d+) T=(\d+\.\d+) L=(\d+\.\d+), V=(\d+\.\d+)")

def parse_log_file(filepath, node_label):
    print(f"Parsing {filepath}...")
    data = []
    
    try:
        df = pd.read_csv(filepath)
        if 'Timestamp' not in df.columns: return pd.DataFrame()
        df['Timestamp'] = pd.to_datetime(df['Timestamp'])
        
        last_neighbor_id = None

        for index, row in df.iterrows():
            ts = row['Timestamp']
            line = str(row['Raw_Line']) if 'Raw_Line' in row and pd.notna(row['Raw_Line']) else ""
            
            # --- Neighbor Scores ---
            m_neigh = Re_NEIGHBOR_SCORE.search(line)
            if m_neigh:
                last_neighbor_id = m_neigh.group(1)
                data.append({'Timestamp': ts, 'Type': 'Neighbor_Score', 'Target_Node': last_neighbor_id, 'Value': float(m_neigh.group(2)), 'Source_Node': node_label})
            
            # --- Trust Metrics ---
            m_trust = Re_TRUST.search(line)
            if m_trust and last_neighbor_id:
                data.append({'Timestamp': ts, 'Type': 'Trust_PDR', 'Target_Node': last_neighbor_id, 'Value': float(m_trust.group(2)), 'Source_Node': node_label})
                
            # --- Role Updates ---
            m_role = Re_ROLE.search(line)
            if m_role:
                data.append({'Timestamp': ts, 'Type': 'Role', 'Value': m_role.group(2), 'Source_Node': node_label})

            # --- Battery Updates ---
            m_batt = Re_BATTERY.search(line)
            if m_batt:
                data.append({'Timestamp': ts, 'Type': 'Battery', 'Value': float(m_batt.group(1)), 'Source_Node': node_label})
                
        # --- CSV Fallbacks ---
        if 'Role' in df.columns:
            df_roles = df[df['Role'] != 'UNKNOWN']
            if not df_roles.empty:
                for _, r in df_roles.iterrows():
                     data.append({'Timestamp': r['Timestamp'], 'Type': 'Role', 'Value': r['Role'], 'Source_Node': node_label})

        if 'Battery' in df.columns:
            for _, r in df.iterrows():
                if pd.notna(r['Battery']):
                    data.append({'Timestamp': r['Timestamp'], 'Type': 'Battery', 'Value': float(r['Battery']), 'Source_Node': node_label})
                    
    except Exception as e:
        print(f"Error parsing {filepath}: {e}")
        
    return pd.DataFrame(data)

def plot_trust_dynamics():
    # Keep as sanity check but not main focus
    print("Generating Trust Plot...")
    plt.figure(figsize=(10, 6))
    colors = {'Node1': 'blue', 'Node2': 'green', 'Node3': 'purple'}
    
    for node_name, filepath in FILES.items():
        try:
            df = parse_log_file(filepath, node_name)
        except: continue
        if df.empty: continue
            
        df_trust = df[df['Type'] == 'Trust_PDR'].copy()
        if not df_trust.empty:
            df_trust['Value'] = pd.to_numeric(df_trust['Value'], errors='coerce')
            df_trust = df_trust.set_index('Timestamp').resample('10s').mean(numeric_only=True).reset_index()
            plt.plot(df_trust['Timestamp'], df_trust['Value'], label=f'{node_name} Reporting', color=colors.get(node_name, 'black'))

    plt.title('Trust Stability During CH Failure')
    plt.ylabel('PDR')
    plt.legend()
    plt.grid(True)
    plt.gca().xaxis.set_major_formatter(mdates.DateFormatter('%H:%M'))
    plt.savefig(f"{OUTPUT_DIR}/Figure1_Trust_Stability.png")

def plot_cluster_stability():
    print("Generating Cluster Stability (Recovery) Plot...")
    
    # We want to see ALL nodes' roles to verify the handover
    fig, ax = plt.subplots(figsize=(12, 6))
    
    role_map = {'NODE': 1, 'MEMBER': 1, 'CH': 2, 'CL_HEAD': 2, 'GATEWAY': 3, 'DISCOVER': 0, 'SYNC_TIME': 0}
    offsets = {'Node1': 0.1, 'Node2': 0.0, 'Node3': -0.1} # Offset to avoid overlap
    colors = {'Node1': 'blue', 'Node2': 'green', 'Node3': 'purple'}
    
    for node_name, filepath in FILES.items():
        df = parse_log_file(filepath, node_name)
        if df.empty: continue
        
        df_role = df[df['Type'] == 'Role'].copy()
        if df_role.empty: continue
        
        df_role['RoleNum'] = df_role['Value'].map(role_map).fillna(0)
        # Add slight offset for visibility
        df_role['RolePlot'] = df_role['RoleNum'] + offsets[node_name]
        
        ax.step(df_role['Timestamp'], df_role['RolePlot'], where='post', label=node_name, color=colors[node_name], linewidth=2)
        
    ax.set_yticks([0, 1, 2, 3], ['Init/Discover', 'Member', 'Cluster Head', 'Gateway'])
    ax.set_title('Cluster Role Transitions: Failure & Recovery Event')
    ax.set_xlabel('Time')
    ax.set_ylabel('Role State')
    ax.legend()
    ax.grid(True, axis='y')
    ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M'))
    
    plt.tight_layout()
    plt.savefig(f"{OUTPUT_DIR}/Figure2_Failure_Recovery_Timeline.png")
    print("Saved Figure2_Failure_Recovery_Timeline.png")

def plot_load_balancing():
    # Standard plot
    print("Generating Load Balancing Plot...")
    dfs = []
    for node_name, filepath in FILES.items():
        dfs.append(parse_log_file(filepath, node_name))
    
    df = pd.concat(dfs)
    if df.empty: return

    df_batt = df[df['Type'] == 'Battery'].copy()
    if not df_batt.empty:
         df_batt['Value'] = pd.to_numeric(df_batt['Value'], errors='coerce')
    
    plt.figure(figsize=(10, 6))
    for node in ['Node1', 'Node2', 'Node3']:
        subset = df_batt[df_batt['Source_Node'] == node]
        if not subset.empty:
            plt.plot(subset['Timestamp'], subset['Value'], label=f'{node} Battery')
            
    plt.title('Battery Profile During Failure')
    plt.legend()
    plt.grid(True)
    plt.gca().xaxis.set_major_formatter(mdates.DateFormatter('%H:%M'))
    plt.savefig(f"{OUTPUT_DIR}/Figure3_Load_Balancing.png")

def plot_algorithm_convergence():
    print("Generating Convergence Plot (Focus on Spike)...")
    
    fig, axes = plt.subplots(2, 3, figsize=(18, 10), sharex=True)
    nodes = ["Node1", "Node2", "Node3"]
    
    for i, node_name in enumerate(nodes):
        filepath = FILES[node_name]
        try:
            df = pd.read_csv(filepath)
            if 'Timestamp' not in df.columns: continue
            df['Timestamp'] = pd.to_datetime(df['Timestamp'])
            
            data_weights = []
            for _, row in df.iterrows():
                line = str(row.get('Raw_Line', ''))
                m = Re_STELLAR_WEIGHTS.search(line)
                if m:
                    data_weights.append({
                        'Timestamp': row['Timestamp'],
                        'Battery': float(m.group(1)),
                        'Uptime': float(m.group(2)),
                        'Trust': float(m.group(3)),
                        'LinkQ': float(m.group(4)),
                        'Lyapunov_V': float(m.group(5))
                    })
            
            if not data_weights: continue
            df_w = pd.DataFrame(data_weights)
            
            # Top Row: Weights
            ax_w = axes[0, i]
            ax_w.plot(df_w['Timestamp'], df_w['Battery'], label='Battery')
            ax_w.plot(df_w['Timestamp'], df_w['Uptime'], label='Uptime')
            ax_w.plot(df_w['Timestamp'], df_w['Trust'], label='Trust')
            ax_w.plot(df_w['Timestamp'], df_w['LinkQ'], ':', label='LinkQ')
            ax_w.set_title(f'{node_name}: Adaptive Weights')
            ax_w.grid(True)
            if i == 0: ax_w.set_ylabel('Weight')
            if i == 2: ax_w.legend(loc='upper right')
            
            # Bottom Row: Lyapunov
            ax_l = axes[1, i]
            ax_l.plot(df_w['Timestamp'], df_w['Lyapunov_V'], color='red', label='V(t)')
            ax_l.set_title(f'{node_name}: Lyapunov Stability (Spike Check)')
            ax_l.grid(True)
            ax_l.axhline(y=0.001, color='gray', linestyle='--')
            if i == 0: ax_l.set_ylabel('V(t)')
            ax_l.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M'))
            ax_l.tick_params(axis='x', rotation=45)
            
        except Exception as e:
            print(f"Error {node_name}: {e}")

    plt.tight_layout()
    plt.savefig(f"{OUTPUT_DIR}/Figure4_Stability_Recovery.png")
    print("Saved Figure4_Stability_Recovery.png")

def main():
    plot_trust_dynamics()
    plot_cluster_stability()
    plot_load_balancing()
    plot_algorithm_convergence()
    print("All Failure/Recovery plots generated in " + OUTPUT_DIR)

if __name__ == "__main__":
    main()
