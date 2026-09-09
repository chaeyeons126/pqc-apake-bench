# PQC aPAKE 실측 성능 분석 (VMware + Raspberry Pi)

제안 2-Round PQC aPAKE(Fig. 5)의 **실제 네트워크 통신 성능**을 측정하기 위한 실험 저장소.
ML-KEM-768(liboqs) + AES-256-GCM(OpenSSL) 기반. 클라이언트–서버가 TCP로 실제 통신한다.

- 실험 1: VMware 게스트 2대(서버 IP / 클라이언트 IP)로 실측
- 실험 2: 라즈베리파이를 클라이언트로 사용, 서버는 VM(Bridged) 또는 PC

---

## 폴더 구조

```
pqc-apake-bench/
├── README.md                  ← 이 문서 (전체 절차)
├── build.sh                   ← 빌드
├── src/
│   └── apake_bench.c          ← 프로토콜 + 벤치마크 (서버/클라 공용)
├── scripts/
│   ├── set_perf.sh            ← CPU 성능모드 고정 / 발열 확인 (측정 전 필수)
│   ├── capture_env.sh         ← 실험 환경(HW/OS/lib 버전) 기록 → env/
│   ├── net_baseline.sh        ← ping 으로 기준 RTT 측정
│   ├── run_server.sh          ← 서버 1회 실행 (CSV 저장)
│   ├── run_client.sh          ← 클라이언트 1회 실행
│   ├── loop_server.sh         ← 서버 N-trial 반복
│   └── loop_client.sh         ← 클라이언트 N-trial 반복
├── analysis/
│   ├── analyze.py             ← CSV 통계 (mean/median/std/p95/p99, 순수네트워크)
│   └── plot.py                ← 박스플롯 / VMware vs Pi 비교 그래프
├── results/
│   ├── vmware/{server,client}/     ← VMware 실험 CSV·로그
│   └── raspberrypi/{server,client}/← 라즈베리파이 실험 CSV·로그
├── env/                       ← capture_env.sh 가 남기는 환경 기록
└── docs/                      ← (선택) 논문용 표·그림 정리
```

**왜 이 구조가 정확한 실험에 좋은가**
- `src` / `results` / `analysis` 분리 → 코드와 데이터가 섞이지 않는다.
- 환경별(`vmware` / `raspberrypi`) · 역할별(`server` / `client`)로 결과를 나눠, 어느 기기에서 나온 수치인지 혼동이 없다.
- 매 실험마다 `capture_env.sh` 로 CPU·커널·liboqs 버전을 `env/` 에 남겨 **재현성**을 확보한다(논문 심사에서 반드시 요구됨).
- 파일명이 타임스탬프/trial 번호로 남아 여러 번 실험해도 덮어쓰지 않는다.

---

## 0. 준비 (서버·클라이언트 공통, VM·Pi 모두)

```bash
sudo apt update
sudo apt install -y build-essential git cmake ninja-build libssl-dev

# liboqs (ML-KEM 포함) 설치
git clone -b main https://github.com/open-quantum-safe/liboqs.git
cd liboqs && mkdir build && cd build
cmake -GNinja -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX=/usr/local ..
ninja && sudo ninja install && sudo ldconfig && cd ../..

# 빌드
cd pqc-apake-bench
./build.sh          # -> ./apake_bench 생성
./capture_env.sh vmware-server   # (예) 환경 기록. 각 기기에서 라벨만 바꿔 실행
```

---

## 실험 1 — VMware 게스트 2대

**개념**: 컴퓨터 1대에 VMware 설치 → 그 안에 가상 컴퓨터 2대(`server-vm`, `client-vm`)를 띄우고
각각 IP를 부여(예: 서버 192.168.116.10 / 클라 192.168.116.20). 두 VM만의 통신 측정.

1. 네트워크: 두 VM을 같은 **Host-only** 세그먼트에 연결(외부 트래픽 차단 → 재현성↑).
   IP 확인 `ip addr`, 서로 `ping` 확인.
2. 서버 VM 방화벽: `sudo ufw allow 8080/tcp`
3. **측정 전 양쪽에서**: `./scripts/set_perf.sh`
4. 기준 RTT(클라 VM): `./scripts/net_baseline.sh 192.168.116.10 100 results/vmware`
5. 실행 — **서버 먼저**, runs/warmup은 서버 값이 기준:
   ```bash
   # server-vm
   ./scripts/run_server.sh 8080 1000 100 results/vmware/server
   # client-vm
   ./scripts/run_client.sh 192.168.116.10 8080 1000 100 results/vmware/client
   ```
6. 여러 trial(권장 30회):
   ```bash
   # server-vm 먼저
   ./scripts/loop_server.sh 30 8080 1000 100 results/vmware/server
   # client-vm
   ./scripts/loop_client.sh 192.168.116.10 30 8080 1000 100 results/vmware/client
   ```

---

## 실험 2 — 라즈베리파이 클라이언트

**중요**: Pi(외부 기기)가 서버 VM에 접속하려면 그 **서버 VM을 Bridged 모드**로 두어야 물리 LAN에서 보인다.
(Host-only/NAT면 Pi에서 접근 불가.) 또는 서버를 PC에서 직접 실행.

1. Pi: 유선 이더넷 연결, 정품 전원, 방열판+팬 권장.
2. Pi에서 위 "0. 준비"와 동일하게 liboqs 설치 + `./build.sh`.
3. **측정 전**: `./scripts/set_perf.sh` → `vcgencmd get_throttled` 가 `0x0` 인지 확인(과열/전압부족 시 결과 무효).
4. 서버(VM-bridged 또는 PC) 먼저 실행:
   ```bash
   ./scripts/run_server.sh 8080 1000 100 results/raspberrypi/server
   ```
5. 라즈베리파이(클라이언트):
   ```bash
   ./scripts/run_client.sh <서버_IP> 8080 1000 100 results/raspberrypi/client
   # 또는 반복
   ./scripts/loop_client.sh <서버_IP> 30 8080 1000 100 results/raspberrypi/client
   ```

---

## 분석

```bash
# 통계 (여러 trial 한꺼번에 glob)
python3 analysis/analyze.py 'results/vmware/client/trial_*.csv' \
        --server 'results/vmware/server/trial_*.csv'

python3 analysis/analyze.py 'results/raspberrypi/client/trial_*.csv' \
        --server 'results/raspberrypi/server/trial_*.csv'

# 그래프 (matplotlib 필요)
python3 analysis/plot.py \
        --vm-client 'results/vmware/client/trial_*.csv' \
        --pi-client 'results/raspberrypi/client/trial_*.csv' \
        --outdir analysis/figures
```

측정 항목: `client_round1_ns`, `net_rtt_ns`, `client_finish_ns`, `total_login_ns`(클라),
`server_round2_ns`(서버). **순수 네트워크 RTT = net_rtt − server_round2**.

메시지 크기(바이트): C1=1184, C2=1088, τ=2428, ψ=3544, **로그인 1회 총 전송 5816**.

---

## 정확한 측정을 위한 원칙 (요약)

1. 워밍업(코드 내장) + 충분한 반복(1000회 × 30 trial) → mean 뿐 아니라 median·p95·p99 보고.
2. `set_perf.sh` 로 CPU governor=performance, taskset 코어 고정(스크립트 기본 core 1).
3. 연산과 네트워크 분리: `net_rtt − server_round2`, `ping` 기준값과 교차검증.
4. TCP_NODELAY(코드 내장) — 작은 메시지 요청-응답의 Nagle 지연 제거.
5. 매 실험 `capture_env.sh` 로 환경 기록 → 재현성.
6. Pi는 발열/전압 스로틀 상시 확인(`get_throttled==0x0`).

## 이상암호(IC1/IC2)에 대한 주의
Fig. 5의 IC1/IC2(이상암호, hp로 키잉)는 본 코드에서 **길이 보존 AES-256-CTR 키스트림**으로
인스턴스화했다(벤치마크용, 완전 가역·길이 보존). 실제 배포/보안 주장을 위해서는
large-domain PRP(예: Feistel/AEZ 계열)로 교체해야 한다. 연산·전송 비용의 성격은 동일하므로
성능 수치 해석에는 영향이 없다.
