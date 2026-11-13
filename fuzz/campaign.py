#!/usr/bin/env python3
"""Continuous fuzz campaign: generate mutants, run them, minimise crashes.

Each round:
  1. generate N mutants from the seed corpus
  2. run the sanitizer-instrumented binary over them in parallel
  3. if anything crashes, minimise it and stop (so it can be fixed)

Run under a process supervisor; it exits non-zero the moment it finds a
reproducible crash.

    python3 campaign.py <rounds> <per_round> [start_seed] [jobs]
"""
import os
import random
import tempfile
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("SVG2IMG_FUZZ_WORK") or os.path.join(tempfile.gettempdir(), "svg2img_fuzz")
SEEDS = os.path.join(ROOT, "seeds")
WORK = os.path.join(ROOT, "campaign")
BIN = os.path.join(HERE, "fuzz_run")


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), **kw)


def minimise(path, timeout=10):
    """Delta-debug a crashing file down to a minimal reproducer."""
    data = open(path, "rb").read()
    if len(data) > 20000:
        data = data[:20000]

    def crashes(d):
        with open(os.path.join(ROOT, "min_case.svg"), "wb") as f:
            f.write(d)
        try:
            r = subprocess.run([os.path.join(HERE, "one"), "-a", "1",
                                os.path.join(ROOT, "min_case.svg"), os.path.join(ROOT, "min.png")],
                               capture_output=True, timeout=timeout)
            return r.returncode != 0
        except subprocess.TimeoutExpired:
            return False

    if not crashes(data):
        return None

    # Coarse chunk removal, then byte-level trimming.
    chunk = max(1, len(data) // 2)
    while chunk >= 1:
        i = 0
        while i < len(data):
            cand = data[:i] + data[i + chunk:]
            if cand and crashes(cand):
                data = cand
            else:
                i += chunk
        if chunk == 1:
            break
        chunk //= 2
    return data


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    per_round = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    jobs = sys.argv[4] if len(sys.argv) > 4 else "0"

    os.makedirs(WORK, exist_ok=True)
    rng = random.Random(seed)

    for rnd in range(rounds):
        gen = os.path.join(WORK, "g")
        shutil.rmtree(gen, ignore_errors=True)
        os.makedirs(gen, exist_ok=True)
        r = subprocess.run([sys.executable, os.path.join(HERE, "mutate.py"),
                            SEEDS, gen, str(per_round), str(rng.randrange(1 << 30))],
                           capture_output=True)
        if r.returncode != 0:
            print("generation failed:", r.stderr.decode()[:300])
            return 2

        files = [os.path.join(gen, f) for f in sorted(os.listdir(gen))]
        if not files:
            continue

        # Run in parallel; the runner itself forks per input.
        lst = os.path.join(WORK, "list.txt")
        with open(lst, "w") as f:
            f.write("\n".join(files) + "\n")

        # Run in parallel across cores; each worker forks per input.
        proc = subprocess.run([os.path.join(HERE, "run_parallel.sh"), lst, jobs, "2"],
                              capture_output=True, text=True, timeout=3600)

        log = proc.stdout
        crashes = [l.split()[1] for l in log.splitlines()
                   if l.startswith(("CRASH", "ABNORMAL"))]
        total = ""
        for line in log.splitlines():
            if line.startswith("done:"):
                total = line

        if crashes:
            print(f"round {rnd}: {len(crashes)} crash(es) -- {total}")
            uniq = sorted(set(crashes))
            print("first crashers:", uniq[:5])
            for c in uniq[:3]:
                path = os.path.join(ROOT, c) if not c.startswith("/") else c
                if not os.path.exists(path):
                    path = os.path.join(gen, os.path.basename(c))
                if os.path.exists(path):
                    small = minimise(path)
                    if small is not None:
                        out = os.path.join(WORK, "minimal.svg")
                        open(out, "wb").write(small)
                        print(f"MINIMISED -> {out} ({len(small)} bytes)")
            return 1

        print(f"round {rnd}: ok ({total})", flush=True)

    print(f"campaign complete: {rounds} rounds x {per_round} clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())