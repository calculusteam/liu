#!/usr/bin/env python3
"""Generate and check THIRD_PARTY_NOTICES.md.

    scripts/gen_third_party_notices.py            # rewrite the file
    scripts/gen_third_party_notices.py --check    # exit 1 if the file is out of date

A released Liu.app includes libssh2 and OpenSSL, SQLite, cJSON, stb, nanosvg,
code adapted from llama.cpp and ggml, icon sets, and fonts. The generated notice
includes the corresponding licence terms.

Dependency versions, font metadata, and licence texts are read from their
source files so the notice matches the shipped tree. Packaging runs --check and
fails when the file is stale. Regenerate it after changing a dependency, font,
icon set, or file under packaging/licenses/.
"""
import argparse
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "THIRD_PARTY_NOTICES.md")


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def must(pattern, text, what):
    m = re.search(pattern, text, re.M)
    if not m:
        sys.exit(f"gen_third_party_notices: cannot find {what}")
    return m.group(1)


def block(text):
    """A licence text as an indented block: readable as Markdown and as plain text."""
    lines = text.replace("\r\n", "\n").strip("\n").split("\n")
    return "\n".join(("    " + ln).rstrip() for ln in lines) + "\n"


def font_names(path):
    """{name id: text} for ids 0 (copyright), 1 and 16 (family), 5 (version) of an sfnt.

    A Unicode record (platform 0 or 3, UTF-16) wins over a Macintosh one, which
    is MacRoman: read as Latin-1, "Bjørnerås" came out as "Bj¿rners".
    """
    with open(path, "rb") as f:
        d = f.read()
    ntab = struct.unpack(">H", d[4:6])[0]
    off = None
    for i in range(ntab):
        tag, _cs, o, _l = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
        if tag == b"name":
            off = o
    if off is None:
        return {}
    _fmt, cnt, so = struct.unpack(">HHH", d[off:off + 6])
    out, unicode_ids = {}, set()
    for i in range(cnt):
        pid, _eid, _lid, nid, ln, o = struct.unpack(">6H", d[off + 6 + 12 * i:off + 18 + 12 * i])
        if nid not in (0, 1, 5, 16):
            continue
        is_unicode = pid in (0, 3)
        if nid in out and (nid in unicode_ids or not is_unicode):
            continue
        raw = d[off + so + o:off + so + o + ln]
        try:
            s = raw.decode("utf-16-be") if is_unicode else raw.decode("mac_roman")
        except UnicodeDecodeError:
            continue
        out[nid] = " ".join(s.split())
        if is_unicode:
            unicode_ids.add(nid)
    return out


# What each bundled face is licensed under. A font that is not listed here
# stops the generator: a new face needs its licence looked up, not guessed.
# Every face but the symbols one is a Nerd Font Mono build: the family's own
# licence for its glyphs, and the icon sets' licences for the icons it carries.
OFL = "SIL Open Font License 1.1"
NF_ICONS = "; its icons: the Nerd Fonts licensing notes (below)"
FONT_LICENSE = {
    "BlexMono Nerd Font Mono": OFL + NF_ICONS,
    "CaskaydiaCove Nerd Font Mono": OFL + NF_ICONS,
    "CommitMono Nerd Font Mono": OFL + NF_ICONS,
    "FiraCode Nerd Font Mono": OFL + NF_ICONS,
    "GeistMono Nerd Font Mono": OFL + NF_ICONS,
    "Hack Nerd Font Mono": "MIT License and Bitstream Vera License (below)" + NF_ICONS,
    "Inconsolata Nerd Font Mono": OFL + NF_ICONS,
    "Iosevka Nerd Font Mono": OFL + NF_ICONS,
    "JetBrainsMono Nerd Font Mono": OFL + NF_ICONS,
    "Maple Mono NF": OFL + NF_ICONS,
    "MonaspiceNe Nerd Font Mono": OFL + NF_ICONS,
    "RobotoMono Nerd Font Mono": "Apache License 2.0 (below)" + NF_ICONS,
    "SauceCodePro Nerd Font Mono": OFL + NF_ICONS,
    "VictorMono Nerd Font Mono": OFL + NF_ICONS,
    "Symbols Nerd Font Mono": "see the Nerd Fonts licensing notes (below)",
}
# The copyright line, for a face whose name table carries none (or a
# placeholder in its place).
FONT_COPYRIGHT = {
    "MonaspiceNe Nerd Font Mono": "Copyright (c) 2023, GitHub (https://github.com/githubnext/monaspace)",
}


def fonts_table():
    d = os.path.join(ROOT, "assets", "fonts")
    rows = []
    for name in sorted(os.listdir(d), key=str.lower):
        if not name.lower().endswith((".ttf", ".otf")):
            continue
        n = font_names(os.path.join(d, name))
        family = n.get(16) or n.get(1, "")      # the typographic family when the face names one
        if family not in FONT_LICENSE:
            sys.exit(f"gen_third_party_notices: assets/fonts/{name} is \"{family}\", "
                     "which has no entry in FONT_LICENSE — look its licence up and add it")
        version = n.get(5, "").split(";")[0].split("(")[0].replace("Version ", "").strip()
        copyright_ = FONT_COPYRIGHT.get(family) or n.get(0)
        if not copyright_:
            sys.exit(f"gen_third_party_notices: assets/fonts/{name} carries no copyright line "
                     "and FONT_COPYRIGHT has none for it")
        rows.append((family, version, name, FONT_LICENSE[family], copyright_))
    return rows


def build():
    cjson_h = read("third_party/cjson/cJSON.h")
    cjson_v = ".".join(must(rf"#define CJSON_VERSION_{k} (\d+)", cjson_h, f"cJSON {k}")
                       for k in ("MAJOR", "MINOR", "PATCH"))
    sqlite_v = must(r'#define SQLITE_VERSION\s+"([0-9.]+)"', read("third_party/sqlite3/sqlite3.h"),
                    "the SQLite version")
    stb_image = read("third_party/stb/stb_image.h")
    stbi_v = must(r"stb_image - v([0-9.]+)", stb_image, "the stb_image version")
    stbtt_v = must(r"stb_truetype\.h - v([0-9.]+)", read("third_party/stb/stb_truetype.h"),
                   "the stb_truetype version")
    stb_license = stb_image[stb_image.index("ALTERNATIVE A - MIT License"):]
    stb_license = stb_license[:stb_license.index("*/")]
    nanosvg = read("third_party/nanosvg/nanosvg.h")
    nanosvg_license = "\n".join(ln.lstrip(" *") for ln in
                                nanosvg[nanosvg.index("Copyright"):nanosvg.index("*/")].split("\n"))
    nanosvg_license = nanosvg_license[:nanosvg_license.index("The SVG parser is based on")].strip()
    deps = read("scripts/build_macos_compat_deps.sh")
    openssl_v = must(r'OPENSSL_VERSION="\$\{OPENSSL_VERSION:-([0-9.]+)\}"', deps, "the OpenSSL version")
    libssh2_v = must(r'LIBSSH2_VERSION="\$\{LIBSSH2_VERSION:-([0-9.]+)\}"', deps, "the libssh2 version")
    patches = len([n for n in os.listdir(os.path.join(ROOT, "scripts", "patches", "libssh2"))
                   if n.endswith(".patch")])

    o = []
    w = o.append
    w("# Third-party notices\n\n")
    w("Liu is released under the Apache License 2.0 (see `LICENSE` and `NOTICE`). A Liu release\n"
      "also contains, or is built from, the works listed here. Each is used under its own licence,\n"
      "reproduced below. This file is generated by `scripts/gen_third_party_notices.py`; do not\n"
      "edit it by hand.\n")

    w("\n## Libraries linked into the application\n")
    w(f"\n### libssh2 {libssh2_v}\n")
    w(f"https://libssh2.org — BSD 3-Clause License. Built from the {libssh2_v} release with {patches} "
      "security patches applied\n(`scripts/patches/libssh2/`).\n\n")
    w(block(read("packaging/licenses/libssh2-COPYING.txt")))
    w(f"\n### OpenSSL {openssl_v}\n")
    w("https://www.openssl.org — Apache License 2.0 (full text at the end of this file).\n\n")
    w(block("Copyright (c) 1998-2026 The OpenSSL Project Authors\n"
            "Copyright (c) 1995-1998 Eric A. Young, Tim J. Hudson\n"
            "All rights reserved."))
    w(f"\n### SQLite {sqlite_v}\n")
    w("https://sqlite.org — public domain. Its authors have dedicated the code to the public domain;\n"
      "no licence is required to use it.\n")
    w(f"\n### cJSON {cjson_v}\n")
    w("https://github.com/DaveGamble/cJSON — MIT License. Liu carries two small edits to it "
      "(a thread-local\nerror pointer and a NULL-safe `cJSON_GetErrorPtr`).\n\n")
    w(block(read("third_party/cjson/LICENSE")))
    w(f"\n### stb_truetype {stbtt_v} and stb_image {stbi_v}\n")
    w("https://github.com/nothings/stb — public domain or MIT License, at the user's choice. "
      "Liu's copies of\nstb_truetype (a bounded CFF reader) and stb_image (a PNG data buffer that grows with the bytes "
      "read)\nare edited; the edits are marked `Liu:`.\n\n")
    w(block(stb_license))
    w("\n### NanoSVG\n")
    w("https://github.com/memononen/nanosvg — zlib License.\n\n")
    w(block(nanosvg_license))

    w("\n## Code copied or adapted into the application\n")
    w("\n### llama.cpp / ggml\n\n")
    w("https://github.com/ggml-org/llama.cpp — MIT License.\n\n")
    w("Liu includes the following copied or adapted quantization code:\n\n")
    w("- The STQ1_0 codebook, block layouts, dequantization and Q8_K dot-product kernels from\n"
      "  llama.cpp PR #22836 and its ggml support code, in `src/llm/llm_stq1_0_kernel.{c,h}`,\n"
      "  `src/llm/llm_stq1_0_codebook.h` and `src/llm/llm_quant_stq1_0.{c,h}`.\n")
    w("- The ggml Q6_K reference dequantizer, in `src/llm/llm_quant_q6k.{c,h}`.\n")
    w("- The corresponding STQ1_0 and Q6_K dequantization portions of\n"
      "  `src/llm/llm_metal_kernels.metal`.\n")
    w("- The ggml block-dequantization routines used by Whisper, in\n"
      "  `src/voice/whisper_quant.{c,h}`, `src/voice/whisper_kernels.metal` and\n"
      "  `src/voice/shaders/whisper_dequant.glsl`.\n\n")
    w("STQ1_0 source: https://github.com/ggml-org/llama.cpp/pull/22836.\n"
      "Licence text reproduced from llama.cpp commit\n"
      "`1e411d8f5a1e23525fa3265dfb4bd76265465397` (`LICENSE`).\n\n")
    w(block(read("packaging/licenses/llama.cpp-LICENSE.txt")))

    w("\n## Icons\n")
    w("\n### Lucide\n")
    w("https://lucide.dev — ISC License; portions derived from Feather are under the MIT License.\n\n")
    w(block(read("packaging/licenses/lucide-LICENSE.txt")))
    w("\n### Seti UI\n")
    w("https://github.com/jesseweed/seti-ui — MIT License. The file-type icons in the file browser.\n\n")
    w(block(read("assets/icons/seti/LICENSE.md")))
    w("\n### Devicon\n")
    w("https://devicon.dev — MIT License. Language logos for file types Seti UI has no icon for.\n\n")
    w(block(read("packaging/licenses/devicon-LICENSE.txt")))
    w("\n### Pi\n")
    w("https://github.com/earendil-works/pi — MIT License. The Pi coding agent's logo "
      "(`assets/agents/pi.png`, and `pi-on-dark.png` derived from it), drawn beside its sessions.\n\n")
    w(block(read("packaging/licenses/pi-LICENSE.txt")))
    w("\n### Oh My Pi\n")
    w("https://github.com/can1357/oh-my-pi — MIT License. The Oh My Pi coding agent's logo "
      "(`assets/agents/omp.png`, and `omp-on-dark.png` derived from it), drawn beside its sessions.\n\n")
    w(block(read("packaging/licenses/oh-my-pi-LICENSE.txt")))

    w("\n## Fonts shipped inside the application\n\n")
    w("| Family | Version | File | Licence |\n|---|---|---|---|\n")
    rows = fonts_table()
    for family, version, name, lic, _c in rows:
        w(f"| {family} | {version} | `{name}` | {lic} |\n")
    w("\nCopyright notices, as each font states its own:\n\n")
    for family, _v, _n, _l, c in rows:
        w(f"- **{family}** — {c}\n")
    w("\n### SIL Open Font License 1.1\n\n")
    ofl = read("packaging/licenses/OFL-1.1.txt")
    # The licence proper; its first lines are a fill-in template for a font's own header.
    w(block(ofl[ofl.index("This Font Software is licensed under the SIL Open Font License"):]))
    w("\n### Hack\n\n")
    w(block(read("packaging/licenses/hack-LICENSE.md")))
    w("\n### Symbols Nerd Font (Nerd Fonts)\n\n")
    w("https://www.nerdfonts.com — the symbol font is assembled from several icon sets, each under "
      "its own licence;\nthe project's licensing notes follow.\n\n")
    w(block(read("packaging/licenses/nerd-fonts-LICENSE.md")))

    w("\n## Downloaded at the user's request, never shipped\n\n")
    w("- **Whisper large-v3-turbo** (speech recognition; `ggerganov/whisper.cpp` on Hugging Face) — "
      "MIT License,\n  Copyright (c) OpenAI.\n")
    w("- **Hy-MT1.5-1.8B** (translation; `AngelSlim/Hy-MT1.5-1.8B-1.25bit-GGUF` on Hugging Face) — "
      "under the terms\n  its publisher states on the model's page.\n")

    w("\n## Trademarks\n\n")
    w("The names and logos of the AI coding tools Liu works with — among them Claude, Codex, ChatGPT, "
      "GitHub Copilot, Cursor,\nGemini, Cline, OpenCode, Amazon Q, Windsurf, Droid, Kiro, Crush, OpenClaw and Hermes — are "
      "trademarks of their respective owners. Liu shows\nthem only to identify those tools and "
      "is not affiliated with or endorsed by their owners.\n")

    w("\n## Apache License 2.0\n\n")
    w(block(read("packaging/licenses/Apache-2.0.txt")))
    return "".join(o)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if THIRD_PARTY_NOTICES.md is out of date")
    args = ap.parse_args()
    text = build()
    if args.check:
        try:
            with open(OUT, encoding="utf-8") as f:
                on_disk = f.read()
        except OSError:
            on_disk = None
        if on_disk != text:
            print("THIRD_PARTY_NOTICES.md is out of date: run scripts/gen_third_party_notices.py",
                  file=sys.stderr)
            return 1
        return 0
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"wrote {os.path.relpath(OUT, ROOT)} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
