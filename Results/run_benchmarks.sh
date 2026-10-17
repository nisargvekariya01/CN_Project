#!/bin/bash
# run_benchmarks.sh - Automated benchmark orchestration for IT305 Lab (Group 103 & 106)
# Fully optimized to eliminate race conditions, redundant dataset indexing, and file collisions.

SERVER_IP="${SERVER_IP:-127.0.0.1}"
PORT="${PORT:-8080}"
TOPIC="${TOPIC:-cane}"
OUT_DIR="${OUT_DIR:-/tmp/client_bench_output}"

# Auto-detect whether dataset topics are in ../Dataset/raw-img or ../Dataset
if [ -d "../Dataset/raw-img" ]; then
    TOPIC_DIR="${TOPIC_DIR:-../Dataset/raw-img}"
else
    TOPIC_DIR="${TOPIC_DIR:-../Dataset}"
fi

# Isolated benchmark dataset directory: symlink ONLY the target topic
# This avoids scanning 25,000 files across 10 unused topics on every single server restart!
BENCH_DATA_DIR="/tmp/cn_bench_dataset_$$"
cleanup() {
    kill $(jobs -p) 2>/dev/null
    rm -rf "$BENCH_DATA_DIR" "$OUT_DIR"
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

mkdir -p "$BENCH_DATA_DIR"
if [ -d "$TOPIC_DIR/$TOPIC" ]; then
    ln -sfn "$(realpath "$TOPIC_DIR/$TOPIC")" "$BENCH_DATA_DIR/$TOPIC"
    ACTIVE_TOPIC_DIR="$BENCH_DATA_DIR"
else
    ACTIVE_TOPIC_DIR="$TOPIC_DIR"
fi

# Function to poll kernel socket table until server socket is actually in LISTEN state
wait_for_server() {
    local pid="$1"
    local port="$2"
    local timeout="${3:-120}"
    local count=0
    while ! ss -tln | grep -E -q ":${port}\b"; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "[ERROR] Server process ($pid) terminated unexpectedly during startup!"
            return 1
        fi
        sleep 0.2
        count=$((count + 1))
        if [ $count -ge $((timeout * 5)) ]; then
            echo "[ERROR] Timed out waiting for server port $port after ${timeout}s!"
            return 1
        fi
    done
    return 0
}

# Ensure any dangling process on $PORT is killed before starting
fuser -k -n tcp "$PORT" 2>/dev/null

echo "Starting Automated Benchmarks with TOPIC_DIR=$ACTIVE_TOPIC_DIR and TOPIC=$TOPIC..."

# ==============================================================================
# Part 1: Concurrency Scaling (N = 1, 2, 4, 8, 16, 32)
# ==============================================================================
echo "--- Running Part 1 (Single-Threaded vs Multi-Threaded) ---"
export FAULT_PROB=0.0
export CSV_FILE="../Results/Part1_metrics.csv"
rm -f "$CSV_FILE" # Clear old benchmark metrics

CLIENT_SCALING="${CLIENT_SCALING:-1 2 4 8 16 32}"

for N in $CLIENT_SCALING; do
    for MODE in "--single-threaded" ""; do
        export NUM_CLIENTS=$N
        export CASE_NAME=$([[ "$MODE" == "--single-threaded" ]] && echo "Part1_Single" || echo "Part1_Multi")
        
        echo "Testing $CASE_NAME | N=$N | Mode: ${MODE:-[multi-threaded]}"
        ../Part1/server $PORT "$ACTIVE_TOPIC_DIR" $MODE &
        SERVER_PID=$!
        
        if ! wait_for_server "$SERVER_PID" "$PORT" 120; then
            kill -9 $SERVER_PID 2>/dev/null
            continue
        fi
        
        CLIENT_PIDS=""
        for ((i=1; i<=N; i++)); do
            CLIENT_DEST="${OUT_DIR}/c${i}"
            mkdir -p "$CLIENT_DEST"
            ../Part1/client $SERVER_IP $PORT $TOPIC "$CLIENT_DEST" &
            CLIENT_PIDS="$CLIENT_PIDS $!"
        done
        
        # Wait strictly for client processes to finish
        wait $CLIENT_PIDS
        
        # Terminate background server cleanly
        kill -15 $SERVER_PID 2>/dev/null || kill -9 $SERVER_PID 2>/dev/null
        wait $SERVER_PID 2>/dev/null
        
        # Wait until port is fully released
        while ss -tln | grep -E -q ":${PORT}\b"; do
            sleep 0.1
        done
        sleep 0.5
    done
done

# ==============================================================================
# Part 2: Fault Tolerance Comparison (Pf = 0.0, 0.1, 0.3, 0.5) at N=8
# ==============================================================================
echo "--- Running Part 2 (Fault Tolerance Scaling) ---"
export NUM_CLIENTS=8
export CSV_FILE="../Results/Part2_metrics.csv"
rm -f "$CSV_FILE" # Clear old benchmark metrics

PF_SCALING="${PF_SCALING:-0.0 0.1 0.3 0.5}"
CASES="${CASES:-Part2_Case1 Part2_Case2 Part2_Case2_Enhanced}"

for PF in $PF_SCALING; do
    export FAULT_PROB=$PF
    for CASE in $CASES; do
        export CASE_NAME=$CASE
        echo "Testing $CASE_NAME | Pf=$PF | N=8"
        
        ../$CASE/server $PORT "$ACTIVE_TOPIC_DIR" $PF &
        SERVER_PID=$!
        
        if ! wait_for_server "$SERVER_PID" "$PORT" 120; then
            kill -9 $SERVER_PID 2>/dev/null
            continue
        fi
        
        CLIENT_PIDS=""
        for ((i=1; i<=8; i++)); do
            CLIENT_DEST="${OUT_DIR}/c${i}"
            mkdir -p "$CLIENT_DEST"
            ../$CASE/client $SERVER_IP $PORT $TOPIC "$CLIENT_DEST" &
            CLIENT_PIDS="$CLIENT_PIDS $!"
        done
        
        wait $CLIENT_PIDS
        
        kill -15 $SERVER_PID 2>/dev/null || kill -9 $SERVER_PID 2>/dev/null
        wait $SERVER_PID 2>/dev/null
        
        while ss -tln | grep -E -q ":${PORT}\b"; do
            sleep 0.1
        done
        sleep 0.5
    done
done

# Clean up temporary staging
rm -rf "$BENCH_DATA_DIR" "$OUT_DIR"

echo "=============================================================================="
echo "Benchmarks complete!"
if command -v python3 >/dev/null 2>&1 && python3 -c "import pandas, matplotlib" >/dev/null 2>&1; then
    echo "Generating performance graphs using python3..."
    python3 plot_results.py
elif command -v python.exe >/dev/null 2>&1; then
    echo "Generating performance graphs using python.exe..."
    python.exe plot_results.py
else
    echo "To generate charts, run 'python plot_results.py' in an environment with pandas and matplotlib."
fi
echo "=============================================================================="
