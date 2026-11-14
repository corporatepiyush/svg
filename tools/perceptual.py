#!/usr/bin/env python3
"""Perceptual image diff with tolerance, a blur radius, and artifacts.

Compares two PNGs the way a human reviewer would rather than the way a
checksum does: differences smaller than ``--tolerance`` are ignored, the
images are optionally blurred so a one-pixel antialiasing shift does not
light up the whole map, and only the *fraction* of pixels over tolerance
matters for the verdict.

Outputs, in ``--outdir`` (default ``<first-arg>.diff``):

  * ``diff.png``        black background, red where the images disagree
  * ``blur_a.png``      the blurred A channel, for eyeballing what was compared
  * ``amp.png``         |A-B| amplified, for locating the worst region
  * ``summary.json``    machine-readable numbers

Exit status: 0 when the images agree, 1 when they do not, 2 on a usage or
decode error.  ``--quiet`` suppresses the human-readable table.

Usage:
    python3 tools/perceptual.py A.png B.png [--tolerance N] [--blur N]
                                [--area PCT] [--outdir DIR] [--json FILE]
                                [--quiet] [--save-artifacts]

This is a measurement tool, not a test oracle: use ``conformance/run.py`` for
pass/fail pixel assertions and this for reviewing a visual change.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pngio  # noqa: E402


def blur_rgba(w, h, px, radius):
    """Separable box blur on premultiplied RGBA; radius 0 is a no-op."""
    if radius <= 0:
        return px
    r = int(radius)
    # Premultiply so transparent pixels do not bleed colour.
    a = bytearray(len(px))
    for i in range(0, len(px), 4):
        al = px[i + 3]
        a[i] = px[i] * al // 255
        a[i + 1] = px[i + 1] * al // 255
        a[i + 2] = px[i + 2] * al // 255
        a[i + 3] = al
    tmp = bytearray(len(a))
    span = 2 * r + 1
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            o = row + x * 4
            for k in range(4):
                s = 0
                n = 0
                for d in range(-r, r + 1):
                    xx = x + d
                    if 0 <= xx < w:
                        s += a[(row + xx * 4) + k]
                        n += 1
                tmp[o + k] = s // n
    out = bytearray(len(tmp))
    for y in range(h):
        for x in range(w):
            o = (y * w + x) * 4
            for k in range(4):
                s = 0
                n = 0
                for d in range(-r, r + 1):
                    yy = y + d
                    if 0 <= yy < h:
                        s += tmp[((yy * w + x) * 4) + k]
                        n += 1
                out[o + k] = s // n
    # Un-premultiply for display.
    for i in range(0, len(out), 4):
        al = out[i + 3]
        if al:
            for k in range(3):
                v = out[i + k] * 255 // al
                out[i + k] = 255 if v > 255 else v
    return bytes(out)


def compare(w, h, a, b, tolerance):
    """Return per-pixel max-channel delta as a bytes object of length w*h."""
    delta = bytearray(w * h)
    for i in range(0, len(a), 4):
        d = 0
        al_a = a[i + 3]
        al_b = b[i + 3]
        # Compare premultiplied colour so a transparent pixel does not count
        # as "different colour" when the other side is opaque black.
        for k in range(3):
            va = a[i + k] * al_a // 255
            vb = b[i + k] * al_b // 255
            d = max(d, abs(va - vb))
        d = max(d, abs(al_a - al_b))
        delta[i // 4] = d
    return bytes(delta)


def main(argv=None):
    p = argparse.ArgumentParser(description="perceptual PNG diff")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--tolerance", type=int, default=12,
                   help="per-channel delta ignored (default 12)")
    p.add_argument("--blur", type=int, default=0,
                   help="box-blur radius applied to both images (default 0)")
    p.add_argument("--area", type=float, default=0.1,
                   help="max %% of differing pixels to pass (default 0.1)")
    p.add_argument("--outdir", default="")
    p.add_argument("--json", default="")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--save-artifacts", action="store_true",
                   help="write diff.png / amp.png (default: summary only)")
    args = p.parse_args(argv)

    try:
        aw, ah, apx = pngio.read_png(args.a)
        bw, bh, bpx = pngio.read_png(args.b)
    except (OSError, ValueError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    if (aw, ah) != (bw, bh):
        print("error: size mismatch %dx%d vs %dx%d" % (aw, ah, bw, bh),
              file=sys.stderr)
        return 2

    if args.blur:
        apx = blur_rgba(aw, ah, apx, args.blur)
        bpx = blur_rgba(aw, ah, bpx, args.blur)

    delta = compare(aw, ah, apx, bpx, args.tolerance)
    total = aw * ah
    over = sum(1 for d in delta if d > args.tolerance)
    pct = 100.0 * over / total if total else 0.0
    maxd = max(delta) if delta else 0
    mean = sum(delta) / total if total else 0.0
    # Mean over only the pixels that differ: how *bad* is a real difference.
    differing = [d for d in delta if d > args.tolerance]
    mean_over = sum(differing) / len(differing) if differing else 0.0
    passed = pct <= args.area

    outdir = args.outdir or (os.path.splitext(args.a)[0] + ".diff")
    os.makedirs(outdir, exist_ok=True)
    summary = {
        "a": args.a, "b": args.b,
        "width": aw, "height": ah,
        "tolerance": args.tolerance, "blur": args.blur,
        "area_percent": round(pct, 4),
        "pixels_over": over, "pixels_total": total,
        "max_delta": maxd, "mean_delta": round(mean, 3),
        "mean_delta_over": round(mean_over, 3),
        "passed": passed,
    }

    if args.save_artifacts:
        diff = bytearray(aw * ah * 4)
        amp = bytearray(aw * ah * 4)
        for i in range(0, len(delta)):
            o = i * 4
            d = delta[i]
            if d > args.tolerance:
                diff[o] = 255
                diff[o + 1] = 0
                diff[o + 2] = 0
                diff[o + 3] = 255
            else:
                diff[o] = diff[o + 1] = diff[o + 2] = 0
                diff[o + 3] = 255
            v = d * 4
            amp[o] = amp[o + 1] = amp[o + 2] = 255 if v > 255 else v
            amp[o + 3] = 255
        pngio.write_png(os.path.join(outdir, "diff.png"), aw, ah, bytes(diff))
        pngio.write_png(os.path.join(outdir, "amp.png"), aw, ah, bytes(amp))
        summary["artifacts"] = {"diff": os.path.join(outdir, "diff.png"),
                                "amp": os.path.join(outdir, "amp.png")}

    jpath = args.json or os.path.join(outdir, "summary.json")
    with open(jpath, "w") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
        f.write("\n")
    summary["summary_path"] = jpath

    if not args.quiet:
        print("A: %s (%dx%d)" % (args.a, aw, ah))
        print("B: %s (%dx%d)" % (args.b, bw, bh))
        print("tolerance=%d blur=%d" % (args.tolerance, args.blur))
        print("pixels over tolerance: %d/%d (%.3f%%)  [budget %.3f%%]"
              % (over, total, pct, args.area))
        print("max delta %d, mean %.2f, mean over %.2f"
              % (maxd, mean, mean_over))
        print("verdict: %s" % ("PASS" if passed else "FAIL"))
        print("summary: %s" % jpath)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())