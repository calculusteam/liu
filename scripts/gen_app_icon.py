#!/usr/bin/env python3
"""Put the app icon on its plate colour and derive every form of it that ships.

The plate is the colour of assets/appicon.png, the full-bleed square the Linux
desktop icon and the About tab are drawn from. On macOS the icon has NO plate of
its own: see "The macOS icon" below.

    scripts/gen_app_icon.py                  # rewrite the outputs
    scripts/gen_app_icon.py --check          # fail if a checked-in output is stale
    scripts/gen_app_icon.py --preview x.png  # the six macOS icon styles, side by side

Input
    assets/appicon.png              the 1024 px master of the mark (a raster: the
                                    artwork exists in no other form)

Outputs (all checked in; nothing here is part of the build)
    assets/appicon.png              the master itself, when it is on another plate
                                    than PLATE below: only the plate and the
                                    antialiased rim of the outline are repainted
    assets/appicon-256.png          the copy the binary embeds (About tab) and the
                                    Linux tarball ships as its desktop icon
    packaging/macos/Liu.icon/       the Icon Composer document: the mark as two flat
                                    layers on the system's own plate
    packaging/macos/Assets.car      that document compiled by actool; what macOS 26
                                    and later draw the Dock icon from
    packaging/macos/Liu.icns        the mark alone on transparency, made here from
                                    the two layers (no actool): what everything that
                                    reads only an .icns draws
    packaging/macos/Liu.icon.sha256 which document the two were compiled from

To change the Linux icon's background, change PLATE and run this.

The macOS icon. A document cannot make the plate TRANSPARENT: the system
(design generations 26 and 27) draws every catalog icon on an opaque squircle
plate, whatever the fill says: "none", "automatic", an unset fill and a solid at
alpha 0 all come out as the system's own plate (near white in the light styles,
grey in the dark ones), and an .icns that is transparent is put on a dark plate
by the system as well. So the document says "automatic", and which plate it is
is the system's and the user's icon style's to say. The .icns, which macOS 15
and earlier draw as it is, IS transparent.

Why a document and not just an .icns: since macOS 26 the user picks an icon
style (Default, Dark, Clear, Tinted). A bitmap icon is given a treatment it has
no say in; a layered one is drawn by the system per style, and the mark can take
the user's tint. One thing the document says for that reason:

  - the fill layer is white in the TINTED appearance (which is also what Clear
    is drawn from). Those styles reduce each layer to one tone taken from its
    lightness, and the mark's violet comes out at about a third of the tint: a
    dim bunny on a dim plate. White takes the tint at full strength, and the
    ink over it stays the darker detail.

The layers carry no glass, shadow or highlight: the mark is flat artwork. All
three group settings are written out because macOS 26 draws a drop shadow under
a group that does not say otherwise (macOS 27 does not).

Why the compiled files are checked in: actool reads a .icon only from Xcode 26
on, and the release runners are older. Compiling needs that Xcode; --check and
everything else need only numpy, scipy and Pillow.
"""
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as ndi

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MASTER = os.path.join(ROOT, "assets", "appicon.png")
OUT_SMALL = os.path.join(ROOT, "assets", "appicon-256.png")
OUT_DIR = os.path.join(ROOT, "packaging", "macos")
OUT_ICON = os.path.join(OUT_DIR, "Liu.icon")
OUT_JSON = os.path.join(OUT_ICON, "icon.json")
OUT_FILL = os.path.join(OUT_ICON, "Assets", "fill.png")
OUT_INK = os.path.join(OUT_ICON, "Assets", "ink.png")
OUT_CAR = os.path.join(OUT_DIR, "Assets.car")
OUT_ICNS = os.path.join(OUT_DIR, "Liu.icns")
OUT_STAMP = os.path.join(OUT_DIR, "Liu.icon.sha256")
ICON_NAME = "Liu"               # CFBundleIconName and CFBundleIconFile

# Colors used to separate the master artwork into plate, fill, and ink layers.
# The plate is configured here; the fill and ink values define the extracted
# artwork colors.
PLATE = np.array([10.0, 10.0, 11.0])        # #0A0A0B, the default theme's obsidian
FILL = np.array([202.0, 126.0, 252.0])
INK = np.array([59.0, 16.0, 91.0])

SNAP = 0.06     # coverage this close to 0 or 1 is codec noise, not an edge
BAND = 3        # Coverage transition width at the mark's outer edge.
DEPTH = 10      # px under the outline where the fill layer ends (the outline is 21 or more wide)

RENDITIONS = ["Default", "Dark", "TintedLight", "TintedDark", "ClearLight", "ClearDark"]
ICTOOL = "Applications/Icon Composer.app/Contents/Executables/ictool"   # beside Xcode's Developer directory


def hexname(colour):
    return "#%02X%02X%02X" % tuple(int(round(c)) for c in colour)


def along(rgb, a, b):
    """Where each pixel sits on the line from colour a (0) to colour b (1)."""
    return np.clip(((rgb - a) @ (b - a)) / ((b - a) @ (b - a)), 0.0, 1.0)


def snap(value):
    return np.where(value < SNAP, 0.0, np.where(value > 1.0 - SNAP, 1.0, value))


def split(rgb, plate):
    """The mark as coverages: (ink, under, depth).

    The mark is plate | ink | fill, in that order from the outside in: the fill
    never meets the plate. So a pixel is read on ONE line, chosen by where it
    is: plate-to-ink in a band round the outer edge, fill-to-ink inside it.
    Solving for all three at once is what not to do. The master has been
    through a sharpening resize, the fill overshoots beside every stroke and
    the ink undershoots, and a three-way solve explains that with whichever
    colour is not there: ink ghosts in the fill, and plate inside the ink,
    which then lets the layer under it show through.

    `under` is the fill carried on beneath the outline to DEPTH, so the two
    layers overlap where the ink is solid and no seam can open between them.
    `depth` is the signed distance from the outer edge, positive inside.
    """
    outer = along(rgb, plate, INK)                  # the fill reads 1 here too
    solid = outer > 0.5
    labels, count = ndi.label(solid)
    if count == 0:
        raise SystemExit("no mark found in the master")
    sizes = ndi.sum(solid, labels, range(1, count + 1))
    mark = ndi.binary_fill_holes(labels == (1 + int(np.argmax(sizes))))
    depth = ndi.distance_transform_edt(mark) - ndi.distance_transform_edt(~mark)
    ink = snap(np.where(depth > BAND, along(rgb, FILL, INK),
                        np.where(depth > -BAND, outer, 0.0)))
    under = np.clip(depth - DEPTH + 0.5, 0.0, 1.0)

    past_band = (depth > BAND) & (depth <= BAND + 1)
    if outer[past_band].min() < 0.9:
        raise SystemExit(f"the mark's outer edge is softer than BAND ({BAND} px)")
    seam = (under > 0.0) & (under < 1.0)
    if ink[seam].min() < 1.0:
        raise SystemExit(f"the outline is thinner than DEPTH ({DEPTH} px): the fill "
                         "layer's edge would show")
    return ink, under, depth


def master_plate(rgb):
    """The colour the master's plate is now: the median of its outermost rows."""
    edge = np.concatenate([rgb[:16].reshape(-1, 3), rgb[-16:].reshape(-1, 3)])
    return np.median(edge, axis=0)


def replate(rgb, old):
    """The master moved from plate `old` to PLATE. Nothing inside the band changes."""
    ink, _, depth = split(rgb, old)
    plate = np.where(depth > BAND, 0.0, 1.0 - ink)[..., None]
    moved = np.where(plate >= 1.0, PLATE, rgb + plate * (PLATE - old))
    return np.clip(np.round(moved), 0, 255).astype(np.uint8)


def layer(colour, coverage):
    """One flat colour, the coverage as alpha."""
    alpha = np.round(coverage * 255.0).astype(np.uint8)
    flat = np.broadcast_to(np.round(colour).astype(np.uint8), alpha.shape + (3,))
    return np.dstack([flat, alpha])


def document():
    def solid(colour):
        return {"solid": "srgb:%.5f,%.5f,%.5f,1.00000" % tuple(colour / 255.0)}

    doc = {
        "fill": "automatic",
        "groups": [{
            "layers": [
                {"glass": False, "image-name": "ink.png", "name": "ink"},
                {"glass": False, "image-name": "fill.png", "name": "fill",
                 "fill-specializations": [
                     {"appearance": "tinted", "value": solid(np.array([255.0, 255.0, 255.0]))},
                 ]},
            ],
            "shadow": {"kind": "none", "opacity": 0.5},
            "specular": False,
            "translucency": {"enabled": False, "value": 0.5},
        }],
        "supported-platforms": {"squares": ["macOS"]},
    }
    # Sorted keys and " : ", the spelling Icon Composer itself saves.
    return (json.dumps(doc, indent=2, sort_keys=True, separators=(",", " : ")) + "\n").encode()


def png_bytes(pixels):
    out = io.BytesIO()
    Image.fromarray(pixels).save(out, "PNG", optimize=True)
    return out.getvalue()


def png_chunks(data):
    at = 8
    while at < len(data):
        (length,) = struct.unpack(">I", data[at:at + 4])
        yield data[at + 4:at + 8], data[at:at + 12 + length]
        at += 12 + length


def master_bytes(pixels):
    """The master's file with its pixels replaced and every other chunk kept
    (the colour space tag, the resolution, the author's metadata)."""
    kept = open(MASTER, "rb").read()
    fresh = b"".join(raw for kind, raw in png_chunks(png_bytes(pixels)) if kind == b"IDAT")
    height, width = pixels.shape[:2]
    out, placed = [kept[:8]], False
    for kind, raw in png_chunks(kept):
        # The pixel data is swapped in as it is, so the header has to describe it.
        if kind == b"IHDR" and raw[8:-4] != struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0):
            raise SystemExit("the master is not a plain 8-bit RGB PNG any more")
        if kind != b"IDAT":
            out.append(raw)
        elif not placed:
            out.append(fresh)
            placed = True
    return b"".join(out)


def same_pixels(path, pixels):
    if not os.path.exists(path):
        return False
    with Image.open(path) as image:
        mode = "RGBA" if pixels.shape[2] == 4 else "RGB"
        return image.mode == mode and np.array_equal(np.asarray(image), pixels)


def document_digest():
    digest = hashlib.sha256()
    for path in (OUT_JSON, OUT_FILL, OUT_INK):
        digest.update(hashlib.sha256(open(path, "rb").read()).digest())
    return digest.hexdigest()


def developer_dir():
    try:
        return subprocess.run(["xcode-select", "-p"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        raise SystemExit("this step needs Xcode 26 or later (xcode-select found none)")


def actool(workdir, target):
    """Compile the document for a deployment target; returns the output directory."""
    out = os.path.join(workdir, target)
    os.makedirs(out)
    run = subprocess.run(
        ["xcrun", "actool", OUT_ICON, "--compile", out, "--app-icon", ICON_NAME,
         "--platform", "macosx", "--target-device", "mac",
         "--minimum-deployment-target", target,
         "--output-partial-info-plist", os.path.join(out, "partial.plist"),
         "--output-format", "human-readable-text", "--errors", "--warnings"],
        capture_output=True, text=True)
    made = [os.path.join(out, name) for name in ("Assets.car", ICON_NAME + ".icns")]
    if run.returncode != 0 or not all(os.path.exists(path) for path in made):
        raise SystemExit("actool could not compile the icon document (it reads a .icon "
                         "from Xcode 26 on):\n" + run.stdout + run.stderr)
    return out


def legacy_icns(workdir):
    """The mark alone on transparency, as the ten sizes of an .icns (16 to 512 at 1x
    and 2x). Made from the document's layers, not by actool: its .icns always has
    a plate (white for a document with no fill of its own), which is the opposite
    of what this file is for. The two layers are stacked as the document stacks
    them, the ink over the fill."""
    art = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
    for path in (OUT_FILL, OUT_INK):
        with Image.open(path) as layer_image:
            art.alpha_composite(layer_image.convert("RGBA"))
    iconset = os.path.join(workdir, ICON_NAME + ".iconset")
    os.makedirs(iconset)
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
            art.resize((size * scale, size * scale), Image.LANCZOS).save(os.path.join(iconset, name))
    out = os.path.join(workdir, ICON_NAME + ".icns")
    run = subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out], capture_output=True, text=True)
    if run.returncode != 0 or not os.path.exists(out):
        raise SystemExit("iconutil could not make the .icns:\n" + run.stdout + run.stderr)
    return out


def compile_document():
    with tempfile.TemporaryDirectory() as workdir:
        # Compile the asset catalog for the app's minimum supported macOS
        # version. Generate the ICNS file with legacy_icns(); the DMG uses it
        # as the volume icon.
        shutil.copyfile(os.path.join(actool(workdir, "11.0"), "Assets.car"), OUT_CAR)
        shutil.copyfile(legacy_icns(workdir), OUT_ICNS)
    with open(OUT_STAMP, "w") as handle:
        handle.write(document_digest() + "\n")


def preview(path):
    """The document as the system draws it in each icon style, on one sheet."""
    tool = os.path.join(os.path.dirname(developer_dir()), ICTOOL)
    if not os.path.exists(tool):
        raise SystemExit(f"Icon Composer's ictool is not at {tool}")
    cell, label = 384, 22
    sheet = Image.new("RGB", (cell * 3, (cell + label) * 2), (96, 96, 96))
    draw = ImageDraw.Draw(sheet)
    with tempfile.TemporaryDirectory() as workdir:
        # A copy under a fresh path: ictool answers from a cache keyed on the
        # document's path, so an edited document at the same path would show
        # as it was before the edit.
        icon = os.path.join(workdir, "Liu.icon")
        shutil.copytree(OUT_ICON, icon)
        for index, rendition in enumerate(RENDITIONS):
            out = os.path.join(workdir, rendition + ".png")
            run = subprocess.run(
                [tool, icon, "--export-image", "--output-file", out, "--platform", "macOS",
                 "--rendition", rendition, "--width", str(cell), "--height", str(cell),
                 "--scale", "1"], capture_output=True, text=True)
            if not os.path.exists(out):
                raise SystemExit(f"ictool did not draw {rendition}:\n" + run.stdout + run.stderr)
            x, y = (index % 3) * cell, (index // 3) * (cell + label)
            wall = (40, 44, 52) if rendition.endswith("Dark") else (200, 205, 215)
            tile = Image.new("RGBA", (cell, cell), wall + (255,))
            tile.alpha_composite(Image.open(out).convert("RGBA"))
            sheet.paste(tile.convert("RGB"), (x, y + label))
            draw.text((x + 6, y + 5), rendition, fill=(255, 255, 255))
    sheet.save(path)
    print(f"wrote {path}")


def main():
    args = sys.argv[1:]
    check = "--check" in args
    sheet = None
    if "--preview" in args:
        if args[-1] == "--preview":
            raise SystemExit("--preview needs the path of the PNG to write")
        sheet = args[args.index("--preview") + 1]
    stale = []

    def put(path, current, data):
        """Write `data` unless the file already holds it (`current`)."""
        if current:
            return
        if check:
            stale.append(os.path.relpath(path, ROOT))
            return
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(data)
        print(f"wrote {os.path.relpath(path, ROOT)} ({len(data)} bytes)")

    pixels = np.asarray(Image.open(MASTER).convert("RGB"))
    was = master_plate(pixels.astype(float))
    if np.abs(was - PLATE).max() >= 0.5:
        if check:
            print(f"assets/appicon.png is on {hexname(was)}, not {hexname(PLATE)}", file=sys.stderr)
        pixels = replate(pixels.astype(float), was)
        put(MASTER, False, master_bytes(pixels))

    ink, under, _ = split(pixels.astype(float), PLATE)
    # A box filter: at exactly 4:1 it is the area average. A windowed sinc rings
    # at the outline and leaves plate pixels that are no longer the plate.
    small = np.asarray(Image.fromarray(pixels).resize((256, 256), Image.BOX))
    fill_layer, ink_layer = layer(FILL, under), layer(INK, ink)
    put(OUT_SMALL, same_pixels(OUT_SMALL, small), png_bytes(small))
    put(OUT_FILL, same_pixels(OUT_FILL, fill_layer), png_bytes(fill_layer))
    put(OUT_INK, same_pixels(OUT_INK, ink_layer), png_bytes(ink_layer))
    text = document()
    put(OUT_JSON, os.path.exists(OUT_JSON) and open(OUT_JSON, "rb").read() == text, text)

    compiled = (not stale and all(os.path.exists(p) for p in (OUT_CAR, OUT_ICNS, OUT_STAMP))
                and open(OUT_STAMP).read().strip() == document_digest())
    if not compiled:
        if check:
            stale += [os.path.relpath(p, ROOT) for p in (OUT_CAR, OUT_ICNS)]
        else:
            compile_document()
            for path in (OUT_CAR, OUT_ICNS):
                print(f"compiled {os.path.relpath(path, ROOT)} ({os.path.getsize(path)} bytes)")

    if stale:
        for path in stale:
            print(f"stale: {path}", file=sys.stderr)
        return 1
    if sheet:
        preview(sheet)
    print(f"plate {hexname(PLATE)}, fill {hexname(FILL)}, ink {hexname(INK)}; "
          f"document {document_digest()[:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
