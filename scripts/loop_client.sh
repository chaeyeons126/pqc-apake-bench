#!/bin/bash
# usage: loop_client.sh <server_ip> [trials] [port] [runs] [warmup] [outdir]
IP=${1:?server_ip 필요}; T=${2:-30}; PORT=${3:-8080}; RUNS=${4:-1000}; WARM=${5:-100}; OUT=${6:-results/vmware/client}
CORE=${CORE:-1}
mkdir -p "$OUT"
for t in $(seq 1 "$T"); do
  echo "[CLIENT] trial $t/$T"
  taskset -c "$CORE" ./apake_bench client "$IP" "$PORT" "$RUNS" "$WARM" \
      > "$OUT/trial_$(printf '%02d' "$t").csv" 2> "$OUT/trial_$(printf '%02d' "$t").log"
  sleep 1
done
echo "[CLIENT] all trials done -> $OUT"
