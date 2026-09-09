# PQC aPAKE Benchmark

2-Round Post-Quantum aPAKE 프로토콜을 구현하고 실제 Client–Server 네트워크 환경에서 성능을 측정하기 위한 벤치마크 프로젝트입니다.

현재 VMware Ubuntu VM 2대를 이용한 **baseline TCP 실험까지 완료**하였습니다.

다음 단계에서는 3명이 각각 다음 실험을 진행합니다.

1. **Network Latency Experiment** — 네트워크 지연 변화
2. **Bandwidth Experiment** — 대역폭 변화
3. **Raspberry Pi Experiment** — 실제 독립 장치 환경

---

# 1. Project Status

## 완료

- [x] 2-Round PQC aPAKE C 구현
- [x] ML-KEM-768 연동
- [x] IC1 / IC2 벤치마크 구현
- [x] AE 암호화 구현
- [x] TCP Client–Server 통신 구현
- [x] VMware Ubuntu Client/Server 구성
- [x] VM 간 실제 TCP/IP 통신 확인
- [x] Client/Server session key 일치 확인
- [x] Network baseline RTT 측정
- [x] 30 trials × 1,000 measured logins
- [x] Client 30,000 / Server 30,000개 결과 분석

## 다음 실험

- [ ] VMware latency 10 / 30 / 50 ms
- [ ] VMware bandwidth 제한 실험
- [ ] Raspberry Pi 실제 장치 실험
- [ ] 환경별 결과 비교
- [ ] latency + bandwidth 조합 실험

---

# 2. Cryptographic Implementation

현재 `src/apake_bench.c` 기준 구현은 다음과 같습니다.

| 기능 | 현재 구현 |
|---|---|
| KEM | **ML-KEM-768** |
| KEM Library | **Open Quantum Safe liboqs** |
| IC1 / IC2 | **AES-256-CTR 기반 benchmark instantiation** |
| AE | **AES-256-GCM** |
| Hash / KDF | SHA-256 기반 |
| TCP | Linux Socket |
| Timing | `CLOCK_MONOTONIC_RAW` |


## Cryptographic Libraries

암호 알고리즘을 직접 구현하지 않고 오픈소스 라이브러리를 사용한다.

- ML-KEM-768: Open Quantum Safe `liboqs`
- IC1 / IC2 benchmark instantiation: OpenSSL `AES-256-CTR`
- Authenticated Encryption: OpenSSL `AES-256-GCM`
- Hash / KDF: OpenSSL SHA-256 based functions
---

## 2.1 ML-KEM

KEM은 `liboqs`의 **ML-KEM-768** 구현을 사용합니다.

프로토콜에서 다음 세 연산이 사용됩니다.

```text
KEM.KeyGen
KEM.Encap
KEM.Decap
```

ML-KEM-768 파라미터:

```text
Public Key  = 1184 bytes
Ciphertext  = 1088 bytes
Secret Key  = 2400 bytes
Shared Key  = 32 bytes
```

---

## 2.2 IC1 / IC2

프로토콜 명세에서는 다음과 같이 Ideal Cipher를 사용합니다.

```text
C1 = IC1.Enc_hp(p̂k)

C2 = IC2.Enc_hp(ĉ)
```

현재 코드에서는 Ideal Cipher를 직접 구현한 것이 아니라,
**벤치마크용 길이 보존 가역 변환으로 AES-256-CTR을 사용합니다.**

실제 코드:

```c
EVP_EncryptInit_ex(
    ctx,
    EVP_aes_256_ctr(),
    NULL,
    key,
    iv
);
```

따라서:

```text
IC1 / IC2
    ↓
현재 benchmark implementation
    ↓
AES-256-CTR
```

입니다.

### 중요

AES-CTR 자체를 논문에서 정의한 Ideal Cipher와 동일하다고 해석하면 안 됩니다.

현재 구현 목적은 다음과 같습니다.

```text
1. 입력과 출력 길이를 동일하게 유지
2. Enc / Dec가 가역적으로 동작
3. IC 연산에 필요한 실제 CPU 비용을 benchmark에 포함
```

따라서 향후 논문 또는 보고서에서는 다음과 같이 표현합니다.

> The ideal-cipher operations IC1 and IC2 are instantiated using a
> length-preserving AES-256-CTR based transformation for benchmarking.

---

## 2.3 AE

프로토콜의 Authenticated Encryption은 **AES-256-GCM**을 사용합니다.

코드:

```c
EVP_aes_256_gcm()
```

다음 두 위치에서 사용됩니다.

### Registration

```text
τ = AE.Enc_π(sk)
```

즉:

```text
AES-256-GCM
key = π = H1(password)
plaintext = ML-KEM secret key
```

입니다.

### Login

```text
ψ = AE.Enc_hk(c || τ)
```

즉:

```text
AES-256-GCM
key = hk = H2(K0)
plaintext = c || τ
```

입니다.

AES-GCM 파라미터:

```text
Key   = 32 bytes
Nonce = 12 bytes
Tag   = 16 bytes
```

---

# 3. Protocol Overview

전체 온라인 로그인 구조는 다음과 같습니다.

```text
CLIENT                                     SERVER

        Client Round 1

        ML-KEM KeyGen
        hp = H0(password)

        C1 = IC1.Enc_hp(p̂k)
                 │
                 │
                 │ C1
                 ├────────────────────────►
                 │
                 │                         IC1.Dec
                 │
                 │                         ML-KEM Encap
                 │
                 │                         K0 생성
                 │
                 │                         C2 생성
                 │
                 │                         ML-KEM Encap
                 │
                 │                         ψ 생성
                 │
                 │                         server ssk
                 │
                 │        C2, ψ
                 ◄─────────────────────────┤
                 │
        Client Finish
                 │
        IC2.Dec
        ML-KEM Decap
        ψ AES-GCM Dec
        τ AES-GCM Dec
        ML-KEM Decap
        client ssk
```

Client와 Server가 마지막으로 생성한 `ssk`가 동일한지도
benchmark에서 검증합니다.

---

# 4. Protocol Message Size

현재 코드 기준:

```text
C1 = 1184 bytes
C2 = 1088 bytes
τ  = 2428 bytes
ψ  = 3544 bytes
```

온라인 로그인에서 실제 application payload는:

```text
Client → Server

C1
= 1184 bytes
```

```text
Server → Client

C2 + ψ
= 1088 + 3544
= 4632 bytes
```

따라서 총:

```text
1184 + 4632
= 5816 bytes
```

즉 약 **5.8 KB**입니다.

주의:

이는 application payload 기준이며 TCP/IP/Ethernet header는 포함하지 않습니다.

---

# 5. Project Structure

```text
pqc-apake-bench/
│
├── README.md
├── build.sh
│
├── src/
│   └── apake_bench.c
│
├── scripts/
│   ├── run_server.sh
│   ├── run_client.sh
│   ├── loop_server.sh
│   ├── loop_client.sh
│   ├── net_baseline.sh
│   ├── set_perf.sh
│   └── capture_env.sh
│
├── analysis/
│   ├── analyze.py
│   └── plot.py
│
├── results/
│   ├── vmware/
│   │   ├── client/
│   │   └── server/
│   │
│   └── raspberrypi/
│       ├── client/
│       └── server/
│
├── docs/
└── env/
```

---

## 주요 파일

| 파일 | 역할 |
|---|---|
| `src/apake_bench.c` | aPAKE 및 TCP Client/Server 구현 |
| `build.sh` | benchmark build |
| `run_server.sh` | 서버 단일 실험 |
| `run_client.sh` | 클라이언트 단일 실험 |
| `loop_server.sh` | 서버 반복 실험 |
| `loop_client.sh` | 클라이언트 반복 실험 |
| `net_baseline.sh` | ping 기반 network baseline |
| `set_perf.sh` | CPU performance 관련 설정 |
| `capture_env.sh` | 실험 환경 기록 |
| `analyze.py` | 통계 분석 |
| `plot.py` | 그래프 생성 |

---

# 6. VMware Baseline Environment

현재 완료된 baseline 실험 환경입니다.

```text
                 Physical Laptop
                       │
                    VMware
                       │
             ┌─────────┴─────────┐
             │                   │

         Client VM            Server VM
       Ubuntu Linux          Ubuntu Linux

     192.168.237.129       192.168.237.128

             │                   │
             └───── TCP/IP ──────┘
```

현재 사용했던 IP:

```text
Server = 192.168.237.128
Client = 192.168.237.129
```

주의:

이 IP는 현재 실험 환경에서 DHCP로 할당된 값입니다.

새로운 환경에서는 반드시:

```bash
hostname -I
```

로 확인하십시오.

---

# 7. VMware Network Verification

IP:

```bash
hostname -I
```

Network Interface / MAC:

```bash
ip link
```

Client → Server:

```bash
ping 192.168.237.128
```

Server → Client:

```bash
ping 192.168.237.129
```

현재 baseline에서는 양방향 모두:

```text
packet loss = 0%
```

를 확인했습니다.

---

# 8. Network Baseline Result

100회 ping 결과:

```text
RTT min/avg/max/mdev

0.258 / 0.502 / 0.850 / 0.115 ms

Packet Loss = 0%
```

따라서 현재 VMware Host-only 환경의 baseline RTT 평균은:

```text
0.502 ms
```

입니다.

이 값은 ICMP ping 결과이고,
aPAKE TCP 실행시간과 동일한 것은 아닙니다.

---

# 9. Environment Setup

Client와 Server 양쪽에서:

```bash
sudo apt update

sudo apt install -y \
build-essential \
git \
cmake \
ninja-build \
libssl-dev
```

확인:

```bash
gcc --version
cmake --version
ninja --version
```

---

# 10. Install liboqs

Client와 Server 양쪽에서:

```bash
cd ~

git clone https://github.com/open-quantum-safe/liboqs.git

cd liboqs

mkdir build

cd build
```

Configure:

```bash
cmake -GNinja \
-DBUILD_SHARED_LIBS=ON \
-DCMAKE_INSTALL_PREFIX=/usr/local \
..
```

Build:

```bash
ninja
```

Install:

```bash
sudo ninja install
sudo ldconfig
```

확인:

```bash
ls /usr/local/include/oqs/oqs.h
```

정상:

```text
/usr/local/include/oqs/oqs.h
```

---

# 11. Build Benchmark

```bash
cd ~/Desktop/pqc-apake-bench/pqc-apake-bench
```

```bash
chmod +x build.sh
chmod +x scripts/*.sh
```

```bash
./build.sh
```

성공 시:

```text
[OK] built ./apake_bench
```

---

# 12. TCP Implementation

하나의 `apake_bench` 실행파일이 두 역할을 모두 지원합니다.

## Server

```text
socket()
bind()
listen()
accept()
read()
write()
```

## Client

```text
socket()
connect()
read()
write()
```

따라서 한 프로그램 내부에서 Client와 Server 함수를 순차 실행하는 것이 아니라,

**서로 다른 VM에서 실제 TCP socket을 이용하여 프로토콜 메시지를 전송합니다.**

---

# 13. Smoke Test

본 실험 전에 반드시 작은 테스트부터 수행합니다.

## Server

먼저 실행:

```bash
./apake_bench server 8080 3 1
```

## Client

```bash
./apake_bench client 192.168.237.128 8080 3 1
```

의미:

```text
8080 = TCP port
3    = measured runs
1    = warm-up
```

확인할 사항:

```text
[ ] Client가 Server에 접속되는가?
[ ] C1이 Server에 전달되는가?
[ ] C2와 ψ가 Client로 전달되는가?
[ ] Client Finish가 성공하는가?
[ ] Client ssk == Server ssk 인가?
[ ] 프로그램이 정상 종료하는가?
```

현재 VMware 환경에서는 위 항목을 모두 확인했습니다.

---

# 14. Performance Measurement

## Server

```bash
./scripts/run_server.sh \
8080 1000 100 results/vmware/server
```

## Client

```bash
./scripts/run_client.sh \
192.168.237.128 \
8080 \
1000 \
100 \
results/vmware/client
```

의미:

```text
1000 = measured runs
100  = warm-up runs
```

---

# 15. Repeated Experiment

현재 baseline은 30 trial을 수행했습니다.

## Server

```bash
./scripts/loop_server.sh \
30 8080 1000 100 results/vmware/server
```

## Client

```bash
./scripts/loop_client.sh \
192.168.237.128 \
30 \
8080 \
1000 \
100 \
results/vmware/client
```

한 trial:

```text
100 warm-up
+
1000 measured
=
1100 protocol executions
```

30 trial:

```text
33,000 actual executions
```

이 중:

```text
30 × 1000
=
30,000 measurements
```

를 통계 분석에 사용합니다.

---

# 16. TCP Connection Scope

중요:

**로그인 1회마다 TCP connection을 새로 생성하지 않습니다.**

한 trial:

```text
TCP Connect
     │
     ▼
Registration 1회
     │
     ▼
Warm-up 100회
     │
     ▼
Measured Login 1000회
     │
     ▼
TCP Close
```

즉 `total_login`에는 TCP 3-way handshake 시간이 포함되지 않습니다.

---

# 17. Timing

코드에서는:

```c
clock_gettime(CLOCK_MONOTONIC_RAW, ...)
```

를 사용합니다.

Server와 Client의 clock 값을 서로 빼지 않습니다.

각 VM 내부에서만:

```text
end_timestamp - start_timestamp
```

를 계산합니다.

따라서 VM 사이의 clock synchronization에 의존하지 않습니다.

---

# 18. Measured Metrics

## Client

```text
client_round1
net_rtt
client_finish
total_login
```

## Server

```text
server_round2
```

---

## client_round1

```text
Client Round1 시작
       ↓
ML-KEM KeyGen
password 관련 처리
IC1
C1 생성
       ↓
Client Round1 종료
```

네트워크는 포함하지 않습니다.

---

## server_round2

```text
C1 수신 완료
       ↓
Server Round2 시작
       ↓
IC1 처리
ML-KEM Encap
K0
IC2
ML-KEM Encap
AES-GCM ψ 생성
Server session key
       ↓
Server Round2 종료
```

네트워크는 포함하지 않습니다.

---

## client_finish

```text
C2 + ψ 수신 완료
       ↓
IC2
ML-KEM Decap
AES-GCM ψ 복호
AES-GCM τ 복호
ML-KEM Decap
Client session key
       ↓
Finish
```

---

## net_rtt

주의:

`net_rtt`는 **순수 네트워크 RTT가 아닙니다.**

측정 구간:

```text
Client

C1 전송 시작
      │
      ▼
TCP / VMware Network
      │
      ▼
Server

C1 수신
Server Round2 수행
C2 + ψ 송신
      │
      ▼
TCP / VMware Network
      │
      ▼
Client

C2 + ψ 전체 수신 완료
```

따라서:

```text
net_rtt
=
TCP 왕복
+
Server Round2
+
socket 처리
+
Linux OS 처리
+
VM scheduling
+
VMware virtual networking
```

입니다.

더 정확한 명칭은:

```text
Response Round-Trip Interval
Including Server Processing
```

입니다.

---

## total_login

온라인 로그인 전체 latency입니다.

```text
total_login
≈
client_round1
+
net_rtt
+
client_finish
```

따라서 이 프로젝트에서 **가장 중요한 end-to-end 성능 지표**입니다.

---

# 19. VMware Baseline Results

최종 데이터:

```text
Client = 30,000 measurements
Server = 30,000 measurements
```

| Metric | Mean | Median | P95 |
|---|---:|---:|---:|
| Client Round1 | 0.014 ms | 0.014 ms | 0.014 ms |
| Server Round2 | 0.031 ms | 0.030 ms | 0.039 ms |
| Client Finish | 0.030 ms | 0.029 ms | 0.040 ms |
| net_rtt | 1.459 ms | 0.708 ms | 4.716 ms |
| **Total Login** | **1.503 ms** | **0.752 ms** | **4.763 ms** |

Total Login:

```text
mean   = 1.503 ms
median = 0.752 ms
p95    = 4.763 ms
p99    = 6.211 ms
max    = 17.110 ms
```

Network baseline:

```text
Ping RTT average = 0.502 ms
Packet loss = 0%
```

---

# 20. Result Interpretation

현재 baseline에서 암호 연산은 대부분 수십 µs 수준입니다.

```text
Client Round1 ≈ 14 µs
Server Round2 ≈ 31 µs
Client Finish ≈ 30 µs
```

반면:

```text
net_rtt

median ≈ 0.708 ms
p95    ≈ 4.716 ms
```

로 변동폭이 상대적으로 큽니다.

따라서 전체 latency variance의 상당 부분은 암호 함수 자체보다 다음 영역에서 발생하는 것으로 관찰됩니다.

```text
TCP
Linux socket
OS scheduling
VM scheduling
VMware virtual network
message transmission
```

이를 정확하게 분석하기 위해 다음 단계에서 latency와 bandwidth를 통제합니다.

---

# 21. Result Analysis

Server 결과를 Client 측으로 복사합니다.

```bash
scp 'rigyo@192.168.237.128:/home/rigyo/Desktop/pqc-apake-bench/pqc-apake-bench/results/vmware/server/trial_*.csv' \
results/vmware/server/
```

확인:

```bash
ls results/vmware/client/trial_*.csv | wc -l

ls results/vmware/server/trial_*.csv | wc -l
```

둘 다:

```text
30
```

이어야 합니다.

분석:

```bash
python3 analysis/analyze.py \
results/vmware/client/trial_*.csv \
--server results/vmware/server/trial_*.csv
```

정상:

```text
CLIENT (30000 rows)
SERVER (30000 rows)
```

---

# 22. Work Assignment

앞으로 3명이 서로 다른 실험을 진행합니다.

공통 원칙:

> 먼저 현재 VMware baseline을 재현하고 각자 맡은 변수를 하나만 변경합니다.

---

## Person A — Network Latency Experiment

### 목적

네트워크 RTT 증가가 2-Round aPAKE 로그인 latency에 미치는 영향을 측정합니다.

### 조건

예:

```text
Baseline
RTT 10 ms
RTT 30 ms
RTT 50 ms
```

Linux `tc` / `netem`을 사용합니다.

### 중요

`netem delay`의 값이 one-way delay인지 최종 RTT 조건인지 반드시 구분합니다.

예를 들어 양방향 각각 5 ms를 추가하면 약 10 ms RTT 증가가 발생합니다.

각 조건은 실제 `ping`으로 확인합니다.

### 각 조건

```text
30 trials
100 warm-up / trial
1000 measurements / trial
```

### 비교

```text
Ping RTT
net_rtt
total_login
median
mean
p95
p99
```

### 권장 결과 폴더

```text
results/vmware/latency/
├── baseline/
├── rtt_10ms/
├── rtt_30ms/
└── rtt_50ms/
```

---

## Person B — Bandwidth Experiment

### 목적

대역폭 제한이 5.8 KB 수준의 aPAKE message exchange에 어떤 영향을 미치는지 측정합니다.

### 예시 조건

```text
1 Gbps 또는 unrestricted baseline
100 Mbps
50 Mbps
10 Mbps
```

Linux traffic control을 사용하여 bandwidth를 제한합니다.

### 중요

latency는 가능한 동일하게 유지합니다.

즉:

```text
Bandwidth만 변경
RTT는 동일
```

하게 만들어야 bandwidth의 영향만 볼 수 있습니다.

각 조건에서 별도로 `ping` 및 bandwidth 상태를 기록합니다.

### 각 조건

```text
30 trials
100 warm-up
1000 measurements
```

### 비교

```text
total_login
net_rtt
median
p95
p99
```

### 권장 결과 폴더

```text
results/vmware/bandwidth/
├── baseline/
├── 100mbps/
├── 50mbps/
└── 10mbps/
```

---

## Person C — Raspberry Pi Experiment

### 목적

VMware 가상환경이 아니라 실제 독립 장치에서 aPAKE 성능을 측정합니다.

예:

```text
Laptop / Ubuntu Client
           │
           │ Ethernet or Wi-Fi
           │
           ▼
    Raspberry Pi Server
```

### 먼저 확인할 것

```text
Raspberry Pi model
CPU
RAM
OS
architecture
gcc
OpenSSL
liboqs version
network interface
```

환경 저장:

```bash
./scripts/capture_env.sh
```

### 준비

Raspberry Pi에도 동일하게:

```text
gcc
cmake
ninja
OpenSSL
liboqs
```

를 설치하고 동일한 코드를 build합니다.

### 실험

VMware와 동일하게:

```text
30 trials
100 warm-up
1000 measured logins
```

을 수행합니다.

### 비교

```text
Client Round1
Server Round2
Client Finish
Total Login
Network RTT
p95
p99
```

특히 Raspberry Pi에서는 CPU 성능 차이 때문에:

```text
Server Round2
Client Round1
Client Finish
```

같은 **암호 연산시간 변화**가 중요합니다.

### 권장 결과 폴더

```text
results/raspberrypi/
├── client/
├── server/
└── network/
```

---

# 23. Experimental Rule

세 실험 모두 다음 조건을 지켜야 합니다.

1. 현재 baseline 코드를 임의로 변경하지 않는다.
2. 각자 맡은 변수만 변경한다.
3. Server를 먼저 실행한다.
4. 실험 전 `hostname -I`로 IP를 확인한다.
5. 실험 전 ping을 기록한다.
6. 동일 warm-up을 사용한다.
7. 동일 trial 수를 사용한다.
8. 동일 measured run 수를 사용한다.
9. CPU core 설정을 가능한 동일하게 유지한다.
10. Client/Server 환경 정보를 저장한다.
11. raw CSV를 삭제하지 않는다.
12. outlier를 임의로 제거하지 않는다.
13. 오류가 발생한 trial은 별도로 기록하고 재실험한다.
14. Client와 Server 데이터 수가 동일한지 확인한다.
15. 실험 조건을 결과 폴더명에 명확히 기록한다.

---

# 24. Important Warning About `pure_network_rtt`

현재 `analysis/analyze.py`에는:

```text
pure_network_rtt
=
net_rtt - server_round2
```

라는 유도값이 있습니다.

하지만 이것을 실제 순수 network propagation RTT라고 해석하면 안 됩니다.

여기에는 여전히:

```text
TCP stack
socket read/write
Linux scheduling
VM scheduling
VMware virtual NIC
```

등이 포함됩니다.

따라서 보고서에서는 다음 표현을 권장합니다.

```text
Server-processing-excluded response interval
```

또는:

```text
서버 암호 연산을 제외한 통신·시스템 처리 구간
```

실제 network baseline은 별도의 `ping` 결과와 함께 제시하십시오.

---

# 25. Before Starting Your Experiment

프로젝트를 처음 받은 경우 아래 순서로 진행하십시오.

```text
1. README 전체 확인
       ↓
2. src/apake_bench.c 구조 확인
       ↓
3. scripts 확인
       ↓
4. VMware Client / Server 실행
       ↓
5. IP 확인
       ↓
6. Ping 확인
       ↓
7. Build
       ↓
8. 3-run smoke test
       ↓
9. Session key 일치 확인
       ↓
10. Baseline 재현
       ↓
11. 본인 담당 실험 수행
```

Baseline이 정상적으로 재현되지 않는 경우 새로운 실험을 진행하지 말고 원인을 먼저 확인합니다.

---

# 26. Recommended Overall Experiment Structure

최종 프로젝트는 다음 비교를 목표로 합니다.

```text
                    PQC aPAKE
                        │
        ┌───────────────┼───────────────┐
        │               │               │
        ▼               ▼               ▼

   Latency Test     Bandwidth Test    Raspberry Pi

  RTT 변화 영향     BW 변화 영향      실제 장치 영향

        │               │               │
        └───────────────┼───────────────┘
                        ▼

               Comparative Analysis

                 Total Login
                 Crypto Time
                 Network Time
                 Median
                 P95
                 P99
```

---

# 27. Current Baseline Summary

```text
Protocol
--------
KEM        : ML-KEM-768 (liboqs)
IC1 / IC2  : AES-256-CTR benchmark instantiation
AE         : AES-256-GCM
Transport  : TCP

VMware
------
Server : 192.168.237.128
Client : 192.168.237.129

Network
-------
Ping RTT avg : 0.502 ms
Packet loss  : 0%

Experiment
----------
Trials            : 30
Warm-up / trial    : 100
Measured / trial   : 1000
Measurements       : 30,000

Performance
-----------
Client Round1 mean : 0.014 ms
Server Round2 mean : 0.031 ms
Client Finish mean : 0.030 ms

net_rtt mean       : 1.459 ms

Total Login
Mean               : 1.503 ms
Median             : 0.752 ms
P95                : 4.763 ms
P99                : 6.211 ms
```

---

# 28. Next Step

각 담당자는 먼저 현재 VMware baseline 실험을 재현합니다.

그 이후:

```text
담당 A
VMware Network Latency

담당 B
VMware Bandwidth

담당 C
Raspberry Pi
```

실험을 독립적으로 진행합니다.

최종적으로 세 결과를 동일한 측정 기준으로 비교 분석합니다.