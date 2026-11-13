#!/usr/bin/env python3
"""Run the big-input suite with per-file timing.

Files run in parallel (default: min(4, online CPUs)); big inputs are memory
heavy, so the default is deliberately lower than the fuzz worker count.  Pass
a third argument to override the worker count (0 = auto).

Reports any file that crashes, exceeds a time budget, or trips a sanitizer.
"""
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

BIG = os.environ.get("SVG2IMG_FUZZ_WORK", "") and os.path.join(os.environ["SVG2IMG_FUZZ_WORK"], "big") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "big")
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "one")
TIME_BUDGET = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0
JOBS = int(sys.argv[3]) if len(sys.argv) > 3 else 0
if JOBS <= 0:
    JOBS = min(4, os.cpu_count() or 1)


def run_one(name):
    path = os.path.join(BIG, name)
    t0 = time.time()
    try:
        r = subprocess.run([BIN, "-a", "2", path, os.path.join(BIG, "_out.png")],
                           capture_output=True, timeout=TIME_BUDGET)
        dt = time.time() - t0
        rc = r.returncode
        err = r.stderr.decode("utf-8", "replace")
        if "runtime error" in err or "ERROR: AddressSanitizer" in err:
            rc = -999
            err = err.strip().splitlines()[0][:160]
        else:
            err = ""
    except subprocess.TimeoutExpired:
        dt = time.time() - t0
        rc = -1
        err = f"TIMEOUT after {TIME_BUDGET:.0f}s"
    return dt, name, rc, os.path.getsize(path) / 1e6, err


def main():
    names = sorted(n for n in os.listdir(BIG) if n.endswith(".svg"))
    if not names:
        print("no big inputs in", BIG)
        return 1
    print(f"bigrun: {len(names)} inputs, {JOBS} workers, budget {TIME_BUDGET:.0f}s")
    rows = []
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        futs = {ex.submit(run_one, n): n for n in names}
        for fut in as_completed(futs):
            dt, name, rc, mb, err = fut.result()
            rows.append((dt, name, rc, mb, err))
            flag = "OK  " if rc == 0 else ("SLOW" if rc == -1 else "FAIL")
            print(f"{flag} {dt:7.2f}s  {mb:6.2f}MB  {name}  {err}", flush=True)

    rows.sort(reverse=True)
    print("\nslowest:")
    for dt, name, rc, mb, err in rows[:5]:
        print(f"  {dt:7.2f}s  {mb:6.2f}MB  {name}")
    bad = [r for r in rows if r[2] != 0]
    print(f"\n{len(rows)} files, {len(bad)} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())