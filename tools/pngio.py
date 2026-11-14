#!/usr/bin/env python3
"""PNG encode/decode helpers for the measurement tools (stdlib only).

Deliberately small: it handles exactly what this repository produces and what
the external renderers emit -- 8-bit non-interlaced grey, RGB, grey+alpha and
RGBA -- with all five PNG row filters on decode and filter 0 on encode.

Used by ``conformance/run.py`` and ``tools/perceptual.py`` so both agree on
pixel semantics.
"""
import struct
import zlib

CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}
COLOR_RGBA = 6


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """Decode *path* to (width, height, rgba_bytes)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)
    i, idat, w, h, depth, ctype = 8, b"", 0, 0, 0, 0
    while i + 8 <= len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        typ = data[i + 4:i + 8]
        chunk = data[i + 8:i + 8 + ln]
        if len(chunk) != ln:
            raise ValueError("truncated %r chunk" % typ)
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
        i += 12 + ln
    if depth != 8 or ctype not in CHANNELS:
        raise ValueError("unsupported PNG depth=%d ctype=%d" % (depth, ctype))
    nch = CHANNELS[ctype]
    raw = zlib.decompress(idat)
    stride = w * nch
    if len(raw) != h * (stride + 1):
        raise ValueError("bad decompressed size")
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
                line[x] = (line[x] + line[x - nch]) & 0xFF
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif f == 3:
            for x in range(stride):
                a = line[x - nch] if x >= nch else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif f == 4:
            for x in range(stride):
                a = line[x - nch] if x >= nch else 0
                b = prev[x]
                c = prev[x - nch] if x >= nch else 0
                line[x] = (line[x] + _paeth(a, b, c)) & 0xFF
        elif f != 0:
            raise ValueError("bad filter %d" % f)
        if nch == 4:
            out[y * w * 4:(y + 1) * w * 4] = line
        else:
            row = y * w * 4
            for x in range(w):
                o = row + x * 4
                s = x * nch
                if nch == 3:
                    out[o:o + 3] = line[s:s + 3]
                    out[o + 3] = 255
                elif nch == 2:
                    out[o] = out[o + 1] = out[o + 2] = line[s]
                    out[o + 3] = line[s + 1]
                elif nch == 1:
                    out[o] = out[o + 1] = out[o + 2] = line[s]
                    out[o + 3] = 255
                else:
                    raise ValueError("unhandled channel count %d" % nch)
        prev = line
    return w, h, bytes(out)


def write_png(path, w, h, rgba):
    """Write RGBA8 *rgba* to *path* using filter 0."""
    if len(rgba) != w * h * 4:
        raise ValueError("buffer size mismatch")
    stride = w * 4
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(typ, payload):
        return (struct.pack(">I", len(payload)) + typ + payload +
                struct.pack(">I", zlib.crc32(typ + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, COLOR_RGBA, 0, 0, 0)
    out = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(out)


def iter_rgba(w, h, rgba):
    """Yield (x, y, r, g, b, a) for every pixel."""
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            o = row + x * 4
            yield x, y, rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3]