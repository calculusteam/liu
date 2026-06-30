# Liu Theming Guide (LLM-oriented)

This document describes how to create a theme for **Liu** (the SSH client / terminal emulator in this repo). It is written so an LLM can produce a correct, drop-in theme JSON without reading the C source.

---

## TL;DR

A user theme is a single JSON file placed at:

```
~/.config/Liu/themes/<your-theme-name>.json   (macOS / Linux)
```

It is auto-loaded on app startup (no rebuild). The theme appears in the Settings → Appearance tab alongside the 14 built-in themes.

Every color is an RGBA float array: `[r, g, b, a]` with each component in `[0.0, 1.0]`.

A minimum-viable theme defines `name` + the 24 color fields listed below. Missing fields are silently zero-filled (transparent black) — **always provide all of them**.

---

## File location

| Item | Value |
|---|---|
| Loader | `src/core/theme_import.c` → `theme_load_user_themes()` |
| Directory accessor | `theme_user_dir()` → `~/.config/Liu/themes/` |
| Filename | Anything ending in `.json` (case-sensitive) |
| Display name | Read from the JSON's `"name"` field, NOT the filename |
| Max user themes | `MAX_USER_THEMES = 32` |
| Max file size | 256 KB |
| Reload | Restart the app (no live reload for user themes) |

If the directory doesn't exist, just create it with `mkdir -p ~/.config/Liu/themes`.

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

`ui_accent` is the **single UI chrome accent**: command-palette
selection pill + accent strip + caret + scrollbar thumb + input
separator, every focused-field underline in the dialogs (SSH Connect,
Create Theme, Port Forward, KBI, Passphrase, Key Manager), and the
fill of every primary action button (Connect / Generate / Submit /
OK), plus the Settings-panel font + theme grid selection highlight
and the `+` / `-` stepper glyphs.

It exists so the chrome can carry a colour that matches the *theme's
mood* without piggy-backing on `ansi[4]` or `ansi[12]` — those still
drive what shells print with `\033[34m` / `\033[96m`, and overloading
them tints regular terminal output.

**Resolution order at runtime** — the first match wins:

1. **`ui_accent` explicit (alpha > 0)** — your value, used verbatim.
2. **`cursor` as auto-accent** — if `ui_accent` is unset *and* `cursor`
   is vividly saturated (saturation > 0.40), the chrome adopts the
   cursor colour. Warm/branded themes (orange cursor, rose cursor,
   etc.) get matching warm chrome for free, no `ui_accent` line
   required.
3. **`ansi[4]` / `ansi[12]` fallback** — legacy behaviour. Built-in
   themes whose cursor is in the gray/near-white family (Kitty,
   Monokai, Dracula, the Catppuccin browns) land here, so they look
   identical to how they did before `ui_accent` was added.

**When to set `ui_accent` explicitly**

- The cursor is neutral / near-white but you still want a warm chrome.
- You want chrome accent ≠ cursor (e.g. white cursor + amber chrome).
- You're writing a strictly-deterministic theme that doesn't want
  any auto-derivation logic in the loop.

Otherwise: pick a vivid cursor and the chrome follows.

The last four keys are **optional style overrides** that change global app
settings while this theme is active. Use them sparingly — only when the
visual identity of the theme genuinely depends on them. All four use **0**
as the "no override" sentinel; omit them or leave them at 0 to keep the
user's existing preferences. See "Optional style overrides" below for the
allowed values.

**Notes on the parser:**

- Format is permissive line-based extraction (`parse_color_array` in `theme_import.c`). Order of keys does not matter.
- Numeric tokens are parsed with `atof()` — do not write `0,8` (locale comma); always use `0.8`.
- Alpha defaults to `1.0` only if the array is shorter than 4 elements; safer to always emit 4 components.
- No support for hex strings (`"#RRGGBB"`), HSL, or named colors. Floats only.
- No comments, no trailing commas (write strict JSON).
- The `"name"` is the display name shown in the picker. If omitted, the filename (without `.json`) is used as a fallback.

---

## What each color controls

### Terminal content (used by the cell renderer + ANSI escape sequences)

| Field | Purpose |
|---|---|
| `fg` | Default foreground / text color when no SGR `38;…` is active |
| `bg` | Default background color of every cell. Also clear color of the framebuffer; combined with `config.opacity` for window transparency |
| `cursor` | Solid cursor block / bar / underline color |
| `selection` | Mouse-selection highlight (alpha is respected; typical `0.5–0.8`) |
| `ansi_0` … `ansi_7` | ANSI standard 8 colors (Black, Red, Green, Yellow, Blue, Magenta, Cyan, White) |
| `ansi_8` … `ansi_15` | ANSI bright variants (same ordering) |

The ANSI palette is also extended on the fly to the xterm 256-color cube and 24-bit truecolor — those are NOT theme-driven, only the base 16 are. See `IS_TRUECOLOR(c)` and the `u32` color encoding rules in `CLAUDE.md`.

### App chrome (drawn by `src/ui/ui.c`)

#### Tab bar / toolbar (`render_toolbar`)

| Field | Where it shows |
|---|---|
| `tab_bg` | Reserved (currently the toolbar uses `bg + 0.025` lighten; this slot is read by the renderer for future tab-strip styling) |
| `tab_active_bg` | Fill of the currently selected tab; also used as the row-alternate color in known-hosts list |
| `tab_active_fg` | Text/glyph color on the active tab |
| `tab_inactive_bg` | Fill of inactive tabs (used as row-alt elsewhere too) |
| `tab_inactive_fg` | Text on inactive tabs, dimmed labels |

The toolbar height is **not** themed and is no longer config-tunable — it is a compile-time constant (`TOOLBAR_HEIGHT_PT` in `src/ui/layout.h`, currently 42pt), as is the per-tab width (`TAB_WIDTH_PT`). The chrome alpha follows `config.opacity` so the title bar matches the body's transparency.

#### Sidebar (hosts / SFTP / snippets panel on the left)

| Field | Where it shows |
|---|---|
| `sidebar_bg` | Sidebar panel background |
| `sidebar_fg` | Default text inside the sidebar |
| `sidebar_hover` | Row hover fill |
| `sidebar_active` | Selected/focused row fill (also used in lists across the app, e.g. known_hosts selection) |

#### Misc UI

| Field | Where it shows |
|---|---|
| `border` | Dividers between tab bar and content, between sidebar and terminal, panel borders, settings dividers, toolbar bottom edge (multiplied by 0.4 alpha) |
| `scrollbar` | Scrollbar track |
| `scrollbar_thumb` | Scrollbar drag handle |

#### Status bar (bottom strip — git status, mode indicators)

| Field | Where it shows |
|---|---|
| `status_bg` | Background of the bottom status bar |
| `status_fg` | Status bar text (path, branch, mode) |

#### UI chrome accent

| Field | Where it shows |
|---|---|
| `ui_accent` | **Single source of truth for every "highlight" colour the app paints.** When set (alpha > 0) it drives: command-palette caret + selection-pill + left accent strip + scrollbar thumb + input separator; the focused-field underline in every modal dialog; the fill of primary action buttons (Connect / Generate / Submit / OK / Add Rule); the Settings panel's active-row accent; the Settings → Appearance font-picker selected-card 1px border; the **Agent History picker selected-row 1px border** (4-sided, drawn around the highlighted "Resume with X" / "Start new X session" row). When **unset (alpha = 0)** the chrome falls back to `ansi[4]` (buttons / underlines) + `ansi[12]` (palette accents) — the pre-`ui_accent` behaviour — so existing themes keep working unchanged. |

`ui_accent` exists because `ansi[4]` ("blue") and `ansi[12]` ("bright cyan") are *terminal* colours: they show up whenever a shell emits `\033[34m` or `\033[96m`. Re-tinting them to match a warm theme (orange / pink / amber) makes regular `ls` / `git status` output look wrong. `ui_accent` lets you paint a warm UI without touching what the terminal renders.

**Pick a value that…**

1. **Has > 3.0:1 luminance contrast with `bg`** — the pill, caret, and accent strip need to read on the panel surface.
2. **Has luminance ≤ ~0.55 on dark themes** — primary buttons (Connect / Generate) fill with this colour and write white labels on top. On light themes, push it *darker* (luminance ≤ 0.40) so the same labels still clear AA.
3. **Reads as the theme's signature hue** — a warm-orange theme picks `[0.93, 0.55, 0.18, 1.0]`; a sage / mint theme picks `[0.45, 0.74, 0.55, 1.0]`. The user *sees* this colour every time they hit Cmd+K, click a button, or focus a field — it carries the theme's personality.

---

## Optional style overrides

These keys live alongside the colours and are applied to `AppConfig` the
moment the theme becomes active. They are **opt-in** — leaving any of
them at `0` (or omitting them) preserves the user's current settings, so
built-in themes never disturb preferences. User themes use them when the
look-and-feel genuinely depends on a specific app setting (e.g. a glass /
acrylic theme that wants reduced opacity).

| Key | Type | Sentinel | Allowed values | Effect |
|---|---|---|---|---|
| `opacity` | float | `0.0` | `0.30`–`1.00` | Window background opacity. `<0.30` is clamped up. Composited with `bg.a` to produce the final framebuffer alpha. |
| `cursor_style` | int | `0` | `1`=block, `2`=underline, `3`=bar | Hardware cursor shape inside the terminal. |
| `cursor_blink` | int | `0` | `1`=on, `2`=off | Whether the cursor blinks. |
| `bold_is_bright` | int | `0` | `1`=on, `2`=off | Whether SGR bold maps to ANSI 8–15 (bright variants). |

**Style guidance for LLM-generated themes:**

1. **Pick `opacity`** when the theme is **explicitly translucent / glassy** in mood ("Liquid Glass", "Frosted Window", "CRT", "Acrylic"). Use `0.85`–`0.95` for a subtle frost, `0.70`–`0.80` for a heavy glass look. **Do not** set opacity for solid / opaque themes — leave at `0.0`.
2. **Pick `cursor_style = 2` (underline)** for retro / typewriter / terminal-revival themes; `3` (bar) for IDE-flavoured themes ("VS Code Dark"); leave at `0` for neutral palettes (default block reads cleanly almost everywhere).
3. **`cursor_blink = 2` (off)** suits zen / minimal themes; `1` (on) is rare — only override when the theme is explicitly retro-CRT and the user expects blinking.
4. **`bold_is_bright`**: only override when the palette's bright (ANSI 8–15) row is meaningfully different from normal (ANSI 0–7); turning it on then makes bold output pop. Leave at `0` for muted/desaturated palettes where bright variants barely differ.

**Important:** `opacity` is the **window** transparency; the per-cell `bg`'s alpha component is independent and almost always `1.0`. Don't try to fake transparency by lowering `bg.a` — it just makes individual cells transparent and the chrome opaque, which looks broken.

---

## Color philosophy / contrast guidance

These are observations from the built-in themes, not hard rules — but following them produces a consistent feel:

1. **`bg` should be very dark for dark themes (typical R/G/B around `0.03–0.10`)** so terminal content + tab bar elevation differences are visible.
2. **Tab-bar and sidebar backgrounds are usually one or two shades lighter than `bg`** so the chrome is visually separable from the terminal cell area.
3. **`tab_active_bg` is brighter than `tab_inactive_bg`** — typically `bg + ~0.10` for active vs `bg + ~0.05` for inactive.
4. **`selection` typically has alpha `0.5–0.8`** — it is composited over the cell, so opaque selections look harsh.
5. **`border` is between bg and tab_inactive_bg in lightness** so dividers don't pop too hard.
6. **ANSI 0 (Black) ≠ `bg`.** Programs print on black backgrounds — keep `ansi_0` slightly different from the cell `bg` so a "black on default" cell is still legible.
7. **ANSI bright (8–15) should be perceptibly lighter/saturated than their normal counterparts (0–7)**, not identical.
8. **`fg` vs `bg` should pass WCAG AA (≥ 4.5:1 contrast)** for body text.
9. **`cursor` should contrast with `bg`** — usually a desaturated near-white for dark themes.

---

## Required color-pair contrast (hard rules)

Below is the **complete list of two-colour combinations the renderer
actually paints on screen**. Every pair is a place where one of your
fields is rendered on top of another — if their luminance ratio collapses
the user simply can't read the UI. Run through this list before
finalising. Targets are WCAG-style luminance ratios:

| Pair | Where it shows | Min contrast |
|---|---|---|
| `fg` ↔ `bg` | terminal body text | **≥ 4.5:1** |
| `cursor` ↔ `bg` | cursor visibility | **≥ 3.0:1** |
| `tab_active_fg` ↔ `tab_active_bg` | active tab title | **≥ 4.5:1** |
| `tab_inactive_fg` ↔ `tab_inactive_bg` | inactive tab title | **≥ 3.0:1** |
| `sidebar_fg` ↔ `sidebar_bg` | sidebar entries (hosts, snippets) | **≥ 4.5:1** |
| `sidebar_fg` ↔ `sidebar_hover` | sidebar hover state | **≥ 4.5:1** |
| `sidebar_fg` ↔ `sidebar_active` | selected sidebar row | **≥ 4.5:1** |
| `status_fg` ↔ `status_bg` | status bar (path, branch, mode) | **≥ 4.5:1** |
| `ansi_0` ↔ `bg` | "black" text on default bg | **≥ 1.5:1** (and ≠ `bg`) |
| `ansi_15` ↔ `bg` | "white" text on default bg | **≥ 4.5:1** |
| `ansi_1` ↔ `bg`, `ansi_2` ↔ `bg`, `ansi_3` ↔ `bg`, `ansi_4` ↔ `bg`, `ansi_5` ↔ `bg`, `ansi_6` ↔ `bg` | every regular ANSI colour as terminal text | **≥ 3.0:1** each |
| `ansi_9..ansi_14` ↔ `bg` | bright ANSI as terminal text | **≥ 4.0:1** each |
| `ansi[5]` ↔ pure-white `(1,1,1)` | accent strip + Generate/Connect button labels | **ansi_5 must be dark enough that white reads cleanly** (luminance ≤ ~0.55) |
| `ansi[4]` ↔ pure-white `(1,1,1)` | secondary action buttons + caret (only when `ui_accent` is unset) | **same rule — ansi_4 luminance ≤ ~0.55** |
| `ui_accent` ↔ `bg` | palette caret, palette selection pill, scrollbar thumb, every focused-field underline | **≥ 3.0:1** |
| `ui_accent` ↔ pure-white `(1,1,1)` | filled primary buttons (Connect / Generate / Submit / OK / Add Rule) draw white labels on top | **`ui_accent` luminance ≤ ~0.55** so labels read |
| `ui_accent` (composited at 0.22 α) ↔ `fg` | the palette's selected-row pill sits under the row's text | label text must still be readable through the tinted pill |
| `ansi[1]` ↔ `tab_inactive_bg` | "Remove" / destructive labels in the SSH dialog | **≥ 3.0:1** |
| `ansi[2]` ↔ `tab_inactive_bg` | success state labels (theme created, etc.) | **≥ 3.0:1** |
| `border` ↔ `bg` | tab-bar divider, sidebar divider | **distinguishable but soft** (1.3–2.5:1) |
| `border` ↔ `tab_inactive_bg` | top/bottom edges of the tab strip | same |
| `scrollbar_thumb` ↔ `scrollbar` | scrollbar drag handle | **≥ 2.5:1** |
| `selection` (composited) ↔ `bg` *and* `selection` ↔ `fg` | mouse selection over text | both directions readable; alpha 0.5–0.8 |

### Surfaces that paint **white text** on top

Several UI elements draw the literal RGBA `(1.0, 1.0, 1.0, 1.0)` directly on
top of one of your theme colours. If that colour is too light, the label
disappears. **Affected surfaces:**

- `ui_accent` (when set) — drives every primary button background (Connect / Generate / Submit / OK / Add Rule), every focused-field underline, the palette selection pill + caret. White labels and a near-white caret read on top, so the colour must be dark enough (luminance ≤ ~0.55) — same rule as `ansi[4]/ansi[5]` below.
- `ansi[5]` — accent strip on every modal header (SSH dialog, port-forward, KBI, settings, **Create Theme**). Generate / Connect / primary action buttons fall back to this colour **only when `ui_accent` is unset** and write white labels.
- `ansi[4]` — caret blink, focused-field underline, "Add Rule" button background — **only when `ui_accent` is unset**. White caret on `ansi[4]` is also an SSH-dialog hint.
- `tab_active_bg` — modal header backgrounds. Title text is `fg`, but checkmark glyphs in checkboxes paint `(1,1,1)` against active filled boxes.
- `sidebar_active` — currently-selected hosts / known-hosts row. The row text is `sidebar_fg`, **so don't push `sidebar_active` so light that `sidebar_fg` (typically near-white) vanishes**.

**Practical rule:** any colour that ends up as a button fill or accent
band must satisfy `luminance ≤ 0.55` so a pure-white label clears WCAG
AA. For light themes, swap: use a *darker* `ansi[4]/ansi[5]` so white
still reads.

### Forbidden combinations (must NOT collide)

Never let any of these end up at the same — or near-same — colour, even
if both look "fine" in isolation. Each row below was a real bug from a
prior auto-generated theme:

| Don't collide | Why |
|---|---|
| `bg` == `tab_inactive_bg` | tab strip vanishes into the terminal area |
| `tab_active_bg` == `tab_inactive_bg` | active vs inactive tab indistinguishable |
| `sidebar_bg` == `bg` | sidebar / terminal divide vanishes |
| `sidebar_hover` == `sidebar_active` | can't tell hover from selection |
| `sidebar_active` == `tab_active_bg` | sidebar selection looks like a stray tab |
| `border` == `bg` (or `tab_inactive_bg`) | dividers disappear |
| `selection` ≈ `fg` | selecting text makes it invisible |
| `cursor` ≈ `bg` | cursor disappears (very common failure mode) |
| `ansi_0` == `bg` | "black-on-default" text becomes invisible |
| `ansi_8` == `ansi_0`, or `ansi_9..15` == `ansi_1..7` | bold/bright variants don't differ from regular |
| `ansi_5` very light (e.g. pastel pink) | every white-on-accent button (Generate, Connect, modal header strip) becomes unreadable |
| `ansi_4` very light | caret + active-field underline + "Add Rule" all break (when `ui_accent` is unset) |
| `ui_accent` ≈ `bg` | palette selection pill, scrollbar thumb, focused-field underline all disappear into the panel |
| `ui_accent` luminance > 0.55 | white labels on Connect / Generate / Submit / OK buttons stop clearing AA |
| `ui_accent` ≈ `ansi_5` | the secondary modal-header accent stripe and the primary chrome accent collapse into a single hue — nothing distinguishes header-tone from button-tone |
| `status_bg` == `bg` | status bar vanishes into the terminal |

### Pre-submit checklist (run mentally before emitting JSON)

For each item below, picture the actual UI element and confirm it still
reads. If any check fails, adjust the involved colours **before** writing
the output.

1. **Body text** — `fg` on `bg` clearly readable, AA-passing.
2. **Cursor** — visible against `bg` even on a blank cell.
3. **Selection** — selected text (composited `selection` over `fg`) is
   still readable, not washed out.
4. **Tabs** — active and inactive tabs distinguishable; both titles
   readable.
5. **Sidebar** — neutral, hover, and selected row backgrounds are three
   visually distinct steps; entry text legible on all three.
6. **Status bar** — bottom strip readable and visually separated from
   the terminal area.
7. **Modal headers and accent strips** — `ansi[5]` dark enough that the
   pure-white "Connect" / "Generate" / dialog title labels read clearly.
8. **Buttons in dialogs** — primary buttons (filled with `ui_accent`
   when set, or `ansi[4]` / `ansi[5]` as fallback) carry white labels
   that must clear AA. Same rule for the palette pill and caret.
8a. **UI accent vs panel** — if you set `ui_accent`, picture the
    Cmd+K palette: the selection pill (your accent at 22 % alpha)
    over the panel, with row text on top. The pill should be visible
    and the text underneath should still read.
9. **ANSI 0–7 vs `bg`** — every basic colour distinguishable as terminal
   text.
10. **ANSI 8–15** — perceptibly different from 0–7.
11. **Borders** — visible but quiet; tab-bar bottom edge (rendered at
    0.4× alpha) still visible.
12. **Light theme exception** — if `bg` is light (mean R/G/B > 0.5),
    invert the `ansi[4]/ansi[5]` rule: use *darker* accents so white
    button labels still clear AA, and ensure `fg` is dark enough.

A theme that passes all twelve points will look consistent across every
panel in the app: terminal, tabs, sidebar, settings, SSH dialog, the
Create Theme dialog itself, port forwarding, and toasts.

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

    "tab_bg":          [0.05, 0.06, 0.09, 1.0],
    "tab_active_bg":   [0.13, 0.15, 0.21, 1.0],
    "tab_active_fg":   [1.00, 1.00, 1.00, 1.0],
    "tab_inactive_bg": [0.08, 0.09, 0.13, 1.0],
    "tab_inactive_fg": [0.58, 0.60, 0.66, 1.0],

    "sidebar_bg":     [0.05, 0.05, 0.08, 1.0],
    "sidebar_fg":     [0.79, 0.81, 0.84, 1.0],
    "sidebar_hover":  [0.10, 0.12, 0.16, 1.0],
    "sidebar_active": [0.18, 0.27, 0.45, 1.0],

    "border":          [0.14, 0.16, 0.22, 1.0],
    "scrollbar":       [0.08, 0.09, 0.13, 1.0],
    "scrollbar_thumb": [0.28, 0.31, 0.38, 1.0],

    "status_bg": [0.05, 0.06, 0.09, 1.0],
    "status_fg": [0.61, 0.64, 0.69, 1.0],

    "ui_accent": [0.00, 0.00, 0.00, 0.0],

    "opacity":         0.0,
    "cursor_style":    0,
    "cursor_blink":    0,
    "bold_is_bright":  0
}
```

Drop into `~/.config/Liu/themes/midnight.json`, restart Liu, pick "Midnight" in Settings → Appearance.

The `ui_accent` line above is zero-alpha — chrome falls back to the legacy `ansi[4]/ansi[12]` accents. A warm-orange variant of the same theme would override it like this:

```json
    "ui_accent": [0.93, 0.55, 0.18, 1.0]
```

…and every focused-field underline, palette selection pill, scrollbar thumb, and primary button (Connect / Generate / Submit / OK) immediately picks up the orange. The terminal's `ansi_4` ("blue") stays whatever you wrote — your `ls --color` output keeps its blue directories.

---

## Example: theme with style overrides

A "Liquid Glass" theme that wants a translucent window and an
underline-style blinking cursor — same colour block as above plus:

```json
    ...

    "status_bg": [0.05, 0.06, 0.09, 1.0],
    "status_fg": [0.61, 0.64, 0.69, 1.0],

    "opacity":         0.88,
    "cursor_style":    2,
    "cursor_blink":    1,
    "bold_is_bright":  1
}
```

When the user picks this theme, Liu adjusts `config.opacity` to 0.88,
sets cursor to underline + blinking, and turns on bold-is-bright. Built-
in themes don't carry these fields, so switching to them later leaves
the user's preferences alone.

---

## Importing themes from other terminals

Liu also ingests two external formats; the importer converts them to the same internal `Theme` struct (only the terminal palette is filled — chrome colors are derived from `bg`).

| Source format | Typical extension | Function |
|---|---|---|
| iTerm2 color preset (XML plist) | `.itermcolors` | `theme_import_itermcolors()` |
| Alacritty | `.yml` / `.yaml` / `.toml` | `theme_import_alacritty()` |
| Auto-detect | any | `theme_import_file()` (sniffs the header) |

After import you can persist the result with `theme_save_user()`, which writes the JSON form documented above into `~/.config/Liu/themes/`.

---

## Reference: source files

| File | What's in it |
|---|---|
| `src/core/config.h` | The `Theme` struct + extern declarations of all built-ins |
| `src/core/config.c` | The 14 built-in theme definitions; useful as palette references |
| `src/core/theme_import.h` / `.c` | User-theme loader, JSON read/write, iTerm/Alacritty importers, `theme_user_dir()` |
| `src/ui/ui.c` | All UI chrome consumers — search for `t->tab_*`, `t->sidebar_*`, `t->border`, `t->status_*` to see where each color is read |

When in doubt, copy `THEME_DARK` from `src/core/config.c:28-66`, rename it, and tweak from there.
