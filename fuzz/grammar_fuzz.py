#!/usr/bin/env python3
"""Grammar-aware SVG fuzzer.

The existing mutators are token-level and the raw fuzzer is byte-level; both
under-sample the *grammar* corners that the XML parser and SVG attribute
parsers mishandle.  This generator emits documents built around:

  * XML grammar: DOCTYPE internal subsets, CDATA, PIs, comments, entities,
    duplicate attributes, namespaces, BOM/UTF-8/invalid bytes/NUL, long names.
  * Reference graphs: nested/forward use, use with children, symbol viewports,
    gradient href chains, clipPath inside use/symbol, duplicate ids, cycles.
  * Numeric/path grammar: hex floats, subnormals, huge exponents, NaN/inf,
    arc flag forms, extreme radii, dash arrays, miter limits, singular CTMs.

CSS is intentionally not supported: `<style>` content is treated as opaque
text and must never crash the parser.  Presentation attributes and inline
`style="..."` declarations are still exercised.

Output is a corpus for the existing crash runner:

    python3 fuzz/grammar_fuzz.py OUT [count] [seed]
    ls OUT/*.svg > list.txt
    ./fuzz_run 2 < list.txt

Files are tagged in ``OUT/manifest.txt`` so a crash can be traced to a feature.
"""
import os
import random
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "grammar"
COUNT = int(sys.argv[2]) if len(sys.argv) > 2 else 600
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1
rng = random.Random(SEED)
os.makedirs(OUT, exist_ok=True)


def doc(body, w=200, h=200, defs=""):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
            'viewBox="0 0 %d %d">%s%s</svg>' % (w, h, w, h, defs, body))


# ---------------------------------------------------------------------------
# Hand-written XML grammar corners (bytes: some are deliberately invalid)
# ---------------------------------------------------------------------------

XML_RAW = [
    b'<!DOCTYPE svg [ <!ENTITY x "a>b"> ]><svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"/></svg>',
    b'<!DOCTYPE svg PUBLIC "a>b" "c"><svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"/></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><desc><![CDATA[ <not markup> & stuff ]]></desc><rect width="20" height="20" fill="red"/></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><text x="2" y="15" font-size="10"><![CDATA[ A &amp; B ]]></text></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><text x="2" y="15" font-size="10">&amp;lt; &bogus; &#0; &#x110000; &#x; &nbsp;</text></svg>',
    b'<?xml version="1.0" encoding="UTF-8"?><svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"/></svg>',
    b'\xef\xbb\xbf<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"/></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red" fill="blue"/></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill=red/></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><g><rect width="20" height="20" fill="red"></g></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"/></svg></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><!-- a -- b --><rect width="20" height="20" fill="red"/></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"/><!-- unterminated</svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"/></svg>',
    b'<svg:rect xmlns:svg="http://www.w3.org/2000/svg" width="20" height="20" fill="red"/>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red" a="' + b'x' * 5000 + b'"/></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><' + b'a' * 5000 + b' width="20" height="20"/></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><text x="2" y="15" font-size="10">\xff\xfe invalid utf8 \xc3</text></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><text x="2" y="15" font-size="10">a\x00b</text></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"/><![CDATA[ ]]></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"/></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"></rect ></svg>',
    b'<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><rect width="20" height="20" fill="red"/></svg>',
]


# ---------------------------------------------------------------------------
# Hand-written reference-graph corners
# ---------------------------------------------------------------------------

REF_BODIES = [
    # use with children: children should be ignored, and must not leak
    '<defs><rect id="r" width="10" height="10" fill="red"/></defs>'
    '<use href="#r"><rect width="5" height="5" fill="blue"/></use>',
    # nested use, target first
    '<defs><rect id="r" width="10" height="10" fill="red"/>'
    '<g id="g"><use href="#r"/><rect x="20" width="10" height="10" fill="blue"/></g></defs>'
    '<use href="#g" x="5" y="5"/>',
    # nested use, forward reference
    '<use href="#g" x="5" y="5"/>'
    '<defs><rect id="r" width="10" height="10" fill="red"/>'
    '<g id="g"><use href="#r"/><rect x="20" width="10" height="10" fill="blue"/></g></defs>',
    # symbol with viewport
    '<defs><symbol id="s" viewBox="0 0 10 10">'
    '<rect width="10" height="10" fill="red"/></symbol></defs>'
    '<use href="#s" x="10" y="10" width="80" height="80"/>',
    # gradient href chain, forward
    '<defs><linearGradient id="g3" href="#g2"/>'
    '<linearGradient id="g2" href="#g1"/>'
    '<linearGradient id="g1"><stop offset="0" stop-color="red"/>'
    '<stop offset="1" stop-color="blue"/></linearGradient></defs>'
    '<rect width="100" height="100" fill="url(#g3)"/>',
    # gradient href chain, backward
    '<defs><linearGradient id="g1"><stop offset="0" stop-color="red"/>'
    '<stop offset="1" stop-color="blue"/></linearGradient>'
    '<linearGradient id="g2" href="#g1"/>'
    '<linearGradient id="g3" href="#g2"/></defs>'
    '<rect width="100" height="100" fill="url(#g3)"/>',
    # clipPath inside a symbol used by use
    '<defs><clipPath id="c"><rect width="5" height="5"/></clipPath>'
    '<symbol id="s"><rect width="10" height="10" fill="red" clip-path="url(#c)"/></symbol></defs>'
    '<use href="#s"/>',
    # duplicate ids
    '<defs><rect id="d" width="10" height="10" fill="red"/>'
    '<rect id="d" width="10" height="10" fill="blue"/></defs>'
    '<use href="#d"/>',
    # self reference
    '<g id="s"><use href="#s"/></g>',
    # two-node cycle
    '<g id="a"><use href="#b"/></g><g id="b"><use href="#a"/></g>',
    # use referencing defs / gradient / clipPath
    '<defs><linearGradient id="g"><stop offset="0" stop-color="red"/>'
    '<stop offset="1" stop-color="blue"/></linearGradient>'
    '<clipPath id="c"><rect width="5" height="5"/></clipPath>'
    '<g id="d"><rect width="10" height="10" fill="url(#g)"/></g></defs>'
    '<use href="#d"/><use href="#g"/><use href="#c"/>',
    # use with transform and x/y
    '<defs><rect id="r" width="10" height="10" fill="red"/></defs>'
    '<use href="#r" x="5" y="5" transform="scale(2)"/>',
    # use with style on the use
    '<defs><rect id="r" width="10" height="10"/></defs>'
    '<use href="#r" fill="red" stroke="blue" stroke-width="2"/>',
    # nested svg viewport
    '<svg x="20" y="20" width="50" height="50" viewBox="0 0 10 10">'
    '<rect width="10" height="10" fill="red"/></svg>',
    # symbol at top level (must not render)
    '<symbol id="s"><rect width="100" height="100" fill="green"/></symbol>',
]


# ---------------------------------------------------------------------------
# Numeric / path grammar corners
# ---------------------------------------------------------------------------

NUMS = ["0", "-0", "1", "-1", "0.5", "-.5", "+3", "1.", "1e3", "1E-3",
        "1e400", "-1e400", "1e-400", "3.4e38", "1.17549435e-38",
        "0x1p3", "0x10", "nan", "NaN", "inf", "-inf", "infinity",
        "1_0", "1,0", "", " ", "00012", "1.2.3", "1e", ".", "-", "+"]

PATH_CMDS = "MmLlHhVvCcSsQqTtAaZz"


def rand_num():
    return rng.choice(NUMS) if rng.random() < 0.5 else "%.6g" % rng.uniform(-1e3, 1e3)


def rand_path():
    out = []
    for _ in range(rng.randint(1, 10)):
        c = rng.choice(PATH_CMDS)
        out.append(c)
        for _ in range(rng.randint(0, 7)):
            out.append(rand_num())
            out.append(rng.choice([",", " ", ""]))
    return "".join(out)


def rand_dash():
    return ",".join(rand_num() for _ in range(rng.randint(0, 6)))


def rand_transform():
    t = rng.choice(["translate", "scale", "rotate", "skewX", "skewY", "matrix"])
    n = {"translate": 2, "scale": 2, "rotate": 3, "skewX": 1,
         "skewY": 1, "matrix": 6}[t]
    return "%s(%s)" % (t, ",".join(rand_num() for _ in range(rng.randint(1, n + 1))))


NUM_BODIES = [
    lambda: '<path d="%s" fill="red" stroke="black" stroke-width="2"/>' % rand_path(),
    lambda: '<path d="%s" fill="none" stroke="black" stroke-width="2" stroke-dasharray="%s"/>'
            % (rand_path(), rand_dash()),
    lambda: '<rect x="%s" y="%s" width="%s" height="%s" fill="red"/>'
            % (rand_num(), rand_num(), rand_num(), rand_num()),
    lambda: '<circle cx="%s" cy="%s" r="%s" fill="red"/>'
            % (rand_num(), rand_num(), rand_num()),
    lambda: '<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="red"/>'
            % (rand_num(), rand_num(), rand_num(), rand_num()),
    lambda: '<g transform="%s"><rect width="20" height="20" fill="red"/></g>' % rand_transform(),
    lambda: '<rect width="20" height="20" fill="red" stroke="black" stroke-width="%s" '
            'stroke-miterlimit="%s"/>' % (rand_num(), rand_num()),
    lambda: '<path d="M0,0 L100,0 L100,100 Z" fill="red" fill-rule="%s"/>'
            % rng.choice(["nonzero", "evenodd", "bogus", ""]),
]


def random_body():
    k = rng.randrange(4)
    if k == 0:
        return rng.choice(REF_BODIES)
    if k == 1:
        return rng.choice(NUM_BODIES)()
    if k == 2:
        # reference + numeric
        return rng.choice(REF_BODIES) + rng.choice(NUM_BODIES)()
    # deep-ish group chain with transforms and opacity
    d = rng.randint(1, 40)
    return "<g transform=\"%s\" opacity=\"%s\">" % (rand_transform(), rand_num()) * d + \
           '<rect width="20" height="20" fill="red"/>' + "</g>" * d


def main():
    manifest = []
    n = 0

    # 1. Raw XML grammar cases.
    for i, data in enumerate(XML_RAW):
        path = os.path.join(OUT, "gx%04d.svg" % i)
        with open(path, "wb") as f:
            f.write(data)
        manifest.append((os.path.basename(path), "xml"))
        n += 1

    # 2. Reference-graph cases.
    for i, body in enumerate(REF_BODIES):
        path = os.path.join(OUT, "gr%04d.svg" % i)
        with open(path, "w") as f:
            f.write(doc(body, 200, 200))
        manifest.append((os.path.basename(path), "ref"))
        n += 1

    # 3. Random grammar-aware documents.
    while n < COUNT:
        body = random_body()
        path = os.path.join(OUT, "gm%05d.svg" % n)
        with open(path, "w") as f:
            f.write(doc(body, 200, 200))
        manifest.append((os.path.basename(path), "mix"))
        n += 1

    with open(os.path.join(OUT, "manifest.txt"), "w") as f:
        for name, tag in manifest:
            f.write("%s\t%s\n" % (name, tag))
    print("wrote %d files to %s" % (n, OUT))


if __name__ == "__main__":
    main()