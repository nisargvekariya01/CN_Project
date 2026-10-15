import pandas as pd
import matplotlib.pyplot as plt

def generate_plots():
    # 1. Part I: Aggregate Transfer Rate vs. Clients
    try:
        df1 = pd.read_csv("Part1_metrics.csv", names=[
            "case", "num_clients", "pf", "t_start", "t_end", "duration", 
            "useful_b", "total_b", "redundant_b", "goodput", "throughput", "overhead", "retries"
        ])
        
        plt.figure(figsize=(8, 5))
        for case_name in df1['case'].unique():
            subset = df1[df1['case'] == case_name]
            agg_goodput = subset.groupby('num_clients')['useful_b'].sum() / subset.groupby('num_clients')['duration'].max() / 1e6
            plt.plot(agg_goodput.index, agg_goodput.values, marker='o', label=case_name)
            
        plt.title('Part 1: Aggregate File Transfer Rate vs. Clients')
        plt.xlabel('Number of Concurrent Clients (N)')
        plt.ylabel('Aggregate Goodput (MB/s)')
        plt.legend()
        plt.grid(True)
        plt.savefig('Part1_SingleVsMulti_throughput.png')
        plt.close()
    except FileNotFoundError:
        print("Part1_metrics.csv not found. Skipping Part 1 plot.")

    # 2. Part II: Goodput and Overhead vs. Fault Probability
    try:
        df2 = pd.read_csv("Part2_metrics.csv", names=[
            "case", "num_clients", "pf", "t_start", "t_end", "duration", 
            "useful_b", "total_b", "redundant_b", "goodput", "throughput", "overhead", "retries"
        ])
        
        # Goodput Plot
        plt.figure(figsize=(8, 5))
        for case_name in df2['case'].unique():
            subset = df2[df2['case'] == case_name]
            mean_goodput = subset.groupby('pf')['goodput'].mean()
            plt.plot(mean_goodput.index, mean_goodput.values, marker='s', label=case_name)
        plt.title('Part 2: Goodput vs. Fault Probability (N=8)')
        plt.xlabel('Failure Probability (Pf)')
        plt.ylabel('Goodput (MB/s)')
        plt.legend()
        plt.grid(True)
        plt.savefig('Part2_AllCases_throughput.png')
        plt.close()

        # Overhead Plot
        plt.figure(figsize=(8, 5))
        for case_name in df2['case'].unique():
            subset = df2[df2['case'] == case_name]
            mean_overhead = subset.groupby('pf')['overhead'].mean()
            plt.plot(mean_overhead.index, mean_overhead.values, marker='^', label=case_name)
        plt.title('Part 2: Retransmission Overhead vs. Fault Probability')
        plt.xlabel('Failure Probability (Pf)')
        plt.ylabel('Overhead (%)')
        plt.legend()
        plt.grid(True)
        plt.savefig('Part2_AllCases_overhead.png')
        plt.close()
    except FileNotFoundError:
        print("Part2_metrics.csv not found. Skipping Part 2 plots.")

if __name__ == "__main__":
    generate_plots()