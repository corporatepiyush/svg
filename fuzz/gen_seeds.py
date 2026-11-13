#!/usr/bin/env python3
"""Generate a large, diverse corpus of valid-ish SVG seeds.

Covers every feature the renderer claims to support, plus adversarial-but-valid
documents: deep nesting, huge coordinate ranges, many siblings, odd attribute
orderings, namespace prefixes, numeric formats, and entity usage.
"""
import os
import random

import tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.environ.get("SVG2IMG_FUZZ_WORK",
                      os.path.join(tempfile.gettempdir(), "svg2img_fuzz"))
OUT = os.path.join(WORK, "seeds")
os.makedirs(OUT, exist_ok=True)
rng = random.Random(0xC0FFEE)

W = 400
H = 300


def svg(body, w=W, h=H, vb=None, extra=""):
    if vb is None:
        vb = f"0 0 {w} {h}"
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" '
            f'height="{h}" viewBox="{vb}"{extra}>{body}</svg>\n')


def r(lo, hi):
    return rng.uniform(lo, hi)


def ri(lo, hi):
    return rng.randint(lo, hi)


def pick(seq):
    return rng.choice(seq)


NAMED = ["red", "blue", "green", "rebeccapurple", "transparent", "currentColor",
         "darkslategray", "lightgoldenrodyellow", "mediumspringgreen"]


def color():
    k = ri(0, 6)
    if k == 0:
        return pick(NAMED)
    if k == 1:
        return "#%02x%02x%02x" % (ri(0, 255), ri(0, 255), ri(0, 255))
    if k == 2:
        return "#%02x%02x%02x%02x" % (ri(0, 255), ri(0, 255), ri(0, 255), ri(0, 255))
    if k == 3:
        return "#%03x" % ri(0, 0xFFF)
    if k == 4:
        return "rgb(%d,%d,%d)" % (ri(0, 255), ri(0, 255), ri(0, 255))
    if k == 5:
        return "hsl(%d,%d%%,%d%%)" % (ri(0, 359), ri(0, 100), ri(0, 100))
    return "rgba(%d,%d,%d,%.2f)" % (ri(0, 255), ri(0, 255), ri(0, 255), r(0, 1))


def length(v=None, unit=True):
    if v is None:
        v = r(-20, W + 20)
    if not unit:
        return f"{v:.3f}"
    u = pick(["", "px", "pt", "pc", "in", "cm", "mm", "em", "ex", "%"])
    if u == "%":
        return f"{v:.2f}%"
    return f"{v:.3f}{u}"


def transform():
    k = ri(0, 6)
    a, b, c, d = r(-2, 2), r(-2, 2), r(-2, 2), r(-2, 2)
    e, f = r(-50, 50), r(-50, 50)
    if k == 0:
        return f"translate({e:.2f},{f:.2f})"
    if k == 1:
        return f"scale({a:.3f},{d:.3f})"
    if k == 2:
        return f"rotate({r(-360,360):.2f})"
    if k == 3:
        return f"rotate({r(-180,180):.2f} {e:.1f} {f:.1f})"
    if k == 4:
        return f"skewX({r(-60,60):.2f})"
    if k == 5:
        return f"skewY({r(-60,60):.2f})"
    return f"matrix({a:.3f},{b:.3f},{c:.3f},{d:.3f},{e:.2f},{f:.2f})"


def style_attrs():
    out = []
    if rng.random() < 0.6:
        out.append(f'fill="{color()}"')
    if rng.random() < 0.5:
        out.append(f'stroke="{color()}"')
    if rng.random() < 0.5:
        out.append(f'stroke-width="{length(r(0, 12), unit=False)}"')
    if rng.random() < 0.3:
        out.append(f'opacity="{r(0, 1):.3f}"')
    if rng.random() < 0.25:
        out.append(f'fill-opacity="{r(0, 1):.3f}"')
    if rng.random() < 0.25:
        out.append(f'stroke-opacity="{r(0, 1):.3f}"')
    if rng.random() < 0.25:
        out.append(f'transform="{transform()}"')
    if rng.random() < 0.2:
        out.append(f'stroke-linecap="{pick(["butt","round","square"])}"')
    if rng.random() < 0.2:
        out.append(f'stroke-linejoin="{pick(["miter","round","bevel"])}"')
    if rng.random() < 0.15:
        out.append(f'stroke-dasharray="{r(1,20):.1f},{r(1,20):.1f}"')
    if rng.random() < 0.2:
        out.append(f'fill-rule="{pick(["nonzero","evenodd"])}"')
    return (" " + " ".join(out)) if out else ""


def path_data(n=None):
    if n is None:
        n = ri(1, 8)
    cmds = []
    x, y = r(0, W), r(0, H)
    for _ in range(n):
        k = ri(0, 9)
        if k == 0:
            cmds.append(f"M{x:.1f},{y:.1f}")
        elif k == 1:
            x, y = r(0, W), r(0, H)
            cmds.append(f"L{x:.1f},{y:.1f}")
        elif k == 2:
            cmds.append(f"H{r(0,W):.1f}")
        elif k == 3:
            cmds.append(f"V{r(0,H):.1f}")
        elif k == 4:
            cmds.append(f"C{r(0,W):.1f},{r(0,H):.1f} {r(0,W):.1f},{r(0,H):.1f} {r(0,W):.1f},{r(0,H):.1f}")
        elif k == 5:
            cmds.append(f"S{r(0,W):.1f},{r(0,H):.1f} {r(0,W):.1f},{r(0,H):.1f}")
        elif k == 6:
            cmds.append(f"Q{r(0,W):.1f},{r(0,H):.1f} {r(0,W):.1f},{r(0,H):.1f}")
        elif k == 7:
            cmds.append(f"T{r(0,W):.1f},{r(0,H):.1f}")
        elif k == 8:
            cmds.append(f"A{r(1,60):.1f},{r(1,60):.1f} {r(0,360):.0f} {ri(0,1)} {ri(0,1)} {r(0,W):.1f},{r(0,H):.1f}")
        else:
            cmds.append("Z")
    return " ".join(cmds)


def shape():
    k = ri(0, 7)
    if k == 0:
        rx = f' rx="{r(0,30):.1f}"' if rng.random() < 0.5 else ""
        return (f'<rect x="{length(r(-20,W))}" y="{length(r(-20,H))}" '
                f'width="{length(r(0,200))}" height="{length(r(0,200))}"'
                f'{rx}'
                f' fill="{color()}"{style_attrs()}/>')
    if k == 1:
        return (f'<circle cx="{length(r(0,W))}" cy="{length(r(0,H))}" '
                f'r="{length(r(0,80))}" fill="{color()}"{style_attrs()}/>')
    if k == 2:
        return (f'<ellipse cx="{length(r(0,W))}" cy="{length(r(0,H))}" '
                f'rx="{length(r(0,80))}" ry="{length(r(0,80))}" fill="{color()}"{style_attrs()}/>')
    if k == 3:
        return (f'<line x1="{r(0,W):.1f}" y1="{r(0,H):.1f}" x2="{r(0,W):.1f}" '
                f'y2="{r(0,H):.1f}" stroke="{color()}" stroke-width="{r(1,8):.1f}"{style_attrs()}/>')
    if k == 4:
        pts = " ".join(f"{r(0,W):.0f},{r(0,H):.0f}" for _ in range(ri(3, 8)))
        return f'<polygon points="{pts}" fill="{color()}"{style_attrs()}/>'
    if k == 5:
        pts = " ".join(f"{r(0,W):.0f},{r(0,H):.0f}" for _ in range(ri(2, 8)))
        return f'<polyline points="{pts}" fill="none" stroke="{color()}" stroke-width="{r(1,6):.1f}"/>'
    if k == 6:
        return f'<path d="{path_data()}" fill="{color()}"{style_attrs()}/>'
    return (f'<text x="{length(r(0,W))}" y="{length(r(0,H))}" font-size="{r(8,32):.0f}" '
            f'fill="{color()}" text-anchor="{pick(["start","middle","end"])}"'
            f'{style_attrs()}>Sample {ri(0,999)}</text>')


def defs():
    parts = []
    for i in range(ri(0, 3)):
        k = ri(0, 3)
        gid = f"g{i}"
        stops = "".join(
            f'<stop offset="{r(0,1):.2f}" stop-color="{color()}" stop-opacity="{r(0,1):.2f}"/>'
            for _ in range(ri(1, 4)))
        if k == 0:
            parts.append(f'<linearGradient id="{gid}" x1="{r(0,1):.2f}" y1="{r(0,1):.2f}" '
                         f'x2="{r(0,1):.2f}" y2="{r(0,1):.2f}">{stops}</linearGradient>')
        elif k == 1:
            parts.append(f'<radialGradient id="{gid}" cx="{r(0,1):.2f}" cy="{r(0,1):.2f}" '
                         f'r="{r(0,1):.2f}" fx="{r(0,1):.2f}" fy="{r(0,1):.2f}">{stops}</radialGradient>')
        elif k == 2:
            parts.append(f'<clipPath id="c{i}"><circle cx="{r(0,W):.0f}" cy="{r(0,H):.0f}" r="{r(10,80):.0f}"/></clipPath>')
        else:
            parts.append(f'<g id="u{i}"><rect x="{r(0,W):.0f}" y="{r(0,H):.0f}" width="30" height="30" fill="{color()}"/></g>')
    return "<defs>" + "".join(parts) + "</defs>" if parts else ""


def body(depth=0):
    out = []
    for _ in range(ri(1, 6)):
        if depth < 4 and rng.random() < 0.3:
            out.append(f'<g opacity="{r(0.1,1):.2f}" transform="{transform()}">'
                       + body(depth + 1) + "</g>")
        else:
            out.append(shape())
    return "".join(out)


def make(name, content=None, **kw):
    if content is None:
        content = body()
    open(os.path.join(OUT, name), "w").write(svg(content, **kw))


# 1. Normal random documents
for i in range(60):
    make(f"rand{i:03d}.svg")

# 2. Gradient / clip / use heavy
for i in range(20):
    b = defs() + "".join(
        f'<rect x="{r(0,W):.0f}" y="{r(0,H):.0f}" width="{r(10,80):.0f}" height="{r(10,80):.0f}" '
        f'fill="url(#g{ri(0,2)})" clip-path="url(#c0)" opacity="{r(0.2,1):.2f}"/>'
        for _ in range(20))
    make(f"grad{i:03d}.svg", content=b)

# 3. Very deep nesting
for i in range(6):
    depth = 100 * (i + 1)
    b = "<g opacity='0.95'>" * depth + shape() + "</g>" * depth
    make(f"deep{i}.svg", content=b)

# 4. Many siblings
for i in range(8):
    n = 2000 * (i + 1)
    make(f"wide{i}.svg", content="".join(shape() for _ in range(n)))

# 5. Huge coordinate ranges / extreme values
for i in range(10):
    scale = 10 ** i
    b = "".join(
        f'<path d="M{r(-1,1)*scale:.4g},{r(-1,1)*scale:.4g} L{r(-1,1)*scale:.4g},{r(-1,1)*scale:.4g}" '
        f'stroke="black" stroke-width="{r(0.001,10):.4g}"/>'
        for _ in range(50))
    make(f"coords{i}.svg", content=b, w=1000, h=1000, vb="0 0 1000 1000")

# 6. Numeric format torture
nums = ["0", "-0", "1e5", "1E-5", ".5", "-.5", "+3", "1.", "0x10",
        "1e400", "-1e400", "1e-400", "3.4028235e38", "1.17549435e-38",
        "00012", "1_000", "1,000", "NaN", "inf", "-inf", "", " ", "1.2.3"]
for i, n in enumerate(nums):
    b = f'<rect x="{n}" y="10" width="50" height="50" fill="red"/>'
    make(f"num{i:02d}.svg", content=b)

# 7. Entity / text torture
texts = ["&amp;", "&lt;&gt;", "&#65;", "&#x41;", "&nbsp;", "&bogus;", "&#;",
         "&#xZZ;", "&amp;amp;", "&#0;", "&#x10FFFF;", "&#xD800;",
         "a&amp;b&lt;c", "&quot;&apos;", "&#128512;", "héllo", "日本語"]
for i, t in enumerate(texts):
    b = f'<text x="20" y="50" font-size="20" fill="black">{t}</text>'
    make(f"ent{i:02d}.svg", content=b)

# 8. Inline-style / presentation-attribute torture (SVG-only styling)
style_bodies = [
    "fill:red",
    "fill:red;stroke:blue;stroke-width:2",
    "fill:url(#nope)",
    "fill:none;stroke:none",
    "fill:rgb(1,2,3)",
    "fill:currentColor;color:lime",
    "opacity:0.5",
    "display:none",
    "visibility:hidden",
    "font-size:150%",
    "font-size:2em;letter-spacing:1px",
    "clip-path:url(#nope)",
]
for i, decl in enumerate(style_bodies):
    b = (f'<rect x="20" y="20" width="60" height="60" fill="black" '
         f'style="{decl}"/>')
    make(f"style{i:02d}.svg", content=b)

# 9. ViewBox / preserveAspectRatio matrix
for i, par in enumerate(["", " none", " xMinYMin meet", " xMaxYMax slice",
                         " xMidYMid meet", " xMinYMax slice"]):
    for j, vb in enumerate(["0 0 100 100", "-50 -50 100 100", "0 0 200 50",
                            "0 0 0 0", "10 20 30 40"]):
        b = '<rect x="0" y="0" width="100" height="100" fill="blue"/>'
        make(f"vb{i}_{j}.svg", content=b, w=300, h=200,
             vb=vb, extra=f' preserveAspectRatio="{par}"' if par else "")

# 10. Stroke torture
strokes = ['stroke-width="0"', 'stroke-width="-5"', 'stroke-width="1e9"',
           'stroke-dasharray="0"', 'stroke-dasharray="5,0"',
           'stroke-dasharray="-1,2"', 'stroke-dasharray="1,2,3"',
           'stroke-linecap="round" stroke-dasharray="1,2"',
           'stroke-linejoin="miter" stroke-miterlimit="0"',
           'stroke-linejoin="miter" stroke-miterlimit="1e9"',
           'stroke-linejoin="bevel"', 'stroke-linejoin="round"']
for i, st in enumerate(strokes):
    b = f'<path d="M20,150 L100,20 L180,150 Z" fill="none" stroke="black" {st}/>'
    make(f"stroke{i:02d}.svg", content=b)

print(f"wrote seeds to {OUT}")
print(f"total files: {len(os.listdir(OUT))}")