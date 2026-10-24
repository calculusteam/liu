/*
 * Liu — decode and cache compiled-in agent PNG logos.
 * Build input:  ${CMAKE_BINARY_DIR}/generated/ui/agent_icons_data.h
 */
#include "ui/agent_icons.h"
#include "ui/chrome_palette.h"
#include "ui/icon.h"          /* ui_icon_downscale_rgba */
#include "core/imgproc.h"     /* img_premultiply_rgba */
#include "stb_image.h"
#define NANOSVG_IMPLEMENTATION_SKIP   /* nanosvg_impl.c owns the implementation */
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"
#include <string.h>
#include "ui/agent_icons_data.h"

#include <stddef.h>
#include <stdlib.h>
#include <math.h>     /* app_icon_round_corners: fabsf/sqrtf */

/* Cached exact-size downscales per logo — created once, NEVER freed (the
 * renderer image cache keys textures by pixel pointer; a freed+reused
 * address would alias a stale texture). Same policy as fb_asset_icons.c. */
#define AGENT_ICON_SCALED_MAX 8
typedef struct { i32 size; const u8 *rgba; } AgentIconScaled;

typedef struct {
    const u8 *png_data;
    i32       png_size;
    /* The decoded master is temporary. Once a downscale has been produced,
     * release it unless its pointer was returned to a caller; retain native
     * dimensions for later scaling decisions. */
    const u8 *rgba;
    i32       w, h;      /* native dimensions — retained after `rgba` is freed */
    bool      tried;     /* set true once decode has been attempted (success or not) */
    bool      decode_failed;   /* stb_image said no: never retry */
    /* The master's ADDRESS escaped to a caller (upscale / non-square / OOM
     * fallback). The renderer's image cache keys textures by pixel pointer, so
     * from that moment the buffer must stay valid — and stay unique — for the
     * process lifetime. Only a master that never escaped may be freed. */
    bool      master_exposed;
    /* Set by the decode: this artwork is a flat silhouette and `ink` is the
     * colour actually stamped into it. The backdrop needs the same answer —
     * it sizes its tile to contrast the mark it will sit under, and asking
     * agent_icon_tint() instead would size it against a colour that is no
     * longer drawn. */
    bool      inked;
    AgentTint ink;
    AgentIconScaled scaled[AGENT_ICON_SCALED_MAX];
    i32             scaled_n;
} IconSlot;

/* CMake embeds every PNG it finds under assets/agents/; missing files produce
 * a NULL pointer + size=0, so the letter-monogram fallback kicks in at render
 * time. Drop a <name>.png into that directory to light up any of these slots. */
/* Designated, so the struct can gain bookkeeping fields without every row
 * needing a new trailing zero (and without -Wmissing-field-initializers). */
#define ICON_SLOT(data_, size_) { .png_data = (data_), .png_size = (size_) }

static IconSlot g_slots[] = {
    [CHAT_TOOL_UNKNOWN]     = ICON_SLOT(NULL, 0),
    [CHAT_TOOL_CLAUDE]      = ICON_SLOT(AGENT_ICON_CLAUDE_data,      AGENT_ICON_CLAUDE_size),
    [CHAT_TOOL_CODEX]       = ICON_SLOT(AGENT_ICON_CODEX_ONDARK_data,
                                        AGENT_ICON_CODEX_ONDARK_size),
    [CHAT_TOOL_COPILOT]     = ICON_SLOT(AGENT_ICON_COPILOT_data,     AGENT_ICON_COPILOT_size),
    [CHAT_TOOL_CURSOR]      = ICON_SLOT(AGENT_ICON_CURSOR_data,      AGENT_ICON_CURSOR_size),
    [CHAT_TOOL_AMP]         = ICON_SLOT(AGENT_ICON_AMP_data,         AGENT_ICON_AMP_size),
    [CHAT_TOOL_CLINE]       = ICON_SLOT(AGENT_ICON_CLINE_data,       AGENT_ICON_CLINE_size),
    [CHAT_TOOL_ROO]         = ICON_SLOT(AGENT_ICON_ROO_data,         AGENT_ICON_ROO_size),
    [CHAT_TOOL_KILO]        = ICON_SLOT(AGENT_ICON_KILO_data,        AGENT_ICON_KILO_size),
    [CHAT_TOOL_KIRO]        = ICON_SLOT(AGENT_ICON_KIRO_data,        AGENT_ICON_KIRO_size),
    [CHAT_TOOL_CRUSH]       = ICON_SLOT(AGENT_ICON_CRUSH_data,       AGENT_ICON_CRUSH_size),
    [CHAT_TOOL_OPENCODE]    = ICON_SLOT(AGENT_ICON_OPENCODE_data,    AGENT_ICON_OPENCODE_size),
    [CHAT_TOOL_DROID]       = ICON_SLOT(AGENT_ICON_DROID_ONDARK_data,
                                        AGENT_ICON_DROID_ONDARK_size),
    [CHAT_TOOL_ANTIGRAVITY] = ICON_SLOT(AGENT_ICON_ANTIGRAVITY_data, AGENT_ICON_ANTIGRAVITY_size),
    [CHAT_TOOL_KIMI]        = ICON_SLOT(AGENT_ICON_KIMI_data,        AGENT_ICON_KIMI_size),
    [CHAT_TOOL_QWEN]        = ICON_SLOT(AGENT_ICON_QWEN_data,        AGENT_ICON_QWEN_size),
    [CHAT_TOOL_AIDER]       = ICON_SLOT(AGENT_ICON_AIDER_data,       AGENT_ICON_AIDER_size),
    [CHAT_TOOL_AMAZON_Q]    = ICON_SLOT(AGENT_ICON_AMAZONQ_data,     AGENT_ICON_AMAZONQ_size),
    [CHAT_TOOL_CONTINUE]    = ICON_SLOT(AGENT_ICON_CONTINUE_data,    AGENT_ICON_CONTINUE_size),
    [CHAT_TOOL_WINDSURF]    = ICON_SLOT(AGENT_ICON_WINDSURF_data,    AGENT_ICON_WINDSURF_size),
    [CHAT_TOOL_COMMANDCODE] = ICON_SLOT(AGENT_ICON_COMMANDCODE_data, AGENT_ICON_COMMANDCODE_size),
    [CHAT_TOOL_XAI]         = ICON_SLOT(AGENT_ICON_XAI_ONDARK_data,
                                        AGENT_ICON_XAI_ONDARK_size),
    /* Pi's mark is a dark plate with a white glyph, so like Codex it is the
     * derived inversion that serves the dark surface (g_slots_on_light below
     * holds the vendor file). */
    [CHAT_TOOL_PI]          = ICON_SLOT(AGENT_ICON_PI_ONDARK_data,
                                        AGENT_ICON_PI_ONDARK_size),
    /* Oh My Pi's near-black plate is difficult to distinguish on dark surfaces.
     * Its dark-surface asset keeps the gradient mark on a light plate. */
    [CHAT_TOOL_OMP]         = ICON_SLOT(AGENT_ICON_OMP_ONDARK_data,
                                        AGENT_ICON_OMP_ONDARK_size),
    /* OpenClaw's colored mark is used on either surface. The Hermes dark-surface
     * variant uses light ink with transparent face and highlight cutouts. */
    [CHAT_TOOL_OPENCLAW]    = ICON_SLOT(AGENT_ICON_OPENCLAW_data,    AGENT_ICON_OPENCLAW_size),
    [CHAT_TOOL_HERMES]      = ICON_SLOT(AGENT_ICON_HERMES_ONDARK_data,
                                        AGENT_ICON_HERMES_ONDARK_size),
};

#define ICON_SLOT_COUNT ((i32)(sizeof g_slots / sizeof g_slots[0]))

/* Alternate artwork used when the icon lands on a LIGHT background.
 *
 * Most logos carry their own colour and read on anything. A monochrome mark
 * does not: OpenCode's is a single flat rectangle, so the cut that reads on the
 * dark tab strip disappears on the near-white tile agent_icon_backdrop() draws
 * (and the reverse). Both cuts are the vendor's own, so this is a variant
 * pick — not a recolour. Everything left NULL simply keeps its one icon. */
static IconSlot g_slots_on_light[] = {
    [CHAT_TOOL_OPENCODE]   = ICON_SLOT(AGENT_ICON_OPENCODE_ONLIGHT_data,
                                       AGENT_ICON_OPENCODE_ONLIGHT_size),
    [CHAT_TOOL_COMMANDCODE] = ICON_SLOT(AGENT_ICON_COMMANDCODE_ONLIGHT_data,
                                        AGENT_ICON_COMMANDCODE_ONLIGHT_size),
    /* Kimi and Continue provide dark-surface icons. Kimi's white mark and
     * Continue's white badge are derived for light surfaces; Continue's
     * variant is inverted to preserve contrast. */
    [CHAT_TOOL_KIMI]        = ICON_SLOT(AGENT_ICON_KIMI_ONLIGHT_data,
                                        AGENT_ICON_KIMI_ONLIGHT_size),
    [CHAT_TOOL_CONTINUE]    = ICON_SLOT(AGENT_ICON_CONTINUE_ONLIGHT_data,
                                        AGENT_ICON_CONTINUE_ONLIGHT_size),
    /* The mirror image of the two above: Codex, Droid, xAI, Pi and Oh My Pi ship a BLACK badge
     * (Hermes a black drawing), so the vendor's artwork is already the light-surface cut and it is
     * the DARK panel that swallows it — the badge disappears and only the mark floats, a size
     * smaller than every logo beside it. The vendor file therefore lands HERE and the derived
     * inversion takes the dark slot above. */
    [CHAT_TOOL_CODEX]       = ICON_SLOT(AGENT_ICON_CODEX_data,
                                        AGENT_ICON_CODEX_size),
    [CHAT_TOOL_DROID]       = ICON_SLOT(AGENT_ICON_DROID_data,
                                        AGENT_ICON_DROID_size),
    [CHAT_TOOL_XAI]         = ICON_SLOT(AGENT_ICON_XAI_data,
                                        AGENT_ICON_XAI_size),
    [CHAT_TOOL_PI]          = ICON_SLOT(AGENT_ICON_PI_data,
                                        AGENT_ICON_PI_size),
    [CHAT_TOOL_OMP]         = ICON_SLOT(AGENT_ICON_OMP_data,
                                        AGENT_ICON_OMP_size),
    [CHAT_TOOL_HERMES]      = ICON_SLOT(AGENT_ICON_HERMES_data,
                                        AGENT_ICON_HERMES_size),
};

/* Whose mark `tool` wears.
 *
 * Two ChatTools can be one vendor's product twice. The standalone Cline CLI is
 * its own value on purpose — its store and its resume flag are not the VS Code
 * extension's (history/event.h) — but its logo, tint and letter are Cline's.
 * Every public entry point resolves its tool through here first, so the tables
 * stay one row per MARK and a slot is decoded once whichever of the two asks
 * for it. */
static ChatTool icon_identity(ChatTool tool) {
    return tool == CHAT_TOOL_CLINE_CLI ? CHAT_TOOL_CLINE : tool;
}

/* Which slot table serves `tool` on a background of this colour. Luminance,
 * not a theme flag: the same theme draws icons on a bright tile in one panel
 * and straight onto the dark surface in another. */
static IconSlot *icon_slot_for_bg(ChatTool tool, Color bg) {
    i32 ti = (i32)tool;
    if (ti < 0 || ti >= ICON_SLOT_COUNT) return NULL;
    if (ti < (i32)(sizeof g_slots_on_light / sizeof g_slots_on_light[0]) &&
        chrome_luminance(bg) > 0.5f) {
        IconSlot *alt = &g_slots_on_light[ti];
        if (alt->png_data && alt->png_size > 0) return alt;
    }
    return &g_slots[ti];
}

/* =========================================================================
 * Silhouettes
 *
 * Several vendors ship their mark as a FLAT MASK: every opaque texel is the
 * same colour, so the PNG's RGB carries no information at all and only its
 * alpha does. Five of them are flat BLACK (Cursor, Cline, Roo, Kilo,
 * Windsurf), and the history and resume pickers draw the logo straight onto
 * the panel body with no tile under it — `theme bg + 0.06`, which is 0.131 on
 * the default Liu theme and 0.060 on Ultradark. A black silhouette there is
 * not a logo, it is a slightly darker blob: on the real panel colour each of
 * those five is a single uniform tone with none of its internal shape
 * readable. Copilot has the same problem from the other end — a flat WHITE
 * mask, which vanishes on the light theme's near-white panel.
 *
 * Because the RGB is uninformative, stamping a colour in loses NOTHING.
 * Claude's mask is flat black too and is stamped with its brand orange. The
 * rule is a property of the artwork, so a logo added later is covered without
 * being listed, and a vendor who swaps in real colour artwork stops being
 * recoloured automatically.
 *
 * Two things deliberately do NOT get inked. A tool that ships a purpose-made
 * pair of cuts (OpenCode, Command Code) is used exactly as supplied — both of
 * those cuts are themselves flat, and inking them would throw away the very
 * artwork made for this. And a mark with more than one colour is real
 * artwork: Amp's red, Antigravity's blue and Kiro's purple stay untouched
 * even when they are dark, because there is no lossless way to recolour them.
 * ========================================================================= */

/* Every opaque texel the same colour? Sampled on the DECODED master, before
 * premultiplication, so the comparison is against the artwork's own values.
 * Texels below half alpha are edge antialiasing and say nothing about the
 * fill. An empty or fully transparent bitmap answers false — there is nothing
 * to ink. */
static bool rgba_is_flat_mask(const u8 *px, i32 w, i32 h, f32 *out_luma) {
    if (!px || w <= 0 || h <= 0) return false;
    i32 n = w * h;
    bool seen = false;
    u8 r0 = 0, g0 = 0, b0 = 0;
    for (i32 i = 0; i < n; i++) {
        if (px[i * 4 + 3] < 128) continue;
        u8 r = px[i * 4 + 0], g = px[i * 4 + 1], b = px[i * 4 + 2];
        if (!seen) { r0 = r; g0 = g; b0 = b; seen = true; continue; }
        /* A tolerance, not equality: a PNG round-trip through a vendor's
         * export can leave a couple of levels of drift across a fill that was
         * authored as one colour. Wider than that and it is real shading. */
        i32 dr = (i32)r - r0, dg = (i32)g - g0, db = (i32)b - b0;
        if (dr < 0) dr = -dr;
        if (dg < 0) dg = -dg;
        if (db < 0) db = -db;
        if (dr > 6 || dg > 6 || db > 6) return false;
    }
    if (seen && out_luma)
        *out_luma = (0.2126f * (f32)r0 + 0.7152f * (f32)g0 +
                     0.0722f * (f32)b0) / 255.0f;
    return seen;
}

/* Does this tool ship a second, purpose-made cut? Then it is already the
 * answer to "which surface is this drawn on" and must not be inked. */
static bool tool_has_alternate_cut(ChatTool tool) {
    i32 ti = (i32)tool;
    if (ti < 0 || ti >= (i32)(sizeof g_slots_on_light / sizeof g_slots_on_light[0]))
        return false;
    const IconSlot *alt = &g_slots_on_light[ti];
    return alt->png_data && alt->png_size > 0;
}

static f32 tint_luma(AgentTint t) {
    return 0.2126f * t.r + 0.7152f * t.g + 0.0722f * t.b;
}

/* The ink a silhouette is stamped with: the tool's own brand colour, moved
 * into a luminance band that survives BOTH panels it can land on.
 *
 * The ink is one colour for both: a silhouette has no second cut to switch
 * to, so the ink has to work on the 0.060 panel of Ultradark and on the 0.955
 * one of Liu Light at the same time. At 0.52 the weakest dark theme (Nord,
 * 0.262) still gives 1.9x and the light theme gives 1.8x.
 *
 * Hue is preserved by scaling the channels together rather than blending
 * toward white, so Cline stays indigo and Kilo stays pink; a tint with no hue
 * at all (Cursor's near-black, Copilot's slate) simply becomes a grey of the
 * right weight, which is what those two marks are anyway. */
#define FLAT_INK_MIN_LUMA 0.52f
#define FLAT_INK_MAX_LUMA 0.78f

/* A silhouette is only re-inked when its own fill FAILS one of the two panels
 * it can land on. Inside this band it already reads on both and the artwork is
 * left exactly as the vendor drew it — Amp's mark is a flat red at 0.44, and
 * inking it would swap the brand's red for its amber accent while making the
 * light panel worse. The edges are the panels themselves: below 0.34 a mark is
 * too close to the 0.131 body of the default dark theme, above 0.66 too close
 * to the 0.955 of the light one. */
#define FLAT_KEEP_MIN_LUMA 0.34f
#define FLAT_KEEP_MAX_LUMA 0.66f

static AgentTint flat_mask_ink(ChatTool tool) {
    AgentTint t = agent_icon_tint(tool);
    f32 l = tint_luma(t);
    if (l <= 0.0f) return (AgentTint){0.82f, 0.82f, 0.84f, 1.0f};  /* pure black tint */
    f32 target = l;
    if (l < FLAT_INK_MIN_LUMA) target = FLAT_INK_MIN_LUMA;
    else if (l > FLAT_INK_MAX_LUMA) target = FLAT_INK_MAX_LUMA;
    if (target == l) return t;
    f32 k = target / l;
    t.r *= k; t.g *= k; t.b *= k;
    /* Scaling can push the brightest channel past 1. Clipping it there would
     * shift the hue toward the remaining channels, so pull the whole colour
     * back and make up the lost luminance by lifting toward white instead —
     * the same thing a paler tint of the brand colour looks like. */
    f32 mx = t.r > t.g ? t.r : t.g;
    if (t.b > mx) mx = t.b;
    if (mx > 1.0f) {
        t.r /= mx; t.g /= mx; t.b /= mx;
        f32 have = tint_luma(t);
        if (have < target && have < 1.0f) {
            f32 m = (target - have) / (1.0f - have);
            if (m > 1.0f) m = 1.0f;
            t.r += (1.0f - t.r) * m;
            t.g += (1.0f - t.g) * m;
            t.b += (1.0f - t.b) * m;
        }
    }
    return t;
}

static const u8 *slot_rgba(IconSlot *s, ChatTool tool, i32 *w, i32 *h);
static const u8 *slot_rgba_scaled(IconSlot *s, ChatTool tool, i32 target_px,
                                  i32 *w, i32 *h);

const u8 *agent_icon_rgba(ChatTool tool, i32 *w, i32 *h) {
    tool = icon_identity(tool);
    if ((i32)tool < 0 || (i32)tool >= ICON_SLOT_COUNT) return NULL;
    IconSlot *s = &g_slots[(i32)tool];
    const u8 *px = slot_rgba(s, tool, w, h);
    /* The master's address is now the caller's (and possibly the renderer image
     * cache's) — pin it for good. */
    if (px) s->master_exposed = true;
    return px;
}

const u8 *agent_icon_rgba_scaled_on(ChatTool tool, i32 target_px, Color bg,
                                    i32 *w, i32 *h) {
    tool = icon_identity(tool);
    IconSlot *s = icon_slot_for_bg(tool, bg);
    if (!s) return NULL;
    return slot_rgba_scaled(s, tool, target_px, w, h);
}

static const u8 *slot_rgba(IconSlot *s, ChatTool tool, i32 *w, i32 *h) {
    if (!s->png_data || s->png_size <= 0) return NULL;

    /* Decode on demand. This runs again when the master was released after a
     * downscale and a size with no cached copy is asked for — bounded by
     * AGENT_ICON_SCALED_MAX distinct sizes per slot over the whole session, and
     * in practice one or two. `decode_failed` keeps a broken PNG from being
     * re-attempted every frame. */
    if (!s->rgba && !s->decode_failed) {
        s->tried = true;
        int iw = 0, ih = 0, ch = 0;
        /* Force 4-channel RGBA so renderer_draw_image's upload path matches. */
        u8 *px = stbi_load_from_memory(s->png_data, s->png_size, &iw, &ih, &ch, 4);
        if (!px) s->decode_failed = true;
        if (px) {
            /* Brand recoloring: a PNG whose opaque texels are all one colour
             * is a shape mask — the alpha carries the geometry and the RGB
             * carries nothing — so stamping an ink into it is lossless. See
             * the "Silhouettes" comment above for why it is needed and which
             * artwork is exempt. */
            f32 mask_luma = 0.0f;
            bool colorize = !tool_has_alternate_cut(tool) &&
                            rgba_is_flat_mask(px, iw, ih, &mask_luma) &&
                            (mask_luma < FLAT_KEEP_MIN_LUMA ||
                             mask_luma > FLAT_KEEP_MAX_LUMA);
            AgentTint tint = colorize ? flat_mask_ink(tool)
                                      : agent_icon_tint(tool);
            s->inked = colorize;
            s->ink   = tint;
            if (colorize) {
                u8 cr = (u8)(tint.r * 255.0f + 0.5f);
                u8 cg = (u8)(tint.g * 255.0f + 0.5f);
                u8 cb = (u8)(tint.b * 255.0f + 0.5f);
                i32 n = iw * ih;
                for (i32 i = 0; i < n; i++) {
                    px[i * 4 + 0] = cr;
                    px[i * 4 + 1] = cg;
                    px[i * 4 + 2] = cb;
                    /* alpha unchanged */
                }
            }
            /* After every straight-alpha edit above, convert to the
             * premultiplied form the renderer's (ONE, 1-SrcAlpha) image blend
             * requires. Skipping this is what made logo edges bloom: a texel at
             * 50 % alpha composited as dst*0.5 + rgb instead of dst*0.5 +
             * rgb*0.5, which on a light logo saturates straight to white. */
            img_premultiply_rgba(px, iw, ih);
            s->rgba = px;
            s->w    = iw;
            s->h    = ih;
        }
    }
    if (!s->rgba) return NULL;
    if (w) *w = s->w;
    if (h) *h = s->h;
    return s->rgba;
}

const u8 *agent_icon_rgba_scaled(ChatTool tool, i32 target_px, i32 *w, i32 *h) {
    tool = icon_identity(tool);
    if ((i32)tool < 0 || (i32)tool >= ICON_SLOT_COUNT) return NULL;
    return slot_rgba_scaled(&g_slots[(i32)tool], tool, target_px, w, h);
}

/* Closest cached size, or NULL when nothing has been scaled yet. Used when the
 * master is gone and re-decoding it would be the wrong trade: the sampler
 * rescales a slightly-off source, which is soft but never blank. */
static const u8 *slot_nearest_scaled(IconSlot *s, i32 target_px,
                                     i32 *w, i32 *h) {
    if (s->scaled_n <= 0) return NULL;
    i32 best = 0, best_d = -1;
    for (i32 i = 0; i < s->scaled_n; i++) {
        i32 d = s->scaled[i].size - target_px;
        if (d < 0) d = -d;
        if (best_d < 0 || d < best_d) { best_d = d; best = i; }
    }
    if (w) *w = s->scaled[best].size;
    if (h) *h = s->scaled[best].size;
    return s->scaled[best].rgba;
}

static const u8 *slot_rgba_scaled(IconSlot *s, ChatTool tool, i32 target_px,
                                  i32 *w, i32 *h) {
    if (!s->png_data || s->png_size <= 0) return NULL;

    /* Exact cached copy first — answered without ever touching the master,
     * which is precisely what lets the master be released below. */
    for (i32 i = 0; i < s->scaled_n; i++) {
        if (s->scaled[i].size == target_px) {
            if (w) *w = target_px;
            if (h) *h = target_px;
            return s->scaled[i].rgba;
        }
    }

    /* Master already released, and this request could not produce a new exact
     * copy anyway (table full, or an upscale/non-square that only the master
     * could serve). Answer from the nearest cached size rather than paying a
     * full PNG decode — and a resident master again — on every frame. */
    bool room = (s->scaled_n < AGENT_ICON_SCALED_MAX);
    bool dims_known = (s->w > 0 && s->h > 0);
    bool downscales = !dims_known ||   /* unknown until the first decode */
                      (target_px > 0 && target_px < s->w && target_px < s->h &&
                       s->w == s->h);
    if (!s->rgba && (!room || !downscales)) {
        const u8 *fb = slot_nearest_scaled(s, target_px, w, h);
        if (fb) return fb;
    }

    i32 nw = 0, nh = 0;
    const u8 *native = slot_rgba(s, tool, &nw, &nh);
    if (!native) {
        /* Decode (or re-decode) failed. Scaled copies from an earlier decode
         * may still be here — soft beats blank. NULL when there are none,
         * which is the letter-monogram fallback callers already handle. */
        return slot_nearest_scaled(s, target_px, w, h);
    }

    if (target_px <= 0 || target_px >= nw || target_px >= nh || nw != nh ||
        !room) {
        /* upscale/exact/non-square, or the table is full: the master IS the
         * answer, so its address escapes and it can never be freed. */
        s->master_exposed = true;
        if (w) *w = nw;
        if (h) *h = nh;
        return native;
    }
    u8 *scaled = ui_icon_downscale_rgba(native, nw, nh, target_px, target_px);
    if (!scaled) {
        s->master_exposed = true;
        if (w) *w = nw;
        if (h) *h = nh;
        return native;
    }
    s->scaled[s->scaled_n].size = target_px;
    s->scaled[s->scaled_n].rgba = scaled;
    s->scaled_n++;

    /* The scaled copy is the only thing anything draws, so drop the master
     * (256×256×4 = 256 KB per slot, ~5 MB across the table). Safe only because
     * this address never reached a caller: freeing an exposed pointer would let
     * a later allocation land on it and alias the renderer's cached texture.
     * `s->w`/`s->h` survive, so the next size is decided without the pixels. */
    if (!s->master_exposed) {
        free((void *)s->rgba);
        s->rgba = NULL;
    }

    if (w) *w = target_px;
    if (h) *h = target_px;
    return scaled;
}

/* Per-tool tile memo. The agent-history picker draws 20+ icons per frame
 * at vsync rates, and the WCAG ratio needs 6 powf() calls per icon — too
 * much for an answer that only changes when the theme changes. Keyed on
 * the theme pointer (theme structs are statically allocated or owned by
 * the user-themes table for the process lifetime, so pointer identity is
 * a sound invalidator). */
static Color  g_backdrop_cache[CHAT_TOOL_COUNT_];
static bool   g_backdrop_cached[CHAT_TOOL_COUNT_];
static const Theme *g_backdrop_theme = NULL;

/* The colour this tool's mark is ACTUALLY drawn in — the stamped ink for a
 * silhouette, the brand tint otherwise. Forces the decode the draw is about to
 * do anyway (it is cached, and the backdrop is only asked for a tool that is
 * about to appear), because flatness is a property of the pixels and cannot be
 * known without them. Falls back to the raw tint if the PNG will not decode,
 * which is also the case where nothing gets drawn over the tile at all. */
static AgentTint effective_mark_tint(ChatTool tool) {
    i32 ti = (i32)tool;
    if (ti < 0 || ti >= ICON_SLOT_COUNT) return agent_icon_tint(tool);
    IconSlot *s = &g_slots[ti];
    i32 w = 0, h = 0;
    const u8 *px = slot_rgba(s, tool, &w, &h);
    if (s->inked) return s->ink;
    /* Not inked, so the artwork is its own colour and the brand tint is only a
     * guess at it, sometimes a bad one: Amazon Q's tint is the AWS orange but
     * its artwork averages a dark brown, and a tile chosen against the tint
     * would be chosen for a colour three stops lighter than the mark it sits
     * under. So what is actually there is averaged. Straight alpha, and only
     * the texels that carry the mark: the transparent surround is not part of
     * the logo, and a premultiplied average would drag every colour toward
     * black by its own coverage. */
    if (px && w > 0 && h > 0) {
        f64 sr = 0, sg = 0, sb = 0;
        i64 n = 0;
        for (i64 i = 0; i < (i64)w * h; i++) {
            f64 a = px[i * 4 + 3] / 255.0;
            if (a < 0.5) continue;
            f64 r = px[i * 4 + 0] / 255.0 / a;
            f64 g = px[i * 4 + 1] / 255.0 / a;
            f64 b = px[i * 4 + 2] / 255.0 / a;
            if (r > 1.0) r = 1.0;
            if (g > 1.0) g = 1.0;
            if (b > 1.0) b = 1.0;
            sr += r; sg += g; sb += b; n++;
        }
        if (n > 0)
            return (AgentTint){ (f32)(sr / n), (f32)(sg / n), (f32)(sb / n), 1.0f };
    }
    return agent_icon_tint(tool);
}

static Color compute_backdrop(ChatTool tool, const Theme *t) {
    ChromePalette cp = chrome_palette_for(t);
    f32 white_mix = cp.is_light ? 0.78f : 0.94f;
    Color tile = {
        t->bg.r * (1.0f - white_mix) + white_mix,
        t->bg.g * (1.0f - white_mix) + white_mix,
        t->bg.b * (1.0f - white_mix) + white_mix,
        1.0f,
    };

    /* When the mark and the tile share luminance the logo disappears (e.g.
     * Claude's rust-orange on a peach theme). Flip to a dark gunmetal tile
     * when the WCAG ratio is too thin. Measured against the ink that will
     * actually be drawn, not the brand tint: a silhouette is stamped with a
     * LIFTED version of that tint, and sizing the tile against the original
     * would put Cursor's 0.10 near-black on a light tile that its 0.52 ink
     * then fails by the very margin this test exists to keep. */
    AgentTint tint = effective_mark_tint(tool);
    Color tint_c = { tint.r, tint.g, tint.b, 1.0f };
    f32 lt = chrome_luminance(tint_c);
    f32 lb = chrome_luminance(tile);
    f32 hi = lt > lb ? lt : lb;
    f32 lo = lt > lb ? lb : lt;
    if ((hi + 0.05f) / (lo + 0.05f) < 2.0f) {
        tile = (Color){ 0.13f, 0.14f, 0.17f, 1.0f };
    }
    return tile;
}

Color agent_icon_backdrop(ChatTool tool, const Theme *t) {
    tool = icon_identity(tool);
    if (!t) return (Color){0.97f, 0.97f, 0.97f, 1.0f};
    if (t != g_backdrop_theme) {
        for (i32 i = 0; i < CHAT_TOOL_COUNT_; i++) g_backdrop_cached[i] = false;
        g_backdrop_theme = t;
    }
    i32 ti = (i32)tool;
    if (ti < 0 || ti >= CHAT_TOOL_COUNT_) ti = 0;
    if (!g_backdrop_cached[ti]) {
        g_backdrop_cache[ti] = compute_backdrop((ChatTool)ti, t);
        g_backdrop_cached[ti] = true;
    }
    return g_backdrop_cache[ti];
}

AgentTint agent_icon_tint(ChatTool tool) {
    switch (icon_identity(tool)) {
    case CHAT_TOOL_CLAUDE:      return (AgentTint){0.95f, 0.55f, 0.28f, 1.0f}; /* rust-orange   */
    case CHAT_TOOL_CODEX:       return (AgentTint){0.12f, 0.70f, 0.56f, 1.0f}; /* teal/emerald  */
    case CHAT_TOOL_COPILOT:     return (AgentTint){0.22f, 0.23f, 0.26f, 1.0f}; /* github slate  */
    case CHAT_TOOL_CURSOR:      return (AgentTint){0.10f, 0.10f, 0.10f, 1.0f}; /* near black    */
    case CHAT_TOOL_AMP:         return (AgentTint){0.98f, 0.78f, 0.20f, 1.0f}; /* amber         */
    case CHAT_TOOL_CLINE:       return (AgentTint){0.36f, 0.42f, 0.88f, 1.0f}; /* indigo        */
    case CHAT_TOOL_ROO:         return (AgentTint){0.78f, 0.36f, 0.92f, 1.0f}; /* purple        */
    case CHAT_TOOL_KILO:        return (AgentTint){0.92f, 0.36f, 0.58f, 1.0f}; /* pink          */
    case CHAT_TOOL_KIRO:        return (AgentTint){0.20f, 0.68f, 0.82f, 1.0f}; /* cyan          */
    case CHAT_TOOL_CRUSH:       return (AgentTint){0.86f, 0.22f, 0.30f, 1.0f}; /* red           */
    case CHAT_TOOL_OPENCODE:    return (AgentTint){0.30f, 0.76f, 0.44f, 1.0f}; /* green         */
    case CHAT_TOOL_DROID:       return (AgentTint){0.45f, 0.55f, 0.62f, 1.0f}; /* steel         */
    case CHAT_TOOL_ANTIGRAVITY: return (AgentTint){0.60f, 0.40f, 0.90f, 1.0f}; /* violet        */
    case CHAT_TOOL_KIMI:        return (AgentTint){0.95f, 0.46f, 0.20f, 1.0f}; /* moonshot orange */
    case CHAT_TOOL_QWEN:        return (AgentTint){0.38f, 0.58f, 0.88f, 1.0f}; /* alibaba blue  */
    case CHAT_TOOL_AIDER:       return (AgentTint){0.92f, 0.34f, 0.26f, 1.0f}; /* aider red     */
    case CHAT_TOOL_AMAZON_Q:    return (AgentTint){0.97f, 0.60f, 0.20f, 1.0f}; /* aws orange    */
    case CHAT_TOOL_CONTINUE:    return (AgentTint){0.28f, 0.65f, 0.50f, 1.0f}; /* continue teal */
    case CHAT_TOOL_WINDSURF:    return (AgentTint){0.10f, 0.72f, 0.62f, 1.0f}; /* codeium teal  */
    case CHAT_TOOL_COMMANDCODE: return (AgentTint){0.36f, 0.50f, 0.96f, 1.0f}; /* command blue  */
    case CHAT_TOOL_XAI:         return (AgentTint){0.12f, 0.12f, 0.14f, 1.0f}; /* xAI near-black */
    case CHAT_TOOL_PI:          return (AgentTint){0.50f, 0.72f, 0.92f, 1.0f}; /* pi sky blue   */
    case CHAT_TOOL_OMP:         return (AgentTint){0.86f, 0.46f, 0.86f, 1.0f}; /* omp orchid    */
    case CHAT_TOOL_OPENCLAW:    return (AgentTint){1.00f, 0.32f, 0.30f, 1.0f}; /* claw red      */
    case CHAT_TOOL_HERMES:      return (AgentTint){0.34f, 0.78f, 0.74f, 1.0f}; /* hermes aqua   */
    default:                    return (AgentTint){0.55f, 0.55f, 0.60f, 1.0f};
    }
}

/* ---------------------------------------------------------------------------
 * Sleeping mascot, themed
 *
 * The artwork ships as SVG so its fills can be re-pointed at the active theme
 * before rasterising. Colours are rewritten as TEXT in a copy of the document —
 * the same trick lucide.c uses for "currentColor" — because nanosvg resolves
 * paint at parse time, so there is no post-parse colour to poke. Every
 * replacement is written over an equal-length "#RRGGBB" run, which keeps the
 * document byte-identical in length and the parser none the wiser.
 * ------------------------------------------------------------------------- */

/* The three literals in assets/liu-sleep.svg. Kept here rather than derived at
 * runtime: matching on exact source colours means an edited SVG that no longer
 * contains one simply keeps that fill, instead of silently recolouring the
 * wrong shape. */
#define SLEEP_SVG_BODY_LO "#C273EF"   /* gradient start — lit side of the coat */
#define SLEEP_SVG_BODY_HI "#D494FA"   /* gradient end                          */
#define SLEEP_SVG_INK     "#351053"   /* outline, closed eyes, the "z z"       */

typedef struct {
    i32  px;
    u32  body_lo, body_hi, ink;
    u8  *rgba;
} SleepThemedEntry;

/* One entry per (size, palette). A theme switch adds one; the pointer must stay
 * valid because the renderer's image cache keys on it. */
#define SLEEP_THEMED_CAP 12
static SleepThemedEntry g_sleep_themed[SLEEP_THEMED_CAP];
static i32 g_sleep_themed_count;

static void sleep_svg_replace(char *doc, const char *from, u32 rgb) {
    char to[8];
    snprintf(to, sizeof to, "#%02X%02X%02X",
             (unsigned)((rgb >> 16) & 0xFF),
             (unsigned)((rgb >> 8)  & 0xFF),
             (unsigned)( rgb        & 0xFF));
    for (char *p = doc; (p = strstr(p, from)) != NULL; p += 7)
        memcpy(p, to, 7);          /* both are exactly "#RRGGBB" */
}

const u8 *liu_sleep_icon_themed(i32 px, u32 body_lo, u32 body_hi, u32 ink) {
    if (px <= 0 || px > 1024) return NULL;
    if (LIU_SLEEP_SVG_size <= 0) return NULL;

    for (i32 i = 0; i < g_sleep_themed_count; i++) {
        SleepThemedEntry *e = &g_sleep_themed[i];
        if (e->px == px && e->body_lo == body_lo &&
            e->body_hi == body_hi && e->ink == ink) return e->rgba;
    }
    if (g_sleep_themed_count >= SLEEP_THEMED_CAP) return NULL;

    usize len = (usize)LIU_SLEEP_SVG_size;
    char *doc = (char *)malloc(len + 1);
    if (!doc) return NULL;
    memcpy(doc, LIU_SLEEP_SVG_data, len);
    doc[len] = '\0';

    sleep_svg_replace(doc, SLEEP_SVG_BODY_LO, body_lo);
    sleep_svg_replace(doc, SLEEP_SVG_BODY_HI, body_hi);
    sleep_svg_replace(doc, SLEEP_SVG_INK,     ink);

    NSVGimage *img = nsvgParse(doc, "px", 96.0f);   /* mutates doc */
    free(doc);
    if (!img) return NULL;
    if (img->width <= 0.0f || img->height <= 0.0f) { nsvgDelete(img); return NULL; }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(img); return NULL; }

    /* Rasterise at 2x and area-average down: the drawing has thin outlines and
     * a 1x pass leaves them crawling. Mirrors lucide_render's supersample. */
    i32 big = px * 2;
    u8 *buf = (u8 *)calloc((usize)big * (usize)big * 4u, 1);
    if (!buf) { nsvgDeleteRasterizer(rast); nsvgDelete(img); return NULL; }

    f32 scale = (f32)big / (img->width > img->height ? img->width : img->height);
    f32 tx = ((f32)big - img->width  * scale) * 0.5f;
    f32 ty = ((f32)big - img->height * scale) * 0.5f;
    nsvgRasterize(rast, img, tx, ty, scale, buf, big, big, big * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);

    /* nanosvg emits straight alpha; the upload path consumes premultiplied. */
    img_premultiply_rgba(buf, big, big);

    u8 *out = ui_icon_downscale_rgba(buf, big, big, px, px);
    free(buf);
    if (!out) return NULL;

    SleepThemedEntry *e = &g_sleep_themed[g_sleep_themed_count++];
    e->px = px; e->body_lo = body_lo; e->body_hi = body_hi; e->ink = ink;
    e->rgba = out;
    return out;
}

const u8 *liu_sleep_icon_rgba(i32 *w, i32 *h) {
    static const u8 *cache_rgba = NULL;
    static i32 cache_w = 0, cache_h = 0;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (LIU_SLEEP_ICON_size > 0) {
            int iw = 0, ih = 0, ch = 0;
            u8 *px = stbi_load_from_memory(LIU_SLEEP_ICON_data, LIU_SLEEP_ICON_size,
                                           &iw, &ih, &ch, 4);
            if (px) {
                /* Near-white chroma key, kept as a safety net.
                 *
                 * The shipped art is a transparent PNG rendered from the
                 * sleeping-bunny SVG in assets, so this pass finds nothing to
                 * strip: its lightest opaque pixel has min(r,g,b) = 146, well
                 * under the 200 threshold. It stays so that a re-export with
                 * an opaque white plate does not paint a white square onto the
                 * dark sleeping-tab card. A hard cutoff at 230 with a short
                 * 200-230 tail removes such a plate without leaving an
                 * anti-aliased halo around the silhouette. */
                i32 n = iw * ih;
                for (i32 i = 0; i < n; i++) {
                    u8 r = px[i * 4 + 0];
                    u8 g = px[i * 4 + 1];
                    u8 b = px[i * 4 + 2];
                    u8 minc = r < g ? (r < b ? r : b) : (g < b ? g : b);
                    if (minc >= 230) {
                        px[i * 4 + 3] = 0;
                    } else if (minc >= 200) {
                        i32 t = ((i32)minc - 200) * 255 / 30;
                        if (t > 255) t = 255;
                        i32 a = (i32)px[i * 4 + 3] * (255 - t) / 255;
                        px[i * 4 + 3] = (u8)a;
                    }
                }
                /* Premultiply after the chroma key, not before — the key writes
                 * alpha, and premultiplying first would bake the old alpha into
                 * RGB. The 200-230 tail above is exactly the partial-alpha band
                 * that composites wrong without this. */
                img_premultiply_rgba(px, iw, ih);
                cache_rgba = px;
                cache_w = iw;
                cache_h = ih;
            }
        }
    }
    if (!cache_rgba) return NULL;
    if (w) *w = cache_w;
    if (h) *h = cache_h;
    return cache_rgba;
}

/* Round the corners of a square PREMULTIPLIED RGBA buffer in place, with ~1px
 * anti-aliased coverage.
 *
 * appicon.png is a full-bleed opaque square — no alpha at all — so drawing it
 * raw puts a hard-edged rectangle on the card. The mask is applied AFTER the
 * downscale, at exactly the pixel size it will be drawn at: masking the 1024px
 * master first and then box-filtering it would smear the corner ramp across
 * several output pixels instead of landing one crisp AA edge.
 *
 * Radius follows Apple's squircle proportion closely enough at these sizes
 * (~22.4 % of the side) that the mark reads as an app icon next to the Dock. */
static void app_icon_round_corners(u8 *px, i32 size) {
    f32 r = (f32)size * 0.224f;
    if (r < 1.0f) return;
    for (i32 yi = 0; yi < size; yi++) {
        for (i32 xi = 0; xi < size; xi++) {
            /* Distance outside the rounded rect, measured from the pixel
             * CENTRE so the ramp straddles the true edge. */
            f32 cx = (f32)xi + 0.5f, cy = (f32)yi + 0.5f;
            f32 dx = fabsf(cx - (f32)size * 0.5f) - ((f32)size * 0.5f - r);
            f32 dy = fabsf(cy - (f32)size * 0.5f) - ((f32)size * 0.5f - r);
            f32 d;
            if (dx <= 0.0f && dy <= 0.0f) continue;         /* deep inside */
            if (dx < 0.0f) dx = 0.0f;
            if (dy < 0.0f) dy = 0.0f;
            d = sqrtf(dx * dx + dy * dy) - r;
            if (d <= -0.5f) continue;
            f32 cov = 0.5f - d;
            if (cov < 0.0f) cov = 0.0f;
            if (cov > 1.0f) cov = 1.0f;
            /* Premultiplied: every channel scales, alpha included. */
            u8 *p = px + ((usize)yi * (usize)size + (usize)xi) * 4u;
            for (i32 c = 0; c < 4; c++) p[c] = (u8)((f32)p[c] * cov + 0.5f);
        }
    }
}

const u8 *liu_app_icon_rgba(i32 target_px, i32 *w, i32 *h) {
    /* Cached per REQUESTED size and NEVER freed — the renderer's image cache
     * keys textures by pixel pointer, so a recycled address would alias a stale
     * texture (same policy as the scaled agent logos above). `req` and `size`
     * are separate because they diverge on the upscale path below. Four slots
     * covers 1x/2x plus a couple of UI-scale steps; past that it stops caching
     * rather than grow an unbounded table for a mark on one settings tab. */
    #define APP_ICON_CACHE_MAX 4
    static struct { i32 req; i32 size; const u8 *rgba; } cache[APP_ICON_CACHE_MAX];
    static i32 cache_n = 0;

    if (target_px <= 0 || LIU_APP_ICON_size <= 0) return NULL;
    for (i32 i = 0; i < cache_n; i++) {
        if (cache[i].req == target_px) {
            if (w) *w = cache[i].size;
            if (h) *h = cache[i].size;
            return cache[i].rgba;
        }
    }
    if (cache_n >= APP_ICON_CACHE_MAX) return NULL;

    /* Decode on demand and release the source as soon as the scaled copy
     * exists, so nothing but the drawn size stays resident. A later, different
     * size pays one more decode — a handful of times per session at most. */
    int iw = 0, ih = 0, comp = 0;
    u8 *master = stbi_load_from_memory(LIU_APP_ICON_data, LIU_APP_ICON_size,
                                       &iw, &ih, &comp, 4);
    if (!master) return NULL;
    if (iw <= 0 || ih <= 0) { free(master); return NULL; }
    img_premultiply_rgba(master, iw, ih);

    u8 *out;
    i32 out_px;
    if (target_px < iw && target_px < ih) {
        /* The normal path: resample to exactly the pixel size it is drawn at.
         * The GPU has no mipmaps here, so any other ratio shimmers. */
        out = ui_icon_downscale_rgba(master, iw, ih, target_px, target_px);
        free(master);
        if (!out) return NULL;
        out_px = target_px;
    } else {
        /* The downscale helper supports minification only. If the requested
         * size is at least the master dimensions, reuse a square master and
         * let the sampler upscale it; unsupported non-square masters return
         * no icon. */
        if (iw != ih) { free(master); return NULL; }
        out = master;
        out_px = iw;
    }

    app_icon_round_corners(out, out_px);
    cache[cache_n].req  = target_px;
    cache[cache_n].size = out_px;
    cache[cache_n].rgba = out;
    cache_n++;
    if (w) *w = out_px;
    if (h) *h = out_px;
    return out;
    #undef APP_ICON_CACHE_MAX
}

/* ui.c owns the UI text helper; every other chrome file reaches it the same
 * way (tab_popup.c, vault_ui.c, browser_ui.c, lightbox.c). */
extern void draw_text_ex(Renderer *r, const char *text, f32 x, f32 y, Color fg,
                         i32 max_chars, f32 step);

void agent_icon_draw_letter(Renderer *r, ChatTool tool,
                            f32 x, f32 y, f32 w, f32 h, Color fg) {
    if (!r || w <= 0.0f || h <= 0.0f) return;
    const char *ltr = agent_icon_letter(tool);
    if (!ltr || !ltr[0]) return;

    /* The scale the surrounding panel set, restored below. Zero means the
     * caller was drawing at terminal scale and wants that back, which is a
     * different call (renderer_reset_ui_scale) rather than set(0,0). */
    f32 save_cw = r->ui_cell_w, save_ch = r->ui_cell_h;

    /* A UI cell is taller than it is wide by the FACE's ratio, not by the
     * 1:2 the chrome layout assumes — renderer_set_ui_scale re-derives the
     * height from the width that way, so asking for a cell as tall as the box
     * means asking for `h * (face width / face height)` of width. Getting this
     * backwards draws a letter wider than its own box on a tall face like
     * JetBrains Mono (17x37). */
    f32 aspect = (r->font.cell_height > 0.0f)
                 ? (r->font.cell_width / r->font.cell_height) : 0.5f;
    f32 cell_w = h * aspect;
    if (cell_w <= 0.0f) return;

    renderer_set_ui_scale(r, cell_w, h);
    /* y is the box top: the glyph centres itself inside the cell just
     * declared, so no vertical term belongs here. Horizontally the cell is
     * narrower than the box, so it is centred by hand. */
    draw_text_ex(r, ltr, x + (w - cell_w) * 0.5f, y, fg, 1, cell_w);
    if (save_cw > 0.0f && save_ch > 0.0f) renderer_set_ui_scale(r, save_cw, save_ch);
    else                                  renderer_reset_ui_scale(r);
}

const char *agent_icon_letter(ChatTool tool) {
    switch (icon_identity(tool)) {
    case CHAT_TOOL_CLAUDE:      return "C";
    case CHAT_TOOL_CODEX:       return "X";
    case CHAT_TOOL_COPILOT:     return "G";
    case CHAT_TOOL_CURSOR:      return "▶";
    case CHAT_TOOL_AMP:         return "A";
    case CHAT_TOOL_CLINE:       return "L";
    case CHAT_TOOL_ROO:         return "R";
    case CHAT_TOOL_KILO:        return "K";
    case CHAT_TOOL_KIRO:        return "◆";
    case CHAT_TOOL_CRUSH:       return "✦";
    case CHAT_TOOL_OPENCODE:    return "O";
    case CHAT_TOOL_DROID:       return "D";
    case CHAT_TOOL_ANTIGRAVITY: return "↑";
    case CHAT_TOOL_KIMI:        return "✧";
    case CHAT_TOOL_QWEN:        return "Q";
    case CHAT_TOOL_AIDER:       return "A";
    case CHAT_TOOL_AMAZON_Q:    return "Q";
    case CHAT_TOOL_CONTINUE:    return "▸";
    case CHAT_TOOL_WINDSURF:    return "W";
    case CHAT_TOOL_COMMANDCODE: return "⌘";
    case CHAT_TOOL_XAI:         return "G";
    case CHAT_TOOL_PI:          return "π";
    case CHAT_TOOL_OMP:         return "ω";
    case CHAT_TOOL_OPENCLAW:    return "🦞";
    case CHAT_TOOL_HERMES:      return "☤";
    default:                    return "?";
    }
}
