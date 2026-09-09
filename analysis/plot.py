#!/usr/bin/env python3
"""
plot.py — 연산시간 분포 박스플롯 + VMware vs 라즈베리파이 비교 막대그래프

의존성: pip install matplotlib
사용법:
    python3 analysis/plot.py \
        --vm-client   results/vmware/client/trial_*.csv \
        --pi-client   results/raspberrypi/client/trial_*.csv \
        --outdir      analysis/figures
"""
import argparse, glob, os, statistics as st

def load_client(paths):
    cols = {"round1": [], "net_rtt": [], "finish": [], "total": []}
    for pat in paths:
        for path in glob.glob(pat):
            with open(path, errors="ignore") as f:
                for line in f:
                    line = line.strip()
                    if not line or not line[0].isdigit():
                        continue
                    p = line.split(",")
                    if len(p) != 5:
                        continue
                    try:
                        _, r1, rtt, fin, tot = (int(x) for x in p)
                    except ValueError:
                        continue
                    cols["round1"].append(r1 / 1000.0)
                    cols["net_rtt"].append(rtt / 1000.0)
                    cols["finish"].append(fin / 1000.0)
                    cols["total"].append(tot / 1000.0)
    return cols

def main():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    ap = argparse.ArgumentParser()
    ap.add_argument("--vm-client", nargs="*", default=[])
    ap.add_argument("--pi-client", nargs="*", default=[])
    ap.add_argument("--outdir", default="analysis/figures")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    vm = load_client(args.vm_client) if args.vm_client else None
    pi = load_client(args.pi_client) if args.pi_client else None

    # 1) 박스플롯 (VMware 클라이언트 연산 분포)
    if vm:
        plt.figure(figsize=(7, 4))
        plt.boxplot([vm["round1"], vm["finish"], vm["total"]],
                    labels=["Round1", "Finish", "Total"], showfliers=False)
        plt.ylabel("time (us)"); plt.title("VMware client timing distribution")
        plt.grid(True, axis="y", alpha=0.3); plt.tight_layout()
        plt.savefig(f"{args.outdir}/vm_box.png", dpi=150); plt.close()

    # 2) VMware vs Pi 평균 비교 (연산 항목)
    if vm and pi:
        labels = ["round1", "finish", "total"]
        vm_m = [st.mean(vm[k]) for k in labels]
        pi_m = [st.mean(pi[k]) for k in labels]
        x = range(len(labels)); w = 0.35
        plt.figure(figsize=(7, 4))
        plt.bar([i - w/2 for i in x], vm_m, w, label="VMware(PC)")
        plt.bar([i + w/2 for i in x], pi_m, w, label="Raspberry Pi")
        plt.xticks(list(x), labels); plt.ylabel("mean time (us)")
        plt.title("VMware vs Raspberry Pi (client compute)")
        plt.legend(); plt.grid(True, axis="y", alpha=0.3); plt.tight_layout()
        plt.savefig(f"{args.outdir}/vm_vs_pi.png", dpi=150); plt.close()
        for k, a, b in zip(labels, vm_m, pi_m):
            print(f"{k:8s}  VMware={a:9.2f}us  Pi={b:9.2f}us  (Pi/VM x{b/a:.2f})")

    print(f"[OK] figures -> {args.outdir}")

if __name__ == "__main__":
    main()
