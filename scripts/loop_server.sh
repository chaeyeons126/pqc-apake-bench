#!/bin/bash
# 여러 trial 반복. usage: loop_server.sh [trials] [port] [runs] [warmup] [outdir]
T=${1:-30}; PORT=${2:-8080}; RUNS=${3:-1000}; WARM=${4:-100}; OUT=${5:-results/vmware/server}
CORE=${CORE:-1}
mkdir -p "$OUT"
for t in $(seq 1 "$T"); do
  echo "[SERVER] trial $t/$T"
  taskset -c "$CORE" ./apake_bench server "$PORT" "$RUNS" "$WARM" \
      > "$OUT/trial_$(printf '%02d' "$t").csv" 2> "$OUT/trial_$(printf '%02d' "$t").log"
done
echo "[SERVER] all trials done -> $OUT"
