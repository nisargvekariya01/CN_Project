#!/usr/bin/env python3
"""
plot_results.py - Generates evaluation charts and coordinates CSVs for IT305 Lab (Group 103 & 106).
Produces:
  1. Part1_SingleVsMulti_throughput.png + Part1_SingleVsMulti_throughput_coords.csv
  2. Part1_SingleVsMulti_overhead.png + Part1_SingleVsMulti_overhead_coords.csv
  3. Part2_AllCases_throughput.png + Part2_AllCases_throughput_coords.csv
  4. Part2_AllCases_overhead.png + Part2_AllCases_overhead_coords.csv
  5. Part2_Retransmission_Overhead_pct.png + Part2_Retransmission_Overhead_pct_coords.csv
  6. Part1_Speedup_Factor.png + Part1_Speedup_Factor_coords.csv
"""

import os
import sys
import pandas as pd
import matplotlib.pyplot as plt

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

def generate_plots():
    p1_csv = os.path.join(SCRIPT_DIR, "Part1_metrics.csv")
    p2_csv = os.path.join(SCRIPT_DIR, "Part2_metrics.csv")

    if not os.path.exists(p1_csv) or not os.path.exists(p2_csv):
        # If files need standardizing first, import and run preparation
        try:
            import build_enhanced_metrics_and_plots
            build_enhanced_metrics_and_plots.main()
            return
        except Exception as e:
            print(f"[ERROR] Failed initializing metrics: {e}")
            return

    p1_df = pd.read_csv(p1_csv)
    p2_df = pd.read_csv(p2_csv)

    # Style configuration
    plt.rcParams['font.sans-serif'] = 'DejaVu Sans'
    plt.rcParams['axes.edgecolor'] = '#333333'
    plt.rcParams['axes.linewidth'] = 1.0

    # -------------------------------------------------------------------------
    # 1. Part I: Throughput Graph (Concurrent Clients vs. Aggregate Throughput)
    # -------------------------------------------------------------------------
    # Use consistent dataset benchmark runs (Payload < 10 MB) to ensure apples-to-apples concurrency comparison
    p1_clean = p1_df[p1_df['Payload_Bytes'] < 10000000]

    # Aggregate throughput = Sum of all client payload bytes transferred / max duration across concurrent clients
    agg_tp = p1_clean.groupby(['Server_Type', 'Concurrent_Clients']).apply(
        lambda g: (g['Payload_Bytes'].sum() / (g['Total_Time_Seconds'].max() * 1e6)) if g['Total_Time_Seconds'].max() > 0 else 0
    ).unstack(level=0)

    # Coordinates CSV
    p1_tp_coords = pd.DataFrame({
        "Concurrent_Clients": agg_tp.index,
        "Single_Threaded_Throughput_MBps": agg_tp['Single'].values,
        "Multi_Threaded_Throughput_MBps": agg_tp['Multi'].values
    })
    p1_tp_coords_path = os.path.join(SCRIPT_DIR, "Part1_SingleVsMulti_throughput_coords.csv")
    p1_tp_coords.to_csv(p1_tp_coords_path, index=False)
    print(f"[OK] Saved coordinates to {p1_tp_coords_path}")

    # Plot
    fig, ax = plt.subplots(figsize=(8.5, 5.2), dpi=300)
    clients = p1_tp_coords['Concurrent_Clients']
    ax.plot(clients, p1_tp_coords['Single_Threaded_Throughput_MBps'], marker='o', color='#E63946', 
            linewidth=2.5, markersize=8, label='Single-Threaded Baseline')
    ax.plot(clients, p1_tp_coords['Multi_Threaded_Throughput_MBps'], marker='s', color='#1D3557', 
            linewidth=2.5, markersize=8, label='Multi-Threaded (Thread-Per-Client)')
    
    for x, y in zip(clients, p1_tp_coords['Multi_Threaded_Throughput_MBps']):
        ax.annotate(f"{y:.2f}", (x, y), textcoords="offset points", xytext=(0, 8), 
                    ha='center', fontsize=8.5, fontweight='bold', color='#1D3557')
    for x, y in zip(clients, p1_tp_coords['Single_Threaded_Throughput_MBps']):
        ax.annotate(f"{y:.2f}", (x, y), textcoords="offset points", xytext=(0, -14), 
                    ha='center', fontsize=8.5, color='#E63946')

    ax.set_title('Part I: Number of Concurrent Clients vs. Aggregate Throughput', fontsize=12, fontweight='bold', pad=12)
    ax.set_xlabel('Number of Concurrent Clients (N)', fontsize=10.5, labelpad=8)
    ax.set_ylabel('Aggregate Throughput (MB/s)', fontsize=10.5, labelpad=8)
    ax.set_xticks([1, 2, 4, 8, 16, 32])
    ax.grid(True, linestyle='--', alpha=0.6)
    ax.legend(frameon=True, facecolor='#F8F9FA', edgecolor='#D3D3D3', fontsize=10)
    plt.tight_layout()
    out_p1_tp = os.path.join(SCRIPT_DIR, "Part1_SingleVsMulti_throughput.png")
    plt.savefig(out_p1_tp)
    plt.close()
    print(f"[OK] Generated {out_p1_tp}")

    # -------------------------------------------------------------------------
    # 2. Part I: Overhead Graph (Concurrent Clients vs. Average Transfer Time)
    # -------------------------------------------------------------------------
    avg_time = p1_clean.groupby(['Server_Type', 'Concurrent_Clients'])['Total_Time_Seconds'].mean().unstack(level=0)
    
    p1_ov_coords = pd.DataFrame({
        "Concurrent_Clients": avg_time.index,
        "Single_Threaded_Avg_Transfer_Time_Sec": avg_time['Single'].values,
        "Multi_Threaded_Avg_Transfer_Time_Sec": avg_time['Multi'].values
    })
    p1_ov_coords_path = os.path.join(SCRIPT_DIR, "Part1_SingleVsMulti_overhead_coords.csv")
    p1_ov_coords.to_csv(p1_ov_coords_path, index=False)
    print(f"[OK] Saved coordinates to {p1_ov_coords_path}")

    fig, ax = plt.subplots(figsize=(8.5, 5.2), dpi=300)
    ax.plot(clients, p1_ov_coords['Single_Threaded_Avg_Transfer_Time_Sec'], marker='^', color='#D90429', 
            linewidth=2.5, markersize=8, label='Single-Threaded (Linear Queuing Delay)')
    ax.plot(clients, p1_ov_coords['Multi_Threaded_Avg_Transfer_Time_Sec'], marker='o', color='#2B9348', 
            linewidth=2.5, markersize=8, label='Multi-Threaded (Concurrent Service)')
    
    for x, y in zip(clients, p1_ov_coords['Single_Threaded_Avg_Transfer_Time_Sec']):
        ax.annotate(f"{y:.2f}s", (x, y), textcoords="offset points", xytext=(-5, 8), 
                    ha='right', fontsize=8.5, fontweight='bold', color='#D90429')
    for x, y in zip(clients, p1_ov_coords['Multi_Threaded_Avg_Transfer_Time_Sec']):
        ax.annotate(f"{y:.2f}s", (x, y), textcoords="offset points", xytext=(5, -12), 
                    ha='left', fontsize=8.5, color='#2B9348')

    ax.set_title('Part I: Concurrency Overhead & Queuing Delay vs. Client Transfer Time', fontsize=12, fontweight='bold', pad=12)
    ax.set_xlabel('Number of Concurrent Clients (N)', fontsize=10.5, labelpad=8)
    ax.set_ylabel('Average Transfer Time per Client (seconds)', fontsize=10.5, labelpad=8)
    ax.set_xticks([1, 2, 4, 8, 16, 32])
    ax.grid(True, linestyle='--', alpha=0.6)
    ax.legend(frameon=True, facecolor='#F8F9FA', edgecolor='#D3D3D3', fontsize=10)
    plt.tight_layout()
    out_p1_ov = os.path.join(SCRIPT_DIR, "Part1_SingleVsMulti_overhead.png")
    plt.savefig(out_p1_ov)
    plt.close()
    print(f"[OK] Generated {out_p1_ov}")

    # -------------------------------------------------------------------------
    # 3. Part II: Throughput Graph (Failure Probability vs. Throughput)
    # -------------------------------------------------------------------------
    p2_tp = p2_df.groupby(['Case_Name', 'Failure_Probability'])['Throughput_MBps'].mean().unstack(level=0)
    
    p2_tp_coords = pd.DataFrame({
        "Failure_Probability": p2_tp.index,
        "Case1_Throughput_MBps": p2_tp['Part2_Case1'].values,
        "Case2_Throughput_MBps": p2_tp['Part2_Case2'].values,
        "Case2_Enhanced_Throughput_MBps": p2_tp['Part2_Case2_Enhanced'].values
    })
    p2_tp_coords_path = os.path.join(SCRIPT_DIR, "Part2_AllCases_throughput_coords.csv")
    p2_tp_coords.to_csv(p2_tp_coords_path, index=False)
    print(f"[OK] Saved coordinates to {p2_tp_coords_path}")

    fig, ax = plt.subplots(figsize=(8.8, 5.4), dpi=300)
    pf_vals = p2_tp_coords['Failure_Probability']
    ax.plot(pf_vals, p2_tp_coords['Case1_Throughput_MBps'], marker='x', color='#D90429', 
            linewidth=2.3, markersize=8, label='Case 1: Stateless Restart (No Session)')
    ax.plot(pf_vals, p2_tp_coords['Case2_Throughput_MBps'], marker='s', color='#F77F00', 
            linewidth=2.3, markersize=7, label='Case 2: Stateful Session Checkpointing')
    ax.plot(pf_vals, p2_tp_coords['Case2_Enhanced_Throughput_MBps'], marker='o', color='#0077B6', 
            linewidth=2.5, markersize=8, label='Case 2 Enhanced: Parallel TCP + Epoll')

    for x, y in zip(pf_vals, p2_tp_coords['Case2_Enhanced_Throughput_MBps']):
        ax.annotate(f"{y:.2f}", (x, y), textcoords="offset points", xytext=(0, 7), 
                    ha='center', fontsize=8, fontweight='bold', color='#0077B6')

    ax.set_title('Part II: Fault Tolerance Scaling — Throughput vs. Failure Probability (N=8)', fontsize=12, fontweight='bold', pad=12)
    ax.set_xlabel('Failure Probability ($P_f$)', fontsize=10.5, labelpad=8)
    ax.set_ylabel('Throughput (MB/s)', fontsize=10.5, labelpad=8)
    ax.set_xticks(pf_vals)
    ax.grid(True, linestyle='--', alpha=0.6)
    ax.legend(frameon=True, facecolor='#F8F9FA', edgecolor='#D3D3D3', fontsize=10)
    plt.tight_layout()
    out_p2_tp = os.path.join(SCRIPT_DIR, "Part2_AllCases_throughput.png")
    plt.savefig(out_p2_tp)
    plt.close()
    print(f"[OK] Generated {out_p2_tp}")

    # -------------------------------------------------------------------------
    # 4. Part II: Overhead Graph (Failure Probability vs. Wasted Transfer Bytes)
    # -------------------------------------------------------------------------
    p2_wasted = p2_df.groupby(['Case_Name', 'Failure_Probability'])['Wasted_Bytes'].mean().unstack(level=0)
    
    p2_ov_coords = pd.DataFrame({
        "Failure_Probability": p2_wasted.index,
        "Case1_Wasted_Bytes": p2_wasted['Part2_Case1'].values,
        "Case2_Wasted_Bytes": p2_wasted['Part2_Case2'].values,
        "Case2_Enhanced_Wasted_Bytes": p2_wasted['Part2_Case2_Enhanced'].values
    })
    p2_ov_coords_path = os.path.join(SCRIPT_DIR, "Part2_AllCases_overhead_coords.csv")
    p2_ov_coords.to_csv(p2_ov_coords_path, index=False)
    print(f"[OK] Saved coordinates to {p2_ov_coords_path}")

    fig, ax = plt.subplots(figsize=(8.8, 5.4), dpi=300)
    c1_mb = p2_ov_coords['Case1_Wasted_Bytes'] / (1024.0 * 1024.0)
    c2_mb = p2_ov_coords['Case2_Wasted_Bytes'] / (1024.0 * 1024.0)
    c2_enh_mb = p2_ov_coords['Case2_Enhanced_Wasted_Bytes'] / (1024.0 * 1024.0)

    ax.plot(pf_vals, c1_mb, marker='x', color='#D90429', linewidth=2.5, markersize=8, 
            label='Case 1 (Stateless): Retransmits All From Byte 0')
    ax.plot(pf_vals, c2_mb, marker='s', color='#F77F00', linewidth=2.3, markersize=7, 
            label='Case 2 (Session Management): Checkpointed Offset')
    ax.plot(pf_vals, c2_enh_mb, marker='o', color='#0077B6', linewidth=2.5, markersize=8, 
            label='Case 2 Enhanced: Chunk Bitmask Recovery')

    for x, y in zip(pf_vals, c1_mb):
        if y > 0:
            ax.annotate(f"{y:.1f} MB", (x, y), textcoords="offset points", xytext=(-5, 8), 
                        ha='right', fontsize=8, fontweight='bold', color='#D90429')
    for x, y in zip(pf_vals, c2_mb):
        if y > 0:
            ax.annotate(f"{y*1024:.0f} KB", (x, y), textcoords="offset points", xytext=(8, -12), 
                        ha='left', fontsize=8, color='#F77F00')

    ax.set_title('Part II: Retransmission Overhead — Wasted Transfer Bytes vs. Failure Probability', fontsize=12, fontweight='bold', pad=12)
    ax.set_xlabel('Failure Probability ($P_f$)', fontsize=10.5, labelpad=8)
    ax.set_ylabel('Wasted Transfer Volume (MB)', fontsize=10.5, labelpad=8)
    ax.set_xticks(pf_vals)
    ax.grid(True, linestyle='--', alpha=0.6)
    ax.legend(frameon=True, facecolor='#F8F9FA', edgecolor='#D3D3D3', fontsize=10)
    plt.tight_layout()
    out_p2_ov = os.path.join(SCRIPT_DIR, "Part2_AllCases_overhead.png")
    plt.savefig(out_p2_ov)
    plt.close()
    print(f"[OK] Generated {out_p2_ov}")

    # -------------------------------------------------------------------------
    # 5. Extra Plot A: Part 2 Retransmission Overhead Percentage (%) vs. Pf
    # -------------------------------------------------------------------------
    ov_pct_df = pd.DataFrame({
        "Failure_Probability": pf_vals,
        "Case1_Overhead_Pct": (p2_ov_coords['Case1_Wasted_Bytes'] / 2497353.0) * 100.0,
        "Case2_Overhead_Pct": (p2_ov_coords['Case2_Wasted_Bytes'] / 2497353.0) * 100.0,
        "Case2_Enhanced_Overhead_Pct": (p2_ov_coords['Case2_Enhanced_Wasted_Bytes'] / 2497353.0) * 100.0
    })
    ov_pct_coords_path = os.path.join(SCRIPT_DIR, "Part2_Retransmission_Overhead_pct_coords.csv")
    ov_pct_df.to_csv(ov_pct_coords_path, index=False)
    print(f"[OK] Saved coordinates to {ov_pct_coords_path}")

    fig, ax = plt.subplots(figsize=(8.8, 5.2), dpi=300)
    ax.plot(pf_vals, ov_pct_df['Case1_Overhead_Pct'], marker='x', color='#D90429', linewidth=2.5, markersize=8, label='Case 1: Stateless Reset')
    ax.plot(pf_vals, ov_pct_df['Case2_Overhead_Pct'], marker='s', color='#F77F00', linewidth=2.3, markersize=7, label='Case 2: Stateful Session Checkpointing')
    ax.plot(pf_vals, ov_pct_df['Case2_Enhanced_Overhead_Pct'], marker='o', color='#0077B6', linewidth=2.5, markersize=8, label='Case 2 Enhanced: Bitmask Chunk Recovery')

    ax.set_title('Extra Evaluation: Retransmission Overhead (%) vs. Failure Probability', fontsize=12, fontweight='bold', pad=12)
    ax.set_xlabel('Failure Probability ($P_f$)', fontsize=10.5, labelpad=8)
    ax.set_ylabel('Retransmission Overhead (%)', fontsize=10.5, labelpad=8)
    ax.set_xticks(pf_vals)
    ax.grid(True, linestyle='--', alpha=0.6)
    ax.legend(frameon=True, facecolor='#F8F9FA', edgecolor='#D3D3D3', fontsize=10)
    plt.tight_layout()
    out_extra_pct = os.path.join(SCRIPT_DIR, "Part2_Retransmission_Overhead_pct.png")
    plt.savefig(out_extra_pct)
    plt.close()
    print(f"[OK] Generated {out_extra_pct}")

    # -------------------------------------------------------------------------
    # 6. Extra Plot B: Part 1 Multi-threaded Speedup Factor vs. Concurrent Clients
    # -------------------------------------------------------------------------
    speedup = p1_tp_coords['Multi_Threaded_Throughput_MBps'] / p1_tp_coords['Single_Threaded_Throughput_MBps']
    speedup_df = pd.DataFrame({
        "Concurrent_Clients": clients,
        "Speedup_Factor": round(speedup, 4)
    })
    speedup_coords_path = os.path.join(SCRIPT_DIR, "Part1_Speedup_Factor_coords.csv")
    speedup_df.to_csv(speedup_coords_path, index=False)
    print(f"[OK] Saved coordinates to {speedup_coords_path}")

    fig, ax = plt.subplots(figsize=(8.5, 5.0), dpi=300)
    ax.bar([str(c) for c in clients], speedup, color='#457B9D', width=0.55, edgecolor='#1D3557', linewidth=1.2)
    ax.axhline(1.0, color='#E63946', linestyle='--', linewidth=1.5, label='Baseline (1.0x)')
    for idx, (c, s) in enumerate(zip(clients, speedup)):
        ax.text(idx, s + 0.1, f"{s:.2f}x", ha='center', fontsize=9, fontweight='bold', color='#1D3557')

    ax.set_title('Part I: Concurrency Speedup Factor (Multi-Threaded / Single-Threaded)', fontsize=12, fontweight='bold', pad=12)
    ax.set_xlabel('Number of Concurrent Clients (N)', fontsize=10.5, labelpad=8)
    ax.set_ylabel('Speedup Factor', fontsize=10.5, labelpad=8)
    ax.set_ylim(0, max(speedup) * 1.2)
    ax.grid(True, axis='y', linestyle='--', alpha=0.6)
    ax.legend(frameon=True, facecolor='#F8F9FA', edgecolor='#D3D3D3', fontsize=10)
    plt.tight_layout()
    out_speedup = os.path.join(SCRIPT_DIR, "Part1_Speedup_Factor.png")
    plt.savefig(out_speedup)
    plt.close()
    print(f"[OK] Generated {out_speedup}")

if __name__ == "__main__":
    generate_plots()
