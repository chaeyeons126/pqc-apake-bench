#!/bin/bash
# usage: run_client.sh <server_ip> [port] [runs] [warmup] [outdir]
IP=${1:?server_ip 필요}; PORT=${2:-8080}; RUNS=${3:-1000}; WARM=${4:-100}; OUT=${5:-results/vmware/client}
CORE=${CORE:-1}
mkdir -p "$OUT"
TS=$(date +%Y%m%d_%H%M%S)
echo "[CLIENT] $IP:$PORT runs=$RUNS warmup=$WARM core=$CORE -> $OUT/run_$TS.csv"
taskset -c "$CORE" ./apake_bench client "$IP" "$PORT" "$RUNS" "$WARM" \
    > "$OUT/run_$TS.csv" 2> "$OUT/log_$TS.txt"
echo "[CLIENT] done."
