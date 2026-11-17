#!/usr/bin/env python3
"""Differential harness against external SVG renderers.

Runs the conformance corpus (and any extra ``--corpus`` directory) through
whichever external rasterisers are installed locally, then compares the
results with svg2img's own output.  It is deliberately advisory: a mismatch
is a *triage signal*, not a build failure, because two correct rasterisers
legitimately differ on antialiasing, colour management, spread methods and
font metrics.

Backends (probed once, cached):

  * ``chrome``  -- Chromium/Chrome/Brave ``--headless --screenshot`` on a
                   tiny HTML wrapper (the wrapper pins the viewport so the
                   SVG is drawn 1:1 with a transparent background).
  * ``inkscape``-- ``inkscape --export-type=png --export-filename=...``.
  * ``resvg``   -- ``resvg --zoom 1 --output ...``.
  * ``qlmanage``-- macOS QuickLook thumbnail (WebKit).

Nothing is installed and no network access is required: missing backends are
reported and skipped.  Exit status is 0 unless ``--strict`` is given and at
least one *classifiable* mismatch was found.

Usage:
    python3 fuzz/diff_external.py [--corpus DIR] [--bin PATH]
                                  [--backends chrome,qlmanage]
                                  [--list] [--strict] [--json FILE]
                                  [--md FILE] [--tol N] [--limit N]

Writes a triage report (markdown by default) plus an optional JSON summary.
"""
import argparse
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_CORPUS = os.path.join(ROOT, "conformance", "cases")

# Per-channel 8-bit tolerance on interior pixels.  Chrome and WebKit apply
# colour management; 24 is loose enough for solid fills but still catches a
# wrong colour or a misplaced edge by more than a pixel.
TOL = 24
# Fraction of compared pixels that may differ before a case is a mismatch.
BAD_FRACTION = 0.02
MIN_INTERIOR = 200          # below this the case is reported as "too thin"


# ---------------------------------------------------------------------------
# PNG decoding (stdlib only; filters 0..4, RGBA8 / RGB8 / grey8)
# ---------------------------------------------------------------------------

def read_png(path):
    """Return (w, h, rgba_bytes).  Supports 8-bit grey/RGB/RGBA, no interlace."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a png")
    i, idat, w, h, depth, ctype = 8, b"", 0, 0, 0, 0
    while i + 8 <= len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        typ = data[i + 4:i + 8]
        chunk = data[i + 8:i + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
        i += 12 + ln
    if depth != 8 or ctype not in (0, 2, 4, 6):
        raise ValueError("unsupported png depth=%d ctype=%d" % (depth, ctype))
    nch = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * nch
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if f == 1:
            for x in range(nch, stride):
                line[x] = (line[x] + line[x - nch]) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x - nch] if x >= nch else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - nch] if x >= nch else 0
                b = prev[x]
                c = prev[x - nch] if x >= nch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        elif f != 0:
            raise ValueError("bad filter %d" % f)
        if nch == 4:
            out[y * w * 4:(y + 1) * w * 4] = line
        elif nch == 3:
            for x in range(w):
                o = (y * w + x) * 4
                out[o:o + 3] = line[x * 3:x * 3 + 3]
                out[o + 3] = 255
        elif nch == 2:
            for x in range(w):
                o = (y * w + x) * 4
                out[o] = out[o + 1] = out[o + 2] = line[x * 2]
                out[o + 3] = line[x * 2 + 1]
        else:
            for x in range(w):
                o = (y * w + x) * 4
                out[o] = out[o + 1] = out[o + 2] = line[x]
                out[o + 3] = 255
        prev = line
    return w, h, bytes(out)


# ---------------------------------------------------------------------------
# Backends
# ---------------------------------------------------------------------------

class Backend:
    name = "?"
    exe = ""
    available = False
    detail = ""
    # Opt-in backends are probed and listed but not used unless named in
    # --backends, because their output is not pixel-comparable.
    default = True
    # QuickLook picks its own thumbnail scale, so its pixels cannot be compared
    # 1:1.  Such backends are resampled by ink bbox before comparison and their
    # mismatches are reported as approximate, never as a hard failure.
    exact = True

    def render(self, svg_path, size, outdir, tmp):
        """Render *svg_path* to a PNG; return its path or raise RuntimeError."""
        raise NotImplementedError


def copy_local_refs(svg_path, outdir):
    """Copy files the document references so a relative href resolves.

    The renderer is pointed at a copy of the SVG in a temporary directory, so
    a sibling file (`<image href="x.png">`, a CSS `url(...)`) would otherwise
    be missing and the reference would silently render nothing.  Relative
    sub-paths are preserved (`assets/x.png` stays `assets/x.png`).
    """
    import re

    try:
        with open(svg_path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read(2 * 1024 * 1024)
    except OSError:
        return
    base = os.path.dirname(os.path.abspath(svg_path))
    seen = set()
    refs = [m.group(1).strip()
            for m in re.finditer(r'(?:href|src)\s*=\s*"([^"]+)"', text)]
    refs += [m.group(1).strip()
             for m in re.finditer(r'url\(\s*["\']?([^"\')]+)["\']?\s*\)', text)]
    for ref in refs:
        if not ref or ref.startswith(("data:", "http:", "https:", "#")):
            continue
        ref = ref.split("#")[0].split("?")[0]
        if not ref or ref in seen:
            continue
        seen.add(ref)
        src = os.path.normpath(os.path.join(base, ref))
        # Refuse paths that escape the document directory; a conformance
        # document must not make the harness read arbitrary local files.
        if os.path.commonpath([base, os.path.abspath(src)]) != base:
            continue
        if not os.path.isfile(src):
            continue
        dst = os.path.join(outdir, ref)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not os.path.exists(dst):
            shutil.copyfile(src, dst)


class ChromeBackend(Backend):
    """Chromium / Chrome / Brave / Edge headless screenshot."""

    name = "chrome"
    # Kept for reproducing older results; the inline variant is the default
    # because <img> cannot load external references.
    default = False
    CANDIDATES = [
        "chromium", "chromium-browser", "google-chrome", "google-chrome-stable",
        "chrome", "brave-browser", "brave",
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
        "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
        "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    ]

    @classmethod
    def probe(cls):
        for cand in cls.CANDIDATES:
            path = cand if os.path.isabs(cand) else shutil.which(cand)
            if not path or not os.path.exists(path):
                continue
            # A wrapper script may exist while the bundle does not; verify the
            # binary actually starts and reports a version.
            try:
                r = subprocess.run([path, "--version"], capture_output=True,
                                   text=True, timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                continue
            if r.returncode != 0 or not r.stdout.strip():
                continue
            b = cls()
            b.exe = path
            b.available = True
            b.detail = r.stdout.strip().splitlines()[0][:120]
            return b
        b = cls()
        b.detail = "no Chromium-family binary on PATH"
        return b

    def render(self, svg_path, size, outdir, tmp):
        name = os.path.splitext(os.path.basename(svg_path))[0]
        # The page must sit next to the SVG so the relative <img src> resolves.
        local = os.path.join(outdir, os.path.basename(svg_path))
        if not os.path.exists(local):
            shutil.copyfile(svg_path, local)
            copy_local_refs(svg_path, outdir)
        html = os.path.join(outdir, name + ".html")
        out = os.path.join(outdir, name + ".chrome.png")
        # Pin the viewport: <img> at its intrinsic size, no margins, no scroll
        # bars, transparent page background.  This keeps both renderers at the
        # same device pixels and the same alpha semantics.
        with open(html, "w") as f:
            f.write("<!doctype html><meta charset=utf-8>"
                    "<style>html,body{margin:0;padding:0;background:transparent}"
                    "img{display:block}</style>"
                    '<img src="%s" width="%d" height="%d">'
                    % (os.path.basename(svg_path), size[0], size[1]))
        cmd = [self.exe, "--headless", "--disable-gpu", "--no-sandbox",
               "--hide-scrollbars", "--force-device-scale-factor=1",
               "--default-background-color=00000000",
               "--screenshot=" + out, "--window-size=%d,%d" % size,
               "file://" + html]
        try:
            # Do not capture output: a helper process can inherit the pipe
            # and keep it open after the screenshot process exits, which
            # makes subprocess.run() wait for EOF until the timeout.
            subprocess.run(cmd, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=60)
        except subprocess.TimeoutExpired:
            raise RuntimeError("timeout")
        if not os.path.exists(out) or os.path.getsize(out) < 24:
            raise RuntimeError("no output")
        return out


class ChromeInlineBackend(ChromeBackend):
    """Chromium with the SVG inlined into the HTML document.

    The legacy backend wraps the SVG in ``<img>``.  That is a replaced
    element: it cannot load external file references and it may scale the
    document, which makes external-resource and exact-coordinate probes
    unreliable.  Inlining the SVG makes it part of the page, so relative
    ``<image href>`` and CSS ``url(...)`` references resolve against the
    copied document directory, and the page's own viewport pins the size.
    """

    name = "chrome-inline"
    # This is the faithful harness; the <img> backend stays available for
    # reproducing older results.
    default = True
    # Optional page CSS injected after the SVG (used by the W3C runner's
    # text-suppressed geometry pass).
    extra_css = ""

    def render(self, svg_path, size, outdir, tmp):
        name = os.path.splitext(os.path.basename(svg_path))[0]
        local = os.path.join(outdir, os.path.basename(svg_path))
        if not os.path.exists(local):
            shutil.copyfile(svg_path, local)
        copy_local_refs(svg_path, outdir)
        with open(svg_path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read(16 * 1024 * 1024)
        # An XML declaration or DOCTYPE is not valid inside an HTML body.
        text = re.sub(r"<\?xml.*?\?>", "", text, flags=re.S)
        text = re.sub(r"<!DOCTYPE.*?>", "", text, flags=re.S | re.I)
        # W3C test metadata uses namespaced elements such as
        # <d:SVGTestCase><d:testDescription><p>... .  The HTML parser treats
        # the XHTML <p> as HTML and can break out of the SVG namespace, which
        # makes the whole document render blank.  The metadata is not part of
        # the rendered document, so drop it.
        text = re.sub(r"<d:SVGTestCase\b.*?</d:SVGTestCase>",
                      "", text, flags=re.S)
        text = re.sub(r"</?d:[A-Za-z][\w:-]*\b[^>]*>", "", text)
        html = os.path.join(outdir, name + ".inline.html")
        out = os.path.join(outdir, name + ".chrome.png")
        with open(html, "w", encoding="utf-8") as f:
            f.write("<!doctype html><meta charset=utf-8>"
                    "<style>html,body{margin:0;padding:0;background:transparent;"
                    "overflow:hidden}svg{display:block;width:%dpx;height:%dpx}"
                    "%s</style>%s" % (size[0], size[1], self.extra_css, text))
        cmd = [self.exe, "--headless", "--disable-gpu", "--no-sandbox",
               "--hide-scrollbars", "--force-device-scale-factor=1",
               "--default-background-color=00000000",
               "--allow-file-access-from-files",
               "--screenshot=" + out, "--window-size=%d,%d" % size,
               "file://" + html]
        try:
            subprocess.run(cmd, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=60)
        except subprocess.TimeoutExpired:
            raise RuntimeError("timeout")
        if not os.path.exists(out) or os.path.getsize(out) < 24:
            raise RuntimeError("no output")
        return out


class InkscapeBackend(Backend):
    name = "inkscape"
    CANDIDATES = ["inkscape", "/Applications/Inkscape.app/Contents/MacOS/inkscape"]

    @classmethod
    def probe(cls):
        for cand in cls.CANDIDATES:
            path = cand if os.path.isabs(cand) else shutil.which(cand)
            if path and os.path.exists(path):
                try:
                    r = subprocess.run([path, "--version"], capture_output=True,
                                       text=True, timeout=30)
                except (OSError, subprocess.TimeoutExpired):
                    continue
                if r.returncode == 0:
                    b = cls()
                    b.exe = path
                    b.available = True
                    b.detail = (r.stdout or r.stderr).strip().splitlines()[0][:120]
                    return b
        b = cls()
        b.detail = "inkscape not installed"
        return b

    def render(self, svg_path, size, outdir, tmp):
        name = os.path.splitext(os.path.basename(svg_path))[0]
        out = os.path.join(outdir, name + ".inkscape.png")
        cmd = [self.exe, "--export-type=png", "--export-filename=" + out,
               "--export-width=%d" % size[0], "--export-height=%d" % size[1],
               "--export-background-opacity=0", svg_path]
        try:
            r = subprocess.run(cmd, capture_output=True, timeout=90)
        except subprocess.TimeoutExpired:
            raise RuntimeError("timeout")
        if not os.path.exists(out) or os.path.getsize(out) < 24:
            raise RuntimeError("no output: " + r.stderr.decode("utf-8", "replace")[:160])
        return out


class ResvgBackend(Backend):
    name = "resvg"
    CANDIDATES = ["resvg"]

    @classmethod
    def probe(cls):
        path = shutil.which("resvg")
        if path:
            try:
                r = subprocess.run([path, "--version"], capture_output=True,
                                   text=True, timeout=30)
                if r.returncode == 0:
                    b = cls()
                    b.exe = path
                    b.available = True
                    b.detail = (r.stdout or r.stderr).strip()[:120]
                    return b
            except (OSError, subprocess.TimeoutExpired):
                pass
        b = cls()
        b.detail = "resvg not installed"
        return b

    def render(self, svg_path, size, outdir, tmp):
        name = os.path.splitext(os.path.basename(svg_path))[0]
        out = os.path.join(outdir, name + ".resvg.png")
        try:
            r = subprocess.run([self.exe, "--zoom", "1", "--output", out, svg_path],
                               capture_output=True, timeout=90)
        except subprocess.TimeoutExpired:
            raise RuntimeError("timeout")
        if not os.path.exists(out) or os.path.getsize(out) < 24:
            raise RuntimeError("no output")
        return out


class QlmanageBackend(Backend):
    name = "qlmanage"
    CANDIDATES = ["qlmanage"]
    exact = False
    # QuickLook renders a thumbnail whose scale and margin we cannot control,
    # so its pixels cannot be compared 1:1.  It is opt-in: useful as a coarse
    # "does it draw anything at all" smoke test, not as a conformance oracle.
    default = False

    @classmethod
    def probe(cls):
        path = shutil.which("qlmanage")
        if path:
            b = cls()
            b.exe = path
            b.available = True
            b.detail = "macOS QuickLook (WebKit)"
            return b
        b = cls()
        b.detail = "qlmanage unavailable (not macOS)"
        return b

    def render(self, svg_path, size, outdir, tmp):
        name = os.path.splitext(os.path.basename(svg_path))[0]
        sub = os.path.join(outdir, name + "_ql")
        shutil.rmtree(sub, ignore_errors=True)
        os.makedirs(sub, exist_ok=True)
        # QuickLook ignores width/height and renders a thumbnail whose scale
        # depends on the requested size in a way we cannot control.  Ask for a
        # fixed side; the comparison resamples by ink bbox (exact=False).
        side = max(size)
        src = rewrite_root(svg_path, sub, side)
        try:
            subprocess.run([self.exe, "-t", "-s", str(side), "-o", sub, src],
                           capture_output=True, timeout=90)
        except subprocess.TimeoutExpired:
            raise RuntimeError("timeout")
        out = os.path.join(sub, os.path.basename(src) + ".png")
        if not os.path.exists(out) or os.path.getsize(out) < 24:
            raise RuntimeError("no output")
        return out


BACKENDS = [ChromeBackend, ChromeInlineBackend, InkscapeBackend,
            ResvgBackend, QlmanageBackend]


# Documented renderer differences.  Keyed by (backend, case) or (backend, None)
# for a backend-wide reason.  These are *expected*: a bitmap 5x7 font will not
# match system font shaping, and this renderer deliberately ignores <style>.
# They are still compared and counted, but reported under "known differences"
# so a new, real mismatch is not buried among them.
KNOWN = {
    ("*", "text_basic"): "bitmap 5x7 font vs system font metrics",
    ("*", "text_anchor"): "bitmap 5x7 font vs system font metrics",
    ("*", "text_spacing"): "bitmap 5x7 font vs system font metrics",
    ("*", "text_fontsize"): "bitmap 5x7 font vs system font metrics",
    ("*", "text_letterspacing"): "bitmap 5x7 font vs system font metrics",
    ("*", "style_element"): "this renderer ignores <style> by design",
}


def known_reason(backend, case):
    return KNOWN.get(("*", case)) or KNOWN.get((backend, case))


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------

def bbox_alpha(px, w, h, thresh=8):
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            if px[row + x * 4 + 3] > thresh:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    return (x0, y0, x1, y1) if x1 >= 0 else None


def bbox_nonwhite(px, w, h, thresh=8):
    """Bounding box of pixels that differ from opaque white."""
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            o = row + x * 4
            if max(abs(px[o + k] - 255) for k in range(3)) > thresh:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    return (x0, y0, x1, y1) if x1 >= 0 else None


def has_alpha(px, thresh=250):
    for i in range(3, len(px), 4):
        if px[i] < thresh:
            return True
    return False


def over_white(px, w, h):
    """Flatten an RGBA buffer over opaque white; returns a new buffer."""
    out = bytearray(px)
    for i in range(0, len(out), 4):
        a = out[i + 3]
        if a == 255:
            continue
        out[i] = (out[i] * a + 255 * (255 - a)) // 255
        out[i + 1] = (out[i + 1] * a + 255 * (255 - a)) // 255
        out[i + 2] = (out[i + 2] * a + 255 * (255 - a)) // 255
        out[i + 3] = 255
    return bytes(out)


def flat(px, w, h, x, y, rad=2):
    """True when the (2*rad+1)^2 neighbourhood is opaque and colour-flat.

    Used to reject antialiased boundaries and hard colour edges on *both*
    sides of a comparison: two correct rasterisers differ there legitimately.
    """
    amin, amax = 255, 0
    cmin = [255, 255, 255]
    cmax = [0, 0, 0]
    for dy in range(-rad, rad + 1):
        yy = y + dy
        if yy < 0 or yy >= h:
            return False
        row = yy * w * 4
        for dx in range(-rad, rad + 1):
            xx = x + dx
            if xx < 0 or xx >= w:
                return False
            q = row + xx * 4
            av = px[q + 3]
            if av < amin:
                amin = av
            if av > amax:
                amax = av
            for k in range(3):
                v = px[q + k]
                if v < cmin[k]:
                    cmin[k] = v
                if v > cmax[k]:
                    cmax[k] = v
    if amax == 0 or amax - amin > 4:
        return False
    return max(cmax[k] - cmin[k] for k in range(3)) <= 16


def normalize(w, h, px, n=64):
    """Resample the ink bbox of *px* onto an n x n grid.

    QuickLook picks its own thumbnail scale and adds an unpredictable margin,
    so absolute pixel coordinates are meaningless for it.  Normalising both
    images to the same grid by their own ink bbox compares *shape and colour*
    instead, which is enough to catch a wrong clip, a missing fill or a wrong
    colour -- and is explicitly approximate.
    """
    bb = bbox_alpha(px, w, h) if has_alpha(px) else bbox_nonwhite(px, w, h)
    if bb is None:
        return None
    x0, y0, x1, y1 = bb
    cw, ch = x1 - x0, y1 - y0
    if cw <= 0 or ch <= 0:
        return None
    out = bytearray(n * n * 4)
    for y in range(n):
        sy = y0 + (y * ch) // n
        if sy >= h:
            sy = h - 1
        for x in range(n):
            sx = x0 + (x * cw) // n
            if sx >= w:
                sx = w - 1
            si = (sy * w + sx) * 4
            di = (y * n + x) * 4
            out[di:di + 4] = px[si:si + 4]
    return n, n, bytes(out)


def compare_approx(name, mw, mh, mpx, rw, rh, rpx, tol, backend):
    """Shape/colour comparison for backends with an uncontrollable scale."""
    # Flatten both over white: QuickLook composites on white, Chrome may keep
    # alpha, and at thumbnail scale coverage edges are noise anyway.  Comparing
    # composited colour is what catches a wrong fill, clip or colour.
    if has_alpha(mpx) or has_alpha(rpx):
        mpx = over_white(mpx, mw, mh)
        rpx = over_white(rpx, rw, rh)
    a = normalize(mw, mh, mpx)
    b = normalize(rw, rh, rpx)
    res = {"case": name, "backend": backend, "verdict": "approx-pass",
           "exact": False}
    if a is None or b is None:
        res["verdict"] = "approx-empty"
        res["detail"] = "empty ink bbox"
        return res
    _, _, ap = a
    _, _, bp = b
    n = 64
    bad = 0
    total = n * n
    first = None
    maxd = 0
    for i in range(0, len(ap), 4):
        d = 0
        for k in range(3):
            d = max(d, abs(ap[i + k] - bp[i + k]))
        if d > tol:
            bad += 1
            if d > maxd:
                maxd = d
            if first is None:
                p = i // 4
                first = (p % n, p // n, list(ap[i:i + 3]), list(bp[i:i + 3]))
    res.update({"total": total, "bad": bad, "maxdelta": round(maxd, 1)})
    if bad > total * 0.10:
        res["verdict"] = "approx-fail"
        res["detail"] = "%d/%d cells differ (max d=%d)" % (bad, total, maxd)
        if first:
            res["first"] = {"xy": [first[0], first[1]],
                            "mine": first[2], "ref": first[3]}
    else:
        res["detail"] = "%d/%d cells over tol" % (bad, total)
    return res


def classify(name, mw, mh, mpx, rw, rh, rpx, tol, backend):
    """Compare one case.  Returns a dict describing the outcome."""
    res = {"case": name, "backend": backend, "verdict": "pass"}
    if (mw, mh) != (rw, rh):
        # Chrome screenshots the viewport; a size mismatch usually means the
        # harness (not the renderer) sized the wrapper wrong.
        res["verdict"] = "skip-size"
        res["detail"] = "mine=%dx%d ref=%dx%d" % (mw, mh, rw, rh)
        return res
    # QuickLook composites on white and drops alpha.  Chromium preserves it.
    # Normalise: flatten both over white when either side has transparency, so
    # the comparison is about colour, not about who composites.
    if has_alpha(rpx) or has_alpha(mpx):
        rpx = over_white(rpx, rw, rh)
        mpx = over_white(mpx, mw, mh)
    mb = bbox_alpha(mpx, mw, mh)
    rb = bbox_alpha(rpx, rw, rh)
    if mb is None and rb is None:
        res["verdict"] = "skip-empty"
        res["detail"] = "both empty"
        return res
    if (mb is None) != (rb is None):
        res["verdict"] = "fail-bbox"
        res["detail"] = "mine=%s ref=%s" % (mb, rb)
        return res
    if max(abs(mb[k] - rb[k]) for k in range(4)) > 2:
        res["verdict"] = "fail-bbox"
        res["detail"] = "bbox mine=%s ref=%s" % (mb, rb)
        return res

    bad = total = 0
    first = None
    maxd = 0
    for y in range(max(0, rb[1] - 1), min(mh, rb[3] + 2)):
        for x in range(max(0, rb[0] - 1), min(mw, rb[2] + 2)):
            # Interior test, applied to *both* images: require a 5x5
            # neighbourhood that is fully opaque and flat.  This excludes
            # antialiased boundaries and hard colour edges on either side,
            # which is where rasterisers legitimately disagree (different AA,
            # different curve flattening) without assuming a rectangular shape.
            if not flat(mpx, mw, mh, x, y) or not flat(rpx, rw, rh, x, y):
                continue
            o = (y * rw + x) * 4
            mine = [mpx[o + k] for k in range(3)]
            ref = [rpx[o + k] for k in range(3)]
            d = max(abs(ref[k] - mine[k]) for k in range(3))
            total += 1
            if d > tol:
                bad += 1
                if d > maxd:
                    maxd = d
                if first is None:
                    first = (x, y, tuple(mine), tuple(ref))
    if total < MIN_INTERIOR:
        res["verdict"] = "skip-thin"
        res["detail"] = "only %d comparable pixels" % total
        return res
    res["total"] = total
    res["bad"] = bad
    res["maxdelta"] = round(maxd, 1)
    if bad > total * BAD_FRACTION:
        res["verdict"] = "fail-pixels"
        res["detail"] = "%d/%d interior pixels differ (max d=%d)" % (bad, total, maxd)
        if first:
            res["first"] = {"xy": list(first[:2]), "mine": list(first[2]),
                            "ref": list(first[3])}
    elif bad:
        res["detail"] = "%d/%d pixels over tol (within %.1f%% budget)" % (
            bad, total, BAD_FRACTION * 100)
    return res


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def read_size(path):
    """Pull width/height out of the root <svg> tag, in px."""
    head = open(path, "r", errors="replace").read(4096)
    m = re.search(r"<svg\b[^>]*>", head)
    if not m:
        return (100, 100)
    tag = m.group(0)

    def attr(name):
        mm = re.search(r'\b%s\s*=\s*"([^"]*)"' % name, tag)
        return mm.group(1) if mm else None

    def px(v, dflt):
        if v is None:
            return dflt
        v = v.strip()
        mm = re.match(r"^([0-9]*\.?[0-9]+)\s*(px|pt|pc|in|cm|mm)?$", v)
        if not mm:
            return dflt
        n = float(mm.group(1))
        unit = mm.group(2) or "px"
        k = {"px": 1.0, "pt": 96.0 / 72, "pc": 16.0, "in": 96.0,
             "cm": 96.0 / 2.54, "mm": 96.0 / 25.4}[unit]
        return max(1, int(round(n * k)))

    return (px(attr("width"), 100), px(attr("height"), 100))


def rewrite_root(svg_path, outdir, side):
    """Copy *svg_path* into *outdir* with an explicit square root size.

    QuickLook ignores width/height and picks its own thumbnail scale, so we
    pin the root to ``side x side`` (keeping the viewBox) to make the pixel
    mapping predictable.  Returns the new path.
    """
    text = open(svg_path, "r", errors="replace").read()
    m = re.search(r"<svg\b[^>]*>", text)
    if not m:
        raise RuntimeError("no <svg> root")
    tag = re.sub(r'\s(width|height)="[^"]*"', "", m.group(0))
    tag = tag[:-1] + ' width="%d" height="%d">' % (side, side)
    out = os.path.join(outdir, os.path.basename(svg_path))
    with open(out, "w") as f:
        f.write(text[:m.start()] + tag + text[m.end():])
    return out


def run(args):
    requested = set(args.backends or [])
    backends = []
    for cls in BACKENDS:
        if requested:
            if cls.name not in requested:
                continue
        elif not cls.default:
            # Probed for --list, but not used unless named explicitly.
            continue
        backends.append(cls.probe())

    if args.list:
        for b in backends:
            print("%-10s %-9s %s" % (b.name, "yes" if b.available else "no",
                                     b.detail))
        for cls in BACKENDS:
            if cls.default or (requested and cls.name not in requested):
                continue
            b = cls.probe()
            print("%-10s %-9s %s (opt-in: not pixel-comparable)"
                  % (b.name, "yes" if b.available else "no", b.detail))
        return 0

    usable = [b for b in backends if b.available]
    print("external renderers:")
    for b in backends:
        print("  %-10s %-9s %s" % (b.name, "yes" if b.available else "no", b.detail))
    if not usable:
        print("\nno external renderer available; nothing to compare "
              "(install chromium, inkscape or resvg, or run on macOS for qlmanage)")
        return 0

    corpus = sorted(os.path.join(args.corpus, f)
                    for f in os.listdir(args.corpus) if f.endswith(".svg"))
    if args.limit:
        corpus = corpus[:args.limit]
    if not corpus:
        print("no SVG cases in %s" % args.corpus)
        return 1

    tmp = tempfile.mkdtemp(prefix="svg2img_diff_")
    results = []
    counts = {}
    for b in usable:
        counts[b.name] = {"pass": 0, "skip": 0, "fail": 0, "known": 0,
                            "approx": 0}

    t0 = time.time()
    for path in corpus:
        name = os.path.splitext(os.path.basename(path))[0]
        size = read_size(path)
        outdir = os.path.join(tmp, name)
        os.makedirs(outdir, exist_ok=True)
        mine_png = os.path.join(outdir, "mine.png")
        try:
            r = subprocess.run([args.bin, "-a", "4", path, mine_png],
                               capture_output=True, timeout=60)
            if r.returncode != 0:
                for b in usable:
                    results.append({"case": name, "backend": b.name,
                                    "verdict": "fail-render",
                                    "detail": "svg2img rc=%d" % r.returncode})
                    counts[b.name]["fail"] += 1
                continue
            mw, mh, mpx = read_png(mine_png)
        except Exception as e:
            for b in usable:
                results.append({"case": name, "backend": b.name,
                                "verdict": "fail-render", "detail": repr(e)[:120]})
                counts[b.name]["fail"] += 1
            continue

        for b in usable:
            try:
                ref = b.render(path, size, outdir, tmp)
                rw, rh, rpx = read_png(ref)
            except Exception as e:
                results.append({"case": name, "backend": b.name,
                                "verdict": "skip-tool",
                                "detail": "%s" % (str(e)[:120])})
                counts[b.name]["skip"] += 1
                continue
            if not b.exact:
                res = compare_approx(name, mw, mh, mpx, rw, rh, rpx,
                                     args.tol, b.name)
            else:
                res = classify(name, mw, mh, mpx, rw, rh, rpx, args.tol, b.name)
            reason = known_reason(b.name, name)
            if reason:
                res["known"] = reason
            results.append(res)
            v = res["verdict"]
            if v.startswith("approx-fail"):
                if reason:
                    counts[b.name]["known"] += 1
                else:
                    counts[b.name]["approx"] += 1
            elif v.startswith("fail"):
                if reason:
                    counts[b.name]["known"] += 1
                else:
                    counts[b.name]["fail"] += 1
            elif v.startswith("skip"):
                counts[b.name]["skip"] += 1
            else:
                counts[b.name]["pass"] += 1

    dt = time.time() - t0
    report = build_report(results, counts, usable, args, dt)
    if args.md:
        open(args.md, "w").write(report["markdown"])
        print("\nwrote %s" % args.md)
    else:
        print("\n" + report["markdown"])
    if args.json:
        open(args.json, "w").write(json.dumps(report["json"], indent=2) + "\n")
        print("wrote %s" % args.json)

    nfail = sum(c["fail"] for c in counts.values())
    print("\n%d cases x %d backend(s) in %.1fs: %d mismatch group(s)"
          % (len(corpus), len(usable), dt, nfail))
    return 1 if (args.strict and nfail) else 0


def build_report(results, counts, usable, args, dt):
    by_case = {}
    for r in results:
        by_case.setdefault(r["case"], []).append(r)

    lines = ["# svg2img differential triage", "",
             "Machine: %s %s (%s), Python %s" % (
                 platform.system(), platform.release(), platform.machine(),
                 platform.python_version()),
             "Renderer: `%s`" % os.path.relpath(args.bin, ROOT),
             "Backends: " + ", ".join("%s (%s)" % (b.name, b.detail)
                                      for b in usable),
             "Tolerance: %d/255 per channel, interiors only, "
             "%.1f%% pixel budget" % (args.tol, BAD_FRACTION * 100),
             "",
             "| backend | pass | skip | mismatch | approx | known |",
             "|---|---|---|---|---|---|"]
    for b in usable:
        c = counts[b.name]
        lines.append("| %s | %d | %d | %d | %d | %d |"
                     % (b.name, c["pass"], c["skip"], c["fail"],
                        c["approx"], c["known"]))

    fails = [r for r in results
             if r["verdict"].startswith(("fail", "approx-fail"))
             and not r.get("known")]
    known = [r for r in results
             if r["verdict"].startswith(("fail", "approx-fail"))
             and r.get("known")]
    lines += ["", "## Mismatches (%d)" % len(fails), ""]
    if not fails:
        lines.append("No classifiable mismatch. Antialiasing, colour management "
                     "and font metrics are excluded by construction.")
    for r in fails:
        loc = "    - `%s` at (%d,%d): mine=%s ref=%s" % (
            r["case"], r["first"]["xy"][0], r["first"]["xy"][1],
            r["first"]["mine"], r["first"]["ref"]) if r.get("first") else ""
        lines.append("- **%s** [%s] %s -- %s%s" % (
            r["case"], r["backend"], r["verdict"], r.get("detail", ""), loc))

    if known:
        lines += ["", "## Known renderer differences (%d)" % len(known), "",
                  "Counted separately: these are expected to differ and are not",
                  "regressions, but they are listed so a new mismatch stands out.",
                  ""]
        for r in known:
            lines.append("- **%s** [%s]: %s" % (r["case"], r["backend"],
                                                r["known"]))

    skips = [r for r in results if r["verdict"].startswith("skip")]
    if skips:
        kinds = {}
        for r in skips:
            kinds.setdefault(r["verdict"], 0)
            kinds[r["verdict"]] += 1
        lines += ["", "## Skipped comparisons (%d)" % len(skips), ""]
        for k, n in sorted(kinds.items()):
            lines.append("- %s: %d" % (k, n))

    lines += ["", "## Known renderer differences (not failures)", "",
              "- Antialiasing of near-vertical edges (2D AA vs scanline coverage).",
              "- Colour management / gamma between Chromium, WebKit and this "
              "renderer.",
              "- Radial `spreadMethod` with an off-centre focal point.",
              "- Text: bitmap 5x7 font vs system font shaping.",
              "- QuickLook always returns a square thumbnail; non-square cases "
              "are compared on their common region only.",
              ""]
    return {"markdown": "\n".join(lines),
            "json": {"machine": {"system": platform.system(),
                                 "machine": platform.machine(),
                                 "release": platform.release(),
                                 "python": platform.python_version()},
                     "bin": args.bin, "tol": args.tol,
                     "seconds": round(dt, 2),
                     "backends": [{"name": b.name, "detail": b.detail,
                                   "available": b.available} for b in usable],
                     "counts": counts, "results": results}}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--corpus", default=DEFAULT_CORPUS,
                   help="directory of SVG cases (default: conformance/cases)")
    p.add_argument("--bin", default=os.path.join(ROOT, "svg2img"))
    p.add_argument("--backends", default="",
                   help="comma list: chrome,inkscape,resvg,qlmanage")
    p.add_argument("--list", action="store_true", help="probe backends and exit")
    p.add_argument("--strict", action="store_true",
                   help="exit 1 when a classifiable mismatch is found")
    p.add_argument("--json", default="", help="write JSON summary here")
    p.add_argument("--md", default="", help="write markdown triage here")
    p.add_argument("--tol", type=int, default=TOL)
    p.add_argument("--limit", type=int, default=0, help="only the first N cases")
    args = p.parse_args(argv)
    if args.backends:
        args.backends = [s.strip() for s in args.backends.split(",") if s.strip()]
    if not os.path.isdir(args.corpus):
        print("corpus not found: %s" % args.corpus, file=sys.stderr)
        return 2
    return run(args)


if __name__ == "__main__":
    sys.exit(main())