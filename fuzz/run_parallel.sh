#!/usr/bin/env bash
# Run fuzz_run over a list of files with N parallel workers.
#
#   ./run_parallel.sh <list.txt> [jobs] [aa]
#
# jobs defaults to the online CPU count (capped by the number of inputs).
# Set JOBS=N in the environment to override.  BIN can point at an alternate
# fuzz_run binary.  Each worker gets a disjoint slice of the list; logs are
# written under a temporary directory and the combined summary is printed.
set -u

LIST=${1:?usage: run_parallel.sh <list> [jobs] [aa]}
AA=${3:-2}
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-"$HERE/fuzz_run"}
KEEP=${KEEP:-0}

if [ -n "${2:-}" ]; then
    JOBS=$2
else
    JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 8)}
fi
case "$JOBS" in
    ''|*[!0-9]*) JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 8) ;;
esac
if [ "$JOBS" -eq 0 ] 2>/dev/null; then
    JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 8)
fi
if [ "$JOBS" -lt 1 ]; then JOBS=1; fi

if [ ! -x "$BIN" ]; then
    echo "fuzz_run not built: $BIN (run 'make fuzz_run' first)" >&2
    exit 1
fi

N=$(wc -l < "$LIST" | tr -d ' ')
if [ "$N" -eq 0 ]; then
    echo "no inputs in $LIST"
    exit 0
fi
if [ "$JOBS" -gt "$N" ]; then JOBS=$N; fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/svg2img_par.XXXXXX")
awk -v n="$JOBS" -v d="$WORK" '{ print > (d "/part." (NR % n)) }' "$LIST"

echo "run_parallel: $N inputs, $JOBS workers, aa=$AA, bin=$BIN"
pids=()
for f in "$WORK"/part.*; do
    [ -e "$f" ] || continue
    "$BIN" "$AA" < "$f" > "$f.log" 2>&1 &
    pids+=($!)
done

rc=0
for p in "${pids[@]}"; do
    wait "$p" || rc=1
done

cat "$WORK"/part.*.log > "$LIST.log"
read -r ok crashes timeouts cf <<EOF
$(grep -h '^done:' "$WORK"/part.*.log 2>/dev/null | awk -F'[ :,]+' '{ok+=$2; cr+=$4; to+=$6; cf+=$8} END {printf "%d %d %d %d", ok, cr, to, cf}')
EOF
echo "total: $ok ok, $crashes crashes, $timeouts timeouts, $cf clean-failures"
problems=$((crashes + timeouts))
echo "problems: $problems"
grep -hE "^(CRASH|ABNORMAL|TIMEOUT)" "$WORK"/part.*.log 2>/dev/null | sed "s|^|  |" | head -40
if [ "$KEEP" = "1" ]; then
    echo "logs kept: $WORK/part.*.log"
else
    echo "logs: $WORK/part.*.log"
    rm -rf "$WORK"
fi
exit $rc