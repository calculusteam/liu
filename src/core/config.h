/*
 * Liu - configuration + theme system
 * Manages user preferences, themes, fonts, and keybindings.
 */
#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H

#include "core/types.h"
#include "renderer/renderer.h"
#include "translate/translate.h"

/* =========================================================================
 * Theme
 * ========================================================================= */

typedef struct {
    char   name[64];

    /* Terminal colors */
    Color  fg;              /* default foreground */
    Color  bg;              /* default background */
    Color  cursor;          /* cursor color */
    Color  selection;       /* selection highlight */
    Color  ansi[16];        /* ANSI 0-15 colors */

    /* UI chrome */
    Color  tab_bg;          /* tab bar background */
    Color  tab_active_bg;   /* active tab background */
    Color  tab_active_fg;   /* active tab text */
    Color  tab_inactive_bg;
    Color  tab_inactive_fg;
    Color  sidebar_bg;
    Color  sidebar_fg;
    Color  sidebar_hover;
    Color  sidebar_active;
    Color  border;          /* divider/border color */
    Color  scrollbar;
    Color  scrollbar_thumb;

    /* Status bar */
    Color  status_bg;
    Color  status_fg;

    /* UI chrome accent — drives the command-palette caret, selection
     * pill, accent strip, scrollbar thumb, focused-field underline,
     * and equivalent highlights in modal dialogs. Kept separate from
     * `ansi[4]` / `ansi[12]` so themes can paint an orange terminal
     * palette while keeping a matching warm-toned UI accent (changing
     * `ansi[4]` would also tint shell-driven `\033[34m` output).
     *
     * Alpha == 0 (the default for every built-in theme) means "fall
     * back to `ansi[12]`", so existing themes look unchanged. User
     * themes can opt-in by setting any non-zero RGBA. */
    Color  ui_accent;

    /* Optional style overrides — applied to AppConfig when this theme
     * becomes active.  All fields use 0 as the "no override" sentinel
     * so existing zero-initialised built-in themes leave config alone.
     * User themes can opt in by setting a nonzero value in the JSON. */
    f32  opacity_override;          /* 0 = no override; otherwise 0.3..1.0 */
    i8   cursor_style_override;     /* 0 = none; 1=block, 2=underline, 3=bar */
    i8   cursor_blink_override;     /* 0 = none; 1=on,    2=off */
    i8   bold_is_bright_override;   /* 0 = none; 1=on,    2=off */
} Theme;

/* Built-in themes */
extern const Theme THEME_DARK;
extern const Theme THEME_LIGHT;
extern const Theme THEME_SOLARIZED_DARK;
extern const Theme THEME_MONOKAI;
extern const Theme THEME_DRACULA;
extern const Theme THEME_NORD;
extern const Theme THEME_GRUVBOX;
extern const Theme THEME_CATPPUCCIN_MOCHA;
extern const Theme THEME_KITTY;
extern const Theme THEME_Liu;
extern const Theme THEME_TOKYO_NIGHT;
extern const Theme THEME_ONE_DARK;
extern const Theme THEME_ROSE_PINE;
extern const Theme THEME_KANAGAWA;

#define THEME_COUNT 21

const Theme *theme_get_by_name(const char *name);
const Theme *theme_get_by_index(i32 index);
const char **theme_list_names(i32 *count);

/* Resolve the effective UI chrome accent for `t`. Returns `t->ui_accent`
 * when the theme defines one (alpha > 0); falls back to `t->ansi[12]`
 * otherwise so themes that don't set the field keep their pre-existing
 * cyan/blue UI accents. Safe to call with `t == NULL` (returns a neutral
 * default). */
Color theme_ui_accent(const Theme *t);

/* =========================================================================
 * Terminal Profiles
 * ========================================================================= */

#define MAX_PROFILES 16

typedef struct {
    char name[64];
    char font_path[512];
    f32  font_size;
    char theme_name[64];
    u8   cursor_style;
    f32  opacity;
    bool has_font;
    bool has_theme;
    bool has_cursor;
    bool has_opacity;
} TermProfile;

/* =========================================================================
 * Sites — dev-server manager registry
 *
 * Persisted projection of a managed dev server (see src/core/sites.h for the
 * live runtime struct). Only the user-defined fields are stored; runtime state
 * (process, logs, status, detected port) is never written to config.json.
 * ========================================================================= */

#define MAX_SITES 32

typedef struct {
    char name[64];
    char path[1024];
    char command[512];
    i32  port;          /* user-pinned port hint, 0 = auto-detect */
} SiteConfig;

/* =========================================================================
 * App Config
 * ========================================================================= */

/* =========================================================================
 * Style — user-customizable UI dimensions (all in points, multiplied by dpi)
 * ========================================================================= */

typedef struct {
    /* Toolbar / Tab bar. NOTE: toolbar height and tab width are no longer
     * user-tunable — they live as compile-time constants (TOOLBAR_HEIGHT_PT /
     * TAB_WIDTH_PT) in src/ui/layout.h, the single source of truth for chrome
     * geometry. */
    f32 tab_gap;               /* default 2pt */
    f32 tab_dot_size;          /* default 6pt */
    f32 tab_close_size;        /* default 14pt */
    f32 tab_close_margin;      /* default 6pt */
    f32 tab_indicator_height;  /* default 2pt */

    /* Status bar */
    f32 status_bar_height;     /* default 22pt */

    /* Sidebar */
    f32 sidebar_default_width; /* default 240pt */
    f32 sidebar_min_width;     /* default 160pt */
    f32 sidebar_max_width;     /* default 500pt */
    f32 sidebar_header_height; /* default 28pt */

    /* Terminal */
    f32 terminal_padding;      /* default 6pt */
    f32 terminal_top_gap;      /* default 2pt */

    /* Active-pane focus indicator. Thickness 0 hides the indicator
     * entirely. Color alpha 0 falls back to theme.cursor. */
    f32   active_pane_indicator_thickness;  /* default 1pt */
    Color active_pane_indicator_color;      /* default {0,0,0,0} → theme.cursor */
} Style;

/* Maximum user-specified fallback fonts in config */
#define MAX_CONFIG_FALLBACK_FONTS 4
#define CONFIG_SCROLLBACK_MIN_LINES 0
#define CONFIG_SCROLLBACK_DEFAULT_LINES 1000
#define CONFIG_SCROLLBACK_MAX_LINES 20000

typedef struct {
    /* Font */
    char    font_path[512];
    char    fallback_fonts[MAX_CONFIG_FALLBACK_FONTS][512]; /* user-specified fallback font paths */
    i32     fallback_font_count;
    f32     font_size;         /* in points */
    f32     line_height;       /* multiplier (1.0 = normal) */
    f32     font_weight;       /* stroke width added to glyphs at rasterize
                                * time; 0 = native weight, >0 thickens, <0
                                * is reserved for future erosion. Range
                                * conceptually -1.0..2.0; UI exposes 0..1.5. */

    /* Theme */
    char    theme_name[64];
    const Theme *theme;

    /* Terminal */
    i32     scrollback_lines;  /* max scrollback */
    f32     tab_sleep_idle_minutes; /* idle minutes before inactive tabs sleep (0 = off) */
    bool    confirm_close_agent;    /* ask before closing a tab whose PTY runs an AI agent CLI */
    bool    cursor_blink;
    u8      cursor_style;      /* 0=block, 1=underline, 2=bar */
    bool    bold_is_bright;
    bool    copy_on_select;
    bool    bidi_enabled;      /* implicit BiDi reordering for RTL scripts */

    /* UI — Liu-style minimal */
    f32     tab_height;        /* pixels (0 = auto-hide when single tab) */
    f32     sidebar_width;
    bool    show_scrollbar;
    f32     opacity;           /* window opacity 0.0-1.0 */
    f32     padding;           /* terminal edge padding in pixels */
    bool    hide_tab_bar_single; /* hide tab bar when only 1 tab (Liu default) */
    bool    show_tab_bar;      /* toggle for the tab row (tabs + close) */
    bool    show_toolbar_icons; /* toggle for the action icons row (sidebar / fonts / ssh / settings) */
    bool    show_status_bar;   /* toggle for the bottom status bar (session type / cwd / time) */
    bool    borderless;        /* no window title bar / borderless mode */

    /* Cell spacing — Liu-style */
    f32     cell_width_scale;  /* horizontal scale (1.0 = normal, >1 = wider) */
    f32     cell_height_scale; /* vertical scale / line spacing (1.0 = tight) */

    /* Bell */
    bool    visual_bell;          /* flash screen on bell */
    bool    audible_bell;         /* play system sound on bell */

    /* OSC 52 clipboard writes from the terminal (remote shells, `cat` of
     * crafted files, etc.) — classic clipboard-poisoning vector, so off by
     * default — the common terminal convention. */
    bool    allow_osc52_write;

    /* Keybindings */
    bool    option_as_alt;     /* macOS: Option key = Alt for terminal */

    /* Smooth cursor animation */
    bool    cursor_animate;    /* animate cursor movement (default true) */

    /* Long-running command notifications */
    f32     notify_command_threshold; /* seconds before notifying (default 10.0) */

    /* Background image */
    char    background_image[512];   /* path to background image (empty = none) */
    f32     background_opacity;      /* opacity of background image (0.0-1.0, default 0.3) */
    char    notes_vault_path[1024];  /* notes/graph Vault root; empty => computed default. 1024 matches platform_open_folder_dialog's buffer so deep picks aren't truncated. */
    i32     background_mode;         /* 0=stretch, 1=center, 2=tile, 3=fill (aspect) */
    bool    background_blur;         /* apply blur to background image */
    f32     background_blur_radius;  /* blur radius (0-50, default 10) */

    /* Ligatures */
    bool    enable_ligatures;  /* render programming font ligatures (default true) */

    /* Quake mode */
    bool    quake_mode;            /* enable quake-style drop-down terminal */
    char    quake_hotkey[32];      /* global hotkey string, e.g. "Ctrl+`" */
    f32     quake_height_ratio;    /* 0.0-1.0, fraction of screen height (default 0.4) */
    f32     quake_animation_duration; /* animation duration in seconds (default 0.2) */

    /* Smart Vault */
    i32     vault_auto_lock_minutes;   /* 0 = never, >0 = lock after N min idle */

    /* User-customizable style dimensions */
    Style   style;

    /* Translate-on-Tab */
    TranslateConfig translate;

    /* Terminal profiles */
    TermProfile profiles[MAX_PROFILES];
    i32         profile_count;

    SiteConfig  sites[MAX_SITES];
    i32         site_count;
} AppConfig;

/* Default style values */
Style style_default(void);

/* Default config */
AppConfig config_default(void);

/* Apply a theme's optional style overrides (opacity, cursor style,
 * cursor blink, bold-is-bright) to the live config. No-op for fields
 * the theme leaves at zero. Built-in themes use zero across the board
 * so they never alter the user's preferences; user-authored themes
 * can opt in. Returns true if any field was changed. */
bool theme_apply_style_overrides(const Theme *t, AppConfig *cfg);

/* Load config from JSON file. Returns true on success. */
bool config_load(AppConfig *cfg, const char *path);

/* Save config to JSON file. */
bool config_save(const AppConfig *cfg, const char *path);

/* Get the effective config root (prefers existing legacy ~/.config/liu on
 * case-sensitive filesystems, otherwise uses ~/.config/Liu). */
const char *config_user_dir(void);

/* Get config file path under the effective config root. */
const char *config_file_path(void);

/* Get the repository/local font directory (prefers assets/fonts in the source tree). */
const char *font_user_dir(void);

/* Absolute path of the directory containing the running `liu` executable.
 * Cached on first call. Returns an empty string if resolution fails.
 * Used to find sibling helper binaries (liu-history, agenthistory, …). */
const char *liu_executable_dir(void);

/* Get the custom font directory under the repository font root. */
const char *font_custom_dir(void);

/* Hot-reload config from disk. Returns true if config changed. */
bool config_reload(AppConfig *cfg);

#endif /* CORE_CONFIG_H */
