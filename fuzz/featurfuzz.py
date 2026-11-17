#!/usr/bin/env python3
"""Feature-targeted SVG generator.

The structure-aware mutator mostly preserves whole documents; this generator
instead builds adversarial-but-valid documents around individual features:
the path grammar, presentation attributes, colour syntax, transform lists,
dash arrays, and id/use reference graphs (including cycles and forward refs).

Usage: python3 featurfuzz.py <out_dir> <count> [seed]
"""
import os
import random
import sys

OUT = sys.argv[1]
COUNT = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1
rng = random.Random(SEED)
os.makedirs(OUT, exist_ok=True)

NUM = ["0", "-0", "1", "-1", "0.5", "-.5", "+3", "1.", "1e3", "1E-3", "1e400",
       "-1e400", "1e-400", "3.4e38", "1.17549435e-38", "NaN", "inf", "-inf",
       "0x10", "1_0", "1,0", "", " ", "00012", "1.2.3", "1e", ".", "-", "+"]
COLORS = ["red", "blue", "rebeccapurple", "transparent", "currentColor",
          "#f00", "#ff0000", "#ff000080", "#f00f", "rgb(1,2,3)",
          "rgb(300,-5,3.5)", "rgba(1,2,3,0.5)", "hsl(120,50%,50%)",
          "hsla(120,50%,50%,0.3)", "hsl(400,-10%,150%)", "notacolor", "",
          "#", "#gggggg", "rgb()", "hsl(1,2)"]
TRANSFORMS = ["translate", "scale", "rotate", "skewX", "skewY", "matrix"]


def num():
    return rng.choice(NUM) if rng.random() < 0.35 else "%.4g" % rng.uniform(-500, 500)


def path_data():
    out = []
    x = y = 0.0
    cmds = "MmLlHhVvCcSsQqTtAaZz"
    for _ in range(rng.randint(1, 14)):
        c = rng.choice(cmds)
        out.append(c)
        k = rng.randint(0, 6)
        if c in "Mm":
            k = rng.randint(0, 3)
        elif c in "LlTt":
            k = rng.randint(0, 3)
        elif c in "HhVv":
            k = rng.randint(0, 2)
        elif c in "Cc":
            k = rng.randint(0, 7)
        elif c in "SsQq":
            k = rng.randint(0, 5)
        elif c in "Aa":
            k = rng.randint(0, 8)
        for _ in range(k):
            out.append(num())
            if rng.random() < 0.5:
                out.append(",")
            else:
                out.append(" ")
    return "".join(out)


def style_decls():
    props = ["fill", "stroke", "stroke-width", "opacity", "fill-opacity",
             "stroke-opacity", "stroke-linecap", "stroke-linejoin",
             "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset",
             "fill-rule", "clip-rule", "color", "font-size", "display",
             "visibility", "stop-color", "stop-opacity"]
    out = []
    for _ in range(rng.randint(0, 5)):
        p = rng.choice(props)
        if p in ("fill", "stroke", "color", "stop-color"):
            v = rng.choice(COLORS)
        elif p in ("opacity", "fill-opacity", "stroke-opacity", "stop-opacity"):
            v = "%.3g" % rng.uniform(-1, 2)
        elif p in ("stroke-width", "font-size", "stroke-miterlimit", "stroke-dashoffset"):
            v = num()
        elif p == "stroke-dasharray":
            v = ",".join(num() for _ in range(rng.randint(0, 4)))
        elif p in ("stroke-linecap",):
            v = rng.choice(["butt", "round", "square", "bogus", ""])
        elif p in ("stroke-linejoin",):
            v = rng.choice(["miter", "round", "bevel", "bogus", ""])
        elif p in ("fill-rule", "clip-rule"):
            v = rng.choice(["nonzero", "evenodd", "bogus", ""])
        else:
            v = rng.choice(["none", "inline", "block", "bogus", ""])
        out.append(f"{p}:{v}")
        if rng.random() < 0.1:
            out.append("!important")
        out.append(";")
    return "".join(out)


def transform():
    k = rng.randint(1, 3)
    parts = []
    for _ in range(k):
        t = rng.choice(TRANSFORMS)
        n = {"translate": rng.randint(1, 2), "scale": rng.randint(1, 2),
             "rotate": rng.randint(1, 3), "skewX": 1, "skewY": 1,
             "matrix": 6}[t]
        parts.append(f"{t}({','.join(num() for _ in range(n))})")
    return " ".join(parts)


def style_attr():
    a = []
    if rng.random() < 0.7:
        a.append('fill="%s"' % rng.choice(COLORS))
    if rng.random() < 0.5:
        a.append('stroke="%s"' % rng.choice(COLORS))
    if rng.random() < 0.5:
        a.append('stroke-width="%s"' % num())
    if rng.random() < 0.3:
        a.append('style="%s"' % style_decls())
    if rng.random() < 0.3:
        a.append('transform="%s"' % transform())
    if rng.random() < 0.2:
        a.append('opacity="%.3g"' % rng.uniform(-1, 2))
    if rng.random() < 0.2:
        a.append('stroke-dasharray="%s"' % ",".join(num() for _ in range(rng.randint(0, 4))))
    if rng.random() < 0.15:
        a.append('clip-path="url(#%s)"' % rng.choice(["a", "b", "missing", ""]))
    return (" " + " ".join(a)) if a else ""


def shape():
    k = rng.randint(0, 6)
    if k == 0:
        return f'<rect x="{num()}" y="{num()}" width="{num()}" height="{num()}"{style_attr()}/>'
    if k == 1:
        return f'<circle cx="{num()}" cy="{num()}" r="{num()}"{style_attr()}/>'
    if k == 2:
        return f'<ellipse cx="{num()}" cy="{num()}" rx="{num()}" ry="{num()}"{style_attr()}/>'
    if k == 3:
        return f'<line x1="{num()}" y1="{num()}" x2="{num()}" y2="{num()}"{style_attr()}/>'
    if k == 4:
        pts = " ".join(f"{num()},{num()}" for _ in range(rng.randint(0, 8)))
        return f'<{"polygon" if rng.random()<0.5 else "polyline"} points="{pts}"{style_attr()}/>'
    if k == 5:
        return f'<path d="{path_data()}"{style_attr()}/>'
    return f'<text x="{num()}" y="{num()}" font-size="{num()}"{style_attr()}>t</text>'


def defs(ids):
    parts = []
    for i in range(rng.randint(0, 3)):
        k = rng.randint(0, 3)
        gid = f"g{i}"
        stops = "".join(
            f'<stop offset="{num()}" stop-color="{rng.choice(COLORS)}" '
            f'stop-opacity="{num()}"/>' for _ in range(rng.randint(0, 4)))
        if k == 0:
            parts.append(f'<linearGradient id="{gid}" x1="{num()}" y1="{num()}" '
                         f'x2="{num()}" y2="{num()}" '
                         f'gradientUnits="{rng.choice(["objectBoundingBox","userSpaceOnUse","bogus"])}" '
                         f'spreadMethod="{rng.choice(["pad","reflect","repeat","bogus"])}">{stops}</linearGradient>')
        elif k == 1:
            parts.append(f'<radialGradient id="{gid}" cx="{num()}" cy="{num()}" r="{num()}" '
                         f'fx="{num()}" fy="{num()}" '
                         f'gradientUnits="{rng.choice(["objectBoundingBox","userSpaceOnUse","bogus"])}">{stops}</radialGradient>')
        elif k == 2:
            parts.append(f'<clipPath id="c{i}" clipPathUnits="{rng.choice(["userSpaceOnUse","objectBoundingBox","bogus"])}">'
                         f'{shape()}</clipPath>')
        else:
            parts.append(f'<g id="u{i}">{shape()}</g>')
        ids.append(gid)
    return "<defs>" + "".join(parts) + "</defs>" if parts else ""


def body():
    ids = []
    parts = []
    if rng.random() < 0.7:
        parts.append(defs(ids))
    # reference graph: uses, fills and clips can point at anything
    refs = ids + ["missing", "", "a", "b"]
    for _ in range(rng.randint(1, 10)):
        k = rng.random()
        if k < 0.25 and ids:
            x, y = num(), num()
            parts.append(f'<use href="#{rng.choice(refs)}" x="{x}" y="{y}"/>')
        elif k < 0.5:
            parts.append(f'<rect x="{num()}" y="{num()}" width="{num()}" height="{num()}" '
                         f'fill="url(#{rng.choice(refs)})" clip-path="url(#{rng.choice(refs)})"{style_attr()}/>')
        elif k < 0.7:
            parts.append(f'<g transform="{transform()}" opacity="{num()}">{shape()}</g>')
        else:
            parts.append(shape())
    return "".join(parts)


def doc():
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" '
            f'viewBox="0 0 200 200">{body()}</svg>')


for i in range(COUNT):
    data = doc()
    if rng.random() < 0.1:
        data = data * rng.randint(2, 8)
    with open(os.path.join(OUT, "ft%06d.svg" % i), "w") as f:
        f.write(data)
print(f"wrote {COUNT} files to {OUT}")