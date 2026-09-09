#!/bin/bash
# usage: run_server.sh [port] [runs] [warmup] [outdir]
PORT=${1:-8080}; RUNS=${2:-1000}; WARM=${3:-100}; OUT=${4:-results/vmware/server}
CORE=${CORE:-1}
mkdir -p "$OUT"
TS=$(date +%Y%m%d_%H%M%S)
echo "[SERVER] port=$PORT runs=$RUNS warmup=$WARM core=$CORE -> $OUT/run_$TS.csv"
taskset -c "$CORE" ./apake_bench server "$PORT" "$RUNS" "$WARM" \
    > "$OUT/run_$TS.csv" 2> "$OUT/log_$TS.txt"
echo "[SERVER] done."
