#!/usr/bin/env python3
"""Visual baseline workflow: capture, compare, and review.

Three subcommands, all driven from the conformance corpus so the baseline and
the conformance run can never drift apart::

    python3 tools/baseline.py capture [--bin PATH] [--out DIR]
    python3 tools/baseline.py compare [--bin PATH] [--baseline DIR]
    python3 tools/baseline.py list   [--baseline DIR]

``capture`` renders every case at the canonical settings (``-a 4``, no
background, no scale) and writes ``<case>.png`` plus ``manifest.json``.
``compare`` re-renders into a scratch directory, runs ``perceptual.py`` on
each case, and writes ``report.json`` with a per-case verdict.

Design notes:

* Baselines are **not** committed.  They are machine- and version-specific;
  ``conformance/expectations.json`` (sampled pixels) is the committed oracle.
  The baseline is a *review* aid: it answers "what changed visually?" after a
  rendering change, which sampled pixels cannot.
* Every case gets a ``diff.png``/``amp.png`` artifact only when it fails, so a
  clean run leaves a small directory.
* Exit status: 0 when every case matches, 1 when any case differs, 2 on error.

Tune with ``--tolerance``, ``--area`` and ``--blur``; the defaults are strict
enough to catch a wrong colour and loose enough to ignore an antialiasing
shift.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import pngio  # noqa: E402
import perceptual  # noqa: E402

CORPUS = os.path.join(ROOT, "conformance", "cases")
DEFAULT_BASELINE = os.path.join(ROOT, "conformance", "baseline")


def cases():
    return sorted(f for f in os.listdir(CORPUS) if f.endswith(".svg"))


def render_all(binpath, outdir, aa=4):
    os.makedirs(outdir, exist_ok=True)
    done = {}
    for name in cases():
        stem = name[:-4]
        src = os.path.join(CORPUS, name)
        out = os.path.join(outdir, stem + ".png")
        r = subprocess.run([binpath, "-a", str(aa), src, out],
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            done[stem] = {"ok": False, "error": r.stderr.strip()[:200]}
        else:
            done[stem] = {"ok": True, "path": out}
    return done


def cmd_capture(args):
    binpath = args.bin
    if not os.path.exists(binpath):
        print("renderer not found: %s (run make)" % binpath, file=sys.stderr)
        return 2
    t0 = time.time()
    rendered = render_all(binpath, args.out, args.aa)
    manifest = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "bin": os.path.relpath(binpath, ROOT),
        "aa": args.aa,
        "cases": rendered,
        "seconds": round(time.time() - t0, 2),
    }
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")
    bad = [k for k, v in rendered.items() if not v["ok"]]
    print("captured %d/%d cases into %s in %.1fs"
          % (len(rendered) - len(bad), len(rendered), args.out,
             manifest["seconds"]))
    if bad:
        for k in bad:
            print("  FAIL %s: %s" % (k, rendered[k].get("error", "")))
        return 1
    return 0


def cmd_compare(args):
    baseline = args.baseline
    if not os.path.isdir(baseline):
        print("no baseline at %s; run 'baseline.py capture' first" % baseline,
              file=sys.stderr)
        return 2
    scratch = args.scratch
    rendered = render_all(args.bin, scratch, args.aa)
    report = {"baseline": baseline, "bin": args.bin, "aa": args.aa,
              "tolerance": args.tolerance, "area": args.area,
              "blur": args.blur, "cases": {}}
    bad = 0
    for stem, info in sorted(rendered.items()):
        entry = {"ok": info["ok"]}
        base = os.path.join(baseline, stem + ".png")
        if not info["ok"]:
            entry["verdict"] = "error"
            entry["detail"] = info.get("error", "render failed")
            bad += 1
            report["cases"][stem] = entry
            continue
        if not os.path.exists(base):
            entry["verdict"] = "missing-baseline"
            bad += 1
            report["cases"][stem] = entry
            continue
        aw, ah, apx = pngio.read_png(base)
        bw, bh, bpx = pngio.read_png(info["path"])
        if (aw, ah) != (bw, bh):
            entry["verdict"] = "size"
            entry["detail"] = "%dx%d vs %dx%d" % (aw, ah, bw, bh)
            bad += 1
            report["cases"][stem] = entry
            continue
        if args.blur:
            apx = perceptual.blur_rgba(aw, ah, apx, args.blur)
            bpx = perceptual.blur_rgba(aw, ah, bpx, args.blur)
        delta = perceptual.compare(aw, ah, apx, bpx, args.tolerance)
        over = sum(1 for d in delta if d > args.tolerance)
        pct = 100.0 * over / (aw * ah) if aw * ah else 0.0
        entry.update({"verdict": "same" if pct <= args.area else "diff",
                      "area_percent": round(pct, 4),
                      "pixels_over": over, "max_delta": max(delta) if delta else 0})
        if entry["verdict"] == "diff":
            bad += 1
            # Artifacts only for regressions.
            outdir = os.path.join(args.artifacts, stem)
            os.makedirs(outdir, exist_ok=True)
            diff = bytearray(aw * ah * 4)
            for i, d in enumerate(delta):
                o = i * 4
                if d > args.tolerance:
                    diff[o] = 255
                    diff[o + 3] = 255
                else:
                    diff[o + 3] = 255
            pngio.write_png(os.path.join(outdir, "diff.png"), aw, ah, bytes(diff))
            entry["diff"] = os.path.join(outdir, "diff.png")
        report["cases"][stem] = entry

    os.makedirs(os.path.dirname(os.path.abspath(args.json)) or ".", exist_ok=True)
    with open(args.json, "w") as f:
        json.dump(report, f, indent=2, sort_keys=True)
        f.write("\n")
    same = sum(1 for v in report["cases"].values() if v["verdict"] == "same")
    print("baseline compare: %d same, %d different (of %d), tol=%d area<=%.3f%%"
          % (same, bad, len(report["cases"]), args.tolerance, args.area))
    for stem, v in sorted(report["cases"].items()):
        if v["verdict"] != "same":
            print("  %-4s %-22s %s" % (v["verdict"].upper(), stem,
                                       v.get("detail") or
                                       "%.3f%% over, max d=%d" % (
                                           v.get("area_percent", 0),
                                           v.get("max_delta", 0))))
    print("report: %s" % args.json)
    return 0 if bad == 0 else 1


def cmd_list(args):
    if not os.path.isdir(args.baseline):
        print("no baseline at %s" % args.baseline)
        return 2
    man = os.path.join(args.baseline, "manifest.json")
    if os.path.exists(man):
        m = json.load(open(man))
        print("baseline: %s" % args.baseline)
        print("  generated %s from %s (aa=%d, %d cases, %.1fs)"
              % (m.get("generated", "?"), m.get("bin", "?"),
                 m.get("aa", 0), len(m.get("cases", {})), m.get("seconds", 0)))
    n = len([f for f in os.listdir(args.baseline) if f.endswith(".png")])
    print("  %d png file(s)" % n)
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description="visual baseline workflow")
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--bin", default=os.path.join(ROOT, "svg2img"))
        sp.add_argument("--baseline", default=DEFAULT_BASELINE)
        sp.add_argument("--aa", type=int, default=4)

    c = sub.add_parser("capture", help="render the corpus into a baseline")
    common(c)
    c.add_argument("--out", default="")
    c.set_defaults(func=lambda a: cmd_capture(
        argparse.Namespace(**{**vars(a), "out": a.out or a.baseline})))

    m = sub.add_parser("compare", help="render and diff against a baseline")
    common(m)
    m.add_argument("--tolerance", type=int, default=12)
    m.add_argument("--area", type=float, default=0.1)
    m.add_argument("--blur", type=int, default=0)
    m.add_argument("--scratch", default="")
    m.add_argument("--artifacts", default="")
    m.add_argument("--json", default="")
    m.set_defaults(func=lambda a: cmd_compare(argparse.Namespace(
        **{**vars(a),
           "scratch": a.scratch or os.path.join(
               os.environ.get("TMPDIR", "/tmp"), "svg2img_baseline_run"),
           "artifacts": a.artifacts or os.path.join(a.baseline, "artifacts"),
           "json": a.json or os.path.join(a.baseline, "report.json")})))

    l = sub.add_parser("list", help="show baseline metadata")
    l.add_argument("--baseline", default=DEFAULT_BASELINE)
    l.set_defaults(func=cmd_list)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())