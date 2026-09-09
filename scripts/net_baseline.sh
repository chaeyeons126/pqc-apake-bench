#!/bin/bash
# 순수 네트워크 기준 RTT 측정 (교차검증용). usage: net_baseline.sh <server_ip> [count] [outdir]
IP=${1:?server_ip 필요}; N=${2:-100}; OUT=${3:-results}
mkdir -p "$OUT"
echo "[*] ping $IP x$N"
ping -c "$N" "$IP" | tee "$OUT/ping_${IP}_$(date +%Y%m%d_%H%M%S).txt"
