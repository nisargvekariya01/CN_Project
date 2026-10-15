#!/bin/bash
# run_benchmarks.sh - Executes Part 1 & Part 2 scaling experiments

SERVER_IP="127.0.0.1" # Replace with actual Node 1 IP during lab testing
PORT=8080
TOPIC_DIR="../Dataset"
TOPIC="Cane"
OUT_DIR="../ClientOutput"

mkdir -p $OUT_DIR
echo "Starting Automated Benchmarks..."

# Part 1: Concurrency Scaling (N = 1, 2, 4, 8, 16, 32)
echo "--- Running Part 1 (Single vs Multi-Threaded) ---"
for N in 1 2 4 8 16 32; do
    for MODE in "--single-threaded" ""; do
        echo "Testing Part 1 | N=$N | Mode: $MODE"
        ../Part1/server $PORT $TOPIC_DIR $MODE &
        SERVER_PID=$!
        sleep 1 # Wait for server to bind

        for ((i=1; i<=N; i++)); do
            ../Part1/client $SERVER_IP $PORT $TOPIC $OUT_DIR &
        done
        wait # Wait for all N clients to finish

        kill $SERVER_PID
        sleep 1
    done
done

# Part 2: Fault Tolerance Comparison (Pf = 0.0, 0.1, 0.3, 0.5) at N=8
echo "--- Running Part 2 (Fault Tolerance) ---"
for PF in 0.0 0.1 0.3 0.5; do
    for CASE in "Part2_Case1" "Part2_Case2" "Part2_Case2_Enhanced"; do
        echo "Testing $CASE | Pf=$PF | N=8"
        ../$CASE/server $PORT $TOPIC_DIR $PF &
        SERVER_PID=$!
        sleep 1

        for ((i=1; i<=8; i++)); do
            ../$CASE/client $SERVER_IP $PORT $TOPIC $OUT_DIR &
        done
        wait

        kill $SERVER_PID
        sleep 1
    done
done

echo "Benchmarks complete. Run plot_results.py to generate graphs."