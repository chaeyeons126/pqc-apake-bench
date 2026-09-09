#!/bin/bash
# 측정 전 시스템을 '성능 고정' 상태로. (서버·클라이언트 양쪽에서 실행)
echo "[*] CPU governor -> performance"
if command -v cpupower >/dev/null 2>&1; then
  sudo cpupower frequency-set -g performance || true
else
  echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null || true
fi
echo "[*] 현재 governor:"; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "  (확인 불가)"
echo "[*] 라즈베리파이라면 아래로 스로틀 확인 (0x0 이어야 정상):"
command -v vcgencmd >/dev/null 2>&1 && { vcgencmd measure_temp; vcgencmd get_throttled; } || echo "  (vcgencmd 없음 = Pi 아님)"
