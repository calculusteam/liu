# Seti UI file icons

Vendored from https://github.com/jesseweed/seti-ui (MIT, see LICENSE.md).

Revision: 2d6c5e68b4ded73c92dac291845ee44e1182d511 2025-10-28

`mapping.less` and `ui-variables.less` are upstream's, verbatim; the SVGs are
the ones the mapping names, verbatim except for two constructs nanosvg
draws wrong (`<use>` expanded in gradle.svg, `stroke="null"` dropped in
platformio.svg). `scripts/gen_seti_icons.py` turns the two stylesheets into
`src/ui/seti_map.h`; `--update <seti-ui checkout>` refreshes this directory.
CMake embeds every SVG here into the binary.
