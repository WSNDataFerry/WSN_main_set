"""
Enhanced Research Plots for STELLAR WSN System (Post Engineering Review)
Captures: Lyapunov convergence, Pareto ranking, Centrality, Score components,
          Role transitions, Battery profile, Trust dynamics.
"""
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import matplotlib.gridspec as gridspec
import re
import os
from datetime import datetime

# ── Configuration ──────────────────────────────────────────────────────────────
DATA_DIR   = "test3_improved"
OUTPUT_DIR = "test3_improved/plots"
NODES      = ["Node1", "Node2", "Node3"]
COLORS     = {"Node1": "#2196F3", "Node2": "#4CAF50", "Node3": "#9C27B0"}
os.makedirs(OUTPUT_DIR, exist_ok=True)

# ── Regex Patterns ─────────────────────────────────────────────────────────────
RE_TS        = re.compile(r"^\[(\d{2}:\d{2}:\d{2})\]")
RE_LYAPUNOV  = re.compile(r"\[STELLAR\] Lyapunov weights: B=([\d.]+) U=([\d.]+) T=([\d.]+) L=([\d.]+), V=([\d.]+), Conv=(\d)")
RE_PARETO    = re.compile(r"\[STELLAR\] Pareto rank=(\d+)/(\d+), Centrality=([\d.]+)")
RE_SCORE     = re.compile(r"\[STELLAR\] Score components: u_B=([\d.]+) u_U=([\d.]+) u_T=([\d.]+) u_L=([\d.]+), base=([\d.]+), κ=([\d.]+), ρ=([\d.]+), Ψ=([\d.]+)")
RE_STATUS    = re.compile(r"STATUS: State=(\w+), Role=(\w+)")
RE_BATTERY   = re.compile(r"PME batt=(\d+)%")
RE_TRUST     = re.compile(r"Trust Update Input: Rep=([\d.]+), Current PDR=([\d.]+), Current HSR=([\d.]+)")
RE_NEIGHBOR  = re.compile(r"Discovered neighbor: node_id=(\d+), score=([\d.]+)")

def parse_log(node_name):
    log_path = os.path.join(DATA_DIR, f"{node_name}_monitor.log")
    if not os.path.exists(log_path):
        print(f"  [SKIP] {log_path} not found")
        return pd.DataFrame()

    today = datetime.now().strftime("%Y-%m-%d")
    rows = []
    with open(log_path) as f:
        for line in f:
            line = line.strip()
            m_ts = RE_TS.match(line)
            if not m_ts:
                continue
            ts = pd.to_datetime(f"{today} {m_ts.group(1)}")
            content = line[len(m_ts.group(0)):].strip()

            m = RE_LYAPUNOV.search(content)
            if m:
                rows.append({"ts": ts, "type": "lyapunov",
                             "w_bat": float(m.group(1)), "w_upt": float(m.group(2)),
                             "w_tru": float(m.group(3)), "w_lnk": float(m.group(4)),
                             "V": float(m.group(5)), "conv": int(m.group(6))})

            m = RE_PARETO.search(content)
            if m:
                rows.append({"ts": ts, "type": "pareto",
                             "rank": int(m.group(1)), "total": int(m.group(2)),
                             "centrality": float(m.group(3))})

            m = RE_SCORE.search(content)
            if m:
                rows.append({"ts": ts, "type": "score",
                             "u_bat": float(m.group(1)), "u_upt": float(m.group(2)),
                             "u_tru": float(m.group(3)), "u_lnk": float(m.group(4)),
                             "base": float(m.group(5)), "kappa": float(m.group(6)),
                             "rho": float(m.group(7)), "psi": float(m.group(8))})

            m = RE_STATUS.search(content)
            if m:
                rows.append({"ts": ts, "type": "status",
                             "state": m.group(1), "role": m.group(1)})  # Use State= field

            m = RE_BATTERY.search(content)
            if m:
                rows.append({"ts": ts, "type": "battery", "pct": float(m.group(1))})

            m = RE_TRUST.search(content)
            if m:
                rows.append({"ts": ts, "type": "trust",
                             "rep": float(m.group(1)), "pdr": float(m.group(2)),
                             "hsr": float(m.group(3))})

    df = pd.DataFrame(rows)
    print(f"  {node_name}: {len(df)} events parsed")
    return df

def load_all():
    dfs = {}
    for n in NODES:
        print(f"Parsing {n}...")
        dfs[n] = parse_log(n)
    return dfs

# ── Plot A: Lyapunov Convergence ───────────────────────────────────────────────
def plot_lyapunov(dfs):
    fig, axes = plt.subplots(2, 1, figsize=(12, 8), sharex=False)
    fig.suptitle("Plot A: STELLAR Lyapunov Weight Convergence", fontsize=14, fontweight="bold")

    ax_w, ax_v = axes

    for node, df in dfs.items():
        d = df[df["type"] == "lyapunov"].copy()
        if d.empty:
            continue
        color = COLORS[node]
        ax_w.plot(d["ts"], d["w_bat"], color=color, linestyle="-",  label=f"{node} Battery")
        ax_w.plot(d["ts"], d["w_upt"], color=color, linestyle="--", label=f"{node} Uptime")
        ax_w.plot(d["ts"], d["w_tru"], color=color, linestyle=":",  label=f"{node} Trust")
        ax_w.plot(d["ts"], d["w_lnk"], color=color, linestyle="-.", label=f"{node} LinkQ")
        ax_v.plot(d["ts"], d["V"],     color=color, linewidth=2,    label=f"{node} V(t)")

    ax_w.set_ylabel("Weight Value")
    ax_w.set_title("Adaptive Weights (Lyapunov Gradient Descent)")
    ax_w.legend(fontsize=7, ncol=3)
    ax_w.grid(True, alpha=0.3)
    ax_w.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))

    ax_v.axhline(y=0.001, color="red", linestyle="--", label="Convergence Threshold (ε=0.001)")
    ax_v.set_ylabel("Lyapunov V(t)")
    ax_v.set_title("Lyapunov Stability Function V(t) → 0 proves algorithm stability")
    ax_v.legend(fontsize=8)
    ax_v.grid(True, alpha=0.3)
    ax_v.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))

    plt.tight_layout()
    out = f"{OUTPUT_DIR}/PlotA_Lyapunov_Convergence.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")

# ── Plot B: Pareto Rank & Centrality ──────────────────────────────────────────
def plot_pareto_centrality(dfs):
    fig, axes = plt.subplots(2, 1, figsize=(12, 7), sharex=False)
    fig.suptitle("Plot B: Pareto Dominance Rank & Network Centrality", fontsize=14, fontweight="bold")

    ax_p, ax_c = axes

    for node, df in dfs.items():
        d = df[df["type"] == "pareto"].copy()
        if d.empty:
            continue
        color = COLORS[node]
        ax_p.step(d["ts"], d["rank"],       color=color, where="post", linewidth=2, label=node)
        ax_c.plot(d["ts"], d["centrality"], color=color, linewidth=2,  label=node)

    ax_p.set_ylabel("Pareto Rank (# neighbors dominated)")
    ax_p.set_title("Pareto Dominance Rank — higher = better CH candidate")
    ax_p.legend()
    ax_p.grid(True, alpha=0.3)
    ax_p.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))

    ax_c.set_ylabel("Centrality κ (0–1)")
    ax_c.set_title("Network Centrality — fraction of max neighbors seen")
    ax_c.set_ylim(0, 1.1)
    ax_c.legend()
    ax_c.grid(True, alpha=0.3)
    ax_c.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))

    plt.tight_layout()
    out = f"{OUTPUT_DIR}/PlotB_Pareto_Centrality.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")

# ── Plot C: STELLAR Score Components ──────────────────────────────────────────
def plot_score_components(dfs):
    fig, axes = plt.subplots(1, len(NODES), figsize=(16, 5), sharey=True)
    fig.suptitle("Plot C: STELLAR Score Components Ψ(n) per Node", fontsize=14, fontweight="bold")

    for i, (node, df) in enumerate(dfs.items()):
        ax = axes[i]
        d = df[df["type"] == "score"].copy()
        if d.empty:
            ax.set_title(f"{node}\n(no data)")
            continue
        ax.plot(d["ts"], d["u_bat"], label="φ_battery", linewidth=1.5)
        ax.plot(d["ts"], d["u_upt"], label="φ_uptime",  linewidth=1.5)
        ax.plot(d["ts"], d["u_tru"], label="φ_trust",   linewidth=1.5)
        ax.plot(d["ts"], d["u_lnk"], label="φ_linkq",   linewidth=1.5)
        ax.plot(d["ts"], d["psi"],   label="Ψ (final)", linewidth=2.5, color="black", linestyle="--")
        ax.set_title(f"{node}")
        ax.set_ylim(0, 1.1)
        ax.grid(True, alpha=0.3)
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
        ax.tick_params(axis="x", rotation=30)
        if i == 0:
            ax.set_ylabel("Utility / Score (0–1)")
        if i == len(NODES) - 1:
            ax.legend(fontsize=8)

    plt.tight_layout()
    out = f"{OUTPUT_DIR}/PlotC_Score_Components.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")

# ── Plot D: Role Transitions ───────────────────────────────────────────────────
def plot_roles(dfs):
    fig, ax = plt.subplots(figsize=(13, 5))
    fig.suptitle("Plot D: Cluster Role Transitions", fontsize=14, fontweight="bold")

    role_map = {"DISCOVER": 0, "SYNC_TIME": 0, "CANDIDATE": 1,
                "MEMBER": 2, "NODE": 2, "CH": 3, "CL_HEAD": 3, "GATEWAY": 4}
    offsets  = {"Node1": 0.08, "Node2": 0.0, "Node3": -0.08}

    for node, df in dfs.items():
        d = df[df["type"] == "status"].copy()
        if d.empty:
            continue
        d["role_num"] = d["role"].map(role_map).fillna(0)
        d["role_plot"] = d["role_num"] + offsets[node]
        ax.step(d["ts"], d["role_plot"], where="post",
                color=COLORS[node], linewidth=2, label=node)

    ax.set_yticks([0, 1, 2, 3, 4])
    ax.set_yticklabels(["Init/Discover", "Candidate", "Member", "Cluster Head", "Gateway"])
    ax.set_ylabel("Role State")
    ax.set_xlabel("Time")
    ax.legend()
    ax.grid(True, axis="y", alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))

    plt.tight_layout()
    out = f"{OUTPUT_DIR}/PlotD_Role_Transitions.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")

# ── Plot E: Battery Profile ────────────────────────────────────────────────────
def plot_battery(dfs):
    fig, ax = plt.subplots(figsize=(12, 5))
    fig.suptitle("Plot E: Battery Profile (PME Simulated Drain)", fontsize=14, fontweight="bold")

    for node, df in dfs.items():
        d = df[df["type"] == "battery"].copy()
        if d.empty:
            continue
        ax.plot(d["ts"], d["pct"], color=COLORS[node], linewidth=2, label=node)

    ax.axhline(y=60, color="orange", linestyle="--", label="POWER_SAVE threshold (60%)")
    ax.axhline(y=10, color="red",    linestyle="--", label="CRITICAL threshold (10%)")
    ax.set_ylabel("Battery (%)")
    ax.set_ylim(0, 105)
    ax.legend()
    ax.grid(True, alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))

    plt.tight_layout()
    out = f"{OUTPUT_DIR}/PlotE_Battery_Profile.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")

# ── Plot F: Trust Dynamics ─────────────────────────────────────────────────────
def plot_trust(dfs):
    fig, axes = plt.subplots(3, 1, figsize=(12, 9), sharex=False)
    fig.suptitle("Plot F: Trust Metric Dynamics (HSR, PDR, Reputation)", fontsize=14, fontweight="bold")

    labels = ["Reputation", "PDR", "HSR"]
    keys   = ["rep", "pdr", "hsr"]

    for i, (key, label) in enumerate(zip(keys, labels)):
        ax = axes[i]
        for node, df in dfs.items():
            d = df[df["type"] == "trust"].copy()
            if d.empty:
                continue
            ax.plot(d["ts"], d[key], color=COLORS[node], linewidth=1.5, label=node)
        ax.set_ylabel(label)
        ax.set_ylim(0, 1.1)
        ax.axhline(y=0.2, color="red", linestyle=":", alpha=0.5, label="Trust floor")
        ax.grid(True, alpha=0.3)
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))
        if i == 0:
            ax.legend(fontsize=8)

    plt.tight_layout()
    out = f"{OUTPUT_DIR}/PlotF_Trust_Dynamics.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")

# ── Plot G: Smoothed STELLAR Score Ψ — All Nodes ─────────────────────────────
def plot_stellar_score(dfs):
    fig, (ax_main, ax_raw) = plt.subplots(2, 1, figsize=(13, 8), sharex=False)
    fig.suptitle("Plot G: Smoothed STELLAR Score Ψ — All Nodes", fontsize=14, fontweight="bold")

    # ── Top panel: smoothed Ψ with election winner annotation ─────────────────
    best_node = None
    best_mean = -1
    for node, df in dfs.items():
        d = df[df["type"] == "score"].copy()
        if d.empty:
            continue
        color = COLORS[node]
        ax_main.plot(d["ts"], d["psi"], color=color, linewidth=2.5, label=f"{node}  Ψ")
        mean_psi = d["psi"].mean()
        if mean_psi > best_mean:
            best_mean = mean_psi
            best_node = node

    ax_main.set_ylabel("Smoothed STELLAR Score Ψ (0–1)", fontsize=11)
    ax_main.set_ylim(0, 1.1)
    ax_main.set_title(
        f"EWMA-smoothed Ψ (α=0.25) — {best_node} consistently highest → elected CH",
        fontsize=10
    )
    ax_main.legend(fontsize=9)
    ax_main.grid(True, alpha=0.3)
    ax_main.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))

    # Shade the region of the winning node's score
    if best_node:
        d_best = dfs[best_node][dfs[best_node]["type"] == "score"]
        if not d_best.empty:
            ax_main.fill_between(
                d_best["ts"], d_best["psi"], alpha=0.12,
                color=COLORS[best_node], label=f"{best_node} area"
            )

    # ── Bottom panel: raw Ψ vs smoothed Ψ for the CH node (shows EWMA effect) ─
    if best_node:
        d_ch = dfs[best_node][dfs[best_node]["type"] == "score"].copy()
        if not d_ch.empty:
            # Reconstruct raw score from base and kappa (base * kappa + rho)
            d_ch["psi_raw"] = d_ch["base"] * d_ch["kappa"] + d_ch["rho"]
            ax_raw.plot(d_ch["ts"], d_ch["psi_raw"], color="gray",
                        linewidth=1, alpha=0.7, label=f"{best_node} raw Ψ (pre-EWMA)")
            ax_raw.plot(d_ch["ts"], d_ch["psi"],     color=COLORS[best_node],
                        linewidth=2.5, label=f"{best_node} smoothed Ψ (EWMA α=0.25)")
            ax_raw.set_ylabel("Ψ (0–1)", fontsize=11)
            ax_raw.set_ylim(0, 1.1)
            ax_raw.set_title(
                f"{best_node} (CH): Raw vs EWMA-smoothed Ψ — smoothing prevents re-election from transient dips",
                fontsize=10
            )
            ax_raw.legend(fontsize=9)
            ax_raw.grid(True, alpha=0.3)
            ax_raw.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))

    plt.tight_layout()
    out = f"{OUTPUT_DIR}/PlotG_STELLAR_Score_Smoothed.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")

# ── Summary Dashboard ──────────────────────────────────────────────────────────
def plot_dashboard(dfs):
    """Single-page summary with key metrics side by side."""
    fig = plt.figure(figsize=(18, 10))
    fig.suptitle("STELLAR WSN System — Research Summary Dashboard\n(Post Engineering Review: Pareto + Centrality Active)",
                 fontsize=14, fontweight="bold")
    gs = gridspec.GridSpec(2, 3, figure=fig, hspace=0.45, wspace=0.35)

    # Panel 1: Lyapunov V(t)
    ax1 = fig.add_subplot(gs[0, 0])
    for node, df in dfs.items():
        d = df[df["type"] == "lyapunov"]
        if not d.empty:
            ax1.plot(d["ts"], d["V"], color=COLORS[node], linewidth=1.5, label=node)
    ax1.axhline(0.001, color="red", linestyle="--", linewidth=1)
    ax1.set_title("Lyapunov V(t) → 0")
    ax1.set_ylabel("V(t)")
    ax1.legend(fontsize=7)
    ax1.grid(True, alpha=0.3)
    ax1.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax1.tick_params(axis="x", rotation=30)

    # Panel 2: Pareto Rank
    ax2 = fig.add_subplot(gs[0, 1])
    for node, df in dfs.items():
        d = df[df["type"] == "pareto"]
        if not d.empty:
            ax2.step(d["ts"], d["rank"], color=COLORS[node], where="post", linewidth=1.5, label=node)
    ax2.set_title("Pareto Dominance Rank")
    ax2.set_ylabel("Rank")
    ax2.legend(fontsize=7)
    ax2.grid(True, alpha=0.3)
    ax2.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax2.tick_params(axis="x", rotation=30)

    # Panel 3: STELLAR Score Ψ
    ax3 = fig.add_subplot(gs[0, 2])
    for node, df in dfs.items():
        d = df[df["type"] == "score"]
        if not d.empty:
            ax3.plot(d["ts"], d["psi"], color=COLORS[node], linewidth=1.5, label=node)
    ax3.set_title("Final STELLAR Score Ψ")
    ax3.set_ylabel("Ψ (0–1)")
    ax3.set_ylim(0, 1.1)
    ax3.legend(fontsize=7)
    ax3.grid(True, alpha=0.3)
    ax3.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax3.tick_params(axis="x", rotation=30)

    # Panel 4: Role Transitions
    ax4 = fig.add_subplot(gs[1, 0])
    role_map = {"DISCOVER": 0, "SYNC_TIME": 0, "CANDIDATE": 1, "MEMBER": 2, "NODE": 2, "CH": 3}
    offsets  = {"Node1": 0.06, "Node2": 0.0, "Node3": -0.06}
    for node, df in dfs.items():
        d = df[df["type"] == "status"].copy()
        if not d.empty:
            d["rn"] = d["role"].map(role_map).fillna(0) + offsets[node]
            ax4.step(d["ts"], d["rn"], color=COLORS[node], where="post", linewidth=1.5, label=node)
    ax4.set_yticks([0, 1, 2, 3])
    ax4.set_yticklabels(["Discover", "Candidate", "Member", "CH"], fontsize=7)
    ax4.set_title("Role Transitions")
    ax4.legend(fontsize=7)
    ax4.grid(True, axis="y", alpha=0.3)
    ax4.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax4.tick_params(axis="x", rotation=30)

    # Panel 5: Battery
    ax5 = fig.add_subplot(gs[1, 1])
    for node, df in dfs.items():
        d = df[df["type"] == "battery"]
        if not d.empty:
            ax5.plot(d["ts"], d["pct"], color=COLORS[node], linewidth=1.5, label=node)
    ax5.axhline(60, color="orange", linestyle="--", linewidth=1, label="Power Save")
    ax5.set_title("Battery Profile")
    ax5.set_ylabel("%")
    ax5.set_ylim(0, 105)
    ax5.legend(fontsize=7)
    ax5.grid(True, alpha=0.3)
    ax5.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax5.tick_params(axis="x", rotation=30)

    # Panel 6: Centrality
    ax6 = fig.add_subplot(gs[1, 2])
    for node, df in dfs.items():
        d = df[df["type"] == "pareto"]
        if not d.empty:
            ax6.plot(d["ts"], d["centrality"], color=COLORS[node], linewidth=1.5, label=node)
    ax6.set_title("Network Centrality κ")
    ax6.set_ylabel("κ (0–1)")
    ax6.set_ylim(0, 1.1)
    ax6.legend(fontsize=7)
    ax6.grid(True, alpha=0.3)
    ax6.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax6.tick_params(axis="x", rotation=30)

    out = f"{OUTPUT_DIR}/Dashboard_Summary.png"
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  Saved {out}")

# ── Main ───────────────────────────────────────────────────────────────────────
def main():
    print("Loading data...")
    dfs = load_all()

    total_events = sum(len(df) for df in dfs.values())
    if total_events == 0:
        print("ERROR: No data parsed. Check log files.")
        return

    print(f"\nGenerating plots ({total_events} total events)...")
    plot_lyapunov(dfs)
    plot_pareto_centrality(dfs)
    plot_score_components(dfs)
    plot_roles(dfs)
    plot_battery(dfs)
    plot_trust(dfs)
    plot_stellar_score(dfs)
    plot_dashboard(dfs)

    print(f"\nAll plots saved to {OUTPUT_DIR}/")

if __name__ == "__main__":
    main()
