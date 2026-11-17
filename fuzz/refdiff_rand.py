#!/usr/bin/env python3
"""Randomised differential test against WebKit (via qlmanage).

Generates valid, unambiguous documents (axis-aligned or simple curved shapes,
solid or gradient fills, clips, transforms) and compares interior pixels with
WebKit's renderer.  This complements the fixed refdiff.py corpus with breadth.

Usage: python3 fuzz/refdiff_rand.py [count] [seed] [svg2img]
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refdiff

COUNT = int(sys.argv[1]) if len(sys.argv) > 1 else 60
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
BIN = sys.argv[3] if len(sys.argv) > 3 else "./svg2img"
refdiff.BIN = BIN
rng = random.Random(SEED)

NAMED = ["red", "blue", "green", "rebeccapurple", "darkslategray", "orange",
         "purple", "teal", "olive", "maroon", "navy", "steelblue", "crimson"]


def color():
    return rng.choice(NAMED)


def shape():
    k = rng.randint(0, 4)
    if k == 0:
        x, y = rng.randint(0, 160), rng.randint(0, 160)
        return (f'<rect x="{x}" y="{y}" width="{rng.randint(10, 40)}" '
                f'height="{rng.randint(10, 40)}" fill="{color()}"/>')
    if k == 1:
        return (f'<circle cx="{rng.randint(30, 170)}" cy="{rng.randint(30, 170)}" '
                f'r="{rng.randint(10, 40)}" fill="{color()}"/>')
    if k == 2:
        return (f'<ellipse cx="{rng.randint(30, 170)}" cy="{rng.randint(30, 170)}" '
                f'rx="{rng.randint(10, 40)}" ry="{rng.randint(10, 30)}" fill="{color()}"/>')
    if k == 3:
        # convex polygon: points on a circle in angular order
        import math
        cx, cy = rng.randint(60, 140), rng.randint(60, 140)
        rad = rng.randint(20, 50)
        n = rng.randint(3, 7)
        a0 = rng.uniform(0, 6.28)
        pts = " ".join(
            f"{cx + rad * math.cos(a0 + 6.283 * i / n):.1f},"
            f"{cy + rad * math.sin(a0 + 6.283 * i / n):.1f}" for i in range(n))
        return f'<polygon points="{pts}" fill="{color()}"/>'
    if k == 5:
        # stroked shape: fill none, solid stroke (interior stays empty)
        x, y = rng.randint(10, 100), rng.randint(10, 100)
        w, h = rng.randint(20, 80), rng.randint(20, 80)
        return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="none" '
                f'stroke="{color()}" stroke-width="{rng.randint(2, 10)}"/>')
    # rectilinear path (integer edges, easy to compare)
    x, y = rng.randint(10, 100), rng.randint(10, 100)
    w, h = rng.randint(20, 80), rng.randint(20, 80)
    return (f'<path d="M{x},{y} h{w} v{h} h-{w} Z" fill="{color()}"/>')


def grad_shape():
    gid = "g"
    offs = sorted(rng.randint(0, 100) / 100 for _ in range(rng.randint(2, 4)))
    stops = "".join(
        f'<stop offset="{o:.2f}" stop-color="{color()}" '
        f'stop-opacity="{rng.randint(50, 100) / 100:.2f}"/>' for o in offs)
    spread = rng.choice(["pad", "reflect", "repeat"])
    is_radial = rng.random() >= 0.5
    if is_radial:
        spread = "pad"       # WebKit's radial reflect/repeat is non-conformant
    if not is_radial:
        defs = (f'<linearGradient id="{gid}" x1="{rng.randint(0,100)/100:.2f}" '
                f'y1="{rng.randint(0,100)/100:.2f}" x2="{rng.randint(0,100)/100:.2f}" '
                f'y2="{rng.randint(0,100)/100:.2f}" spreadMethod="{spread}">'
                f'{stops}</linearGradient>')
    else:
        cx, cy = rng.randint(20, 80) / 100, rng.randint(20, 80) / 100
        rad = rng.randint(20, 70) / 100
        # focal point at most 0.8*r from the centre (strictly inside)
        ang = rng.uniform(0, 6.283)
        fr = rng.uniform(0, 0.8) * rad
        fx, fy = cx + fr * __import__("math").cos(ang), cy + fr * __import__("math").sin(ang)
        defs = (f'<radialGradient id="{gid}" cx="{cx:.2f}" cy="{cy:.2f}" r="{rad:.2f}" '
                f'fx="{fx:.2f}" fy="{fy:.2f}" spreadMethod="{spread}">{stops}</radialGradient>')
    return (f'<defs>{defs}</defs><rect x="0" y="0" width="200" height="200" '
            f'fill="url(#{gid})"/>')


def case(i):
    k = rng.randint(0, 3)
    if k == 0:
        body = "".join(shape() for _ in range(rng.randint(1, 4)))
    elif k == 1:
        body = (f'<g opacity="{rng.randint(50, 100) / 100:.2f}" '
                f'transform="translate({rng.randint(0, 40)},{rng.randint(0, 40)})">'
                f'{shape()}</g>')
    elif k == 2:
        body = grad_shape()
    else:
        body = ('<defs><clipPath id="c"><rect x="0" y="0" width="200" '
                'height="200"/></clipPath></defs>'
                f'<g clip-path="url(#c)">{shape()}</g>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">'
            f'{body}</svg>')


def main():
    bad = 0
    tested = 0
    for i in range(COUNT):
        name = f"rnd{i:03d}"
        svg = case(i)
        msg = refdiff.compare(name, svg)
        if msg:
            print("FAIL", msg)
            bad += 1
        else:
            tested += 1
        if (i + 1) % 10 == 0:
            print(f"  ... {i+1}/{COUNT}", flush=True)
    print(f"\n{tested}/{COUNT} compared, {bad} failure(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())