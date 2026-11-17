#!/usr/bin/env python3
"""Structure-aware SVG fuzzer.

Generates large mutated documents by splicing, duplicating and rewriting
seed fragments, then writes them to a directory for the batch runner.

Strategy: rather than random byte flips (which mostly produce XML errors the
parser rejects in a few instructions), we mutate at the *token* level so the
documents stay parseable and reach deep into layout, path, gradient, stroke
and text code.  Every generated file is also biased to be large, so the
rasteriser's hot loops and allocation paths get real work.

Usage:
    python3 mutate.py <seed_dir> <out_dir> <count> [seed]
"""
import os
import random
import re
import sys

TAG_RE = re.compile(rb"<(/?)([A-Za-z_:-]+)([^>]*?)(/?)>", re.S)
NUM_RE = re.compile(rb"-?\d+\.?\d*(?:[eE][-+]?\d+)?")
ATTR_RE = re.compile(rb'([A-Za-z_:-]+)\s*=\s*("([^"]*)"|\'([^\']*)\')')
ENTITY_RE = re.compile(rb"&[#A-Za-z0-9]{1,10};")

BAD_NUMS = [b"NaN", b"inf", b"-inf", b"1e400", b"-1e400", b"1e-400",
            b"0", b"-0", b"1e30", b"-1e30", b"3.4e38", b"1e-30"]


def tokenize(data):
    """Split into tags and text so we can splice at safe boundaries."""
    out = []
    pos = 0
    for m in TAG_RE.finditer(data):
        if m.start() > pos:
            out.append(("text", data[pos:m.start()]))
        out.append(("tag", m.group(0)))
        pos = m.end()
    if pos < len(data):
        out.append(("text", data[pos:]))
    return out


def mutate_numbers(tok, rng):
    def sub(m):
        if rng.random() < 0.25:
            return rng.choice(BAD_NUMS)
        v = float(m.group(0))
        k = rng.randrange(6)
        if k == 0:
            v = -v
        elif k == 1:
            v *= rng.choice([0.5, 2, 10, 0.1, 1000])
        elif k == 2:
            v = rng.uniform(-1e4, 1e4)
        elif k == 3:
            v = float(rng.randint(-500, 500))
        return ("%.6g" % v).encode()
    return NUM_RE.sub(sub, tok)


def mutate_tag(tok, rng):
    kind, body = tok
    if kind != "tag":
        return tok
    body = mutate_numbers(body, rng)
    k = rng.randrange(10)
    if k == 0:   # drop the whole tag (may unbalance XML; parser is lenient)
        return ("text", b"")
    if k == 1:   # duplicate an attribute
        m = ATTR_RE.search(body)
        if m:
            return ("tag", body + b" " + m.group(0))
    if k == 2:   # remove an attribute
        m = ATTR_RE.search(body)
        if m:
            return ("tag", body[:m.start()] + body[m.end():])
    if k == 3:   # inject an entity
        return ("tag", body.replace(b">", b" x='&amp;&#65;&#x42;&bogus;' >", 1))
    if k == 4:   # truncate mid-tag
        return ("text", body[: max(1, len(body) // 2)])
    if k == 5:   # inject whitespace noise
        return ("tag", body.replace(b" ", b"  \t\n ", 1))
    if k == 6:   # break a quoted value
        return ("tag", body.replace(b'"', b"'", 1))
    return tok


def generate(seed_data, rng):
    toks = tokenize(seed_data)

    # Splice: repeat a random slice to inflate the document.
    if len(toks) > 8 and rng.random() < 0.7:
        a = rng.randrange(len(toks) // 2)
        b = rng.randrange(a + 1, len(toks))
        reps = rng.randint(2, 12)
        toks = toks[:b] + toks[a:b] * reps + toks[b:]
        if len(toks) > 200000:                 # keep generation fast
            toks = toks[:200000]

    # Token-level mutation.
    toks = [mutate_tag(t, rng) for t in toks]

    # Randomly drop a chunk (breaks nesting in interesting ways).
    if len(toks) > 20 and rng.random() < 0.5:
        a = rng.randrange(len(toks))
        b = min(len(toks), a + rng.randint(1, len(toks) // 3))
        del toks[a:b]

    return b"".join(t[1] for t in toks)


def main():
    seed_dir, out_dir, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    os.makedirs(out_dir, exist_ok=True)

    seeds = []
    for name in sorted(os.listdir(seed_dir)):
        if name.endswith(".svg"):
            with open(os.path.join(seed_dir, name), "rb") as f:
                seeds.append(f.read())
    if not seeds:
        print("no seeds", file=sys.stderr)
        return 1

    rng = random.Random(seed)
    for i in range(count):
        base = rng.choice(seeds)
        data = generate(base, rng)
        # Bias towards larger documents so hot loops get real work.
        while len(data) < 60000 and rng.random() < 0.5:
            data = data * 2
        if len(data) > 400000:
            data = data[:400000]
        with open(os.path.join(out_dir, "f%06d.svg" % i), "wb") as f:
            f.write(data)
    print(f"wrote {count} files to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())