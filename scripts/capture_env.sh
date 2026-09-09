#!/bin/bash
# 실험 환경(하드웨어/OS/라이브러리 버전)을 기록. usage: capture_env.sh <label>
LABEL=${1:-unknown}
OUT="env/env_${LABEL}_$(date +%Y%m%d_%H%M%S).txt"
mkdir -p env
{
  echo "==== ENV: $LABEL  ($(date -Is)) ===="
  echo "-- host --"; hostname; uname -a
  echo "-- cpu --"; (lscpu 2>/dev/null || cat /proc/cpuinfo | head -30)
  echo "-- mem --"; free -h 2>/dev/null || true
  echo "-- governor --"; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a
  echo "-- os --"; (cat /etc/os-release 2>/dev/null | head -4)
  echo "-- gcc --"; gcc --version | head -1
  echo "-- openssl --"; openssl version 2>/dev/null
  echo "-- liboqs --"; ls -l /usr/local/lib/ 2>/dev/null | grep -i oqs
  echo "-- rpi throttle --"; command -v vcgencmd >/dev/null 2>&1 && { vcgencmd measure_temp; vcgencmd measure_clock arm; vcgencmd get_throttled; } || echo "n/a"
} | tee "$OUT"
echo "[OK] saved $OUT"
