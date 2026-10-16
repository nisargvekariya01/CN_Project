#!/bin/bash
# run_benchmarks.sh - Automated benchmark orchestration for IT305 Lab (Group 103 & 106)

# Trap to kill all background subprocesses if benchmark is aborted
trap 'kill $(jobs -p) 2>/dev/null' EXIT INT TERM

SERVER_IP="127.0.0.1"
PORT=8080
TOPIC_DIR="../Dataset"
TOPIC="Cane"
OUT_DIR="../ClientOutput"

mkdir -p "$OUT_DIR"
echo "Starting Automated Benchmarks..."

# ==============================================================================
# Part 1: Concurrency Scaling (N = 1, 2, 4, 8, 16, 32)
# ==============================================================================
echo "--- Running Part 1 (Single-Threaded vs Multi-Threaded) ---"
export FAULT_PROB=0.0
export CSV_FILE="../Results/Part1_metrics.csv"
rm -f "$CSV_FILE" # Clear old benchmark metrics

for N in 1 2 4 8 16 32; do
    for MODE in "--single-threaded" ""; do
        export NUM_CLIENTS=$N
        export CASE_NAME=$([[ "$MODE" == "--single-threaded" ]] && echo "Part1_Single" || echo "Part1_Multi")
        
        echo "Testing $CASE_NAME | N=$N | Mode: $MODE"
        ../Part1/server $PORT $TOPIC_DIR $MODE &
        SERVER_PID=$!
        sleep 1 # Wait for server socket to bind and start listening
        
        CLIENT_PIDS=""
        for ((i=1; i<=N; i++)); do
            ../Part1/client $SERVER_IP $PORT $TOPIC $OUT_DIR &
            CLIENT_PIDS="$CLIENT_PIDS $!"
        done
        
        # Wait strictly for client processes to finish
        wait $CLIENT_PIDS
        
        # Terminate background server cleanly
        kill -15 $SERVER_PID 2>/dev/null || kill -9 $SERVER_PID 2>/dev/null
        wait $SERVER_PID 2>/dev/null
        sleep 1
    done
done

# ==============================================================================
# Part 2: Fault Tolerance Comparison (Pf = 0.0, 0.1, 0.3, 0.5) at N=8
# ==============================================================================
echo "--- Running Part 2 (Fault Tolerance Scaling) ---"
export NUM_CLIENTS=8
export CSV_FILE="../Results/Part2_metrics.csv"
rm -f "$CSV_FILE" # Clear old benchmark metrics

for PF in 0.0 0.1 0.3 0.5; do
    export FAULT_PROB=$PF
    for CASE in "Part2_Case1" "Part2_Case2" "Part2_Case2_Enhanced"; do
        export CASE_NAME=$CASE
        echo "Testing $CASE_NAME | Pf=$PF | N=8"
        
        ../$CASE/server $PORT $TOPIC_DIR $PF &
        SERVER_PID=$!
        sleep 1
        
        CLIENT_PIDS=""
        for ((i=1; i<=8; i++)); do
            ../$CASE/client $SERVER_IP $PORT $TOPIC $OUT_DIR &
            CLIENT_PIDS="$CLIENT_PIDS $!"
        done
        
        wait $CLIENT_PIDS
        
        kill -15 $SERVER_PID 2>/dev/null || kill -9 $SERVER_PID 2>/dev/null
        wait $SERVER_PID 2>/dev/null
        sleep 1
    done
done

echo "Benchmarks complete. Run 'python3 plot_results.py' to generate comparison graphs."
