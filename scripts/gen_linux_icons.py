#!/usr/bin/env python3
"""Write the Linux desktop icons: assets/appicon.png as hicolor PNGs named `liu`.

    scripts/gen_linux_icons.py <out-dir>

writes <out-dir>/<N>x<N>/apps/liu.png for each size below — the layout of
share/icons/hicolor, so a desktop (and `notify-send --icon=liu`, which is how
the app posts a banner) finds the icon by NAME once the tree is installed under
an XDG data directory.

Each size is the window icon the X11 backend publishes (_NET_WM_ICON,
platform_linux.c): the 1024 px master box-averaged down, then its corners
rounded at 22.4 % of the side, applied per size so the corner lands one crisp
antialiased edge. Taskbar, launcher and banner then show one icon, not two
versions of it. Straight (not premultiplied) alpha, as PNG wants it.

Standard library only (zlib): the release container needs nothing extra to run
this, and the output is the same bytes on every machine — no timestamps, fixed
compression — so a rebuilt package carries identical icons.
"""
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MASTER = os.path.join(ROOT, "assets", "appicon.png")
SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
CORNER = 0.224          # of the side; platform_linux.c icon_round_corners
NAME = "liu.png"


def fail(msg):
    sys.exit(f"gen_linux_icons: {msg}")


def read_png(path):
    """(width, height, rows) for an 8-bit, non-interlaced RGB or RGBA PNG; each
    row a bytes object of RGBA pixels. Anything else is refused, not guessed."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        fail(f"{path} is not a PNG")
    pos, idat, ihdr = 8, [], None
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
        pos += 12 + length
    if not ihdr:
        fail(f"{path} has no IHDR")
    w, h, depth, ctype, _comp, _filt, interlace = ihdr
    if depth != 8 or ctype not in (2, 6) or interlace:
        fail(f"{path}: only 8-bit non-interlaced RGB/RGBA is read "
             f"(depth {depth}, colour type {ctype}, interlace {interlace})")
    bpp = 3 if ctype == 2 else 4
    stride = w * bpp
    raw = zlib.decompress(b"".join(idat))
    if len(raw) != h * (stride + 1):
        fail(f"{path}: image data is {len(raw)} bytes, expected {h * (stride + 1)}")
    rows, prev = [], bytearray(stride)
    for y in range(h):
        off = y * (stride + 1)
        ftype, cur = raw[off], bytearray(raw[off + 1:off + 1 + stride])
        if ftype == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 0xFF
        elif ftype == 2:
            cur = bytearray((a + b) & 0xFF for a, b in zip(cur, prev))
        elif ftype == 3:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (cur[i] + pred) & 0xFF
        elif ftype != 0:
            fail(f"{path}: unknown filter {ftype} on row {y}")
        prev = cur
        if bpp == 3:
            rgba = bytearray(w * 4)
            rgba[0::4], rgba[1::4], rgba[2::4] = cur[0::3], cur[1::3], cur[2::3]
            rgba[3::4] = b"\xff" * w
            rows.append(bytes(rgba))
        else:
            rows.append(bytes(cur))
    return w, h, rows


def downsample(w, h, rows, size):
    """Box average to size x size — icon_downsample's bins and integer maths."""
    out = []
    for y in range(size):
        y0 = y * h // size
        y1 = max((y + 1) * h // size, y0 + 1)
        y1 = min(y1, h)
        line = bytearray(size * 4)
        for x in range(size):
            x0 = x * w // size
            x1 = min(max((x + 1) * w // size, x0 + 1), w)
            n = (y1 - y0) * (x1 - x0)
            for ch in range(4):
                s = 0
                for sy in range(y0, y1):
                    s += sum(rows[sy][x0 * 4 + ch:x1 * 4:4])
                line[x * 4 + ch] = s // n
        out.append(line)
    return out


def round_corners(px, size):
    """icon_round_corners: coverage of a rounded rect, measured from the pixel
    centre, multiplied into alpha only."""
    r = size * CORNER
    if r < 1.0:
        return
    half = size * 0.5
    for y in range(size):
        for x in range(size):
            dx = abs(x + 0.5 - half) - (half - r)
            dy = abs(y + 0.5 - half) - (half - r)
            if dx <= 0.0 and dy <= 0.0:
                continue
            dx, dy = max(dx, 0.0), max(dy, 0.0)
            d = (dx * dx + dy * dy) ** 0.5 - r
            if d <= -0.5:
                continue
            cov = min(max(0.5 - d, 0.0), 1.0)
            px[y][x * 4 + 3] = int(px[y][x * 4 + 3] * cov + 0.5)


def png_bytes(size, px):
    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))
    raw = b"".join(b"\x00" + bytes(line) for line in px)
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen_linux_icons.py <out-dir>")
    out_dir = sys.argv[1]
    w, h, rows = read_png(MASTER)
    for size in SIZES:
        px = downsample(w, h, rows, size)
        round_corners(px, size)
        dest = os.path.join(out_dir, f"{size}x{size}", "apps")
        os.makedirs(dest, exist_ok=True)
        with open(os.path.join(dest, NAME), "wb") as f:
            f.write(png_bytes(size, px))
    print(f"gen_linux_icons: {len(SIZES)} sizes in {out_dir}")


if __name__ == "__main__":
    main()
