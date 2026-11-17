#!/usr/bin/env python3
"""Analyse a binary allocation trace written by tools/alloc_trace.c.

Usage:
    python3 tools/analyze_alloc_trace.py TRACE [--json OUT] [--top N]

The trace contains one event per allocation, reallocation, free and phase
mark.  This script reconstructs allocation lifetimes so the report answers
the questions an allocator design needs:

* which call sites dominate count, bytes, and peak live bytes;
* whether objects die quickly or live until teardown;
* whether frees are LIFO/stack-like or arbitrary;
* how reallocates move between size classes;
* how much of the peak is a single document-lifetime allocation.
"""
import argparse
import collections
import json
import statistics
import struct
import sys

HEADER = struct.Struct("<8sIIQ")
EVENT = struct.Struct("<IIQQQQQII")
OPS = {0: "alloc", 1: "realloc", 2: "free", 3: "calloc",
       4: "mark", 5: "free-untracked"}

# Candidate classes for the in-tree slab.
SLAB_CLASSES = [
    16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240,
    256, 288, 320, 352, 384, 416, 448, 480, 512, 576, 640, 704, 768, 832,
    896, 960, 1024, 1152, 1280, 1408, 1536, 1664, 1792, 1920, 2048,
]


def slab_class(n):
    for c in SLAB_CLASSES:
        if n <= c:
            return c
    return None


def human(n):
    if n >= 1024 * 1024:
        return "%.2f MiB" % (n / (1024.0 * 1024.0))
    if n >= 1024:
        return "%.1f KiB" % (n / 1024.0)
    return "%d B" % n


def pct(values, q):
    if not values:
        return 0
    values = sorted(values)
    i = min(len(values) - 1, int(q * (len(values) - 1)))
    return values[i]


class Trace:
    def __init__(self, path):
        data = open(path, "rb").read()
        magic, version, nsites, nevents = HEADER.unpack_from(data, 0)
        if magic != b"SVGTRAC1" or version != 1:
            raise SystemExit("not a svg2img trace: %r version %d"
                             % (magic, version))
        off = HEADER.size
        self.sites = []
        for _ in range(nsites):
            (n,) = struct.unpack_from("<I", data, off)
            off += 4
            self.sites.append(data[off:off + n].decode("utf-8"))
            off += n
        self.events = []
        for _ in range(nevents):
            self.events.append(EVENT.unpack_from(data, off))
            off += EVENT.size
        if off != len(data):
            raise SystemExit("trailing bytes in trace")


def analyse(tr, topn=15):
    events = tr.events
    sites = tr.sites
    live = {}                    # id -> (size, origin, alloc_seq)
    by_id = {}                   # id -> dict
    live_site = collections.defaultdict(int)
    peak_site = collections.defaultdict(int)
    phase_alloc = collections.defaultdict(lambda: [0, 0])
    phase_peak = collections.defaultdict(int)
    phase = "start"
    cur_live_bytes = 0
    peak = (0, 0, 0, "")        # bytes, count, event index, phase
    exact_sizes = collections.Counter()
    class_sizes = collections.Counter()
    alloc_bytes_by_class = collections.Counter()
    reallocs = []
    frees = []
    nalloc = nrealloc = nfree = 0
    marks = []
    untracked = 0

    # A Fenwick tree over allocation ids answers "how many live allocations
    # were created after this one" without scanning the live set each free.
    max_id = max((e[6] for e in events if e[6]), default=0)
    bit = [0] * (max_id + 2)

    def bit_add(i, v):
        while i < len(bit):
            bit[i] += v
            i += i & -i

    def bit_sum(i):
        s = 0
        while i > 0:
            s += bit[i]
            i -= i & -i
        return s

    for seq, ev in enumerate(events):
        op, site, ptr, old_ptr, size, old_size, aid, origin, _pad = ev
        opname = OPS.get(op, "?")
        if opname == "mark":
            name = sites[site]
            marks.append((seq, name, cur_live_bytes, len(live)))
            phase = name
            phase_peak[phase] = max(phase_peak[phase], cur_live_bytes)
            continue

        if opname in ("alloc", "calloc"):
            nalloc += 1
            exact_sizes[size] += 1
            cls = slab_class(size)
            class_sizes[cls if cls else "large"] += 1
            alloc_bytes_by_class[cls if cls else "large"] += size
            if aid:
                live[aid] = (size, origin, seq)
                by_id[aid] = dict(size=size, origin=origin, alloc_seq=seq,
                                  free_seq=None, reallocs=0)
                bit_add(aid, 1)
            live_site[origin] += size
            peak_site[origin] = max(peak_site[origin], live_site[origin])
            phase_alloc[phase][0] += 1
            phase_alloc[phase][1] += size
            cur_live_bytes += size
            if cur_live_bytes > peak[0]:
                peak = (cur_live_bytes, len(live), seq, phase)
        elif opname == "realloc":
            nrealloc += 1
            if aid and aid in by_id:
                by_id[aid]["reallocs"] += 1
            had = aid in live
            if ptr:
                if had:
                    old_live_size = live[aid][0]
                    live_site[origin] -= old_live_size
                    cur_live_bytes -= old_live_size
                else:
                    bit_add(aid, 1)
                live[aid] = (size, origin,
                             by_id[aid]["alloc_seq"] if aid in by_id else seq)
                if aid not in by_id:
                    by_id[aid] = dict(size=size, origin=origin, alloc_seq=seq,
                                      free_seq=None, reallocs=0)
                if old_size == 0:
                    # realloc(NULL, n) is an allocation in disguise.
                    nalloc += 1
                    exact_sizes[size] += 1
                    cls = slab_class(size)
                    class_sizes[cls if cls else "large"] += 1
                    alloc_bytes_by_class[cls if cls else "large"] += size
                    phase_alloc[phase][0] += 1
                    phase_alloc[phase][1] += size
                reallocs.append((old_size, size, origin))
                live_site[origin] += size
                peak_site[origin] = max(peak_site[origin], live_site[origin])
                cur_live_bytes += size
                if cur_live_bytes > peak[0]:
                    peak = (cur_live_bytes, len(live), seq, phase)
            # A failed realloc leaves the old block live and unchanged.
        elif opname == "free":
            nfree += 1
            if aid in by_id:
                by_id[aid]["free_seq"] = seq
                lst = live.pop(aid, None)
                if lst:
                    live_site[origin] -= lst[0]
                    cur_live_bytes -= lst[0]
                    bit_add(aid, -1)
                    stack_dist = bit_sum(max_id) - bit_sum(aid)
                    frees.append((stack_dist, lst[0], origin,
                                  seq - lst[2], seq))
            phase_peak[phase] = max(phase_peak[phase], cur_live_bytes)
        elif opname == "free-untracked":
            untracked += 1
        phase_peak[phase] = max(phase_peak[phase], cur_live_bytes)

    # Snapshot the peak composition by replaying to the peak event.
    peak_live = collections.defaultdict(int)
    for ev in events[:peak[2] + 1]:
        op, site, ptr, old_ptr, size, old_size, aid, origin, _pad = ev
        if op in (0, 3):
            peak_live[origin] += size
        elif op == 1:
            if ptr:
                peak_live[origin] -= old_size
            if aid:
                peak_live[origin] += size
    peak_top = sorted(peak_live.items(), key=lambda kv: -kv[1])[:topn]

    lifetimes = []
    lifetime_bytes = []
    same_phase = 0
    freed = 0
    for a in by_id.values():
        if a["free_seq"] is None:
            continue
        freed += 1
        life = a["free_seq"] - a["alloc_seq"]
        lifetimes.append(life)
        lifetime_bytes.append((life, a["size"]))
    lifetime_bytes.sort()
    byte_sum = sum(s for _, s in lifetime_bytes)
    half = byte_sum * 0.5
    acc = 0
    bytes_p50 = 0
    for life, size in lifetime_bytes:
        acc += size
        if acc >= half:
            bytes_p50 = life
            break

    # Phase attribution for every allocation's free.
    alloc_phase = {}
    phase_at = "start"
    seq_phase = []
    for seq, ev in enumerate(events):
        if ev[0] == 4:
            phase_at = sites[ev[1]]
        seq_phase.append(phase_at)
    for aid, a in by_id.items():
        if a["free_seq"] is not None and \
                seq_phase[a["free_seq"]] == seq_phase[a["alloc_seq"]]:
            same_phase += 1

    # Realloc movement in candidate classes.
    same_class = grow = shrink = 0
    move_bytes = 0
    for old, new, _origin in reallocs:
        if new > old:
            grow += 1
        elif new < old:
            shrink += 1
        oc, nc = slab_class(old), slab_class(new)
        if oc is not None and oc == nc:
            same_class += 1
        move_bytes += new

    # Free-order: a free is LIFO when no live allocation was created later.
    lifo = sum(1 for d, _, _, _, _ in frees if d == 0)
    stack_dists = [d for d, _, _, _, _ in frees]

    site_count = collections.Counter()
    site_bytes = collections.Counter()
    site_realloc = collections.Counter()
    for a in by_id.values():
        site_count[a["origin"]] += 1
        site_bytes[a["origin"]] += a["size"]
        site_realloc[a["origin"]] += a["reallocs"]

    def site_name(i):
        return sites[i] if 0 <= i < len(sites) else "<%d>" % i

    top_count = [(site_name(i), n) for i, n in site_count.most_common(topn)]
    top_bytes = [(site_name(i), n) for i, n in site_bytes.most_common(topn)]
    top_peak = [(site_name(i), n) for i, n in peak_top]
    top_sizes = [(s, n) for s, n in exact_sizes.most_common(topn)]

    summary = {
        "events": len(events),
        "alloc": nalloc,
        "realloc_count": nrealloc,
        "free": nfree,
        "untracked_free": untracked,
        "live_at_end": len(live),
        "peak_bytes": peak[0],
        "peak_count": peak[1],
        "peak_event": peak[2],
        "peak_phase": peak[3],
        "peak_top": top_peak,
        "phase_alloc": {k: v for k, v in phase_alloc.items()},
        "phase_peak": dict(phase_peak),
        "exact_sizes": top_sizes,
        "class_sizes": {str(k): v for k, v in class_sizes.items()},
        "class_bytes": {str(k): v for k, v in alloc_bytes_by_class.items()},
        "realloc": {
            "count": len(reallocs),
            "same_class": same_class,
            "grow": grow,
            "shrink": shrink,
            "new_bytes": move_bytes,
        },
        "lifetime": {
            "freed": freed,
            "same_phase": same_phase,
            "p50_events": pct(lifetimes, 0.5),
            "p90_events": pct(lifetimes, 0.9),
            "p99_events": pct(lifetimes, 0.99),
            "bytes_p50_events": bytes_p50,
        },
        "free_order": {
            "lifo": lifo,
            "frees": len(frees),
            "p50_stack_distance": pct(stack_dists, 0.5),
            "p90_stack_distance": pct(stack_dists, 0.9),
        },
        "top_count": top_count,
        "top_bytes": top_bytes,
    }
    return summary


def report(name, s):
    print("=" * 78)
    print(name)
    print("=" * 78)
    print("events %d | alloc %d | realloc %d | free %d | untracked %d"
          % (s["events"], s["alloc"], s["realloc_count"], s["free"],
             s["untracked_free"]))
    print("peak live: %s in %d objects at event %d (%s)"
          % (human(s["peak_bytes"]), s["peak_count"], s["peak_event"],
             s["peak_phase"]))
    print("\nphase allocation:")
    for phase, (n, b) in s["phase_alloc"].items():
        print("  %-18s %8d allocs %12s   peak live %12s"
              % (phase, n, human(b), human(s["phase_peak"].get(phase, 0))))
    print("\nexact sizes (top):")
    for size, n in s["exact_sizes"]:
        print("  %-10s %8d" % (human(size), n))
    print("\nslab classes:")
    for cls in sorted(s["class_sizes"], key=lambda x: (x == "large", x)):
        print("  %-10s %8d allocs %12s"
              % (cls, s["class_sizes"][cls], human(s["class_bytes"][cls])))
    r = s["realloc"]
    print("\nrealloc: %d total, %d stay in class, %d grow, %d shrink, %s new"
          % (r["count"], r["same_class"], r["grow"], r["shrink"],
             human(r["new_bytes"])))
    l = s["lifetime"]
    print("lifetime: p50 %d p90 %d p99 %d events; bytes-p50 %d events; "
          "%d/%d freed in the same phase"
          % (l["p50_events"], l["p90_events"], l["p99_events"],
             l["bytes_p50_events"], l["same_phase"], l["freed"]))
    f = s["free_order"]
    print("free order: LIFO %d/%d (%.1f%%), p50 stack distance %d, p90 %d"
          % (f["lifo"], f["frees"], 100.0 * f["lifo"] / max(1, f["frees"]),
             f["p50_stack_distance"], f["p90_stack_distance"]))
    print("\ntop call sites by count:")
    for site, n in s["top_count"]:
        print("  %8d  %s" % (n, site))
    print("\ntop call sites by allocated bytes:")
    for site, b in s["top_bytes"]:
        print("  %12s  %s" % (human(b), site))
    print("\npeak composition:")
    for site, b in s["peak_top"]:
        print("  %12s  %s" % (human(b), site))
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--json", default="")
    ap.add_argument("--top", type=int, default=15)
    args = ap.parse_args()
    out = {}
    for path in args.traces:
        tr = Trace(path)
        s = analyse(tr, args.top)
        report(path, s)
        out[path] = s
    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=2, sort_keys=True)
        print("wrote", args.json)


if __name__ == "__main__":
    main()
