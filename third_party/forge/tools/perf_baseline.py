#!/usr/bin/env python3
"""Performance baselines: run `forge bench` (model prefill/decode + GEMM per lane) and record or check them.
  tools/perf_baseline.py --forge build/forge --model build/golden/tiny_f32.fpk            record perf/baseline-<arch>.json
  tools/perf_baseline.py --forge ... --model ... --check [--tolerance 0.5]                 fail if any metric < (1-tol) x baseline
Throughput metrics only (higher is better). Baselines are per machine class; regenerate when hardware changes."""
import argparse, json, os, platform, re, subprocess, sys

def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout

def measure(forge, model, gemm, reps):
    m = {}
    for _ in range(reps):   # best of N: least sensitive to noise from other load
        out = run([forge, "bench", model, "-p", "32", "-n", "32"])
        for k, v in re.findall(r"^(prefill|decode)\s+\d+ tokens:\s+([\d.]+) tok/s", out, re.M):
            m[f"{k}_tok_s"] = max(m.get(f"{k}_tok_s", 0), float(v))
        out = run([forge, "bench", "--gemm", str(gemm)])
        for k, v in re.findall(r"^(\S+)\s+gemm_nt \d+\^3:\s+([\d.]+) GFLOP/s", out, re.M):
            key = f"gemm{gemm}_{k.replace('::', '_')}_gflops"; m[key] = max(m.get(key, 0), float(v))
    return m

def cpu_name():
    try:
        for ln in open("/proc/cpuinfo"):
            if ln.startswith("model name"): return ln.split(":", 1)[1].strip()
    except OSError: pass
    return platform.processor() or "unknown"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--forge", required=True); ap.add_argument("--model", required=True)
    ap.add_argument("--gemm", type=int, default=256); ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--baseline"); ap.add_argument("--check", action="store_true"); ap.add_argument("--tolerance", type=float, default=0.5)
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = a.baseline or os.path.join(root, "perf", f"baseline-{platform.machine()}.json")
    got = measure(a.forge, a.model, a.gemm, a.reps)
    if not got: sys.exit("no metrics parsed from forge bench output")
    if not a.check:
        doc = {"machine": platform.machine(), "cpu": cpu_name(), "threads": os.cpu_count(), "metrics": dict(sorted(got.items()))}
        open(path, "w").write(json.dumps(doc, indent=2) + "\n"); print("wrote", path); return 0
    base = json.load(open(path))["metrics"]; bad = 0
    for k, v in sorted(base.items()):
        g = got.get(k)
        ok = g is not None and g >= (1 - a.tolerance) * v
        print(f"{'ok  ' if ok else 'SLOW'} {k:40s} {g if g is not None else float('nan'):10.2f}  baseline {v:10.2f}")
        bad += not ok
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
