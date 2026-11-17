#!/bin/sh
# finish_spec_and_perf.sh - one-shot verification and reporting for svg2img.
#
# Subcommands:
#   status    build + pixel checks + conformance + semantic + regression suites
#   external  verify conformance assertions against Chrome (inline harness)
#   w3c       run the W3C SVG 1.1 suite twice (normal and text-suppressed)
#   triage    regenerate the per-test W3C triage from the saved result JSON
#   bench     generate and run the deterministic benchmark
#   all       status + external + w3c + triage + bench
#   clean     make clean
#
# Everything is written under conformance/ and bench/ so a run is reviewable.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
BIN=./svg2img
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}
PYTHON=${PYTHON:-python3}

say() { printf '\n=== %s ===\n' "$1"; }

build() {
    say "build"
    make -j"$JOBS"
}

status() {
    build
    say "strict build"
    make strict
    say "pixel checks"
    make check
    say "conformance corpus"
    "$PYTHON" conformance/run.py --bin "$BIN" --json conformance/summary.json
    say "semantic oracle"
    "$PYTHON" fuzz/semantic_cases.py "$BIN"
    say "saved regressions"
    "$PYTHON" fuzz/regress.py "$BIN"
}

external() {
    build
    say "external conformance assertions (Chrome inline)"
    "$PYTHON" conformance/verify_external.py \
        --backends chrome-inline \
        --json conformance/verify_external.json
}

w3c() {
    build
    say "W3C SVG 1.1 suite (normal)"
    # w3c_chrome.py exits 1 when any test differs; that is a report, not a
    # harness failure, so keep going to the text-suppressed pass and triage.
    "$PYTHON" conformance/w3c_chrome.py --json conformance/w3c_results.json \
        || echo "warning: W3C normal pass reported mismatches (expected)"
    say "W3C SVG 1.1 suite (text-suppressed geometry pass)"
    "$PYTHON" conformance/w3c_chrome.py --suppress-text \
        --json conformance/w3c_suppress_text.json \
        || echo "warning: W3C text-suppressed pass reported mismatches (expected)"
    triage
}

triage() {
    say "W3C per-test triage"
    "$PYTHON" conformance/w3c_triage.py \
        --json conformance/w3c_results.json \
        --md conformance/w3c_triage.md
    if [ -f conformance/w3c_suppress_text.json ]; then
        "$PYTHON" conformance/w3c_triage.py \
            --json conformance/w3c_suppress_text.json \
            --md conformance/w3c_suppress_text_triage.md
    fi
}

bench() {
    build
    say "benchmark"
    "$PYTHON" bench/gen.py
    "$PYTHON" bench/run.py --bin "$BIN" --repeat "${REPEAT:-5}" \
        --json bench/results.json
}

clean() {
    say "clean"
    make clean
}

case "${1:-all}" in
    status)   status ;;
    external) external ;;
    w3c)      w3c ;;
    triage)   triage ;;
    bench)    bench ;;
    all)      status; external; w3c; bench ;;
    clean)    clean ;;
    *)
        echo "usage: $0 [status|external|w3c|triage|bench|all|clean]" >&2
        exit 2
        ;;
esac