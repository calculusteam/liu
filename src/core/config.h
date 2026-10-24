/*
 * Liu - configuration + theme system
 * Manages user preferences, themes, fonts, and keybindings.
 */
#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H

#include "core/types.h"
#include "renderer/renderer.h"
#include "translate/translate.h"
#include "voice/voice.h"
#include "browser/browser_nav.h"

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
     * pill, accent strip, scrollbar thumb, focused-field underline, and
     * equivalent highlights in modal dialogs. Kept separate from
     * `ansi[4]` / `ansi[12]` so themes can paint an orange terminal
     * palette while keeping a matching warm-toned UI accent (changing
     * `ansi[4]` would also tint shell-driven `\033[34m` output).
     *
     * Alpha == 0 means "not set": theme_ui_accent() then falls back to
     * a vividly coloured cursor or to `ansi[12]`. A theme opts in by
     * setting any non-zero RGBA. */
    Color  ui_accent;

    /* Voice-card colours, same alpha == 0 "not set" convention as ui_accent.
     *
     * They are their own slots rather than reuses of ui_accent because the
     * card says two different things and one colour cannot carry both:
     * `voice_record` is the recording light and the live waveform, and it has
     * to read as ARMED — a theme's accent is frequently the same blue as its
     * cursor and its selection, which is precisely the wrong signal for "your
     * microphone is open". `voice_accent` is the transcription scan, where the
     * chrome accent IS right, because that state is the app working rather
     * than the user being recorded.
     *
     * Left at zero, theme_voice_record() falls back to the theme's bright red
     * when the palette has a convincing one (and to a canonical red when it
     * does not — a recording light that reads grey on a monochrome palette
     * defeats the point), and theme_voice_accent() falls back to
     * theme_ui_accent(). */
    Color  voice_record;
    Color  voice_accent;

    /* Optional style overrides — applied to AppConfig when this theme
     * becomes active.  All fields use 0 as the "no override" sentinel
     * so existing zero-initialised built-in themes leave config alone.
     * User themes can opt in by setting a nonzero value in the JSON. */
    f32  opacity_override;          /* 0 = no override; otherwise 0.3..1.0 */
    i8   cursor_style_override;     /* 0 = none; 1=block, 2=underline, 3=bar */
    i8   cursor_blink_override;     /* 0 = none; 1=on,    2=off */
    i8   bold_is_bright_override;   /* 0 = none; 1=on,    2=off */
} Theme;

/* Built-in themes.
 *
 * THEME_Liu, its light half THEME_LIU_LIGHT and THEME_ULTRADARK are the app's
 * own; the rest are ports of existing themes (see config.c for the source
 * each palette was transcribed from). Declared in the order the Settings grid
 * shows them so this list and g_themes[] read the same.
 *
 * Code that needs a theme when config hasn't resolved one uses THEME_Liu. */
extern const Theme THEME_Liu;
extern const Theme THEME_LIU_LIGHT;
extern const Theme THEME_ULTRADARK;
extern const Theme THEME_GITHUB_DARK_DIMMED;
extern const Theme THEME_PIERRE_DARK;
extern const Theme THEME_ONE_DARK_PRO;
extern const Theme THEME_DRACULA;
extern const Theme THEME_DRACULA_SOFT;
extern const Theme THEME_CATPPUCCIN_MOCHA;
extern const Theme THEME_NORD;
extern const Theme THEME_SOLARIZED_DARK;
extern const Theme THEME_VITESSE_DARK;
extern const Theme THEME_MIN_DARK;
extern const Theme THEME_MONOKAI;
extern const Theme THEME_TOKYO_NIGHT;
extern const Theme THEME_NIGHT_OWL;
extern const Theme THEME_ROSE_PINE;
extern const Theme THEME_AYU_DARK;
extern const Theme THEME_SLACK_DARK;

#define THEME_COUNT 19

const Theme *theme_get_by_name(const char *name);
const Theme *theme_get_by_index(i32 index);
const char **theme_list_names(i32 *count);

/* Resolve the effective UI chrome accent for `t`. Returns `t->ui_accent`
 * when the theme defines one (alpha > 0); falls back to `t->ansi[12]`
 * otherwise so themes that don't set the field keep their pre-existing
 * cyan/blue UI accents. Safe to call with `t == NULL` (returns a neutral
 * default). */
Color theme_ui_accent(const Theme *t);

/* Voice-card colours (see the Theme fields). Both accept t == NULL. */
Color theme_voice_record(const Theme *t);   /* recording light + live wave  */
Color theme_voice_accent(const Theme *t);   /* transcription scan           */

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
    /* Toolbar and tab dimensions are compile-time layout constants, not
     * user settings. */
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
    f32 terminal_top_gap;      /* default 0pt — flush under the tab bar */

    /* Active-pane focus indicator. Thickness 0 hides the indicator
     * entirely. Color alpha 0 falls back to theme.cursor. */
    f32   active_pane_indicator_thickness;  /* default 1pt */
    Color active_pane_indicator_color;      /* default {0,0,0,0} → theme.cursor */
} Style;

/* Which physical Option key macOS should report as Alt/Meta (ESC-prefixed
 * input) instead of composing a character. Accepted values are false, true,
 * left, and right. OFF is the default, preserving Option-layer characters
 * used by non-US keyboard layouts, such as Turkish Option+Q for @, Option+3
 * for #, Option+8 for [, and Option+9 for ]. */
typedef enum {
    OPTION_AS_ALT_OFF   = 0,
    OPTION_AS_ALT_LEFT  = 1,
    OPTION_AS_ALT_RIGHT = 2,
    OPTION_AS_ALT_BOTH  = 3,
} OptionAsAlt;

/* Config-file spelling of an OptionAsAlt ("false"/"left"/"right"/"true") and
 * its inverse. The parser also accepts "off"/"both" and a bare 0-3. */
const char *config_option_as_alt_name(OptionAsAlt mode);
OptionAsAlt config_option_as_alt_parse(const char *s);

/* Config-file spelling of a FontSmoothing ("auto"/"always"/"never") and its
 * inverse. A bare true is "always"; a bare false is "auto", because every
 * config saved while the key was a switch carries false as its default. */
const char *config_font_smoothing_name(FontSmoothing mode);
FontSmoothing config_font_smoothing_parse(const char *s);

/* Rewrites a font path that names a face Liu no longer bundles to the bundled
 * face of the same family; config_load applies it to every font path it reads. */
void config_font_path_replace_retired(char *path, usize cap);

/* Prompt Recall — the badge in an agent pane's top-right corner that answers
 * "what did I ask this one?".
 *
 * An agent CLI runs in the alt screen, which has no scrollback, so once the
 * answer is longer than the window the question that produced it is gone; with
 * several agents open the user is looking at three panes of output and cannot
 * say which one they asked what. The badge holds onto the last prompt liu typed
 * into that pane (terminal/prompt_capture.h).
 *
 * BADGE is the default: a chip in the corner, opened by a click. ALWAYS keeps
 * the panel open — more visible, but it sits over the agent's own top-right
 * chrome the whole time, so it is opt-in. */
typedef enum {
    PROMPT_RECALL_OFF    = 0,
    PROMPT_RECALL_BADGE  = 1,
    PROMPT_RECALL_ALWAYS = 2,
} PromptRecallMode;

/* Config-file spelling of a PromptRecallMode ("off"/"badge"/"always") and its
 * inverse. The parser also accepts "false"/"true" and a bare 0-2. */
const char *config_prompt_recall_name(PromptRecallMode mode);
PromptRecallMode config_prompt_recall_parse(const char *s);

/* Maximum user-specified fallback fonts in config */
#define MAX_CONFIG_FALLBACK_FONTS 4
#define CONFIG_SCROLLBACK_MIN_LINES 0
#define CONFIG_SCROLLBACK_DEFAULT_LINES 1000

/* The manual text weight (AppConfig.text_weight_terminal / _ui). One range for
 * the loader's clamp and the Settings stepper, so they cannot disagree about what
 * "the end of the scale" is.
 *
 * It is a real erode / dilate of the glyph mask (LiuShaders.metal::weight_morph),
 * not a tone curve: a curve on the coverage can only move antialiased edge
 * pixels, which leaves the ends of the scale close to the default weight. +-2 is
 * about x1.3-1.7 / x0.6-0.8 of the ink depending on stem width (thin stems move
 * most), so one 0.5 step is a visible change; finer values stay legal in
 * config.json. */
#define TEXT_WEIGHT_MIN  (-2.0f)
#define TEXT_WEIGHT_MAX  ( 2.0f)
#define TEXT_WEIGHT_STEP ( 0.5f)
#define CONFIG_SCROLLBACK_MAX_LINES 20000
/* The size a fresh config has, and the size "reset" goes back to: one number,
 * because the two drifted (the default went to 16 and the reset stayed 12). */
#define CONFIG_FONT_SIZE_DEFAULT 16.0f

typedef struct {
    /* Font */
    char    font_path[512];
    char    fallback_fonts[MAX_CONFIG_FALLBACK_FONTS][512]; /* user-specified fallback font paths */
    i32     fallback_font_count;
    /* OpenType features for the primary face, comma separated: "zero",
     * "+ss01", "-calt", "cv01=2". Empty = the face's defaults. */
    char    font_features[256];
    /* Variation axis values for a variable primary face: "wght=450, wdth=90".
     * Empty = Regular (a variable face's default instance otherwise). */
    char    font_variations[256];
    /* Codepoint ranges drawn from a named face before any other:
     * "U+E000-U+F8FF=Symbols Nerd Font; U+2190-U+21FF=/path/to/face.ttf". */
    char    font_codepoint_map[512];
    f32     font_size;         /* in points */
    f32     line_height;       /* row-pitch multiplier (1.0 = normal) */
    bool    font_normalize;    /* scale each face so a point size means the same
                                * letter height and row pitch in every font;
                                * off = use the face's own hhea metrics */
    FontSmoothing font_smoothing; /* macOS Core Graphics outline dilation —
                                * solid stems instead of a shader-thickened
                                * halo; AUTO: light at 1x, none at 2x */
    f32     font_weight;       /* stroke width added to glyphs at rasterize
                                * time; 0 = native weight, >0 thickens, <0
                                * is reserved for future erosion. Range
                                * conceptually -1.0..2.0; UI exposes 0..1.5. */
    /* Manual trim on how heavy text draws, applied to the glyph's COVERAGE in
     * the shader after the terminal-weight correction, so it works instantly
     * (no atlas rebuild) and in both directions. 0 = as drawn; the curve is
     * an erode / dilate of the glyph mask, TEXT_WEIGHT_MIN..MAX (calibration: see above).
     * Two knobs because two surfaces: the terminal grid, and everything else
     * (palette, tabs, sidebar, editor). */
    f32     text_weight_terminal;
    f32     text_weight_ui;

    /* Theme */
    char    theme_name[64];
    const Theme *theme;

    /* Terminal */
    i32     scrollback_lines;  /* max scrollback */
    f32     tab_sleep_idle_minutes; /* idle minutes before inactive tabs sleep (0 = off) */
    bool    confirm_close_agent;    /* ask before closing a tab whose PTY runs an AI agent CLI */
    bool    paste_protection;       /* ask before pasting text with line breaks into a program that
                                     * did not enable bracketed paste (every line would run as Enter) */
    bool    shell_integration;      /* give a pane's zsh or fish the marks Liu reads (OSC 133 prompt / command /
                                     * exit status, OSC 7 folder) by way of its environment — nothing is typed
                                     * into the shell and no file of the user's is edited (default ON) */
    bool    command_marks;          /* draw the left-margin bars for prompt and output rows and the exit-code
                                     * badge that those marks make possible (default OFF: the marks feed
                                     * history, folder tracking and "command finished" without them) */
    bool    secure_keyboard_entry;  /* keep macOS Secure Event Input on while Liu is the active app, so no
                                     * other process can read what is typed (default OFF: it also stops
                                     * text expanders while Liu is in front) */
    bool    secure_input_auto;      /* ...and take it by itself for as long as the focused pane's program is
                                     * reading a password (echo off, line mode) (default ON) */
    bool    ssh_legacy_algorithms;  /* also offer an SSH server what OpenSSH stopped offering: SHA-1 key
                                     * exchanges and `ssh-rsa` signatures, CBC ciphers, MD5 and truncated MACs
                                     * (ssh/ssh_algs.h). Default OFF; for the switch or NAS that speaks nothing else. */
    bool    command_history;        /* keep a per-folder history of the commands run at a shell prompt and
                                     * suggest from it (core/cmd_history.h). Default ON; off = nothing is
                                     * recorded and nothing is suggested. */
    char    lang[64];               /* the locale a pane's programs start in when Liu itself was started with
                                     * none (a launch from Finder): "" = C.UTF-8 (en_US.UTF-8 where the system
                                     * lacks it), "system" = the system's language and region, or a locale's
                                     * name. ssh/local_lang.h has the rule and why the default is not "system" */
    char    term[48];               /* TERM for a pane: "xterm-256color" (default) or "xterm-liu", Liu's own
                                     * entry, which is shipped next to the app and found through TERMINFO */
    bool    agent_hooks;            /* while Liu runs, add its notify hook to the settings of every agent CLI
                                     * that is installed, and take it out again on quit (default ON). OFF =
                                     * Liu edits no agent's files at launch; a hook can still be installed
                                     * by hand in Settings > Notifications. */
    bool    agent_hooks_notice_seen;/* the one-time notice that says so has been shown */
    bool    cursor_blink;
    u8      cursor_style;      /* 0=block, 1=underline, 2=bar */
    bool    bold_is_bright;
    bool    copy_on_select;
    /* Drop the blanks at the end of every copied line. Blanks a program left
     * INSIDE a line with a cursor move are never touched — only what sits
     * between the last glyph and the line break. */
    bool    clipboard_trim_trailing_spaces;
    /* Off by default: programs that capture mouse input receive clicks and
     * drags; Shift+drag and Shift+double-click select text. When enabled, a
     * plain drag or double/triple-click selects text without forwarding the
     * drag to the program; single clicks still reach it. Stored as
     * drag_selects. */
    bool    mouse_drag_selects;
    bool    bidi_enabled;      /* implicit BiDi reordering for RTL scripts */

    /* UI — Liu-style minimal */
    f32     tab_height;        /* pixels (0 = auto-hide when single tab) */
    f32     sidebar_width;
    bool    show_scrollbar;
    bool    scrollbar_autohide;   /* overlay: shown on scroll/hover, then fades */
    f32     opacity;           /* window opacity 0.0-1.0 */
    f32     padding;           /* terminal edge padding in pixels */
    bool    hide_tab_bar_single; /* hide tab bar when only 1 tab (Liu default) */
    bool    show_tab_bar;      /* toggle for the tab row (tabs + close) */
    bool    show_toolbar_icons; /* toggle for the action icons row (sidebar / fonts / ssh / settings) */
    /* Sidebar customisation (Settings › Terminal › Sidebar). The sidebar is a
     * stack of sections — tabs, files, agents, notes — in `sidebar_order`, each
     * on or off in `sidebar_enabled`; indices are ui/sidebar_layout.h's
     * SidebarSection (0 tabs, 1 files, 2 agents, 3 notes). `sidebar_tabs`
     * moves the tab strip off the top bar and into the sidebar as a vertical
     * list; below `sidebar_collapse_below_pt` of window width the docked
     * sidebar folds to an icon rail (0 = never). */
    bool    sidebar_tabs;
    u8      sidebar_order[4];
    bool    sidebar_enabled[4];
    bool    sidebar_compact;
    f32     sidebar_collapse_below_pt;
    bool    hide_title_bar;    /* Linux only: hide the whole top chrome strip (icon + path + window
                                * controls + tab row), hiding the complete top chrome strip.
                                * Overrides show_tab_bar / show_toolbar_icons when set. */
    bool    show_status_bar;   /* toggle for the bottom status bar (session type / cwd / time) */
    bool    show_git_badge;    /* toggle for the bottom-right git badge (repo / branch / +N -M) */
    bool    borderless;        /* no window title bar / borderless mode */

    /* In-app browser (TAB_BROWSER / WKWebView) */
    bool    browser_enabled;       /* master switch; default true on macOS */
    BrowserAllowlist browser_allowlist; /* additional sites, beyond local networks */
    char    browser_last_url[512]; /* last navigated URL for quick reopen */
#define BROWSER_RECENTS_MAX 8
    char    browser_recents[BROWSER_RECENTS_MAX][512]; /* MRU visited URLs */
    i32     browser_recents_count;

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
    OptionAsAlt option_as_alt; /* macOS: which Option key acts as Alt/Meta */
    PromptRecallMode prompt_recall; /* agent pane's "what did I ask?" badge */

    /* Smooth cursor animation */
    bool    cursor_animate;    /* animate cursor movement (default true) */

    /* Long-running command notifications */
    f32     notify_command_threshold; /* seconds before notifying (default 10.0) */

    /* Background image */
    char    background_image[512];   /* path to background image (empty = none) */
    f32     background_opacity;      /* opacity of background image (0.0-1.0, default 0.3) */
    char    notes_vault_path[1024];  /* notes/graph Vault root; empty => computed default. 1024 matches platform_open_folder_dialog's buffer so deep picks aren't truncated. */
    i32     background_mode;         /* 0=stretch, 1=center, 2=tile, 3=fill (aspect) */
    /* Blur applied to whatever shows THROUGH the window — the desktop behind
     * it, not the background image above. Only visible while `opacity` is
     * below 1; at full opacity nothing shows through to blur. Platforms that
     * cannot honour the radius ignore the setting entirely — there is no
     * fallback (see platform_window_blur_is_exact). Turning blur on also makes
     * the window non-opaque, since an opaque window has nothing behind it to
     * show. */
    bool    background_blur;         /* window backdrop blur on/off */
    f32     background_blur_radius;  /* blur radius in points (0-50, default 10) */

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

    /* Voice input (local Whisper) */
    VoiceConfig voice;

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

/* Load config from JSON file. Returns true on success.
 *
 * Any layout of the JSON is read — a minified file on one line too: the text
 * is laid out one member a line before the two key chains see it
 * (core/json_lines.h). A file that has something in it and not one
 * "key": value pair is NOT read: false, and a copy of its bytes is kept as
 * `<path>.unread` before anything can save over it (config_last_load_unread).
 * The whole file is read up to 16 MiB; a larger one is not read either. */
bool config_load(AppConfig *cfg, const char *path);
/* The last config_load() found text and no member it could read, and kept the
 * text as `<path>.unread`: whoever loaded it says so to the user. */
bool config_last_load_unread(void);

/* Save config to JSON file. */
bool config_save(const AppConfig *cfg, const char *path);
/* "tabs,files,-agents,notes" — the sidebar_sections value as config_save writes it. */
void config_format_sidebar_sections(const AppConfig *cfg, char *out, usize cap);
/* How many sections the sidebar would show: the enabled ones, with tabs
 * counting only while docked (the strip has them otherwise). */
i32  config_sidebar_visible_sections(const AppConfig *cfg);
/* At least one sidebar section must remain enabled. If all are off, enable
 * the explorer. Returns true when it changes the settings. */
bool config_sidebar_ensure_section(AppConfig *cfg);

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

/* Did the config file on disk change since Liu last wrote it itself?
 *
 * config_save() writes atomically (temp file + rename), which the file watcher
 * sees as a DELETE/RENAME on the path it is watching — so every setting the app
 * persists comes straight back as "the config changed, reload everything". That
 * round trip is not free: it re-reads the file, re-applies the theme,
 * invalidates every render cache, recomputes the layout and re-pushes window
 * opacity/blur to the window server, which makes the backdrop blur blink out
 * and return. Callers gate the reload on this so only edits Liu did NOT make (a
 * text editor, another instance, a sync tool) trigger it.
 *
 * The identity being compared is captured from the descriptor config_save()
 * wrote, before the rename made it visible — never by stat'ing the path after
 * the commit, which would let a writer that replaced the file in that window be
 * recorded as Liu's own write and have their edit discarded here. Only a save
 * to config_file_path() updates it; an export elsewhere is not this file.
 *
 * Returns true when the file's identity differs from the last successful save,
 * including when nothing has been saved yet or the file cannot be stat'ed.
 * Every error path must keep reporting "changed": a wrong "unchanged" silently
 * throws away a real user edit, while a wrong "changed" costs one reload. */
bool config_file_changed_externally(const char *path);

#endif /* CORE_CONFIG_H */
