#!/usr/bin/env python3
"""Seti UI file icons (https://github.com/jesseweed/seti-ui, MIT) for the file
browser: which icon and which colour a file name gets.

The rules are Seti's own, read from its mapping.less. Two files are vendored
verbatim under assets/icons/seti/ (mapping.less, ui-variables.less) together
with the SVGs the mapping names; this script turns them into src/ui/seti_map.h.

    scripts/gen_seti_icons.py                    regenerate src/ui/seti_map.h
    scripts/gen_seti_icons.py --check            fail if it is stale, or an SVG is missing
    scripts/gen_seti_icons.py --update <dir>     re-vendor from a seti-ui checkout, then regenerate

How a pattern is read: a pattern that starts with a dot is an EXTENSION and
matches the longest dot-suffix of a name ("a.spec.ts" tries "spec.ts", then
"ts"); anything else is a whole file NAME. Both compare lower-cased. Seti's own
stylesheet matches by substring instead, which paints "mixin.less" with the Hex
mark because it contains "mix".

`.icon-partial` rules ("Dockerfile", "Makefile", "LICENSE", ...) are also kept
as PREFIX rules, tried last and only at a separator, so "Dockerfile.dev" and
"Makefile.am" keep the mark their family has while "todo.service.ts" stays
TypeScript.

SAME_AS below is the one thing here that is not Seti's: names of a language
Seti already draws, which its mapping does not list (".mts", ".jsonc",
".bash"). An editor with a language service reaches those through the file's
LANGUAGE; the explorer has none, so each is spelled as "the same as <that
pattern>". Without it ".ts" would wear Seti's mark and ".mts" beside it a
fallback icon.
"""
import argparse
import copy
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "assets" / "icons" / "seti"
HEADER = ROOT / "src" / "ui" / "seti_map.h"

RULE = re.compile(
    r"""^\s*\.icon-(set|partial)\(\s*(['"])(.+?)\2\s*,\s*(['"])(.+?)\4\s*,\s*@([\w-]+)\s*\)""",
    re.M)
COLOR = re.compile(r"^@([\w-]+):\s*#([0-9a-fA-F]{6})\s*;", re.M)
# styles/default-theme.less: `@seti-primary: @blue;`
ALIASES = {"seti-primary": "blue"}
DEFAULT_ICON = "default"       # mapping.less: `.icon("default", @white)`
DEFAULT_COLOR = "white"
PATTERN_OK = re.compile(r"[\w.+\-]+")
# pattern -> the Seti pattern it is the same language as (both in mapping.less
# syntax: a leading dot is an extension, anything else a whole name). A target
# Seti stops listing fails the run.
SAME_AS = {
    ".mts": ".ts", ".cts": ".ts",
    ".jsonc": ".json", ".json5": ".json",
    ".bash": ".sh", ".ksh": ".sh", ".bats": ".sh", ".profile": ".sh", ".bash_profile": ".sh",
    ".htm": ".html", ".xhtml": ".html",
    ".pyi": ".py", ".pyw": ".py", ".pyx": ".py", "Pipfile": ".py",
    ".gemspec": ".rb", ".rake": ".rb", ".ru": ".rb", ".podspec": ".rb", "Rakefile": ".rb",
    ".pm": ".pl", ".pod": ".pl",
    ".phtml": ".php",
    ".fsi": ".fs", ".csx": ".cs",
    ".ipp": ".cpp", ".tpp": ".cpp",
    ".mdx": ".md",
    ".heex": ".ex", ".eex": ".ex", ".leex": ".ex",
    ".hcl": ".tf", ".zon": ".zig",
    "go.mod": ".go", "go.sum": ".go",
    ".cmake": "CMakeLists.txt",
}


def parse():
    mapping = (VENDOR / "mapping.less").read_text()
    palette = dict(COLOR.findall((VENDOR / "ui-variables.less").read_text()))
    rules = RULE.findall(mapping)
    looks_like = sum(1 for ln in mapping.splitlines()
                     if re.match(r"\s*\.icon-(set|partial)\(", ln))
    if looks_like != len(rules):
        sys.exit(f"gen_seti_icons: {looks_like - len(rules)} rule lines did not parse")

    ext, name, prefix = {}, {}, {}
    for kind, _, pat, _, icon, color in rules:
        color = ALIASES.get(color, color)
        if color not in palette:
            sys.exit(f"gen_seti_icons: unknown colour @{color} for {pat!r}")
        if not PATTERN_OK.fullmatch(pat):
            sys.exit(f"gen_seti_icons: pattern {pat!r} needs escaping")
        key = pat.lower()
        # Later rules win, as they do in the stylesheet's cascade.
        if key.startswith("."):
            ext[key[1:]] = (icon, color)
        else:
            name[key] = (icon, color)
            # A pattern with an extension of its own is a whole name already.
            if kind == "partial" and "." not in key:
                prefix[key] = (icon, color)

    def table_of(pat):
        return (ext, pat[1:].lower()) if pat.startswith(".") else (name, pat.lower())

    added = set()
    for pat, same in SAME_AS.items():
        src_table, src_key = table_of(same)
        dst_table, dst_key = table_of(pat)
        if src_key not in src_table:
            sys.exit(f"gen_seti_icons: SAME_AS {pat!r}: Seti has no rule for {same!r}")
        if dst_key in dst_table:
            sys.exit(f"gen_seti_icons: SAME_AS {pat!r}: Seti lists it now; drop the entry")
        dst_table[dst_key] = src_table[src_key]
        added.add((id(dst_table), dst_key))
    parse.added = {("ext" if t == id(ext) else "name", k) for t, k in added}
    return palette, ext, name, prefix


def render():
    palette, ext, name, prefix = parse()
    used = sorted({v[0] for t in (ext, name, prefix) for v in t.values()} | {DEFAULT_ICON})
    colors = sorted({v[1] for t in (ext, name, prefix) for v in t.values()} | {DEFAULT_COLOR})
    if len(used) > 255 or len(colors) > 255:
        sys.exit("gen_seti_icons: the rule table stores icon and colour in a u8")
    icon_ix = {n: i for i, n in enumerate(used)}
    color_ix = {n: i for i, n in enumerate(colors)}

    def cid(c):
        return "SETI_COLOR_" + c.upper().replace("-", "_")

    def table(ident, rows, order, kind=None):
        out = [f"static const SetiRule {ident}[] = {{"]
        for pat in sorted(rows, key=order):
            icon, color = rows[pat]
            ours = ", Liu: SAME_AS" if (kind, pat) in parse.added else ""
            out.append(f'    {{ "{pat}", {icon_ix[icon]}, {cid(color)} }},'
                       f"   /* {icon}{ours} */")
        out.append("};")
        return out

    o = []
    o.append("/* GENERATED by scripts/gen_seti_icons.py from assets/icons/seti/mapping.less")
    o.append(" * and ui-variables.less (Seti UI, MIT). Do not edit; re-run the script. */")
    o.append("#ifndef UI_SETI_MAP_H")
    o.append("#define UI_SETI_MAP_H")
    o.append("")
    o.append('#include "core/types.h"')
    o.append("")
    o.append("/* One row per assets/icons/seti/<name>.svg the mapping names, in strcmp")
    o.append(" * order; a rule's `icon` indexes it. */")
    o.append(f"#define SETI_ICON_COUNT {len(used)}")
    o.append("static const char *const SETI_ICON_NAMES[SETI_ICON_COUNT] = {")
    line = "   "
    for n in used:
        piece = f' "{n}",'
        if len(line) + len(piece) > 78:
            o.append(line)
            line = "   "
        line += piece
    o.append(line)
    o.append("};")
    o.append(f"#define SETI_ICON_DEFAULT {icon_ix[DEFAULT_ICON]}   /* {DEFAULT_ICON} */")
    o.append("")
    o.append("enum {")
    for c in colors:
        o.append(f"    {cid(c)},")
    o.append("    SETI_COLOR_COUNT")
    o.append("};")
    o.append("static const u32 SETI_COLORS[SETI_COLOR_COUNT] = {")
    for c in colors:
        o.append(f"    0x{palette[c].lower()},   /* @{c} */")
    o.append("};")
    o.append(f"#define SETI_COLOR_DEFAULT {cid(DEFAULT_COLOR)}")
    o.append("")
    o.append("typedef struct { const char *pat; u8 icon; u8 color; } SetiRule;")
    o.append("")
    o.append("/* Extensions, without the leading dot, lower-case, in strcmp order. */")
    o += table("SETI_EXT_RULES", ext, lambda p: p.encode(), "ext")
    o.append("")
    o.append("/* Whole file names, lower-case, in strcmp order. */")
    o += table("SETI_NAME_RULES", name, lambda p: p.encode(), "name")
    o.append("")
    o.append("/* Name prefixes, lower-case, longest first. */")
    o += table("SETI_PREFIX_RULES", prefix, lambda p: (-len(p), p.encode()))
    o.append("")
    o.append("#endif /* UI_SETI_MAP_H */")
    return "\n".join(o) + "\n", used


SVG_NS = "http://www.w3.org/2000/svg"
XLINK_HREF = "{http://www.w3.org/1999/xlink}href"


def nanosvg_ready(text: str) -> str:
    """The two constructs in Seti's artwork that nanosvg draws wrong, rewritten.

    `<use>` is not implemented (gradle.svg would rasterise to nothing), and
    `stroke="null"` is read as an unknown colour NAME, i.e. a grey one-unit
    outline. Everything else is byte-identical to upstream."""
    text = re.sub(r'\s+stroke="null"', "", text)
    if "<use" not in text:
        return text
    ET.register_namespace("", SVG_NS)
    root = ET.fromstring(text)
    by_id = {el.get("id"): el for el in root.iter() if el.get("id")}
    for parent in list(root.iter()):
        for i, child in enumerate(list(parent)):
            if child.tag != f"{{{SVG_NS}}}use":
                continue
            ref = (child.get(XLINK_HREF) or child.get("href") or "").lstrip("#")
            if ref not in by_id:
                sys.exit(f"gen_seti_icons: <use> of unknown id {ref!r}")
            inst = copy.deepcopy(by_id[ref])
            del inst.attrib["id"]
            shift = (child.get("x"), child.get("y"))
            for key, val in child.attrib.items():
                if key not in (XLINK_HREF, "href", "x", "y", "width", "height"):
                    inst.set(key, val)
            if any(shift):
                inst.set("transform", (inst.get("transform", "") +
                                       f" translate({shift[0] or 0} {shift[1] or 0})").strip())
            parent[i] = inst
    for parent in list(root.iter()):
        for child in list(parent):
            if child.tag == f"{{{SVG_NS}}}defs":
                parent.remove(child)
    return ET.tostring(root, encoding="unicode")


def update(src: Path):
    mapping = src / "styles" / "components" / "icons" / "mapping.less"
    if not mapping.is_file():
        sys.exit(f"gen_seti_icons: {src} is not a seti-ui checkout")
    VENDOR.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(mapping, VENDOR / "mapping.less")
    shutil.copyfile(src / "styles" / "ui-variables.less", VENDOR / "ui-variables.less")
    shutil.copyfile(src / "LICENSE.md", VENDOR / "LICENSE.md")
    _, used = render()
    for old in VENDOR.glob("*.svg"):
        old.unlink()
    for n in used:
        (VENDOR / f"{n}.svg").write_text(nanosvg_ready((src / "icons" / f"{n}.svg").read_text()))
    rev = subprocess.run(["git", "-C", str(src), "log", "-1", "--format=%H %ad", "--date=short"],
                         capture_output=True, text=True).stdout.strip() or "unknown"
    (VENDOR / "README.md").write_text(
        "# Seti UI file icons\n\n"
        "Vendored from https://github.com/jesseweed/seti-ui (MIT, see LICENSE.md).\n\n"
        f"Revision: {rev}\n\n"
        "`mapping.less` and `ui-variables.less` are upstream's, verbatim; the SVGs are\n"
        "the ones the mapping names, verbatim except for two constructs nanosvg\n"
        "draws wrong (`<use>` expanded in gradle.svg, `stroke=\"null\"` dropped in\n"
        "platformio.svg). `scripts/gen_seti_icons.py` turns the two stylesheets into\n"
        "`src/ui/seti_map.h`; `--update <seti-ui checkout>` refreshes this directory.\n"
        "CMake embeds every SVG here into the binary.\n")
    print(f"vendored {len(used)} icons from {rev}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--update", metavar="SETI_UI_DIR", type=Path)
    args = ap.parse_args()
    if args.update:
        update(args.update)
    text, used = render()
    missing = [n for n in used if not (VENDOR / f"{n}.svg").is_file()]
    stray = sorted(p.stem for p in VENDOR.glob("*.svg") if p.stem not in set(used))
    if missing:
        sys.exit(f"gen_seti_icons: no SVG for {', '.join(missing)}")
    if stray:
        sys.exit(f"gen_seti_icons: SVGs no rule names: {', '.join(stray)}")
    if args.check:
        if not HEADER.is_file() or HEADER.read_text() != text:
            sys.exit(f"gen_seti_icons: {HEADER.relative_to(ROOT)} is stale; re-run without --check")
        print(f"ok: {len(used)} icons, {HEADER.relative_to(ROOT)} is current")
        return
    HEADER.write_text(text)
    print(f"wrote {HEADER.relative_to(ROOT)} ({len(used)} icons)")


if __name__ == "__main__":
    main()
