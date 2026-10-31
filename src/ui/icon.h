/*
 * Liu — unified UI icon API.
 *
 * One enum, one draw call, one source of truth for every glyph-style icon
 * the app paints (tab close, toolbar buttons, modal close, sidebar header
 * actions, etc.).
 *
 * Implementation: every icon is a Lucide vector (MIT), embedded as SVG source
 * and rasterised by nanosvg at the exact pixel size it will be drawn at, then
 * tinted from the alpha mask (see ui/lucide.h).
 *
 * Drawing the real path keeps curves smooth (a curve approximated by stacked
 * axis-aligned rects is a staircase), and one source means macOS and Linux
 * render identically.
 *
 * Why a separate module from fb_icon / fb_asset_icons? Those are file-type
 * semantic icons backed by PNGs. This module is for *UI structure* icons that
 * belong to the app chrome itself — monoline, weight-matched, tinted at draw
 * time, and vector all the way down.
 */
#ifndef LIU_UI_ICON_H
#define LIU_UI_ICON_H

#include "core/types.h"
#include "renderer/renderer.h"

typedef enum {
    ICON_NONE = 0,
    /* Toolbar */
    ICON_SIDEBAR,        /* SF: sidebar.left */
    ICON_GEAR,           /* SF: gearshape */
    ICON_NETWORK,        /* SF: network */
    ICON_FONT_LARGER,    /* SF: textformat.size.larger / fallback: A↑ */
    ICON_FONT_SMALLER,   /* SF: textformat.size.smaller / fallback: A↓ */
    /* General */
    ICON_CLOSE,          /* SF: xmark — tab/modal close */
    ICON_PLUS,           /* SF: plus  — new tab, increase */
    ICON_MINUS,          /* SF: minus — decrease */
    ICON_SEARCH,         /* SF: magnifyingglass */
    ICON_UP,             /* SF: arrow.up — sidebar parent / browser attach */
    ICON_REFRESH,        /* SF: arrow.clockwise */
    ICON_CHEVRON_DOWN,   /* SF: chevron.down */
    ICON_CHEVRON_RIGHT,  /* SF: chevron.right */
    ICON_CHEVRON_LEFT,   /* SF: chevron.left — browser back */
    ICON_ARROW_LEFT,     /* SF: arrow.left — browser back */
    ICON_ARROW_RIGHT,    /* SF: arrow.right — browser forward */
    ICON_ARROW_UP_RIGHT, /* SF: arrow.up.right — open external */
    ICON_SCOPE,          /* SF: plus.viewfinder — select element */
    ICON_DEVICE,         /* SF: iphone — browser viewport picker */
    ICON_STOP,           /* SF: stop.fill — stop page load */
    ICON_BELL,           /* SF: bell.fill */
    ICON_MOON,           /* SF: moon.fill — sleeping tab */
    ICON_CHECK,          /* checkmark — completion/confirmation only */
    ICON_TOGGLE,         /* a switch: commands that flip a setting */
    ICON_EYE,            /* visibility: "Show …" commands */
    ICON_PALETTE,        /* appearance / theming */
    ICON_KEYBOARD,       /* key bindings */
    ICON_INFO,           /* about / informational */
    ICON_LANGUAGES,      /* translation */
    ICON_MIC,            /* microphone — voice input */
    ICON_COPY,           /* copy to clipboard */
    ICON_PENCIL,         /* rename / edit in place */
    ICON_PANEL_RIGHT,    /* split right — a panel divided down its right side */
    ICON_PANEL_LEFT,     /* split left */
    ICON_PANEL_BOTTOM,   /* split down */
    ICON_PANEL_TOP,      /* split up */
    ICON_GRAPH,          /* nodes + edges — the notes knowledge graph */
    ICON_PROMPT,         /* Quoted speech bubble; distinct from the chevrons used for picker rows and collapsed sections. */
    ICON_TERMINAL,       /* `>_` — a plain shell tab, so a tab running nothing
                          * but a shell carries an icon in the strip's leading
                          * slot and reads as the default kind of tab. Same
                          * vector as ICON_FT_TERMINAL, listed separately
                          * because that one means "this file is a shell
                          * script". */
    /* File-type icons (file-browser list) — same monoline language as the
     * chrome icons above. */
    ICON_FT_FOLDER,      /* SF: folder */
    ICON_FT_DOC,         /* SF: doc — generic file */
    ICON_FT_DOC_TEXT,    /* SF: doc.text — text / markdown / data files */
    ICON_FT_CODE,        /* SF: chevron.left.forwardslash.chevron.right */
    ICON_FT_IMAGE,       /* SF: photo */
    ICON_FT_FILM,        /* SF: film */
    ICON_FT_MUSIC,       /* SF: music.note */
    ICON_FT_ARCHIVE,     /* SF: archivebox */
    ICON_FT_PDF,         /* SF: doc.richtext */
    ICON_FT_TERMINAL,    /* SF: terminal — shell scripts */
    ICON_FT_BRANCH,      /* SF: arrow.triangle.branch — git */
    ICON_FT_LOCK,        /* SF: lock — lockfiles */
    /* Sidebar sections (ui/sidebar_layout.h): the customize popover's title
     * and its palette / Settings entry, the agents rail button, the notes rail
     * button. */
    ICON_SLIDERS,        /* sliders-horizontal — "Customize Sidebar" */
    ICON_BOT,            /* bot — the Agents section */
    ICON_NOTEBOOK,       /* notebook — the Notes section */
    ICON_GRIP,           /* grip-vertical — a row that drags to reorder */
    /* Sketch tools and history actions. */
    ICON_LINE,
    ICON_SQUARE,
    ICON_ERASER,
    ICON_UNDO,
    ICON_REDO,
    ICON_TRASH,
    ICON_COMMAND,
    ICON_SHIFT,
    ICON_OPTION,
    ICON_CONTROL,
    ICON_DOWN,
    /* File-browser context menu (ui/fb_menu.h). */
    ICON_SCISSORS,
    ICON_CLIPBOARD,
    ICON_FILE_PLUS,
    ICON_FOLDER_PLUS,
    /* Floating notifications (ui/toast_stack.h): one mark per kind, with
     * ICON_INFO and ICON_CHECK covering the other two. */
    ICON_ALERT,          /* triangle-alert — a warning */
    ICON_CIRCLE_X,       /* circle-x — a failure */
    /* The SFTP tab (ui/sftp_tab.h): the two sides, and what moves between. */
    ICON_LAPTOP,         /* laptop — this machine */
    ICON_SERVER,         /* server — the host */
    ICON_FILTER,         /* list-filter — a pane's Filter */
    ICON_UPLOAD,
    ICON_DOWNLOAD,
    ICON_HOME,           /* house — Go to Home Folder */
    ICON_EYE_OFF,        /* eye-off — hidden files */
    ICON_CHEVRON_UP,     /* a column sorted the other way */
    ICON_FOLDER_SYNC,    /* folder-sync — the SFTP tab itself, and its form */
    /* Command-palette categories (ui/palette_row.c): one mark per subject. */
    ICON_WINDOW,         /* app-window — Window: …, New Window, Quake Mode */
    ICON_COLUMNS,        /* columns-2 — a side-by-side split */
    ICON_ROWS,           /* rows-2 — a stacked split */
    ICON_GAUGE,          /* gauge — the render benchmark */
    ICON_SHAPES,         /* shapes — toolbar icons */
    ICON_ROCKET,         /* rocket — Sites (the dev-server launcher) */
    ICON_GROUP,          /* group — tab groups */
    ICON_TYPE,           /* type — font size, line height, ligatures */
    ICON_BLEND,          /* blend — opacity */
    ICON_USER,           /* user — a terminal profile */
    ICON_TEXT_CURSOR,    /* text-cursor — the cursor's style, blink, motion */
    ICON_SCROLL,         /* scroll-text — scrollback */
    ICON_KEY,            /* key-round — SSH keys */
    ICON_FINGERPRINT,    /* fingerprint — known hosts */
    ICON_ARROW_LEFT_RIGHT, /* arrow-left-right — port forwarding */
    ICON_BROADCAST,      /* radio-tower — broadcast input */
    ICON_HASH,           /* hash — a Markdown heading */
    ICON_HISTORY,        /* history — connection history */
    ICON_BRUSH,          /* brush — Sketch */
    ICON_MAXIMIZE,       /* maximize-2 — a pane zoomed to fill its tab */
    ICON_COUNT,
} IconKind;

/* Draw one icon centered on (x, y, size, size). Color is sRGB; alpha is
 * respected (passes through the per-glyph alpha pipeline). When the
 * platform layer can't produce an SF Symbol the call falls back to a
 * vector approximation drawn with renderer_draw_rect primitives, so a
 * call is *always* safe to make — never a no-op. */
void icon_draw(Renderer *r, IconKind kind, f32 x, f32 y, f32 size, Color color);

/* Horizontal ink insets of an icon inside the size×size box icon_draw paints
 * it into, in pixels: `left` = blank columns before the first inked column,
 * `right` = blank columns after the last.
 *
 * SF Symbols carry their own internal padding, so an icon anchored by its BOX
 * sits visually further from an edge than text anchored by its cell — which is
 * exactly why the tab pill's ✕ read as floating while the title's left padding
 * looked tight. Call sites that need *optical* alignment subtract these.
 *
 * Cheap after the first call per (kind, size): the result is memoised and the
 * underlying rasteriser is already cached. Both outputs are 0 when the symbol
 * can't be rasterised (Linux fallback path), which degrades to box alignment.
 * Either pointer may be NULL. */
void icon_ink_insets(IconKind kind, f32 size, f32 *left, f32 *right);

/* Verify every IconKind maps to an embedded Lucide SVG. Returns the number of
 * broken rows (0 = healthy) and, via `missing_kind_name`, the id of the first
 * one — NULL when that row has no name at all. The compiler already pins the
 * table's shape (see ICON_LUCIDE's _Static_assert); this checks the part only
 * the data can answer: that each id actually resolves. */
i32 icon_table_selfcheck(const char **missing_kind_name);

/* High-quality box (area-average) downscale of premultiplied RGBA pixels — the
 * shared resampler behind every compiled-in PNG icon (file-browser file types,
 * agent logos). The GPU can't do this well on the default Metal path: icon
 * textures are uploaded without mipmaps, so a 128px source drawn at ~28px
 * samples 4 of the ~21 covered texels and shimmers/aliases. Callers downscale
 * ONCE to the exact on-screen pixel size and cache the result, so sampling
 * becomes 1:1.
 *
 * Input and output are both PREMULTIPLIED alpha — the space every renderer
 * image upload consumes, since the pipeline blend is (ONE, 1-SrcAlpha). Callers
 * run img_premultiply_rgba() on the decoded PNG once and pass that in; this
 * function then just area-averages all four channels, which is alpha-correct
 * because fully-transparent texels carry zero colour weight.
 *
 * Returns a malloc'd dw*dh*4 buffer the caller owns, or NULL on OOM / bad args.
 * Minification only: dw<=sw, dh<=sh. */
u8 *ui_icon_downscale_rgba(const u8 *src, i32 sw, i32 sh, i32 dw, i32 dh);

#endif /* LIU_UI_ICON_H */
