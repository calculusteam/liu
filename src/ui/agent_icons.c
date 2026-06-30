/*
 * Liu — decode and cache compiled-in agent PNG logos.
 * Build input:  ${CMAKE_BINARY_DIR}/generated/ui/agent_icons_data.h
 */
#include "ui/agent_icons.h"
#include "ui/chrome_palette.h"
#include "stb_image.h"
#include "ui/agent_icons_data.h"

#include <stddef.h>

typedef struct {
    const u8 *png_data;
    i32       png_size;
    const u8 *rgba;      /* decoded cache */
    i32       w, h;
    bool      tried;     /* set true once decode has been attempted (success or not) */
} IconSlot;

/* CMake embeds every PNG it finds under assets/agents/; missing files produce
 * a NULL pointer + size=0, so the letter-monogram fallback kicks in at render
 * time. Drop a <name>.png into that directory to light up any of these slots. */
static IconSlot g_slots[] = {
    [CHAT_TOOL_UNKNOWN]     = { NULL, 0, NULL, 0, 0, false },
    [CHAT_TOOL_CLAUDE]      = { AGENT_ICON_CLAUDE_data,      AGENT_ICON_CLAUDE_size,      NULL, 0, 0, false },
    [CHAT_TOOL_CODEX]       = { AGENT_ICON_CODEX_data,       AGENT_ICON_CODEX_size,       NULL, 0, 0, false },
    [CHAT_TOOL_COPILOT]     = { AGENT_ICON_COPILOT_data,     AGENT_ICON_COPILOT_size,     NULL, 0, 0, false },
    [CHAT_TOOL_CURSOR]      = { AGENT_ICON_CURSOR_data,      AGENT_ICON_CURSOR_size,      NULL, 0, 0, false },
    [CHAT_TOOL_AMP]         = { AGENT_ICON_AMP_data,         AGENT_ICON_AMP_size,         NULL, 0, 0, false },
    [CHAT_TOOL_CLINE]       = { AGENT_ICON_CLINE_data,       AGENT_ICON_CLINE_size,       NULL, 0, 0, false },
    [CHAT_TOOL_ROO]         = { AGENT_ICON_ROO_data,         AGENT_ICON_ROO_size,         NULL, 0, 0, false },
    [CHAT_TOOL_KILO]        = { AGENT_ICON_KILO_data,        AGENT_ICON_KILO_size,        NULL, 0, 0, false },
    [CHAT_TOOL_KIRO]        = { AGENT_ICON_KIRO_data,        AGENT_ICON_KIRO_size,        NULL, 0, 0, false },
    [CHAT_TOOL_CRUSH]       = { AGENT_ICON_CRUSH_data,       AGENT_ICON_CRUSH_size,       NULL, 0, 0, false },
    [CHAT_TOOL_OPENCODE]    = { AGENT_ICON_OPENCODE_data,    AGENT_ICON_OPENCODE_size,    NULL, 0, 0, false },
    [CHAT_TOOL_DROID]       = { AGENT_ICON_DROID_data,       AGENT_ICON_DROID_size,       NULL, 0, 0, false },
    [CHAT_TOOL_ANTIGRAVITY] = { AGENT_ICON_ANTIGRAVITY_data, AGENT_ICON_ANTIGRAVITY_size, NULL, 0, 0, false },
    [CHAT_TOOL_KIMI]        = { AGENT_ICON_KIMI_data,        AGENT_ICON_KIMI_size,        NULL, 0, 0, false },
    [CHAT_TOOL_QWEN]        = { AGENT_ICON_QWEN_data,        AGENT_ICON_QWEN_size,        NULL, 0, 0, false },
    [CHAT_TOOL_AIDER]       = { AGENT_ICON_AIDER_data,       AGENT_ICON_AIDER_size,       NULL, 0, 0, false },
    [CHAT_TOOL_AMAZON_Q]    = { AGENT_ICON_AMAZONQ_data,     AGENT_ICON_AMAZONQ_size,     NULL, 0, 0, false },
    [CHAT_TOOL_CONTINUE]    = { AGENT_ICON_CONTINUE_data,    AGENT_ICON_CONTINUE_size,    NULL, 0, 0, false },
    [CHAT_TOOL_WINDSURF]    = { AGENT_ICON_WINDSURF_data,    AGENT_ICON_WINDSURF_size,    NULL, 0, 0, false },
    [CHAT_TOOL_ZED]         = { AGENT_ICON_ZED_data,         AGENT_ICON_ZED_size,         NULL, 0, 0, false },
    [CHAT_TOOL_COMMANDCODE] = { AGENT_ICON_COMMANDCODE_data, AGENT_ICON_COMMANDCODE_size, NULL, 0, 0, false },
    [CHAT_TOOL_XAI]         = { AGENT_ICON_XAI_data,         AGENT_ICON_XAI_size,         NULL, 0, 0, false },
};

#define ICON_SLOT_COUNT ((i32)(sizeof g_slots / sizeof g_slots[0]))

const u8 *agent_icon_rgba(ChatTool tool, i32 *w, i32 *h) {
    if ((i32)tool < 0 || (i32)tool >= ICON_SLOT_COUNT) return NULL;
    IconSlot *s = &g_slots[(i32)tool];
    if (!s->png_data || s->png_size <= 0) return NULL;

    if (!s->tried) {
        s->tried = true;
        int iw = 0, ih = 0, ch = 0;
        /* Force 4-channel RGBA so renderer_draw_image's upload path matches. */
        u8 *px = stbi_load_from_memory(s->png_data, s->png_size, &iw, &ih, &ch, 4);
        if (px) {
            /* Brand recoloring: treat the PNG as a shape mask (alpha carries
             * geometry) and stamp the brand color into RGB. Claude specifically
             * must render in its orange brand color on the white backdrop. */
            AgentTint tint = agent_icon_tint(tool);
            bool colorize = (tool == CHAT_TOOL_CLAUDE);
            if (colorize) {
                u8 cr = (u8)(tint.r * 255.0f);
                u8 cg = (u8)(tint.g * 255.0f);
                u8 cb = (u8)(tint.b * 255.0f);
                i32 n = iw * ih;
                for (i32 i = 0; i < n; i++) {
                    px[i * 4 + 0] = cr;
                    px[i * 4 + 1] = cg;
                    px[i * 4 + 2] = cb;
                    /* alpha unchanged */
                }
            }
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

/* Per-tool tile memo. The agent-history picker draws 20+ icons per frame
 * at vsync rates, and the WCAG ratio needs 6 powf() calls per icon — too
 * much for an answer that only changes when the theme changes. Keyed on
 * the theme pointer (theme structs are statically allocated or owned by
 * the user-themes table for the process lifetime, so pointer identity is
 * a sound invalidator). */
static Color  g_backdrop_cache[CHAT_TOOL_COUNT_];
static bool   g_backdrop_cached[CHAT_TOOL_COUNT_];
static const Theme *g_backdrop_theme = NULL;

static Color compute_backdrop(ChatTool tool, const Theme *t) {
    ChromePalette cp = chrome_palette_for(t);
    f32 white_mix = cp.is_light ? 0.78f : 0.94f;
    Color tile = {
        t->bg.r * (1.0f - white_mix) + white_mix,
        t->bg.g * (1.0f - white_mix) + white_mix,
        t->bg.b * (1.0f - white_mix) + white_mix,
        1.0f,
    };

    /* When the brand tint and tile share luminance the logo disappears
     * (e.g. Claude's rust-orange on a peach theme). Flip to a dark
     * gunmetal tile when the WCAG ratio is too thin. */
    AgentTint tint = agent_icon_tint(tool);
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
    switch (tool) {
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
    case CHAT_TOOL_ZED:         return (AgentTint){0.30f, 0.44f, 0.88f, 1.0f}; /* zed blue      */
    case CHAT_TOOL_COMMANDCODE: return (AgentTint){0.36f, 0.50f, 0.96f, 1.0f}; /* command blue  */
    case CHAT_TOOL_XAI:         return (AgentTint){0.12f, 0.12f, 0.14f, 1.0f}; /* xAI near-black */
    default:                    return (AgentTint){0.55f, 0.55f, 0.60f, 1.0f};
    }
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
                /* Source PNG ships with an opaque white backdrop.
                 * Chroma-key it to transparent.  A previous attempt
                 * with a soft 205-255 linear falloff still left the
                 * anti-aliased ring around the silhouette at 30-70 %
                 * opacity — visible as a halo on the dark sleeping-tab
                 * card.  Switch to a hard cutoff at 230 (near-white
                 * → fully transparent) with a short 200-230 linear
                 * tail that catches the AA edge.  The inner cat
                 * silhouette is black or saturated purple — both
                 * have min(r,g,b) ≪ 200 so they're untouched. */
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

const char *agent_icon_letter(ChatTool tool) {
    switch (tool) {
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
    case CHAT_TOOL_ZED:         return "Z";
    case CHAT_TOOL_COMMANDCODE: return "⌘";
    case CHAT_TOOL_XAI:         return "G";
    default:                    return "?";
    }
}
