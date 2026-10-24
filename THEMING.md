# Liu Theming Guide (LLM-oriented)

How to write a theme for **Liu**. Written so a model can emit a correct, drop-in theme JSON without reading the C source.

---

## TL;DR

A user theme is one JSON file at:

```
~/.config/Liu/themes/<your-theme-name>.json     (macOS / Linux)
```

It is picked up on app start (no rebuild) and appears in Settings → Appearance next to the **19 built-in themes**.

Every colour is an RGBA float array — `[r, g, b, a]`, each component `0.0`–`1.0`.

Define `name` plus all 25 colour fields. Missing fields are silently zero-filled (transparent black), so a partial theme does not fall back to sensible defaults — it paints holes. **Always emit every field.**

---

## Built-in themes

`Liu`, its light half `Liu Light`, and `Ultradark` are the app's own. `Liu` and `Liu Light` track the website's palette — see [Liu and Liu Light follow the site](#liu-and-liu-light-follow-the-site). The other sixteen are ports, and each one is transcribed from that project's **own** source of truth — the upstream repo's theme JSON or palette file, cited in a comment above each block in `src/core/config.c`. They are not sampled from screenshots, so a theme called "Nord" here shows the colours Nord shows everywhere else.

| # | Theme | Palette taken from |
|---|---|---|
| 1 | Liu | `website/src/style.css` `:root` — the ink ramp and the violet accent |
| 2 | Liu Light | the same ramp turned over; accent `violet-700`, the step the site reserves for a light theme |
| 3 | Ultradark | this project |
| 4 | GitHub Dark Dimmed | `@primer/primitives` `dark_dimmed.json` |
| 5 | Pierre Dark | `pierrecomputer/theme` → `themes/pierre-dark.json` |
| 6 | One Dark Pro | `Binaryify/OneDark-Pro` |
| 7 | Dracula | `dracula/visual-studio-code` → `src/dracula.yml` |
| 8 | Dracula Soft | the same file, put through upstream's own `transformSoft` |
| 9 | Catppuccin Mocha | `catppuccin/palette` → `palette.json` |
| 10 | Nord | `arcticicestudio/nord-visual-studio-code` |
| 11 | Solarized Dark | Ethan Schoonover's published 16-colour mapping |
| 12 | Vitesse Dark | `antfu/vscode-theme-vitesse` |
| 13 | Min Dark | `miguelsolorio/min-theme` |
| 14 | Monokai | the original Monokai |
| 15 | Tokyo Night | `folke/tokyonight.nvim` → `extras/kitty` |
| 16 | Night Owl | `sdras/night-owl-vscode-theme` |
| 17 | Rosé Pine | `rose-pine/kitty` → `dist/rose-pine.conf` |
| 18 | Ayu Dark | `ayu-theme/vscode-ayu` |
| 19 | Slack Dark | Slack's documented dark surfaces + brand accents |

Three of those ports carry a quirk that looks like a transcription bug and is not:

- **Solarized Dark** puts `base03` — its own background — in ANSI slot 8. Bright-black text is therefore invisible. That is what the spec says and what every other Solarized terminal does.
- **Rosé Pine** maps *pine* (a teal) to GREEN and *rose* to CYAN, and repeats its normal row as the bright row. Upstream does this deliberately.
- **Dracula Soft** is not a second hand-tuned palette. Upstream generates it by desaturating Dracula's ANSI set by 20 HSL points; this port runs the identical transform.

**Where a terminal export exists, it wins over the editor theme.** Tokyo Night's VS Code port maps `ansiGreen` to a teal and repeats normals as brights; its kitty export doesn't. Liu is a terminal, so it ships the kitty numbers.

### Liu and Liu Light follow the site

The site (`website/src/style.css`, the "Brand palette" block) is the source of truth; `src/core/config.c` follows it, never the other way round. A neutral **ink** ramp carries the page and one accent, **violet**, carries interaction. The site only ships the dark side plus one light hint (`violet-700`, "light theme" in its comment), so every other Liu Light value is the dark theme's *role* read off the opposite end of the ramp.

| Role | Liu | Liu Light |
|---|---|---|
| `bg` | ink-950 `#0A0A0B` | ink-50 `#F7F7F8` (Paper) |
| `fg`, `ansi_15`, `tab_active_fg` | ink-50 `#F7F7F8` | ink-950 `#0A0A0B` |
| `ui_accent`, `cursor` | violet-500 `#A287FF` | violet-700 `#7151C7` |
| `selection` | violet-800 at 0.70 | violet-300 at 0.85 |
| `sidebar_bg` (the well) | ink-900 `#161618` | ink-100 `#EEEEF0` |
| `sidebar_hover`, `tab_active_bg` | ink-800 `#252529` | ink-200 `#E0E0E4` |
| `sidebar_active` | bg 26 % toward the accent | bg 26 % toward the accent |
| `border` | ink-700 `#3A3A3F` | ink-300 `#C6C6CC` |
| `sidebar_fg` (dim text) | ink-300 `#C6C6CC` | ink-700 `#3A3A3F` |
| `status_fg` | ink-400 `#A1A1AA` | ink-600 `#55555C` |
| `tab_inactive_fg` | ink-500 `#7A7A83` | ink-500 `#7A7A83` |
| `scrollbar_thumb` | violet-600 `#8C68F0` | violet-600 `#8C68F0` |
| `ansi_0` / `7` / `8` | bg · ink-300 · ink-600 | ink-200 · ink-600 · ink-400 |
| `ansi_1`–`6` | the status colours one ramp step down; magenta violet-600 | the same hues, OKLCH L 0.46; magenta violet-800 |
| `ansi_9`–`14` | danger / success / warning, derived blue and cyan, magenta violet-500 | L 0.55; magenta violet-700 |

Blue and cyan are the two hues the site has no colour for; they are derived (see [When the source palette is small](#when-the-source-palette-is-small)).

**Known drift.** Colours that mirror the *previous* Liu palette (`#121212` canvas, `#C674F3` accent family) and have not been moved: the built-in browser's injected CSS (`src/browser/browser_macos.m`, `src/browser/pick_element_js.h`) and the DMG artwork (`assets/dmg/background.html`, `scripts/dmg_build_settings.py`). They do not read `THEME_Liu`, so changing the theme does not change them.

---

## File location

| Item | Value |
|---|---|
| Loader | `src/core/theme_import.c` → `theme_load_user_themes()` |
| Directory | `theme_user_dir()` → `~/.config/Liu/themes/` |
| Filename | anything ending in `.json` |
| Display name | the JSON's `"name"` field, not the filename |
| Max user themes | `MAX_USER_THEMES` = 32 |
| Max file size | 256 KB |
| Reload | restart the app |

`mkdir -p ~/.config/Liu/themes` if it isn't there.

---

## JSON schema

```json
{
    "name": "Theme Display Name",

    "fg":        [r, g, b, a],
    "bg":        [r, g, b, a],
    "cursor":    [r, g, b, a],
    "selection": [r, g, b, a],

    "ansi_0":  [r, g, b, a],
    "ansi_1":  [r, g, b, a],
    "ansi_2":  [r, g, b, a],
    "ansi_3":  [r, g, b, a],
    "ansi_4":  [r, g, b, a],
    "ansi_5":  [r, g, b, a],
    "ansi_6":  [r, g, b, a],
    "ansi_7":  [r, g, b, a],
    "ansi_8":  [r, g, b, a],
    "ansi_9":  [r, g, b, a],
    "ansi_10": [r, g, b, a],
    "ansi_11": [r, g, b, a],
    "ansi_12": [r, g, b, a],
    "ansi_13": [r, g, b, a],
    "ansi_14": [r, g, b, a],
    "ansi_15": [r, g, b, a],

    "tab_bg":          [r, g, b, a],
    "tab_active_bg":   [r, g, b, a],
    "tab_active_fg":   [r, g, b, a],
    "tab_inactive_bg": [r, g, b, a],
    "tab_inactive_fg": [r, g, b, a],

    "sidebar_bg":     [r, g, b, a],
    "sidebar_fg":     [r, g, b, a],
    "sidebar_hover":  [r, g, b, a],
    "sidebar_active": [r, g, b, a],

    "border":          [r, g, b, a],
    "scrollbar":       [r, g, b, a],
    "scrollbar_thumb": [r, g, b, a],

    "status_bg": [r, g, b, a],
    "status_fg": [r, g, b, a],

    "ui_accent": [r, g, b, a],

    "opacity":         0.0,
    "cursor_style":    0,
    "cursor_blink":    0,
    "bold_is_bright":  0
}
```

**Parser notes** (`parse_color_array` in `theme_import.c`):

- Line-based extraction; key order does not matter.
- Numbers go through `atof()` — write `0.8`, never `0,8`.
- Alpha defaults to `1.0` only when the array is shorter than 4 elements. Always emit 4.
- No hex strings, no HSL, no named colours. Floats only.
- Strict JSON: no comments, no trailing commas.

---

## How the chrome actually gets its colours

This is the part that changed, and it is the part most worth understanding before picking values.

The UI **does not** read your fields directly for most of what it paints. `chrome_palette_for()` (`src/ui/chrome_palette.c`) derives a set of *semantic* colours from your theme once per frame, and the whole interface paints from those:

| Semantic colour | Derived from |
|---|---|
| `surface_neutral` / `surface_raised` / `surface_sunken` | `bg`, shifted ±0.03–0.12 (direction flips on light themes) |
| `btn_primary_bg` | the resolved **accent** (below), auto-darkened if too bright |
| `btn_primary_fg` | pure black or white, whichever reads on `btn_primary_bg` |
| `btn_secondary_bg` / `_fg` | `surface_raised`, and `fg` — swapped for black/white if their contrast drops under 3.0 |
| `btn_destructive_bg` / `_fg` | a fixed red, tone-adjusted for light vs dark |
| `divider_subtle` / `divider_strong` | `border` at 0.45 / 1.0 alpha |

Two consequences worth internalising:

1. **You cannot make the chrome unreadable by picking a bad accent.** If `btn_primary_bg` lands above 0.45 luminance it gets blended 55 % toward near-black so white labels still clear AA. This is a safety net, not a licence: a pastel accent will be visibly *darkened* on screen and won't look like the colour you chose. Pick one that survives on its own.
2. **If `border` equals `bg`, dividers are synthesised** from `bg` blended 20 % toward `fg` rather than disappearing. Again a net, not a feature — set `border` properly.

### The accent, and why to always set it explicitly

`ui_accent` is the single highlight colour for the entire app: the command-palette caret, its selection pill and left accent strip, the scrollbar thumb, every focused-field underline, every primary action button (Connect / Generate / Submit / OK / **Create Theme**), the Settings selection highlight, and the tab underline.

It resolves in this order:

1. **`ui_accent` with alpha > 0** — used verbatim.
2. **`cursor`, if vividly saturated** (saturation > 0.40) — a warm or branded cursor implies warm chrome without duplicating the value.
3. **A legacy ANSI slot** — and here the two resolvers *disagree*: `chrome_palette_for()` falls through to `ansi[4]`, while `theme_ui_accent()` falls through to `ansi[12]`. A theme that reaches step 3 can therefore get two different accents in two different parts of the UI.

Because the resolvers use different ANSI fallbacks, **every built-in theme sets `ui_accent` explicitly**. Custom themes should set it too. Steps 2 and 3 preserve compatibility with older theme files.

`ui_accent` is kept separate from `ansi[4]` / `ansi[12]` because those are *terminal* colours — they are what a shell prints for `\033[34m` and `\033[96m`. Re-tinting them so the UI matches a warm theme makes `ls --color` and `git status` output look wrong.

**`ansi[5]` supplies terminal magenta and tints inline-tag backgrounds in the Markdown viewer.** Primary-button labels use `ui_accent`.

### Hover is one shared engine

Every clickable control — toolbar buttons, tabs, settings cards, list rows, dialog buttons — routes through `ui_hover_amount()` (`src/ui/anim.c`), which returns a time-eased 0→1 and is painted as a translucent wash of the control's *own* foreground colour. Themes do not get a per-control hover colour and don't need one; what you control is the surface underneath, via `bg` and `sidebar_hover`.

---

## What each colour controls

### Terminal content

| Field | Purpose |
|---|---|
| `fg` | default text colour when no SGR `38;…` is active |
| `bg` | every cell's background, the framebuffer clear colour, and the base for all derived chrome surfaces |
| `cursor` | the cursor block / bar / underline; also the accent fallback when saturated |
| `selection` | mouse-selection highlight, composited (alpha 0.5–0.9 is typical) |
| `ansi_0` … `ansi_7` | ANSI standard 8 (Black, Red, Green, Yellow, Blue, Magenta, Cyan, White) |
| `ansi_8` … `ansi_15` | the bright variants, same order |

The 256-colour cube and 24-bit truecolor are computed, not themed — only these 16 come from you.

### Tab bar / toolbar

| Field | Where it shows |
|---|---|
| `tab_bg` | tab-strip background |
| `tab_active_bg` | active tab fill; also the row-alternate colour in list views |
| `tab_active_fg` | active tab title |
| `tab_inactive_bg` | inactive tab fill |
| `tab_inactive_fg` | inactive tab titles and dimmed labels |

Toolbar height and tab width are compile-time constants (`TOOLBAR_HEIGHT_PT`, `TAB_WIDTH_PT` in `src/ui/layout.h`), not themeable. Chrome alpha follows `config.opacity`.

### Sidebar and file browser

| Field | Where it shows |
|---|---|
| `sidebar_bg` | the *well* colour: input fields, cards, the file-browser tab's list rail. The docked sidebar itself paints **no** fill — it sits on `bg`, the window's clear colour, the same rule as the status bar — so it can never differ from the terminal beside it, by theme or by opacity |
| `sidebar_fg` | default text in the panel; also the tint of the shared hover wash |
| `sidebar_hover` | secondary-button, list-row and floating-bar fills in the panels that paint their own (e.g. broadcast, port forwards). Those sit on `bg` *or* on the `sidebar_bg` well, so it has to read on both — see [Easy-to-miss places](#easy-to-miss-places) |
| `sidebar_active` | selected row fill, here and in lists across the app |

File-browser rows hang their icons on one left rail (`FB_ROW_PAD_PT`) and are anchored by icon **ink**, not by icon box — that is layout, not theming, but it means a row's icon colour (`sidebar_fg`, accent for folders) reads as a clean column.

### Misc

| Field | Where it shows |
|---|---|
| `border` | every divider — tab strip, sidebar edge, panel outlines, settings rules. Also the source of `divider_subtle`/`divider_strong` |
| `scrollbar` | scrollbar track |
| `scrollbar_thumb` | scrollbar handle |
| `status_bg` | reserved; the status bar paints no fill of its own and sits on `bg` (see `render_status_bar`) |
| `status_fg` | status text (path, branch, mode) |
| `ui_accent` | see above — the one highlight colour |

---

## Optional style overrides

These sit alongside the colours and are pushed into `AppConfig` when the theme becomes active. All are **opt-in**: `0` (or omitted) leaves the user's setting alone, which is why built-in themes never disturb preferences.

| Key | Type | Sentinel | Values | Effect |
|---|---|---|---|---|
| `opacity` | float | `0.0` | `0.30`–`1.00` | window background opacity (clamped up below 0.30) |
| `cursor_style` | int | `0` | `1`=block, `2`=underline, `3`=bar | terminal cursor shape |
| `cursor_blink` | int | `0` | `1`=on, `2`=off | cursor blink |
| `bold_is_bright` | int | `0` | `1`=on, `2`=off | whether SGR bold maps to ANSI 8–15 |

Guidance:

- Set `opacity` only when the theme is *explicitly* translucent in mood. `0.85`–`0.95` for a subtle frost, `0.70`–`0.80` for heavy glass. Leave solid themes at `0.0`.
- `cursor_style = 2` suits retro / typewriter themes, `3` suits IDE-flavoured ones, `0` is right for most.
- `cursor_blink = 2` suits zen / minimal themes. `1` is rare.
- `bold_is_bright = 1` only pays off when your bright row genuinely differs from the normal row. On a muted palette where they nearly match, it does nothing.

**`opacity` is the window's, not the cell's.** Don't fake transparency by lowering `bg.a` — that makes individual cells transparent while the chrome stays opaque, which looks broken.

---

## Contrast rules

Every pair below is somewhere the renderer paints one of your colours on top of another. Ratios are WCAG-style luminance ratios.

| Pair | Where | Minimum |
|---|---|---|
| `fg` ↔ `bg` | terminal body text | **4.5:1** |
| `cursor` ↔ `bg` | cursor visibility | **3.0:1** |
| `tab_active_fg` ↔ `tab_active_bg` | active tab title | **4.5:1** |
| `tab_inactive_fg` ↔ `tab_inactive_bg` | inactive tab title | **3.0:1** |
| `sidebar_fg` ↔ `bg` / `sidebar_hover` / `sidebar_active` | panel text on all three row states (the sidebar sits on `bg`) | **4.5:1** each |
| `status_fg` ↔ `bg` | status bar (sits on `bg`) | **4.5:1** |
| `ansi_1`…`ansi_6` ↔ `bg` | regular ANSI as text | **3.0:1** each |
| `ansi_9`…`ansi_14` ↔ `bg` | bright ANSI as text | **4.0:1** each |
| `ansi_15` ↔ `bg` | "white" text | **4.5:1** |
| `ui_accent` ↔ `bg` | caret, pill, thumb, focused underline | **3.0:1** |
| `scrollbar_thumb` ↔ `scrollbar` | scrollbar handle | **2.5:1** |
| `border` ↔ `bg` | dividers | **1.3–2.5:1** — visible but quiet |
| `selection` composited ↔ both `bg` and `fg` | selected text | readable in both directions |

Two slots are exempt from the "must be visible" rule, and only these two: **`ansi_0`** (the palette's black, conventionally at or near `bg`) and **`ansi_8`** (bright black — Solarized puts its own canvas there).

Liu's theme check enforces the load-bearing subset of this table over every built-in theme.

**The check is a floor, not the table.** It asserts: registry/`THEME_COUNT` agreement, unique names, opaque `bg`, an explicit `ui_accent`, `fg`/`bg` ≥ 4.5, every ANSI slot but 0 and 8 ≥ **1.6** (not the 3.0 / 4.0 above), `ui_accent`/`bg` ≥ 2.0, `border` ≠ `bg`, and `tab_inactive_fg`/`tab_bg` ≥ 2.0. It does **not** look at `cursor`, the active-tab pair, `sidebar_fg` on hover/active, `status_fg`, the scrollbar thumb, `border`'s upper bound, `selection`, or any of the collisions below. A theme can therefore be "ALL PASS" and still ship invisible status text — Liu Light did, until it was moved to the site palette. Run the full contrast table by hand (or script it) for any theme you touch.

### Combinations that must not collide

Each row was a real failure in a previously generated theme.

| Don't collide | Why |
|---|---|
| `bg` == `tab_inactive_bg` | the tab strip vanishes into the terminal |
| `tab_active_bg` == `tab_inactive_bg` | active and inactive tabs become indistinguishable |
| `sidebar_hover` == `sidebar_active` | hover can't be told from selection |
| `border` == `bg` | dividers disappear (auto-synthesised, but wrongly) |
| `selection` ≈ `fg` | selecting text makes it invisible |
| `cursor` ≈ `bg` | the cursor disappears — the most common failure |
| `ansi_9`…`ansi_15` == `ansi_1`…`ansi_7` | bold/bright output stops differing |
| `ui_accent` ≈ `bg` | pill, thumb and underlines dissolve into the panel |

The sidebar and the status bar sit on `bg` by construction (neither paints a fill), so `sidebar_bg == bg` and `status_bg == bg` are not collisions — the sidebar/terminal divide is the `border` hairline at the bar's right edge.

### Easy-to-miss places

Things that pass review, pass `test_theme_table`, and are still wrong on screen. Each one was found the hard way.

| Where | The trap | Do this |
|---|---|---|
| **Status text on a light theme** | The status bar paints no fill, so `status_fg` is drawn straight on `bg`. A "dark strip with light text" design (`status_bg` dark, `status_fg` light) leaves light text on a light canvas — nearly invisible, and the test does not check it. | `status_bg = bg`; pick `status_fg` against **`bg`** (≥ 4.5). |
| **`sidebar_hover` vs the well** | `sidebar_hover` fills secondary buttons, list rows and floating bars that sit on `bg` *or* on the `sidebar_bg` well. If it equals the well it vanishes there; if it is only a hair off `bg` it vanishes on `bg`. | Keep it distinct from **both** (the built-ins sit at 1.14–1.30:1 against each), and keep `sidebar_active` distinguishable from it by hue as well as lightness. |
| **Tinting surfaces with the accent** | The renderer blends in linear space, so a violet-tinted fill reads far stronger on screen than its nominal value and the whole UI blooms. | Keep chrome surfaces neutral (tab, well, hover). The accent belongs in the thumb, the palette pill, underlines and the selected row — and the selected row is `bg` blended ≤ ~26 % toward it, not a saturated fill. |
| **`sidebar_fg` is a role colour** | It is the dim/secondary text *everywhere* (family prefixes, detail columns, hints), not just inside the sidebar. Tinting it tints half the interface. | Neutral, ≥ 4.5 on `bg`, the well, hover **and** active. |
| **A light canvas cannot be "lifted"** | On a dark theme the active tab and hover are one step *lighter* than the bar. If `bg` is already the lightest step of your ramp, "raised" can only mean one step *darker* — copying the dark theme's direction gives you nothing to draw. | Go one step darker (or use pure white only if your palette has it). The accent tab underline carries the "active" signal. |
| **"Bright" on a light canvas** | `ansi_9`–`14` must still clear **4.0:1** on `bg`, so on a light theme the bright row is not a pastel — both rows are dark, and "bright" only means a notch lighter than regular. | Darken both rows; keep regular ≈ 0.09 OKLCH L under bright (the gap the dark theme has) and check every bright slot against `bg`, not against your eye. |
| **`ansi_0`, `ansi_7`, `ansi_8`, `ansi_15` flip on light themes** | They are *roles*, not colours: `ansi_0` is the palette's black (a surface step on light), `ansi_7` a mid-dark grey, `ansi_8` the dimmest grey (autosuggestions and comments use it), `ansi_15` the strongest text. | Map them by role. Slot 8 is exempt from the visibility rule but is the dim-text colour: choose its contrast on purpose (Liu is 2.7:1, Liu Light 2.4:1). |
| **Accent label colour** | `btn_primary_fg` is picked for you (black or white, whichever reads); the worst case across all accents is ~4.6:1, so it is always legible. But a light accent (luminance above ~0.18) gets a *dark* label, and above 0.45 it is darkened instead (see above). | Check which label you get and that you meant it. violet-500 on ink takes an ink label (white would be 2.8:1); a dark accent such as violet-700 takes white (5.7:1). |
| **The palette lives in more than one place** | Colours copied into the built-in browser's CSS, the element-picker tooltip, the DMG art and the website do not read the theme table. | When you change a built-in's colours, grep the old hex values across `src/`, `assets/`, `scripts/` and `website/` — see [Changing a built-in palette](#changing-a-built-in-palette). |

### Pre-submit checklist

Picture each element and confirm it reads:

1. **Body text** — `fg` on `bg`, AA-passing.
2. **Cursor** — visible on a blank cell.
3. **Selection** — composited `selection` over `fg` still readable.
4. **Tabs** — active vs inactive distinguishable, both titles readable.
5. **Sidebar** — neutral / hover / selected are three distinct steps, text legible on all three.
6. **Status bar** — readable and separated from the terminal.
7. **Accent** — Cmd+K: the selection pill (accent at 22 % alpha) visible over the panel, row text still readable through it.
8. **Primary buttons** — the label colour you get (black or white, chosen for you) is the one you intended, and the accent does not rely on the palette darkening it. A mid-light accent such as violet-500 is fine with an ink label; just don't design around a white one.
9. **ANSI 0–7** — each distinguishable against `bg`.
10. **ANSI 8–15** — perceptibly different from 0–7.
11. **Borders** — visible but quiet.
12. **Light themes** — if `bg` is light (mean > 0.5): push the accent *darker*, make sure `fg` is dark enough, darken **both** ANSI rows (bright still ≥ 4.0), set `status_fg` against `bg` (the status bar paints no strip), and raise surfaces by going one step *darker*, not lighter. See [Easy-to-miss places](#easy-to-miss-places).
13. **Hover vs the well** — `sidebar_hover` is visibly different from both `bg` and `sidebar_bg`, and from `sidebar_active`.
14. **The test is a floor** — `test_theme_table` passing is necessary, not sufficient; run the full contrast table above.

---

## Example: minimal correct theme

```json
{
    "name": "Midnight",

    "fg":        [0.92, 0.93, 0.96, 1.0],
    "bg":        [0.04, 0.04, 0.07, 1.0],
    "cursor":    [0.85, 0.90, 1.00, 1.0],
    "selection": [0.20, 0.30, 0.50, 0.7],

    "ansi_0":  [0.10, 0.10, 0.12, 1.0],
    "ansi_1":  [0.93, 0.36, 0.34, 1.0],
    "ansi_2":  [0.32, 0.79, 0.52, 1.0],
    "ansi_3":  [0.95, 0.82, 0.32, 1.0],
    "ansi_4":  [0.33, 0.58, 0.96, 1.0],
    "ansi_5":  [0.79, 0.50, 0.88, 1.0],
    "ansi_6":  [0.30, 0.80, 0.86, 1.0],
    "ansi_7":  [0.88, 0.90, 0.93, 1.0],
    "ansi_8":  [0.35, 0.37, 0.42, 1.0],
    "ansi_9":  [0.98, 0.49, 0.47, 1.0],
    "ansi_10": [0.51, 0.92, 0.63, 1.0],
    "ansi_11": [0.99, 0.92, 0.45, 1.0],
    "ansi_12": [0.56, 0.73, 0.98, 1.0],
    "ansi_13": [0.92, 0.60, 0.93, 1.0],
    "ansi_14": [0.49, 0.92, 0.96, 1.0],
    "ansi_15": [1.00, 1.00, 1.00, 1.0],

    "tab_bg":          [0.04, 0.04, 0.07, 1.0],
    "tab_active_bg":   [0.13, 0.15, 0.21, 1.0],
    "tab_active_fg":   [1.00, 1.00, 1.00, 1.0],
    "tab_inactive_bg": [0.04, 0.04, 0.07, 1.0],
    "tab_inactive_fg": [0.58, 0.60, 0.66, 1.0],

    "sidebar_bg":     [0.05, 0.05, 0.08, 1.0],
    "sidebar_fg":     [0.79, 0.81, 0.84, 1.0],
    "sidebar_hover":  [0.10, 0.12, 0.16, 1.0],
    "sidebar_active": [0.18, 0.27, 0.45, 1.0],

    "border":          [0.14, 0.16, 0.22, 1.0],
    "scrollbar":       [0.04, 0.04, 0.07, 1.0],
    "scrollbar_thumb": [0.28, 0.31, 0.38, 1.0],

    "status_bg": [0.04, 0.04, 0.07, 1.0],
    "status_fg": [0.61, 0.64, 0.69, 1.0],

    "ui_accent": [0.33, 0.58, 0.96, 1.0],

    "opacity":         0.0,
    "cursor_style":    0,
    "cursor_blink":    0,
    "bold_is_bright":  0
}
```

Drop it in `~/.config/Liu/themes/midnight.json`, restart, pick "Midnight" in Settings → Appearance.

Swapping the accent for a warm one:

```json
    "ui_accent": [0.93, 0.55, 0.18, 1.0]
```

…repaints every focused underline, palette pill, scrollbar thumb and primary button in orange. `ansi_4` stays blue, so `ls --color` keeps its blue directories.

### With style overrides

A translucent theme with an underline cursor — same colour block plus:

```json
    "opacity":         0.88,
    "cursor_style":    2,
    "cursor_blink":    1,
    "bold_is_bright":  1
}
```

---

## Deriving chrome from a palette you already have

If you are porting a theme and only have terminal colours, the built-ins were generated by one rule set. Reproducing it gives chrome that matches the rest of Liu:

| Field | Rule |
|---|---|
| `tab_bg`, `tab_inactive_bg`, `scrollbar`, `status_bg` | `bg` |
| `tab_active_bg` | upstream's active-tab colour, else `bg` blended 8 % toward `fg` |
| `tab_active_fg` | `ansi_15` |
| `tab_inactive_fg` | upstream's, else `fg` blended 45 % toward `bg` |
| `sidebar_bg` / `sidebar_fg` | upstream's sidebar colours, else `bg` / `fg` |
| `sidebar_hover` | `bg` blended 7 % toward `fg` |
| `sidebar_active` | `bg` blended 22 % toward the accent |
| `border` | upstream's panel border, else `bg` blended 14 % toward `fg` |
| `scrollbar_thumb` | accent blended 25 % toward `bg` |
| `status_fg` | upstream's, else `fg` blended 30 % toward `bg` |

Upstream's own value wins wherever the source theme defines one; the blends are only for what it leaves unspecified. Sixteen hand-tuned chromes would be sixteen slightly different interfaces — this way they are one interface wearing different colours.

### When the source palette is small

Some sources define a handful of colours, not sixteen — the site gives three status colours and one hue. Don't borrow another theme's blues to fill the gaps; derive them at the same weight as what you do have:

1. Take the source's own colours for the slots they cover (danger → red, success → green, warning → yellow, the accent hue → magenta).
2. Convert them to OKLCH and read their lightness and chroma.
3. Derive the missing hues at that weight, pulling chroma in until the colour is inside sRGB. Liu: blue at hue ≈ 256, cyan at ≈ 208 (L 0.73 / 0.80 on a dark canvas).
4. Derive the regular row from the bright row. Dark canvas: same hue and chroma, L − 0.085. Light canvas: regular L 0.46, bright L 0.55.
5. **Blue stays a true blue.** It is what `ls` prints for directories; the themed accent must not repaint it (that is why `ui_accent` is separate from `ansi_4` / `ansi_12`).
6. Check the result against the contrast table with numbers, not by eye: regular ≥ 3.0, bright ≥ 4.0.

### Light themes: the same roles from the other end of the ramp

Don't invert colours — invert *roles*. If the source gives you a ramp, the canvas becomes its lightest step, text its darkest, and each chrome role is read off the mirrored step (Liu Light: well ink-100, hover and active tab ink-200, border ink-300, dim text ink-700). The accent is the source's darker step. Then re-check everything against `bg`: a pastel that carries on a dark canvas is highlighter ink on a light one.

---

## Changing a built-in palette

Whenever you edit a block in `src/core/config.c`:

1. Keep the source cited in the comment above the block, and say in a comment *why* any value departs from it.
2. `cmake --build build --target test_theme_table && ./build/test_theme_table`.
3. Run the **full** contrast table for the theme (the test is only a floor — see Contrast rules) and the collisions list.
4. Grep the **old** hex values across `src/`, `assets/`, `scripts/` and `website/` — copies of a palette in the built-in browser's CSS, the DMG art or the website do not read the table and drift silently (see *Known drift* above).
5. If you changed `Liu`, change `Liu Light` in the same commit (and vice versa); update the relationship comments between them and the table in *Liu and Liu Light follow the site*.
6. Update the *Built-in themes* table here if the source changed.

---

## Importing from other terminals

Liu ingests two external formats. The importer fills the terminal palette and derives chrome from `bg`.

| Format | Extension | Function |
|---|---|---|
| iTerm2 colour preset (XML plist) | `.itermcolors` | `theme_import_itermcolors()` |
| Alacritty | `.yml` / `.yaml` / `.toml` | `theme_import_alacritty()` |
| Auto-detect | any | `theme_import_file()` |

`theme_save_user()` writes the result as the JSON above into `~/.config/Liu/themes/`.

---

## Reference

| File | Contents |
|---|---|
| `src/core/config.h` | the `Theme` struct, the extern list, `THEME_COUNT` |
| `src/core/config.c` | all 19 built-in definitions, each with its upstream source cited |
| `src/ui/chrome_palette.h` / `.c` | the semantic-colour derivation the whole UI paints from |
| `src/core/theme_import.h` / `.c` | user-theme loader, JSON read/write, importers, `theme_user_dir()` |
| `src/ui/anim.c` | `ui_hover_amount()` — the one hover engine |

Starting point: copy `THEME_Liu` from `src/core/config.c`, rename it, and work from there — it sets every field including `ui_accent`.
