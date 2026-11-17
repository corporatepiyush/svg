#!/usr/bin/env python3
"""Large-input stress generator.

Builds documents that are individually huge (hundreds of thousands of nodes,
millions of path points, very large canvases) to exercise the rasteriser at
scale: edge-table growth, coverage buffers, layer allocation, and the PNG
compressor on multi-megapixel images.
"""
import os
import random
import sys

import tempfile
_HERE = os.path.dirname(os.path.abspath(__file__))
_WORK = os.environ.get("SVG2IMG_FUZZ_WORK",
                       os.path.join(tempfile.gettempdir(), "svg2img_fuzz"))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_WORK, "big")
os.makedirs(OUT, exist_ok=True)
rng = random.Random(4242)


def write(name, s):
    with open(os.path.join(OUT, name), "w") as f:
        f.write(s)
    print(f"{name}: {len(s)/1e6:.1f} MB")


# 1. One path with a million points
pts = []
for i in range(120000):
    a = i * 0.001
    pts.append(f"{500 + 400 * __import__('math').cos(a):.1f},{500 + 400 * __import__('math').sin(a):.1f}")
write("one_huge_path.svg",
      '<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="1000">'
      '<path d="M' + " L".join(pts) + ' Z" fill="teal" fill-opacity="0.5"/></svg>')

# 2. One hundred thousand tiny shapes
parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="1000">']
for i in range(100000):
    x = (i * 37) % 1000
    y = (i * 91) % 1000
    parts.append(f'<rect x="{x}" y="{y}" width="4" height="4" fill="hsl({i % 360},70%,50%)"/>')
parts.append('</svg>')
write("100k_shapes.svg", "".join(parts))

# 3. Deep nesting: 5000 groups
d = 5000
write("5000_deep_groups.svg",
      '<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300">' +
      "<g>" * d + '<rect x="10" y="10" width="50" height="50" fill="red"/>' +
      "</g>" * d + "</svg>")

# 4. Very wide flat group
parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="2000" height="2000"><g>']
for i in range(30000):
    parts.append(f'<circle cx="{(i*17)%2000}" cy="{(i*29)%2000}" r="3" fill="blue" opacity="0.4"/>')
parts.append('</g></svg>')
write("30k_flat_group.svg", "".join(parts))

# 5. Nested opacity layers (worst case for layer allocation)
parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="600" height="600">']
for i in range(200):
    parts.append('<g opacity="0.9">')
for i in range(2000):
    parts.append(f'<rect x="{(i*13)%500}" y="{(i*7)%500}" width="40" height="40" fill="green"/>')
for i in range(200):
    parts.append('</g>')
parts.append('</svg>')
write("200_nested_opacity.svg", "".join(parts))

# 6. Millions of path segments across many paths
parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1500" height="1500">']
for k in range(2000):
    seg = [f'M{rng.uniform(0,1500):.0f},{rng.uniform(0,1500):.0f}']
    for i in range(200):
        seg.append(f'L{rng.uniform(0,1500):.0f},{rng.uniform(0,1500):.0f}')
    parts.append(f'<path d="{" ".join(seg)}" fill="none" stroke="black" stroke-width="1"/>')
parts.append('</svg>')
write("400k_segments.svg", "".join(parts))

# 7. Large canvas, moderate content
parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="4000" height="4000">']
for i in range(3000):
    parts.append(f'<circle cx="{rng.uniform(0,4000):.0f}" cy="{rng.uniform(0,4000):.0f}" '
                 f'r="{rng.uniform(10,100):.0f}" fill="hsl({i%360},60%,50%)" fill-opacity="0.6"/>')
parts.append('</svg>')
write("4000x4000.svg", "".join(parts))

# 8. Text stress: many glyphs
parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="2000" height="2000">']
for i in range(20000):
    parts.append(f'<text x="{(i*31)%1900}" y="{(i*17)%1990 + 10}" font-size="14">ABC {i}</text>')
parts.append('</svg>')
write("20k_text.svg", "".join(parts))

# 9. Dashed strokes over huge paths
parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="1200">']
for k in range(300):
    seg = [f'M{rng.uniform(0,1200):.0f},{rng.uniform(0,1200):.0f}']
    for i in range(60):
        seg.append(f'L{rng.uniform(0,1200):.0f},{rng.uniform(0,1200):.0f}')
    parts.append(f'<path d="{" ".join(seg)}" fill="none" stroke="black" '
                 f'stroke-width="2" stroke-dasharray="5,3"/>')
parts.append('</svg>')
write("300_dashed.svg", "".join(parts))

# 10. Gradient soup
parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="1200"><defs>']
for i in range(2000):
    parts.append(f'<linearGradient id="g{i}"><stop offset="0" stop-color="hsl({i%360},80%,50%)"/>'
                 f'<stop offset="1" stop-color="hsl({(i+180)%360},80%,50%)"/></linearGradient>')
parts.append('</defs>')
for i in range(20000):
    parts.append(f'<circle cx="{rng.uniform(0,1200):.0f}" cy="{rng.uniform(0,1200):.0f}" '
                 f'r="{rng.uniform(5,40):.0f}" fill="url(#g{i%2000})"/>')
parts.append('</svg>')
write("20k_gradients.svg", "".join(parts))

print(f"\nall big inputs in {OUT}")