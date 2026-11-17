#!/usr/bin/env python3
"""Raw random-byte fuzzer: writes N random files, biased to look like XML.

This explores the parser's error-recovery paths (truncated tags, bad nesting,
junk between elements) rather than deep rendering, which the structure-aware
mutator covers.  Kept small so the parser's fast-fail paths dominate.
"""
import os
import random
import sys

import tempfile
_HERE = os.path.dirname(os.path.abspath(__file__))
_WORK = os.environ.get("SVG2IMG_FUZZ_WORK",
                       os.path.join(tempfile.gettempdir(), "svg2img_fuzz"))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_WORK, "raw")
N = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1

os.makedirs(OUT, exist_ok=True)
rng = random.Random(SEED)

# Fragments that keep the input looking like markup so the XML scanner
# actually does work instead of failing on byte 0.
FRAGS = [
    b"<svg", b"<svg ", b"<g>", b"</g>", b"<rect", b"<path", b"<circle",
    b"<text>", b"</text>", b"<defs>", b"</defs>", b"<desc>", b"</desc>", b"<metadata>", b"</metadata>",
    b"<clipPath>", b"</clipPath>", b"<linearGradient>", b"</linearGradient>",
    b"<stop", b"<use", b"<polygon", b"<polyline", b"<ellipse", b"<line",
    b"<!--", b"-->", b"<![CDATA[", b"]]>", b"<?xml", b"?>", b"<!DOCTYPE",
    b"<tspan>", b"</tspan>", b"<symbol>", b"</symbol>", b"<title>", b"</title>",
]
ATTRS = [b'fill="red"', b'stroke="blue"', b'x="10"', b'y="20"', b'width="30"',
         b'height="40"', b'd="M0,0 L10,10 Z"', b'transform="scale(2)"',
         b'opacity="0.5"', b'id="a"', b'class="c"', b'points="0,0 1,1 2,0"',
         b'cx="5"', b'cy="6"', b'r="7"', b'viewBox="0 0 10 10"', b'd=""',
         b'fill="url(#g)"', b'clip-path="url(#c)"', b'href="#a"', b'="x"',
         b"='y'", b'=', b'""', b"''", b'fill=', b'x="1e400"', b'y="NaN"']
JUNK = [b"<", b">", b"/", b"&", b";", b"\"", b"'", b"=", b" ", b"\n", b"\t",
        b"\x00", b"\xff", b"\xc3", b"\xa9", b"<!--", b"-->", b"<!", b"<?",
        b"]]>", b"\xef\xbb\xbf"]

def gen():
    parts = []
    if rng.random() < 0.8:
        parts.append(b'<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">')
    for _ in range(rng.randint(1, 120)):
        k = rng.randrange(10)
        if k < 5:
            parts.append(rng.choice(FRAGS))
        elif k < 8:
            parts.append(rng.choice(ATTRS))
        elif k < 9:
            parts.append(rng.choice(JUNK))
        else:
            parts.append(bytes(rng.randrange(256) for _ in range(rng.randint(1, 8))))
    if rng.random() < 0.7:
        parts.append(b"</svg>")
    return b"".join(parts)

for i in range(N):
    data = gen()
    # Occasionally pad to a larger size to stress buffers.
    if rng.random() < 0.15:
        data = data * rng.randint(2, 20)
    with open(os.path.join(OUT, "r%06d.svg" % i), "wb") as f:
        f.write(data)

print(f"wrote {N} raw files to {OUT}")