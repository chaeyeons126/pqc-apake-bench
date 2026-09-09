#!/bin/bash
# 빌드 스크립트. liboqs 가 /usr/local 에 설치되어 있다고 가정.
set -e
CC=${CC:-gcc}
$CC -O2 -Wall -Wextra -Wno-deprecated-declarations \
    -I/usr/local/include -L/usr/local/lib \
    src/apake_bench.c -o apake_bench -loqs -lcrypto -lm
echo "[OK] built ./apake_bench"
