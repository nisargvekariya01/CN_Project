#!/usr/bin/env python3
"""
plot_results.py - Generates evaluation charts for IT305 Lab (Group 103 & 106).
Produces:
  1. Part1_SingleVsMulti_throughput.png (Aggregate File Transfer Rate vs N)
  2. Part2_AllCases_throughput.png (Goodput vs Pf at N=8)
  3. Part2_AllCases_overhead.png (Retransmission Overhead % vs Pf at N=8)
"""

import os
import sys
import pandas as pd
import matplotlib.pyplot as plt

def load_metrics(filepath, col_names):
    if not os.path.exists(filepath):
        print(f"File not found: {filepath}. Skipping.")
        return None
    try:
        # Check if first line contains string headers
        with open(filepath, 'r') as f:
            first_line = f.readline().strip()
        if first_line.startswith("case_name") or first_line.startswith("case,"):
            df = pd.read_csv(filepath)
            df.columns = col_names
        else:
            df = pd.read_csv(filepath, names=col_names, header=None)
        
        # Ensure numerical types
        numeric_cols = ["num_clients", "pf", "t_start", "t_end", "duration", 
                        "useful_b", "total_b", "redundant_b", "goodput", "throughput", "overhead", "retries"]
        for col in numeric_cols:
            if col in df.columns:
                df[col] = pd.to_numeric(df[col], errors='coerce')
        return df.dropna()
    except Exception as e:
        print(f"Error loading {filepath}: {e}")
        return None

def generate_plots():
    col_names = [
        "case", "num_clients", "pf", "t_start", "t_end", "duration", 
        "useful_b", "total_b", "redundant_b", "goodput", "throughput", "overhead", "retries"
    ]
    
    script_dir = os.path.dirname(os.path.abspath(__file__))

    # -------------------------------------------------------------------------
    # 1. Part I: Aggregate Transfer Rate vs. Number of Clients (N)
    # -------------------------------------------------------------------------
    csv1_path = os.path.join(script_dir, "Part1_metrics.csv")
    df1 = load_metrics(csv1_path, col_names)
    if df1 is not None and not df1.empty:
        plt.figure(figsize=(9, 5.5))
        for case_name in sorted(df1['case'].unique()):
            subset = df1[df1['case'] == case_name]
            # Aggregate goodput: Sum of useful bytes across clients / max experiment duration
            agg = subset.groupby('num_clients').apply(
                lambda g: (g['useful_b'].sum() / (g['duration'].max() * 1e6)) if g['duration'].max() > 0 else 0
            )
            plt.plot(agg.index, agg.values, marker='o', linewidth=2, label=case_name)
            
        plt.title('Part 1: Aggregate File Transfer Rate vs. Concurrent Clients', fontsize=12, fontweight='bold')
        plt.xlabel('Number of Concurrent Clients (N)', fontsize=10)
        plt.ylabel('Aggregate Goodput (MB/s)', fontsize=10)
        plt.grid(True, linestyle='--', alpha=0.6)
        plt.legend(frameon=True)
        out1 = os.path.join(script_dir, 'Part1_SingleVsMulti_throughput.png')
        plt.tight_layout()
        plt.savefig(out1, dpi=300)
        plt.close()
        print(f"[OK] Saved {out1}")

    # -------------------------------------------------------------------------
    # 2. Part II: Goodput and Retransmission Overhead vs. Fault Probability (Pf)
    # -------------------------------------------------------------------------
    csv2_path = os.path.join(script_dir, "Part2_metrics.csv")
    df2 = load_metrics(csv2_path, col_names)
    if df2 is not None and not df2.empty:
        # Plot A: Goodput vs Pf
        plt.figure(figsize=(9, 5.5))
        for case_name in sorted(df2['case'].unique()):
            subset = df2[df2['case'] == case_name]
            mean_goodput = subset.groupby('pf')['goodput'].mean()
            plt.plot(mean_goodput.index, mean_goodput.values, marker='s', linewidth=2, label=case_name)
            
        plt.title('Part 2: Average Client Goodput vs. Fault Probability (N=8)', fontsize=12, fontweight='bold')
        plt.xlabel('Fault Injection Probability ($P_f$)', fontsize=10)
        plt.ylabel('Goodput (MB/s)', fontsize=10)
        plt.grid(True, linestyle='--', alpha=0.6)
        plt.legend(frameon=True)
        out2 = os.path.join(script_dir, 'Part2_AllCases_throughput.png')
        plt.tight_layout()
        plt.savefig(out2, dpi=300)
        plt.close()
        print(f"[OK] Saved {out2}")

        # Plot B: Overhead (%) vs Pf
        plt.figure(figsize=(9, 5.5))
        for case_name in sorted(df2['case'].unique()):
            subset = df2[df2['case'] == case_name]
            mean_overhead = subset.groupby('pf')['overhead'].mean()
            plt.plot(mean_overhead.index, mean_overhead.values, marker='^', linewidth=2, label=case_name)
            
        plt.title('Part 2: Retransmission Overhead (%) vs. Fault Probability (N=8)', fontsize=12, fontweight='bold')
        plt.xlabel('Fault Injection Probability ($P_f$)', fontsize=10)
        plt.ylabel('Overhead (%)', fontsize=10)
        plt.grid(True, linestyle='--', alpha=0.6)
        plt.legend(frameon=True)
        out3 = os.path.join(script_dir, 'Part2_AllCases_overhead.png')
        plt.tight_layout()
        plt.savefig(out3, dpi=300)
        plt.close()
        print(f"[OK] Saved {out3}")

if __name__ == "__main__":
    generate_plots()
