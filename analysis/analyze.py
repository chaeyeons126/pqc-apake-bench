#!/usr/bin/env python3
"""
analyze.py — apake_bench CSV 통계 분석기

사용법:
    python3 analysis/analyze.py <client_csv...> [--server <server_csv...>]

예:
    python3 analysis/analyze.py results/vmware/client/trial_*.csv \
            --server results/vmware/server/trial_*.csv

- '#' 또는 숫자로 시작하지 않는 줄(상태 메시지/헤더)은 자동 무시.
- 클라이언트 CSV: run,client_round1_ns,net_rtt_ns,client_finish_ns,total_login_ns (5열)
- 서버 CSV:       run,server_round2_ns (2열)
- ns -> us 로 변환해 mean/median/std/min/p95/p99/max 출력.
- 서버 CSV 가 있으면 net_rtt 에서 server_round2 를 빼 '순수 네트워크 RTT'도 산출.
"""
import sys, argparse, glob, statistics as st

def load(paths, ncols):
    rows = []
    for pat in paths:
        for path in glob.glob(pat):
            with open(path, errors="ignore") as f:
                for line in f:
                    line = line.strip()
                    if not line or not (line[0].isdigit()):
                        continue
                    parts = line.split(",")
                    if len(parts) != ncols:
                        continue
                    try:
                        rows.append([int(x) for x in parts])
                    except ValueError:
                        continue
    return rows

def summarize(vals, name):
    if not vals:
        print(f"  {name:18s}  (데이터 없음)")
        return
    v = sorted(vals); n = len(v)
    us = lambda x: x / 1000.0
    pct = lambda p: v[min(n - 1, int(p * n))]
    print(f"  {name:18s} n={n:6d}  "
          f"mean={us(st.mean(v)):9.2f}  median={us(st.median(v)):9.2f}  "
          f"std={us(st.pstdev(v)):8.2f}  min={us(v[0]):8.2f}  "
          f"p95={us(pct(0.95)):9.2f}  p99={us(pct(0.99)):9.2f}  "
          f"max={us(v[-1]):9.2f}   [us]")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("client", nargs="+", help="client CSV (glob 허용)")
    ap.add_argument("--server", nargs="*", default=[], help="server CSV (glob 허용)")
    args = ap.parse_args()

    cli = load(args.client, 5)
    print("=" * 78)
    print(f"CLIENT  ({len(cli)} rows)   단위: microseconds(us)")
    print("=" * 78)
    summarize([r[1] for r in cli], "client_round1")
    summarize([r[2] for r in cli], "net_rtt(측정)")
    summarize([r[3] for r in cli], "client_finish")
    summarize([r[4] for r in cli], "total_login")

    if args.server:
        srv = load(args.server, 2)
        print("-" * 78)
        print(f"SERVER  ({len(srv)} rows)")
        print("-" * 78)
        summarize([r[1] for r in srv], "server_round2")

        # 순수 네트워크 = net_rtt - server_round2 (같은 run 번호끼리)
        # 여러 trial 을 합친 경우 run 번호가 중복되므로, 각 파일 단위 정렬이 이상적이지만
        # 여기서는 근사로 '평균 server_round2' 를 빼는 방식과, run 매칭 방식 둘 다 제공.
        by_run = {}
        for r in srv:
            by_run.setdefault(r[0], []).append(r[1])
        srv_mean_by_run = {k: st.mean(v) for k, v in by_run.items()}
        pure = []
        for r in cli:
            if r[0] in srv_mean_by_run:
                d = r[2] - srv_mean_by_run[r[0]]
                if d > 0:
                    pure.append(d)
        print("-" * 78)
        print("유도값: 순수 네트워크 RTT = net_rtt - server_round2")
        print("-" * 78)
        summarize(pure, "pure_network_rtt")
        if [r[1] for r in srv]:
            avg_s2 = st.mean([r[1] for r in srv])
            avg_rtt = st.mean([r[2] for r in cli]) if cli else 0
            print(f"\n  (참고) 평균 net_rtt={avg_rtt/1000:.2f}us  "
                  f"- 평균 server_round2={avg_s2/1000:.2f}us  "
                  f"= 순수네트워크≈{(avg_rtt-avg_s2)/1000:.2f}us")

if __name__ == "__main__":
    main()
