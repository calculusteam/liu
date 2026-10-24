/*
 * Liu - configuration + built-in themes
 */
#include "core/config.h"
#include "core/atomic_file.h"
#include "core/json_lines.h"
#include "core/string_utils.h"
#include "core/utf8.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

#ifdef PLATFORM_MACOS
    #include <mach-o/dyld.h>
#elif defined(PLATFORM_LINUX)
    #include <limits.h>
#endif

/* =========================================================================
 * Built-in themes
 * ========================================================================= */
/* Built-in themes define terminal and UI colors. Shared rules derive tab,
 * sidebar, border, and status colors from each theme's background, foreground,
 * and accent; explicit theme values take precedence. Each theme sets ui_accent
 * explicitly because an unset accent falls back to ansi[12]. */

/* primer/primitives dark_dimmed — canvas.default bg, fg.default text, ansi block verbatim */
const Theme THEME_GITHUB_DARK_DIMMED = {
    .name = "GitHub Dark Dimmed",
    .fg        = { 0.678f, 0.729f, 0.780f, 1.00f },
    .bg        = { 0.133f, 0.153f, 0.180f, 1.00f },
    .cursor    = { 0.325f, 0.608f, 0.961f, 1.00f },
    .selection = { 0.255f, 0.518f, 0.894f, 0.40f },
    .ansi = {
        { 0.329f, 0.365f, 0.408f, 1.00f },  /* 0  Black */
        { 0.957f, 0.439f, 0.404f, 1.00f },  /* 1  Red */
        { 0.341f, 0.671f, 0.353f, 1.00f },  /* 2  Green */
        { 0.776f, 0.565f, 0.149f, 1.00f },  /* 3  Yellow */
        { 0.325f, 0.608f, 0.961f, 1.00f },  /* 4  Blue */
        { 0.690f, 0.514f, 0.941f, 1.00f },  /* 5  Magenta */
        { 0.224f, 0.773f, 0.812f, 1.00f },  /* 6  Cyan */
        { 0.565f, 0.616f, 0.671f, 1.00f },  /* 7  White */
        { 0.388f, 0.431f, 0.482f, 1.00f },  /* 8  Bright Black */
        { 1.000f, 0.576f, 0.541f, 1.00f },  /* 9  Bright Red */
        { 0.420f, 0.769f, 0.427f, 1.00f },  /* 10 Bright Green */
        { 0.855f, 0.667f, 0.247f, 1.00f },  /* 11 Bright Yellow */
        { 0.424f, 0.714f, 1.000f, 1.00f },  /* 12 Bright Blue */
        { 0.863f, 0.741f, 0.984f, 1.00f },  /* 13 Bright Magenta */
        { 0.337f, 0.831f, 0.867f, 1.00f },  /* 14 Bright Cyan */
        { 0.804f, 0.851f, 0.898f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.133f, 0.153f, 0.180f, 1.00f },
    .tab_active_bg   = { 0.176f, 0.200f, 0.231f, 1.00f },
    .tab_active_fg   = { 0.804f, 0.851f, 0.898f, 1.00f },
    .tab_inactive_bg = { 0.133f, 0.153f, 0.180f, 1.00f },
    .tab_inactive_fg = { 0.463f, 0.514f, 0.565f, 1.00f },
    .sidebar_bg      = { 0.110f, 0.129f, 0.157f, 1.00f },
    .sidebar_fg      = { 0.678f, 0.729f, 0.780f, 1.00f },
    .sidebar_hover   = { 0.171f, 0.193f, 0.222f, 1.00f },
    .sidebar_active  = { 0.176f, 0.253f, 0.352f, 1.00f },
    .border          = { 0.267f, 0.298f, 0.337f, 1.00f },
    .scrollbar       = { 0.133f, 0.153f, 0.180f, 1.00f },
    .scrollbar_thumb = { 0.277f, 0.494f, 0.766f, 1.00f },
    .status_bg       = { 0.133f, 0.153f, 0.180f, 1.00f },
    .status_fg       = { 0.463f, 0.514f, 0.565f, 1.00f },
    .ui_accent       = { 0.325f, 0.608f, 0.961f, 1.00f },
};

/* Near-black background with dark-gray surfaces and a bright-blue cursor and accent. */
const Theme THEME_PIERRE_DARK = {
    .name = "Pierre Dark",
    .fg        = { 0.980f, 0.980f, 0.980f, 1.00f },
    .bg        = { 0.039f, 0.039f, 0.039f, 1.00f },
    .cursor    = { 0.000f, 0.624f, 1.000f, 1.00f },
    .selection = { 0.000f, 0.624f, 1.000f, 0.30f },
    .ansi = {
        { 0.090f, 0.090f, 0.090f, 1.00f },  /* 0  Black */
        { 1.000f, 0.180f, 0.247f, 1.00f },  /* 1  Red */
        { 0.051f, 0.745f, 0.306f, 1.00f },  /* 2  Green */
        { 1.000f, 0.792f, 0.000f, 1.00f },  /* 3  Yellow */
        { 0.000f, 0.624f, 1.000f, 1.00f },  /* 4  Blue */
        { 0.882f, 0.188f, 0.675f, 1.00f },  /* 5  Magenta */
        { 0.031f, 0.753f, 0.937f, 1.00f },  /* 6  Cyan */
        { 0.737f, 0.737f, 0.737f, 1.00f },  /* 7  White */
        { 0.251f, 0.251f, 0.251f, 1.00f },  /* 8  Bright Black */
        { 1.000f, 0.180f, 0.247f, 1.00f },  /* 9  Bright Red */
        { 0.525f, 0.769f, 0.153f, 1.00f },  /* 10 Bright Green */
        { 1.000f, 0.792f, 0.000f, 1.00f },  /* 11 Bright Yellow */
        { 0.000f, 0.624f, 1.000f, 1.00f },  /* 12 Bright Blue */
        { 0.882f, 0.188f, 0.675f, 1.00f },  /* 13 Bright Magenta */
        { 0.031f, 0.753f, 0.937f, 1.00f },  /* 14 Bright Cyan */
        { 0.980f, 0.980f, 0.980f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.039f, 0.039f, 0.039f, 1.00f },
    .tab_active_bg   = { 0.090f, 0.090f, 0.090f, 1.00f },
    .tab_active_fg   = { 0.980f, 0.980f, 0.980f, 1.00f },
    .tab_inactive_bg = { 0.039f, 0.039f, 0.039f, 1.00f },
    .tab_inactive_fg = { 0.451f, 0.451f, 0.451f, 1.00f },
    .sidebar_bg      = { 0.039f, 0.039f, 0.039f, 1.00f },
    .sidebar_fg      = { 0.639f, 0.639f, 0.639f, 1.00f },
    .sidebar_hover   = { 0.105f, 0.105f, 0.105f, 1.00f },
    .sidebar_active  = { 0.031f, 0.168f, 0.251f, 1.00f },
    .border          = { 0.122f, 0.122f, 0.122f, 1.00f },
    .scrollbar       = { 0.039f, 0.039f, 0.039f, 1.00f },
    .scrollbar_thumb = { 0.010f, 0.477f, 0.760f, 1.00f },
    .status_bg       = { 0.039f, 0.039f, 0.039f, 1.00f },
    .status_fg       = { 0.639f, 0.639f, 0.639f, 1.00f },
    .ui_accent       = { 0.000f, 0.624f, 1.000f, 1.00f },
};

/* Dark charcoal background with a blue cursor and accent. */
const Theme THEME_ONE_DARK_PRO = {
    .name = "One Dark Pro",
    .fg        = { 0.671f, 0.698f, 0.749f, 1.00f },
    .bg        = { 0.157f, 0.173f, 0.204f, 1.00f },
    .cursor    = { 0.322f, 0.545f, 1.000f, 1.00f },
    .selection = { 0.404f, 0.463f, 0.588f, 0.55f },
    .ansi = {
        { 0.247f, 0.267f, 0.318f, 1.00f },  /* 0  Black */
        { 0.878f, 0.333f, 0.380f, 1.00f },  /* 1  Red */
        { 0.549f, 0.761f, 0.396f, 1.00f },  /* 2  Green */
        { 0.820f, 0.561f, 0.322f, 1.00f },  /* 3  Yellow */
        { 0.290f, 0.647f, 0.941f, 1.00f },  /* 4  Blue */
        { 0.757f, 0.384f, 0.871f, 1.00f },  /* 5  Magenta */
        { 0.259f, 0.702f, 0.761f, 1.00f },  /* 6  Cyan */
        { 0.843f, 0.855f, 0.878f, 1.00f },  /* 7  White */
        { 0.310f, 0.337f, 0.400f, 1.00f },  /* 8  Bright Black */
        { 1.000f, 0.380f, 0.431f, 1.00f },  /* 9  Bright Red */
        { 0.647f, 0.878f, 0.459f, 1.00f },  /* 10 Bright Green */
        { 0.941f, 0.643f, 0.365f, 1.00f },  /* 11 Bright Yellow */
        { 0.302f, 0.769f, 1.000f, 1.00f },  /* 12 Bright Blue */
        { 0.871f, 0.451f, 1.000f, 1.00f },  /* 13 Bright Magenta */
        { 0.298f, 0.820f, 0.878f, 1.00f },  /* 14 Bright Cyan */
        { 0.902f, 0.902f, 0.902f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.157f, 0.173f, 0.204f, 1.00f },
    .tab_active_bg   = { 0.157f, 0.173f, 0.204f, 1.00f },
    .tab_active_fg   = { 0.902f, 0.902f, 0.902f, 1.00f },
    .tab_inactive_bg = { 0.157f, 0.173f, 0.204f, 1.00f },
    .tab_inactive_fg = { 0.498f, 0.518f, 0.557f, 1.00f },
    .sidebar_bg      = { 0.129f, 0.145f, 0.169f, 1.00f },
    .sidebar_fg      = { 0.671f, 0.698f, 0.749f, 1.00f },
    .sidebar_hover   = { 0.193f, 0.209f, 0.242f, 1.00f },
    .sidebar_active  = { 0.193f, 0.255f, 0.379f, 1.00f },
    .border          = { 0.243f, 0.267f, 0.322f, 1.00f },
    .scrollbar       = { 0.157f, 0.173f, 0.204f, 1.00f },
    .scrollbar_thumb = { 0.280f, 0.452f, 0.801f, 1.00f },
    .status_bg       = { 0.157f, 0.173f, 0.204f, 1.00f },
    .status_fg       = { 0.616f, 0.647f, 0.706f, 1.00f },
    .ui_accent       = { 0.322f, 0.545f, 1.000f, 1.00f },
};

/* ANSI colors use the theme's COLOR0 through COLOR15 mapping. */
const Theme THEME_DRACULA = {
    .name = "Dracula",
    .fg        = { 0.973f, 0.973f, 0.949f, 1.00f },
    .bg        = { 0.157f, 0.165f, 0.212f, 1.00f },
    .cursor    = { 0.973f, 0.973f, 0.949f, 1.00f },
    .selection = { 0.267f, 0.278f, 0.353f, 0.85f },
    .ansi = {
        { 0.129f, 0.133f, 0.173f, 1.00f },  /* 0  Black */
        { 1.000f, 0.333f, 0.333f, 1.00f },  /* 1  Red */
        { 0.314f, 0.980f, 0.482f, 1.00f },  /* 2  Green */
        { 0.945f, 0.980f, 0.549f, 1.00f },  /* 3  Yellow */
        { 0.741f, 0.576f, 0.976f, 1.00f },  /* 4  Blue */
        { 1.000f, 0.475f, 0.776f, 1.00f },  /* 5  Magenta */
        { 0.545f, 0.914f, 0.992f, 1.00f },  /* 6  Cyan */
        { 0.973f, 0.973f, 0.949f, 1.00f },  /* 7  White */
        { 0.384f, 0.447f, 0.643f, 1.00f },  /* 8  Bright Black */
        { 1.000f, 0.431f, 0.431f, 1.00f },  /* 9  Bright Red */
        { 0.412f, 1.000f, 0.580f, 1.00f },  /* 10 Bright Green */
        { 1.000f, 1.000f, 0.647f, 1.00f },  /* 11 Bright Yellow */
        { 0.839f, 0.675f, 1.000f, 1.00f },  /* 12 Bright Blue */
        { 1.000f, 0.573f, 0.875f, 1.00f },  /* 13 Bright Magenta */
        { 0.643f, 1.000f, 1.000f, 1.00f },  /* 14 Bright Cyan */
        { 1.000f, 1.000f, 1.000f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.157f, 0.165f, 0.212f, 1.00f },
    .tab_active_bg   = { 0.204f, 0.216f, 0.275f, 1.00f },
    .tab_active_fg   = { 1.000f, 1.000f, 1.000f, 1.00f },
    .tab_inactive_bg = { 0.157f, 0.165f, 0.212f, 1.00f },
    .tab_inactive_fg = { 0.384f, 0.447f, 0.643f, 1.00f },
    .sidebar_bg      = { 0.129f, 0.133f, 0.173f, 1.00f },
    .sidebar_fg      = { 0.973f, 0.973f, 0.949f, 1.00f },
    .sidebar_hover   = { 0.214f, 0.221f, 0.263f, 1.00f },
    .sidebar_active  = { 0.285f, 0.255f, 0.380f, 1.00f },
    .border          = { 0.259f, 0.267f, 0.314f, 1.00f },
    .scrollbar       = { 0.157f, 0.165f, 0.212f, 1.00f },
    .scrollbar_thumb = { 0.595f, 0.474f, 0.785f, 1.00f },
    .status_bg       = { 0.157f, 0.165f, 0.212f, 1.00f },
    .status_fg       = { 0.384f, 0.447f, 0.643f, 1.00f },
    .ui_accent       = { 0.741f, 0.576f, 0.976f, 1.00f },
};

/* Dracula Soft uses the Dracula ANSI palette with saturation reduced by 20 HSL points. */
const Theme THEME_DRACULA_SOFT = {
    .name = "Dracula Soft",
    .fg        = { 0.973f, 0.973f, 0.949f, 1.00f },
    .bg        = { 0.157f, 0.165f, 0.212f, 1.00f },
    .cursor    = { 0.973f, 0.973f, 0.949f, 1.00f },
    .selection = { 0.267f, 0.278f, 0.353f, 0.85f },
    .ansi = {
        { 0.151f, 0.151f, 0.151f, 1.00f },  /* 0  Black */
        { 0.933f, 0.400f, 0.400f, 1.00f },  /* 1  Red */
        { 0.384f, 0.910f, 0.517f, 1.00f },  /* 2  Green */
        { 0.906f, 0.933f, 0.596f, 1.00f },  /* 3  Yellow */
        { 0.749f, 0.621f, 0.932f, 1.00f },  /* 4  Blue */
        { 0.947f, 0.527f, 0.769f, 1.00f },  /* 5  Magenta */
        { 0.591f, 0.884f, 0.946f, 1.00f },  /* 6  Cyan */
        { 0.965f, 0.965f, 0.957f, 1.00f },  /* 7  White */
        { 0.482f, 0.497f, 0.546f, 1.00f },  /* 8  Bright Black */
        { 0.943f, 0.488f, 0.488f, 1.00f },  /* 9  Bright Red */
        { 0.471f, 0.941f, 0.605f, 1.00f },  /* 10 Bright Green */
        { 0.965f, 0.965f, 0.682f, 1.00f },  /* 11 Bright Yellow */
        { 0.839f, 0.707f, 0.967f, 1.00f },  /* 12 Bright Blue */
        { 0.957f, 0.615f, 0.857f, 1.00f },  /* 13 Bright Magenta */
        { 0.679f, 0.964f, 0.964f, 1.00f },  /* 14 Bright Cyan */
        { 1.000f, 1.000f, 1.000f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.157f, 0.165f, 0.212f, 1.00f },
    .tab_active_bg   = { 0.204f, 0.216f, 0.275f, 1.00f },
    .tab_active_fg   = { 1.000f, 1.000f, 1.000f, 1.00f },
    .tab_inactive_bg = { 0.157f, 0.165f, 0.212f, 1.00f },
    .tab_inactive_fg = { 0.384f, 0.447f, 0.643f, 1.00f },
    .sidebar_bg      = { 0.129f, 0.133f, 0.173f, 1.00f },
    .sidebar_fg      = { 0.973f, 0.973f, 0.949f, 1.00f },
    .sidebar_hover   = { 0.214f, 0.221f, 0.263f, 1.00f },
    .sidebar_active  = { 0.287f, 0.265f, 0.370f, 1.00f },
    .border          = { 0.259f, 0.267f, 0.314f, 1.00f },
    .scrollbar       = { 0.157f, 0.165f, 0.212f, 1.00f },
    .scrollbar_thumb = { 0.601f, 0.507f, 0.752f, 1.00f },
    .status_bg       = { 0.157f, 0.165f, 0.212f, 1.00f },
    .status_fg       = { 0.384f, 0.447f, 0.643f, 1.00f },
    .ui_accent       = { 0.749f, 0.621f, 0.932f, 1.00f },
};

/* Mocha palette with mauve as the UI accent. */
const Theme THEME_CATPPUCCIN_MOCHA = {
    .name = "Catppuccin Mocha",
    .fg        = { 0.804f, 0.839f, 0.957f, 1.00f },
    .bg        = { 0.118f, 0.118f, 0.180f, 1.00f },
    .cursor    = { 0.961f, 0.878f, 0.863f, 1.00f },
    .selection = { 0.345f, 0.357f, 0.439f, 0.70f },
    .ansi = {
        { 0.271f, 0.278f, 0.353f, 1.00f },  /* 0  Black */
        { 0.953f, 0.545f, 0.659f, 1.00f },  /* 1  Red */
        { 0.651f, 0.890f, 0.631f, 1.00f },  /* 2  Green */
        { 0.976f, 0.886f, 0.686f, 1.00f },  /* 3  Yellow */
        { 0.537f, 0.706f, 0.980f, 1.00f },  /* 4  Blue */
        { 0.961f, 0.761f, 0.906f, 1.00f },  /* 5  Magenta */
        { 0.580f, 0.886f, 0.835f, 1.00f },  /* 6  Cyan */
        { 0.729f, 0.761f, 0.871f, 1.00f },  /* 7  White */
        { 0.345f, 0.357f, 0.439f, 1.00f },  /* 8  Bright Black */
        { 0.953f, 0.545f, 0.659f, 1.00f },  /* 9  Bright Red */
        { 0.651f, 0.890f, 0.631f, 1.00f },  /* 10 Bright Green */
        { 0.976f, 0.886f, 0.686f, 1.00f },  /* 11 Bright Yellow */
        { 0.537f, 0.706f, 0.980f, 1.00f },  /* 12 Bright Blue */
        { 0.961f, 0.761f, 0.906f, 1.00f },  /* 13 Bright Magenta */
        { 0.580f, 0.886f, 0.835f, 1.00f },  /* 14 Bright Cyan */
        { 0.651f, 0.678f, 0.784f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.118f, 0.118f, 0.180f, 1.00f },
    .tab_active_bg   = { 0.192f, 0.196f, 0.267f, 1.00f },
    .tab_active_fg   = { 0.651f, 0.678f, 0.784f, 1.00f },
    .tab_inactive_bg = { 0.118f, 0.118f, 0.180f, 1.00f },
    .tab_inactive_fg = { 0.498f, 0.518f, 0.612f, 1.00f },
    .sidebar_bg      = { 0.094f, 0.094f, 0.145f, 1.00f },
    .sidebar_fg      = { 0.804f, 0.839f, 0.957f, 1.00f },
    .sidebar_hover   = { 0.166f, 0.168f, 0.235f, 1.00f },
    .sidebar_active  = { 0.267f, 0.235f, 0.354f, 1.00f },
    .border          = { 0.192f, 0.196f, 0.267f, 1.00f },
    .scrollbar       = { 0.118f, 0.118f, 0.180f, 1.00f },
    .scrollbar_thumb = { 0.626f, 0.518f, 0.772f, 1.00f },
    .status_bg       = { 0.118f, 0.118f, 0.180f, 1.00f },
    .status_fg       = { 0.651f, 0.678f, 0.784f, 1.00f },
    .ui_accent       = { 0.796f, 0.651f, 0.969f, 1.00f },
};

/* arcticicestudio/nord — polar-night bg ramp, frost accent */
const Theme THEME_NORD = {
    .name = "Nord",
    .fg        = { 0.847f, 0.871f, 0.914f, 1.00f },
    .bg        = { 0.180f, 0.204f, 0.251f, 1.00f },
    .cursor    = { 0.847f, 0.871f, 0.914f, 1.00f },
    .selection = { 0.263f, 0.298f, 0.369f, 0.80f },
    .ansi = {
        { 0.231f, 0.259f, 0.322f, 1.00f },  /* 0  Black */
        { 0.749f, 0.380f, 0.416f, 1.00f },  /* 1  Red */
        { 0.639f, 0.745f, 0.549f, 1.00f },  /* 2  Green */
        { 0.922f, 0.796f, 0.545f, 1.00f },  /* 3  Yellow */
        { 0.506f, 0.631f, 0.757f, 1.00f },  /* 4  Blue */
        { 0.706f, 0.557f, 0.678f, 1.00f },  /* 5  Magenta */
        { 0.533f, 0.753f, 0.816f, 1.00f },  /* 6  Cyan */
        { 0.898f, 0.914f, 0.941f, 1.00f },  /* 7  White */
        { 0.298f, 0.337f, 0.416f, 1.00f },  /* 8  Bright Black */
        { 0.749f, 0.380f, 0.416f, 1.00f },  /* 9  Bright Red */
        { 0.639f, 0.745f, 0.549f, 1.00f },  /* 10 Bright Green */
        { 0.922f, 0.796f, 0.545f, 1.00f },  /* 11 Bright Yellow */
        { 0.506f, 0.631f, 0.757f, 1.00f },  /* 12 Bright Blue */
        { 0.706f, 0.557f, 0.678f, 1.00f },  /* 13 Bright Magenta */
        { 0.561f, 0.737f, 0.733f, 1.00f },  /* 14 Bright Cyan */
        { 0.925f, 0.937f, 0.957f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.180f, 0.204f, 0.251f, 1.00f },
    .tab_active_bg   = { 0.231f, 0.259f, 0.322f, 1.00f },
    .tab_active_fg   = { 0.925f, 0.937f, 0.957f, 1.00f },
    .tab_inactive_bg = { 0.180f, 0.204f, 0.251f, 1.00f },
    .tab_inactive_fg = { 0.482f, 0.533f, 0.631f, 1.00f },
    .sidebar_bg      = { 0.180f, 0.204f, 0.251f, 1.00f },
    .sidebar_fg      = { 0.847f, 0.871f, 0.914f, 1.00f },
    .sidebar_hover   = { 0.227f, 0.251f, 0.297f, 1.00f },
    .sidebar_active  = { 0.258f, 0.325f, 0.375f, 1.00f },
    .border          = { 0.231f, 0.259f, 0.322f, 1.00f },
    .scrollbar       = { 0.180f, 0.204f, 0.251f, 1.00f },
    .scrollbar_thumb = { 0.445f, 0.616f, 0.675f, 1.00f },
    .status_bg       = { 0.180f, 0.204f, 0.251f, 1.00f },
    .status_fg       = { 0.545f, 0.596f, 0.682f, 1.00f },
    .ui_accent       = { 0.533f, 0.753f, 0.816f, 1.00f },
};

/* Solarized uses base03 as the background and maps the bright row to the base01/base00/base0 gray
 * ramp. ANSI slot 8 is also base03, so bright-black may match the background. */
const Theme THEME_SOLARIZED_DARK = {
    .name = "Solarized Dark",
    .fg        = { 0.514f, 0.580f, 0.588f, 1.00f },
    .bg        = { 0.000f, 0.169f, 0.212f, 1.00f },
    .cursor    = { 0.576f, 0.631f, 0.631f, 1.00f },
    .selection = { 0.027f, 0.212f, 0.259f, 0.85f },
    .ansi = {
        { 0.027f, 0.212f, 0.259f, 1.00f },  /* 0  Black */
        { 0.863f, 0.196f, 0.184f, 1.00f },  /* 1  Red */
        { 0.522f, 0.600f, 0.000f, 1.00f },  /* 2  Green */
        { 0.710f, 0.537f, 0.000f, 1.00f },  /* 3  Yellow */
        { 0.149f, 0.545f, 0.824f, 1.00f },  /* 4  Blue */
        { 0.827f, 0.212f, 0.510f, 1.00f },  /* 5  Magenta */
        { 0.165f, 0.631f, 0.596f, 1.00f },  /* 6  Cyan */
        { 0.933f, 0.910f, 0.835f, 1.00f },  /* 7  White */
        { 0.000f, 0.169f, 0.212f, 1.00f },  /* 8  Bright Black */
        { 0.796f, 0.294f, 0.086f, 1.00f },  /* 9  Bright Red */
        { 0.345f, 0.431f, 0.459f, 1.00f },  /* 10 Bright Green */
        { 0.396f, 0.482f, 0.514f, 1.00f },  /* 11 Bright Yellow */
        { 0.514f, 0.580f, 0.588f, 1.00f },  /* 12 Bright Blue */
        { 0.424f, 0.443f, 0.769f, 1.00f },  /* 13 Bright Magenta */
        { 0.576f, 0.631f, 0.631f, 1.00f },  /* 14 Bright Cyan */
        { 0.992f, 0.965f, 0.890f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.000f, 0.169f, 0.212f, 1.00f },
    .tab_active_bg   = { 0.027f, 0.212f, 0.259f, 1.00f },
    .tab_active_fg   = { 0.992f, 0.965f, 0.890f, 1.00f },
    .tab_inactive_bg = { 0.000f, 0.169f, 0.212f, 1.00f },
    .tab_inactive_fg = { 0.345f, 0.431f, 0.459f, 1.00f },
    .sidebar_bg      = { 0.000f, 0.169f, 0.212f, 1.00f },
    .sidebar_fg      = { 0.576f, 0.631f, 0.631f, 1.00f },
    .sidebar_hover   = { 0.036f, 0.197f, 0.238f, 1.00f },
    .sidebar_active  = { 0.033f, 0.251f, 0.346f, 1.00f },
    .border          = { 0.027f, 0.212f, 0.259f, 1.00f },
    .scrollbar       = { 0.000f, 0.169f, 0.212f, 1.00f },
    .scrollbar_thumb = { 0.112f, 0.451f, 0.671f, 1.00f },
    .status_bg       = { 0.000f, 0.169f, 0.212f, 1.00f },
    .status_fg       = { 0.345f, 0.431f, 0.459f, 1.00f },
    .ui_accent       = { 0.149f, 0.545f, 0.824f, 1.00f },
};

/* Muted, low-chroma colors on a #121212 background. */
const Theme THEME_VITESSE_DARK = {
    .name = "Vitesse Dark",
    .fg        = { 0.859f, 0.843f, 0.792f, 1.00f },
    .bg        = { 0.071f, 0.071f, 0.071f, 1.00f },
    .cursor    = { 0.302f, 0.576f, 0.459f, 1.00f },
    .selection = { 0.933f, 0.933f, 0.933f, 0.14f },
    .ansi = {
        { 0.224f, 0.227f, 0.204f, 1.00f },  /* 0  Black */
        { 0.796f, 0.463f, 0.463f, 1.00f },  /* 1  Red */
        { 0.302f, 0.576f, 0.459f, 1.00f },  /* 2  Green */
        { 0.902f, 0.800f, 0.467f, 1.00f },  /* 3  Yellow */
        { 0.388f, 0.580f, 0.749f, 1.00f },  /* 4  Blue */
        { 0.851f, 0.451f, 0.624f, 1.00f },  /* 5  Magenta */
        { 0.369f, 0.667f, 0.710f, 1.00f },  /* 6  Cyan */
        { 0.859f, 0.843f, 0.792f, 1.00f },  /* 7  White */
        { 0.467f, 0.467f, 0.467f, 1.00f },  /* 8  Bright Black */
        { 0.796f, 0.463f, 0.463f, 1.00f },  /* 9  Bright Red */
        { 0.302f, 0.576f, 0.459f, 1.00f },  /* 10 Bright Green */
        { 0.902f, 0.800f, 0.467f, 1.00f },  /* 11 Bright Yellow */
        { 0.388f, 0.580f, 0.749f, 1.00f },  /* 12 Bright Blue */
        { 0.851f, 0.451f, 0.624f, 1.00f },  /* 13 Bright Magenta */
        { 0.369f, 0.667f, 0.710f, 1.00f },  /* 14 Bright Cyan */
        { 1.000f, 1.000f, 1.000f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.071f, 0.071f, 0.071f, 1.00f },
    .tab_active_bg   = { 0.094f, 0.094f, 0.094f, 1.00f },
    .tab_active_fg   = { 1.000f, 1.000f, 1.000f, 1.00f },
    .tab_inactive_bg = { 0.071f, 0.071f, 0.071f, 1.00f },
    .tab_inactive_fg = { 0.459f, 0.522f, 0.459f, 1.00f },
    .sidebar_bg      = { 0.071f, 0.071f, 0.071f, 1.00f },
    .sidebar_fg      = { 0.749f, 0.729f, 0.667f, 1.00f },
    .sidebar_hover   = { 0.126f, 0.125f, 0.121f, 1.00f },
    .sidebar_active  = { 0.121f, 0.182f, 0.156f, 1.00f },
    .border          = { 0.098f, 0.098f, 0.098f, 1.00f },
    .scrollbar       = { 0.071f, 0.071f, 0.071f, 1.00f },
    .scrollbar_thumb = { 0.244f, 0.450f, 0.362f, 1.00f },
    .status_bg       = { 0.071f, 0.071f, 0.071f, 1.00f },
    .status_fg       = { 0.545f, 0.529f, 0.471f, 1.00f },
    .ui_accent       = { 0.302f, 0.576f, 0.459f, 1.00f },
};

/* Near-monochrome palette with distinct accent colors. */
const Theme THEME_MIN_DARK = {
    .name = "Min Dark",
    .fg        = { 0.733f, 0.733f, 0.733f, 1.00f },
    .bg        = { 0.122f, 0.122f, 0.122f, 1.00f },
    .cursor    = { 0.475f, 0.722f, 1.000f, 1.00f },
    .selection = { 0.227f, 0.227f, 0.227f, 0.85f },
    .ansi = {
        { 0.165f, 0.165f, 0.165f, 1.00f },  /* 0  Black */
        { 0.976f, 0.459f, 0.514f, 1.00f },  /* 1  Red */
        { 0.522f, 0.910f, 0.616f, 1.00f },  /* 2  Green */
        { 1.000f, 0.671f, 0.439f, 1.00f },  /* 3  Yellow */
        { 0.475f, 0.722f, 1.000f, 1.00f },  /* 4  Blue */
        { 0.702f, 0.573f, 0.941f, 1.00f },  /* 5  Magenta */
        { 0.616f, 0.694f, 0.773f, 1.00f },  /* 6  Cyan */
        { 0.733f, 0.733f, 0.733f, 1.00f },  /* 7  White */
        { 0.361f, 0.361f, 0.361f, 1.00f },  /* 8  Bright Black */
        { 1.000f, 0.478f, 0.518f, 1.00f },  /* 9  Bright Red */
        { 0.635f, 0.949f, 0.714f, 1.00f },  /* 10 Bright Green */
        { 1.000f, 0.753f, 0.541f, 1.00f },  /* 11 Bright Yellow */
        { 0.620f, 0.796f, 1.000f, 1.00f },  /* 12 Bright Blue */
        { 0.784f, 0.682f, 0.984f, 1.00f },  /* 13 Bright Magenta */
        { 0.706f, 0.776f, 0.847f, 1.00f },  /* 14 Bright Cyan */
        { 0.980f, 0.980f, 0.980f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.122f, 0.122f, 0.122f, 1.00f },
    .tab_active_bg   = { 0.149f, 0.149f, 0.149f, 1.00f },
    .tab_active_fg   = { 0.980f, 0.980f, 0.980f, 1.00f },
    .tab_inactive_bg = { 0.122f, 0.122f, 0.122f, 1.00f },
    .tab_inactive_fg = { 0.447f, 0.447f, 0.447f, 1.00f },
    .sidebar_bg      = { 0.102f, 0.102f, 0.102f, 1.00f },
    .sidebar_fg      = { 0.620f, 0.620f, 0.620f, 1.00f },
    .sidebar_hover   = { 0.164f, 0.164f, 0.164f, 1.00f },
    .sidebar_active  = { 0.199f, 0.254f, 0.315f, 1.00f },
    .border          = { 0.169f, 0.169f, 0.169f, 1.00f },
    .scrollbar       = { 0.122f, 0.122f, 0.122f, 1.00f },
    .scrollbar_thumb = { 0.386f, 0.572f, 0.780f, 1.00f },
    .status_bg       = { 0.122f, 0.122f, 0.122f, 1.00f },
    .status_fg       = { 0.494f, 0.494f, 0.494f, 1.00f },
    .ui_accent       = { 0.475f, 0.722f, 1.000f, 1.00f },
};

/* Dark canvas with pink, green, and cyan accents. */
const Theme THEME_MONOKAI = {
    .name = "Monokai",
    .fg        = { 0.973f, 0.973f, 0.949f, 1.00f },
    .bg        = { 0.153f, 0.157f, 0.133f, 1.00f },
    .cursor    = { 0.973f, 0.973f, 0.941f, 1.00f },
    .selection = { 0.286f, 0.282f, 0.243f, 0.85f },
    .ansi = {
        { 0.153f, 0.157f, 0.133f, 1.00f },  /* 0  Black */
        { 0.976f, 0.149f, 0.447f, 1.00f },  /* 1  Red */
        { 0.651f, 0.886f, 0.180f, 1.00f },  /* 2  Green */
        { 0.957f, 0.749f, 0.459f, 1.00f },  /* 3  Yellow */
        { 0.400f, 0.851f, 0.937f, 1.00f },  /* 4  Blue */
        { 0.682f, 0.506f, 1.000f, 1.00f },  /* 5  Magenta */
        { 0.631f, 0.937f, 0.894f, 1.00f },  /* 6  Cyan */
        { 0.973f, 0.973f, 0.949f, 1.00f },  /* 7  White */
        { 0.459f, 0.443f, 0.369f, 1.00f },  /* 8  Bright Black */
        { 0.976f, 0.149f, 0.447f, 1.00f },  /* 9  Bright Red */
        { 0.651f, 0.886f, 0.180f, 1.00f },  /* 10 Bright Green */
        { 0.957f, 0.749f, 0.459f, 1.00f },  /* 11 Bright Yellow */
        { 0.400f, 0.851f, 0.937f, 1.00f },  /* 12 Bright Blue */
        { 0.682f, 0.506f, 1.000f, 1.00f },  /* 13 Bright Magenta */
        { 0.631f, 0.937f, 0.894f, 1.00f },  /* 14 Bright Cyan */
        { 0.976f, 0.973f, 0.961f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.153f, 0.157f, 0.133f, 1.00f },
    .tab_active_bg   = { 0.204f, 0.208f, 0.184f, 1.00f },
    .tab_active_fg   = { 0.976f, 0.973f, 0.961f, 1.00f },
    .tab_inactive_bg = { 0.153f, 0.157f, 0.133f, 1.00f },
    .tab_inactive_fg = { 0.459f, 0.443f, 0.369f, 1.00f },
    .sidebar_bg      = { 0.118f, 0.122f, 0.110f, 1.00f },
    .sidebar_fg      = { 0.973f, 0.973f, 0.949f, 1.00f },
    .sidebar_hover   = { 0.210f, 0.214f, 0.190f, 1.00f },
    .sidebar_active  = { 0.263f, 0.317f, 0.144f, 1.00f },
    .border          = { 0.231f, 0.235f, 0.208f, 1.00f },
    .scrollbar       = { 0.153f, 0.157f, 0.133f, 1.00f },
    .scrollbar_thumb = { 0.526f, 0.704f, 0.169f, 1.00f },
    .status_bg       = { 0.153f, 0.157f, 0.133f, 1.00f },
    .status_fg       = { 0.647f, 0.624f, 0.522f, 1.00f },
    .ui_accent       = { 0.651f, 0.886f, 0.180f, 1.00f },
};

/* The ANSI mapping uses teal (#73daca) for green and repeats normal colors in the bright slots. */
const Theme THEME_TOKYO_NIGHT = {
    .name = "Tokyo Night",
    .fg        = { 0.753f, 0.792f, 0.961f, 1.00f },
    .bg        = { 0.102f, 0.106f, 0.149f, 1.00f },
    .cursor    = { 0.753f, 0.792f, 0.961f, 1.00f },
    .selection = { 0.157f, 0.204f, 0.341f, 0.95f },
    .ansi = {
        { 0.082f, 0.086f, 0.118f, 1.00f },  /* 0  Black */
        { 0.969f, 0.463f, 0.557f, 1.00f },  /* 1  Red */
        { 0.620f, 0.808f, 0.416f, 1.00f },  /* 2  Green */
        { 0.878f, 0.686f, 0.408f, 1.00f },  /* 3  Yellow */
        { 0.478f, 0.635f, 0.969f, 1.00f },  /* 4  Blue */
        { 0.733f, 0.604f, 0.969f, 1.00f },  /* 5  Magenta */
        { 0.490f, 0.812f, 1.000f, 1.00f },  /* 6  Cyan */
        { 0.663f, 0.694f, 0.839f, 1.00f },  /* 7  White */
        { 0.255f, 0.282f, 0.408f, 1.00f },  /* 8  Bright Black */
        { 1.000f, 0.537f, 0.616f, 1.00f },  /* 9  Bright Red */
        { 0.624f, 0.878f, 0.267f, 1.00f },  /* 10 Bright Green */
        { 0.980f, 0.729f, 0.290f, 1.00f },  /* 11 Bright Yellow */
        { 0.553f, 0.690f, 1.000f, 1.00f },  /* 12 Bright Blue */
        { 0.780f, 0.663f, 1.000f, 1.00f },  /* 13 Bright Magenta */
        { 0.643f, 0.855f, 1.000f, 1.00f },  /* 14 Bright Cyan */
        { 0.753f, 0.792f, 0.961f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.102f, 0.106f, 0.149f, 1.00f },
    .tab_active_bg   = { 0.125f, 0.137f, 0.188f, 1.00f },
    .tab_active_fg   = { 0.753f, 0.792f, 0.961f, 1.00f },
    .tab_inactive_bg = { 0.102f, 0.106f, 0.149f, 1.00f },
    .tab_inactive_fg = { 0.471f, 0.486f, 0.600f, 1.00f },
    .sidebar_bg      = { 0.086f, 0.086f, 0.118f, 1.00f },
    .sidebar_fg      = { 0.663f, 0.694f, 0.839f, 1.00f },
    .sidebar_hover   = { 0.148f, 0.154f, 0.206f, 1.00f },
    .sidebar_active  = { 0.185f, 0.222f, 0.329f, 1.00f },
    .border          = { 0.063f, 0.063f, 0.078f, 1.00f },
    .scrollbar       = { 0.102f, 0.106f, 0.149f, 1.00f },
    .scrollbar_thumb = { 0.384f, 0.503f, 0.764f, 1.00f },
    .status_bg       = { 0.102f, 0.106f, 0.149f, 1.00f },
    .status_fg       = { 0.471f, 0.486f, 0.600f, 1.00f },
    .ui_accent       = { 0.478f, 0.635f, 0.969f, 1.00f },
};

/* sdras/night-owl — deep navy canvas, its terminal.ansi block verbatim */
const Theme THEME_NIGHT_OWL = {
    .name = "Night Owl",
    .fg        = { 0.839f, 0.871f, 0.922f, 1.00f },
    .bg        = { 0.004f, 0.086f, 0.153f, 1.00f },
    .cursor    = { 0.502f, 0.643f, 0.761f, 1.00f },
    .selection = { 0.114f, 0.231f, 0.325f, 0.90f },
    .ansi = {
        { 0.004f, 0.086f, 0.153f, 1.00f },  /* 0  Black */
        { 0.937f, 0.325f, 0.314f, 1.00f },  /* 1  Red */
        { 0.133f, 0.855f, 0.431f, 1.00f },  /* 2  Green */
        { 0.773f, 0.894f, 0.471f, 1.00f },  /* 3  Yellow */
        { 0.510f, 0.667f, 1.000f, 1.00f },  /* 4  Blue */
        { 0.780f, 0.573f, 0.918f, 1.00f },  /* 5  Magenta */
        { 0.129f, 0.780f, 0.659f, 1.00f },  /* 6  Cyan */
        { 1.000f, 1.000f, 1.000f, 1.00f },  /* 7  White */
        { 0.341f, 0.337f, 0.337f, 1.00f },  /* 8  Bright Black */
        { 0.937f, 0.325f, 0.314f, 1.00f },  /* 9  Bright Red */
        { 0.133f, 0.855f, 0.431f, 1.00f },  /* 10 Bright Green */
        { 1.000f, 0.922f, 0.584f, 1.00f },  /* 11 Bright Yellow */
        { 0.510f, 0.667f, 1.000f, 1.00f },  /* 12 Bright Blue */
        { 0.780f, 0.573f, 0.918f, 1.00f },  /* 13 Bright Magenta */
        { 0.498f, 0.859f, 0.792f, 1.00f },  /* 14 Bright Cyan */
        { 1.000f, 1.000f, 1.000f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.004f, 0.086f, 0.153f, 1.00f },
    .tab_active_bg   = { 0.043f, 0.161f, 0.259f, 1.00f },
    .tab_active_fg   = { 1.000f, 1.000f, 1.000f, 1.00f },
    .tab_inactive_bg = { 0.004f, 0.086f, 0.153f, 1.00f },
    .tab_inactive_fg = { 0.373f, 0.494f, 0.592f, 1.00f },
    .sidebar_bg      = { 0.004f, 0.086f, 0.153f, 1.00f },
    .sidebar_fg      = { 0.537f, 0.643f, 0.733f, 1.00f },
    .sidebar_hover   = { 0.062f, 0.141f, 0.207f, 1.00f },
    .sidebar_active  = { 0.115f, 0.214f, 0.339f, 1.00f },
    .border          = { 0.071f, 0.176f, 0.259f, 1.00f },
    .scrollbar       = { 0.004f, 0.086f, 0.153f, 1.00f },
    .scrollbar_thumb = { 0.383f, 0.522f, 0.788f, 1.00f },
    .status_bg       = { 0.004f, 0.086f, 0.153f, 1.00f },
    .status_fg       = { 0.373f, 0.494f, 0.592f, 1.00f },
    .ui_accent       = { 0.510f, 0.667f, 1.000f, 1.00f },
};

/* The ANSI mapping uses pine (teal) for green and rose for cyan, and repeats normal colors in the
 * bright slots. */
const Theme THEME_ROSE_PINE = {
    .name = "Rosé Pine",
    .fg        = { 0.878f, 0.871f, 0.957f, 1.00f },
    .bg        = { 0.098f, 0.090f, 0.141f, 1.00f },
    .cursor    = { 0.322f, 0.310f, 0.404f, 1.00f },
    .selection = { 0.251f, 0.239f, 0.322f, 0.95f },
    .ansi = {
        { 0.149f, 0.137f, 0.227f, 1.00f },  /* 0  Black */
        { 0.922f, 0.435f, 0.573f, 1.00f },  /* 1  Red */
        { 0.192f, 0.455f, 0.561f, 1.00f },  /* 2  Green */
        { 0.965f, 0.757f, 0.467f, 1.00f },  /* 3  Yellow */
        { 0.612f, 0.812f, 0.847f, 1.00f },  /* 4  Blue */
        { 0.769f, 0.655f, 0.906f, 1.00f },  /* 5  Magenta */
        { 0.922f, 0.737f, 0.729f, 1.00f },  /* 6  Cyan */
        { 0.878f, 0.871f, 0.957f, 1.00f },  /* 7  White */
        { 0.431f, 0.416f, 0.525f, 1.00f },  /* 8  Bright Black */
        { 0.922f, 0.435f, 0.573f, 1.00f },  /* 9  Bright Red */
        { 0.192f, 0.455f, 0.561f, 1.00f },  /* 10 Bright Green */
        { 0.965f, 0.757f, 0.467f, 1.00f },  /* 11 Bright Yellow */
        { 0.612f, 0.812f, 0.847f, 1.00f },  /* 12 Bright Blue */
        { 0.769f, 0.655f, 0.906f, 1.00f },  /* 13 Bright Magenta */
        { 0.922f, 0.737f, 0.729f, 1.00f },  /* 14 Bright Cyan */
        { 0.878f, 0.871f, 0.957f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.098f, 0.090f, 0.141f, 1.00f },
    .tab_active_bg   = { 0.149f, 0.137f, 0.227f, 1.00f },
    .tab_active_fg   = { 0.878f, 0.871f, 0.957f, 1.00f },
    .tab_inactive_bg = { 0.098f, 0.090f, 0.141f, 1.00f },
    .tab_inactive_fg = { 0.565f, 0.549f, 0.667f, 1.00f },
    .sidebar_bg      = { 0.098f, 0.090f, 0.141f, 1.00f },
    .sidebar_fg      = { 0.565f, 0.549f, 0.667f, 1.00f },
    .sidebar_hover   = { 0.153f, 0.145f, 0.198f, 1.00f },
    .sidebar_active  = { 0.246f, 0.214f, 0.309f, 1.00f },
    .border          = { 0.149f, 0.137f, 0.227f, 1.00f },
    .scrollbar       = { 0.098f, 0.090f, 0.141f, 1.00f },
    .scrollbar_thumb = { 0.601f, 0.514f, 0.715f, 1.00f },
    .status_bg       = { 0.098f, 0.090f, 0.141f, 1.00f },
    .status_fg       = { 0.565f, 0.549f, 0.667f, 1.00f },
    .ui_accent       = { 0.769f, 0.655f, 0.906f, 1.00f },
};

/* Dark navy background with an amber cursor and accent. */
const Theme THEME_AYU_DARK = {
    .name = "Ayu Dark",
    .fg        = { 0.749f, 0.741f, 0.714f, 1.00f },
    .bg        = { 0.051f, 0.063f, 0.090f, 1.00f },
    .cursor    = { 0.902f, 0.706f, 0.314f, 1.00f },
    .selection = { 0.200f, 0.533f, 1.000f, 0.35f },
    .ansi = {
        { 0.106f, 0.122f, 0.161f, 1.00f },  /* 0  Black */
        { 0.941f, 0.420f, 0.451f, 1.00f },  /* 1  Red */
        { 0.439f, 0.749f, 0.337f, 1.00f },  /* 2  Green */
        { 0.992f, 0.690f, 0.298f, 1.00f },  /* 3  Yellow */
        { 0.310f, 0.749f, 1.000f, 1.00f },  /* 4  Blue */
        { 0.816f, 0.631f, 1.000f, 1.00f },  /* 5  Magenta */
        { 0.576f, 0.886f, 0.784f, 1.00f },  /* 6  Cyan */
        { 0.780f, 0.780f, 0.780f, 1.00f },  /* 7  White */
        { 0.408f, 0.408f, 0.408f, 1.00f },  /* 8  Bright Black */
        { 0.941f, 0.443f, 0.471f, 1.00f },  /* 9  Bright Red */
        { 0.667f, 0.851f, 0.298f, 1.00f },  /* 10 Bright Green */
        { 1.000f, 0.706f, 0.329f, 1.00f },  /* 11 Bright Yellow */
        { 0.349f, 0.761f, 1.000f, 1.00f },  /* 12 Bright Blue */
        { 0.824f, 0.651f, 1.000f, 1.00f },  /* 13 Bright Magenta */
        { 0.584f, 0.902f, 0.796f, 1.00f },  /* 14 Bright Cyan */
        { 1.000f, 1.000f, 1.000f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.051f, 0.063f, 0.090f, 1.00f },
    .tab_active_bg   = { 0.063f, 0.078f, 0.110f, 1.00f },
    .tab_active_fg   = { 1.000f, 1.000f, 1.000f, 1.00f },
    .tab_inactive_bg = { 0.051f, 0.063f, 0.090f, 1.00f },
    .tab_inactive_fg = { 0.353f, 0.388f, 0.471f, 1.00f },
    .sidebar_bg      = { 0.051f, 0.063f, 0.090f, 1.00f },
    .sidebar_fg      = { 0.749f, 0.741f, 0.714f, 1.00f },
    .sidebar_hover   = { 0.100f, 0.110f, 0.134f, 1.00f },
    .sidebar_active  = { 0.238f, 0.204f, 0.139f, 1.00f },
    .border          = { 0.106f, 0.122f, 0.161f, 1.00f },
    .scrollbar       = { 0.051f, 0.063f, 0.090f, 1.00f },
    .scrollbar_thumb = { 0.689f, 0.545f, 0.258f, 1.00f },
    .status_bg       = { 0.051f, 0.063f, 0.090f, 1.00f },
    .status_fg       = { 0.353f, 0.388f, 0.471f, 1.00f },
    .ui_accent       = { 0.902f, 0.706f, 0.314f, 1.00f },
};

/* Slack's documented dark surfaces (#1a1d21 canvas, #22252a rail, #2c2d30 hover) with its brand palette in the ansi slots — Slack ships no terminal colours of its own */
const Theme THEME_SLACK_DARK = {
    .name = "Slack Dark",
    .fg        = { 0.820f, 0.824f, 0.827f, 1.00f },
    .bg        = { 0.102f, 0.114f, 0.129f, 1.00f },
    .cursor    = { 0.114f, 0.608f, 0.820f, 1.00f },
    .selection = { 0.114f, 0.608f, 0.820f, 0.32f },
    .ansi = {
        { 0.133f, 0.145f, 0.165f, 1.00f },  /* 0  Black */
        { 0.878f, 0.118f, 0.353f, 1.00f },  /* 1  Red */
        { 0.180f, 0.714f, 0.490f, 1.00f },  /* 2  Green */
        { 0.925f, 0.698f, 0.180f, 1.00f },  /* 3  Yellow */
        { 0.114f, 0.608f, 0.820f, 1.00f },  /* 4  Blue */
        { 0.757f, 0.451f, 0.765f, 1.00f },  /* 5  Magenta */
        { 0.212f, 0.773f, 0.941f, 1.00f },  /* 6  Cyan */
        { 0.820f, 0.824f, 0.827f, 1.00f },  /* 7  White */
        { 0.380f, 0.376f, 0.380f, 1.00f },  /* 8  Bright Black */
        { 0.949f, 0.325f, 0.498f, 1.00f },  /* 9  Bright Red */
        { 0.306f, 0.812f, 0.612f, 1.00f },  /* 10 Bright Green */
        { 0.949f, 0.769f, 0.333f, 1.00f },  /* 11 Bright Yellow */
        { 0.212f, 0.773f, 0.941f, 1.00f },  /* 12 Bright Blue */
        { 0.851f, 0.604f, 0.855f, 1.00f },  /* 13 Bright Magenta */
        { 0.435f, 0.851f, 0.969f, 1.00f },  /* 14 Bright Cyan */
        { 1.000f, 1.000f, 1.000f, 1.00f },  /* 15 Bright White */
    },
    .tab_bg          = { 0.102f, 0.114f, 0.129f, 1.00f },
    .tab_active_bg   = { 0.173f, 0.176f, 0.188f, 1.00f },
    .tab_active_fg   = { 1.000f, 1.000f, 1.000f, 1.00f },
    .tab_inactive_bg = { 0.102f, 0.114f, 0.129f, 1.00f },
    .tab_inactive_fg = { 0.608f, 0.608f, 0.608f, 1.00f },
    .sidebar_bg      = { 0.133f, 0.145f, 0.165f, 1.00f },
    .sidebar_fg      = { 0.820f, 0.824f, 0.827f, 1.00f },
    .sidebar_hover   = { 0.152f, 0.163f, 0.178f, 1.00f },
    .sidebar_active  = { 0.105f, 0.222f, 0.281f, 1.00f },
    .border          = { 0.220f, 0.227f, 0.247f, 1.00f },
    .scrollbar       = { 0.102f, 0.114f, 0.129f, 1.00f },
    .scrollbar_thumb = { 0.111f, 0.484f, 0.647f, 1.00f },
    .status_bg       = { 0.102f, 0.114f, 0.129f, 1.00f },
    .status_fg       = { 0.608f, 0.608f, 0.608f, 1.00f },
    .ui_accent       = { 0.114f, 0.608f, 0.820f, 1.00f },
};

const Theme THEME_Liu = {
    /* Liu's theme uses a neutral ink ramp for chrome surfaces and violet
     * accents for interaction. Terminal colors use corresponding steps of the
     * same palette. */
    .name = "Liu",
    .fg  = { 0.969f, 0.969f, 0.973f, 1.0f },   /* #F7F7F8 ink-50, the type colour */
    .bg  = { 0.039f, 0.039f, 0.043f, 1.0f },   /* #0A0A0B ink-950 — Obsidian, the page */
    /* The cursor is the accent. */
    .cursor    = { 0.635f, 0.529f, 1.000f, 1.0f },   /* #A287FF violet-500, 7.0:1 on bg */
    /* violet-800 at 70% — composites to #3E2C6C: 11:1 under fg, still a
     * clearly visible wash on bg. */
    .selection = { 0.329f, 0.231f, 0.588f, 0.70f }, /* #543B96 violet-800 */
    /* The site defines only three status colours (danger, success, warning)
     * and one hue (violet). Those become the bright red/green/yellow and the
     * magenta pair; the regular row is each of them one ramp step down
     * (OKLCH L -0.085, the same 500 -> 600 step the violet ramp takes).
     * Blue and cyan have no site counterpart, so they are derived in OKLCH at
     * the status colours' weight (L .73 / .80, chroma as far as sRGB allows)
     * rather than borrowing a foreign palette. Blue stays a true blue — it is
     * what `ls` prints for directories, and the accent must not repaint it. */
    .ansi = {
        { 0.039f, 0.039f, 0.043f, 1.0f },  /* 0  black = bg #0A0A0B */
        { 0.875f, 0.396f, 0.373f, 1.0f },  /* 1  red #DF655F */
        { 0.200f, 0.722f, 0.475f, 1.0f },  /* 2  green #33B879 */
        { 0.843f, 0.682f, 0.384f, 1.0f },  /* 3  yellow #D7AE62 */
        { 0.322f, 0.561f, 0.875f, 1.0f },  /* 4  blue #528FDF */
        { 0.549f, 0.408f, 0.941f, 1.0f },  /* 5  magenta = violet-600 #8C68F0 */
        { 0.298f, 0.706f, 0.761f, 1.0f },  /* 6  cyan #4CB4C2 */
        { 0.776f, 0.776f, 0.800f, 1.0f },  /* 7  white = ink-300 #C6C6CC */
        { 0.333f, 0.333f, 0.361f, 1.0f },  /* 8  bright black = ink-600 #55555C */
        { 1.000f, 0.494f, 0.467f, 1.0f },  /* 9  bright red = danger #FF7E77 */
        { 0.314f, 0.831f, 0.573f, 1.0f },  /* 10 bright green = success #50D492 */
        { 0.957f, 0.788f, 0.478f, 1.0f },  /* 11 bright yellow = warning #F4C97A */
        { 0.416f, 0.667f, 0.996f, 1.0f },  /* 12 bright blue #6AAAFE */
        { 0.635f, 0.529f, 1.000f, 1.0f },  /* 13 bright magenta = violet-500 #A287FF */
        { 0.400f, 0.816f, 0.875f, 1.0f },  /* 14 bright cyan #66D0DF */
        { 0.969f, 0.969f, 0.973f, 1.0f },  /* 15 bright white = ink-50 #F7F7F8 */
    },
    .tab_bg          = { 0.039f, 0.039f, 0.043f, 1.0f },   /* #0A0A0B */
    /* The active tab uses a neutral raised surface; violet remains on selection
     * and focus indicators. */
    .tab_active_bg   = { 0.145f, 0.145f, 0.161f, 1.0f },   /* #252529 ink-800 */
    .tab_active_fg   = { 0.969f, 0.969f, 0.973f, 1.0f },   /* #F7F7F8 ink-50 = ansi 15 */
    .tab_inactive_bg = { 0.039f, 0.039f, 0.043f, 1.0f },   /* #0A0A0B */
    .tab_inactive_fg = { 0.478f, 0.478f, 0.514f, 1.0f },   /* #7A7A83 ink-500, 4.7:1 */
    /* sidebar_bg is the WELL colour (input fields, cards, the file-browser
     * rail); the docked bar itself paints no fill. The site's panel step. */
    .sidebar_bg      = { 0.086f, 0.086f, 0.094f, 1.0f },   /* #161618 ink-900 */
    /* This colour is the palette's dim/secondary text everywhere (family
     * prefixes, detail columns, hints), so it stays neutral. It is the
     * body-text step: ink-50 at 78% over bg, i.e. ink-300. */
    .sidebar_fg      = { 0.776f, 0.776f, 0.800f, 1.0f },   /* #C6C6CC ink-300 */
    /* Hover is the surface step (ink-800): it has to read on the bg AND on the
     * well above (it fills secondary buttons and list rows that sit on
     * either), so it cannot be the well's own colour. */
    .sidebar_hover   = { 0.145f, 0.145f, 0.161f, 1.0f },   /* #252529 ink-800 */
    /* The selected row is bg 26% toward the accent — a violet step you can
     * tell from hover by hue as well as lightness, but not a saturated fill
     * (see the tab_active_bg note). */
    .sidebar_active  = { 0.196f, 0.165f, 0.290f, 1.0f },   /* #322A4A */
    .border          = { 0.227f, 0.227f, 0.247f, 1.0f },   /* #3A3A3F ink-700, 1.75:1 */
    .scrollbar       = { 0.039f, 0.039f, 0.043f, 1.0f },   /* #0A0A0B */
    /* One of the few places the brand colour stays unmistakable: violet-600,
     * one ramp step under the accent. */
    .scrollbar_thumb = { 0.549f, 0.408f, 0.941f, 1.0f },   /* #8C68F0 violet-600 */
    .status_bg       = { 0.039f, 0.039f, 0.043f, 1.0f },   /* #0A0A0B */
    .status_fg       = { 0.631f, 0.631f, 0.667f, 1.0f },   /* #A1A1AA ink-400, 7.7:1 */
    /* Drives every primary button, palette pill, tab underline, focused-field
     * underline and the Settings selection. violet-500 is used as-is on ink
     * (7.0:1); it is light enough that a WHITE label fails on it (2.8:1), so
     * chrome_legible_on picks ink for text on the accent. */
    .ui_accent       = { 0.635f, 0.529f, 1.000f, 1.0f },   /* #A287FF violet-500 */
    /* The voice card's "listening" light. Set here rather than left to ansi[9]
     * so the default theme shows this exact red. */
    .voice_record    = { 1.000f, 0.353f, 0.322f, 1.0f },   /* #FF5A52 */
};

/* Liu's light half — the same palette with the ink ramp turned over: Paper
 * (ink-50) is the canvas, ink-950 is the type, and the accent is violet-700,
 * the light theme's accent step (5.3:1 on paper). Each chrome value is the
 * dark theme's role read off the other end of the ramp: the well is ink-100,
 * hover and the active tab ink-200, the border ink-300, secondary text
 * ink-600/ink-700.
 *
 * The ANSI hues are the dark theme's, re-weighted rather than reused: a pastel
 * that carries on ink reads as highlighter ink on paper, so both rows are
 * darkened (regular OKLCH L .46, bright L .55 — the same 0.09 gap the dark
 * theme has) and every bright slot still clears 4:1 on the canvas. */
const Theme THEME_LIU_LIGHT = {
    .name = "Liu Light",
    .fg  = { 0.039f, 0.039f, 0.043f, 1.0f },   /* #0A0A0B ink-950 */
    .bg  = { 0.969f, 0.969f, 0.973f, 1.0f },   /* #F7F7F8 ink-50 — Paper */
    .cursor    = { 0.443f, 0.318f, 0.780f, 1.0f },   /* #7151C7 violet-700, 5.3:1 on bg */
    /* violet-300 at 85% — composites to #CDC4FD: ink reads at 13:1 on it and
     * the wash is plainly visible on paper. */
    .selection = { 0.776f, 0.733f, 0.996f, 0.85f }, /* #C6BBFE violet-300 */
    .ansi = {
        { 0.878f, 0.878f, 0.894f, 1.0f },  /* 0  black = ink-200 surface #E0E0E4 */
        { 0.616f, 0.137f, 0.153f, 1.0f },  /* 1  red #9D2327 */
        { 0.020f, 0.412f, 0.251f, 1.0f },  /* 2  green #056940 */
        { 0.447f, 0.322f, 0.016f, 1.0f },  /* 3  yellow #725204 */
        { 0.086f, 0.341f, 0.639f, 1.0f },  /* 4  blue #1657A3 */
        { 0.329f, 0.231f, 0.588f, 1.0f },  /* 5  magenta = violet-800 #543B96 */
        { 0.016f, 0.392f, 0.431f, 1.0f },  /* 6  cyan #04646E */
        { 0.333f, 0.333f, 0.361f, 1.0f },  /* 7  white = ink-600 #55555C */
        { 0.631f, 0.631f, 0.667f, 1.0f },  /* 8  bright black = ink-400 #A1A1AA */
        { 0.737f, 0.259f, 0.251f, 1.0f },  /* 9  bright red #BC4240 */
        { 0.012f, 0.529f, 0.329f, 1.0f },  /* 10 bright green #038754 */
        { 0.569f, 0.416f, 0.059f, 1.0f },  /* 11 bright yellow #916A0F */
        { 0.200f, 0.447f, 0.757f, 1.0f },  /* 12 bright blue #3372C1 */
        { 0.443f, 0.318f, 0.780f, 1.0f },  /* 13 bright magenta = violet-700 #7151C7 */
        { 0.012f, 0.502f, 0.557f, 1.0f },  /* 14 bright cyan #03808E */
        { 0.039f, 0.039f, 0.043f, 1.0f },  /* 15 bright white = ink-950 #0A0A0B */
    },
    .tab_bg          = { 0.969f, 0.969f, 0.973f, 1.0f },   /* #F7F7F8 */
    /* The active tab is the ink-200 surface step: on a canvas that is already
     * the lightest ramp step, "raised" can only mean one step darker. The tab
     * underline (accent) is what actually marks it. */
    .tab_active_bg   = { 0.878f, 0.878f, 0.894f, 1.0f },   /* #E0E0E4 ink-200 */
    .tab_active_fg   = { 0.039f, 0.039f, 0.043f, 1.0f },   /* #0A0A0B ink-950 = ansi 15 */
    .tab_inactive_bg = { 0.969f, 0.969f, 0.973f, 1.0f },   /* #F7F7F8 */
    .tab_inactive_fg = { 0.478f, 0.478f, 0.514f, 1.0f },   /* #7A7A83 ink-500, 4.0:1 */
    /* The well colour (input fields, cards, the file-browser rail): ink-100,
     * one step under the page — the dark theme's panel step, mirrored. */
    .sidebar_bg      = { 0.933f, 0.933f, 0.941f, 1.0f },   /* #EEEEF0 ink-100 */
    /* The palette's dim/secondary text: ink-950 at 78% over paper, i.e. ink-700. */
    .sidebar_fg      = { 0.227f, 0.227f, 0.247f, 1.0f },   /* #3A3A3F ink-700 */
    .sidebar_hover   = { 0.878f, 0.878f, 0.894f, 1.0f },   /* #E0E0E4 ink-200 */
    /* bg 26% toward the accent: tells from hover by hue as well as lightness. */
    .sidebar_active  = { 0.831f, 0.800f, 0.922f, 1.0f },   /* #D4CCEB */
    .border          = { 0.776f, 0.776f, 0.800f, 1.0f },   /* #C6C6CC ink-300, 1.6:1 */
    .scrollbar       = { 0.969f, 0.969f, 0.973f, 1.0f },   /* #F7F7F8 */
    .scrollbar_thumb = { 0.549f, 0.408f, 0.941f, 1.0f },   /* #8C68F0 violet-600, 3.7:1 */
    /* The status bar paints no fill and sits on bg, so its text must read on
     * paper. */
    .status_bg       = { 0.969f, 0.969f, 0.973f, 1.0f },   /* #F7F7F8 */
    .status_fg       = { 0.333f, 0.333f, 0.361f, 1.0f },   /* #55555C ink-600, 6.9:1 */
    /* violet-700: dark enough that a white label on a filled button reads
     * (5.7:1) and it is legible as text on the canvas (5.3:1). */
    .ui_accent       = { 0.443f, 0.318f, 0.780f, 1.0f },   /* #7151C7 violet-700 */
};

/* Ultradark uses a true-black canvas and near-neutral chrome so terminal
 * colors remain distinct from UI accents.
 *
 * sidebar_bg and status_bg are #080808, but these controls do not paint a
 * fill. Their values color input wells, cards, and the file-browser list rail.
 * The inactive tab slot uses #0f0f0f to remain visible against the canvas. The
 * #292929 border has 1.44:1 contrast against black, within the divider range
 * in THEMING.md.
 *
 * The ANSI palette uses a mid-saturation pastel row and a bright row,
 * separated from the near-neutral chrome. */
const Theme THEME_ULTRADARK = {
    .name = "Ultradark",
    .fg  = { 0.900f, 0.900f, 0.900f, 1.0f },   /* #e6e6e6 */
    .bg  = { 0.000f, 0.000f, 0.000f, 1.0f },   /* #000000 — true black */
    .cursor    = { 0.850f, 0.850f, 0.850f, 1.0f },   /* #d9d9d9 */
    .selection = { 0.220f, 0.220f, 0.220f, 0.70f },  /* #383838 wash */
    .ansi = {
        { 0.200f, 0.200f, 0.200f, 1.0f },  /* 0  black    #333333 */
        { 0.910f, 0.380f, 0.360f, 1.0f },  /* 1  red      #e8615c */
        { 0.400f, 0.800f, 0.540f, 1.0f },  /* 2  green    #66cc8a */
        { 0.920f, 0.800f, 0.380f, 1.0f },  /* 3  yellow   #ebcc61 */
        { 0.400f, 0.620f, 0.950f, 1.0f },  /* 4  blue     #669ef2 */
        { 0.780f, 0.520f, 0.880f, 1.0f },  /* 5  magenta  #c785e0 */
        { 0.380f, 0.800f, 0.850f, 1.0f },  /* 6  cyan     #61ccd9 */
        { 0.860f, 0.860f, 0.860f, 1.0f },  /* 7  white    #dbdbdb */
        { 0.420f, 0.420f, 0.420f, 1.0f },  /* 8  br black #6b6b6b */
        { 0.980f, 0.520f, 0.500f, 1.0f },  /* 9  br red   #fa8580 */
        { 0.550f, 0.920f, 0.660f, 1.0f },  /* 10 br green #8ceba8 */
        { 0.980f, 0.900f, 0.520f, 1.0f },  /* 11 br yellow#fae685 */
        { 0.580f, 0.760f, 0.990f, 1.0f },  /* 12 br blue  #94c2fc */
        { 0.900f, 0.640f, 0.960f, 1.0f },  /* 13 br mag   #e6a3f5 */
        { 0.540f, 0.920f, 0.950f, 1.0f },  /* 14 br cyan  #8aebf2 */
        { 1.000f, 1.000f, 1.000f, 1.0f },  /* 15 br white #ffffff */
    },
    .tab_bg          = { 0.000f, 0.000f, 0.000f, 1.0f },   /* #000000 */
    .tab_active_bg   = { 0.150f, 0.150f, 0.150f, 1.0f },   /* #262626 */
    .tab_active_fg   = { 1.000f, 1.000f, 1.000f, 1.0f },   /* #ffffff */
    .tab_inactive_bg = { 0.060f, 0.060f, 0.060f, 1.0f },   /* #0f0f0f */
    .tab_inactive_fg = { 0.580f, 0.580f, 0.580f, 1.0f },   /* #949494 */
    .sidebar_bg      = { 0.030f, 0.030f, 0.030f, 1.0f },   /* #080808 */
    .sidebar_fg      = { 0.820f, 0.820f, 0.820f, 1.0f },   /* #d1d1d1 */
    .sidebar_hover   = { 0.100f, 0.100f, 0.100f, 1.0f },   /* #1a1a1a */
    /* The one place a trace of blue survives, so "selected" reads as a state
     * rather than as one more grey step above hover. */
    .sidebar_active  = { 0.220f, 0.220f, 0.240f, 1.0f },   /* #38383d */
    .border          = { 0.160f, 0.160f, 0.160f, 1.0f },   /* #292929 */
    .scrollbar       = { 0.050f, 0.050f, 0.050f, 1.0f },   /* #0d0d0d */
    .scrollbar_thumb = { 0.360f, 0.360f, 0.360f, 1.0f },   /* #5c5c5c */
    .status_bg       = { 0.030f, 0.030f, 0.030f, 1.0f },   /* #080808 */
    .status_fg       = { 0.620f, 0.620f, 0.620f, 1.0f },   /* #9e9e9e */
    .ui_accent       = { 0.460f, 0.460f, 0.500f, 1.0f },   /* #757580 */
};

/* Theme registry — the order the Settings › Appearance grid and the
 * command palette list them in. Liu leads, its light half sits directly
 * beside it, then Ultradark; the ports follow. */
static const Theme *g_themes[] = {
    &THEME_Liu, &THEME_LIU_LIGHT, &THEME_ULTRADARK,
    &THEME_GITHUB_DARK_DIMMED, &THEME_PIERRE_DARK,
    &THEME_ONE_DARK_PRO, &THEME_DRACULA, &THEME_DRACULA_SOFT,
    &THEME_CATPPUCCIN_MOCHA, &THEME_NORD, &THEME_SOLARIZED_DARK,
    &THEME_VITESSE_DARK, &THEME_MIN_DARK, &THEME_MONOKAI, &THEME_TOKYO_NIGHT,
    &THEME_NIGHT_OWL, &THEME_ROSE_PINE, &THEME_AYU_DARK, &THEME_SLACK_DARK,
};

/* User themes are stored in theme_import.c */
extern Theme g_user_themes[32];
extern i32   g_user_theme_count;

const Theme *theme_get_by_name(const char *name) {
    for (i32 i = 0; i < THEME_COUNT; i++) {
        if (strcmp(g_themes[i]->name, name) == 0) return g_themes[i];
    }
    /* Search user themes */
    for (i32 i = 0; i < g_user_theme_count; i++) {
        if (strcmp(g_user_themes[i].name, name) == 0) return &g_user_themes[i];
    }
    return &THEME_Liu;
}

const Theme *theme_get_by_index(i32 index) {
    if (index >= 0 && index < THEME_COUNT) return g_themes[index];
    i32 ui = index - THEME_COUNT;
    if (ui >= 0 && ui < g_user_theme_count) return &g_user_themes[ui];
    return &THEME_Liu;
}

const char **theme_list_names(i32 *count) {
    static const char *names[THEME_COUNT + 32];
    for (i32 i = 0; i < THEME_COUNT; i++) names[i] = g_themes[i]->name;
    for (i32 i = 0; i < g_user_theme_count; i++) names[THEME_COUNT + i] = g_user_themes[i].name;
    *count = THEME_COUNT + g_user_theme_count;
    return names;
}

Color theme_ui_accent(const Theme *t) {
    if (!t) return (Color){0.56f, 0.73f, 0.98f, 1.0f};
    if (t->ui_accent.a > 0.0f) return t->ui_accent;

    /* When ui_accent is unset, use a saturated cursor color as the chrome
     * accent; otherwise fall back to ANSI blue.
     */
    f32 r = t->cursor.r, g = t->cursor.g, b = t->cursor.b;
    f32 max_c = r > g ? (r > b ? r : b) : (g > b ? g : b);
    f32 min_c = r < g ? (r < b ? r : b) : (g < b ? g : b);
    f32 sat   = max_c > 0.001f ? (max_c - min_c) / max_c : 0.0f;
    if (t->cursor.a > 0.5f && sat > 0.40f) return t->cursor;
    return t->ansi[12];
}

Color theme_voice_record(const Theme *t) {
    /* The fallback is a canonical red rather than the accent: this is the
     * "your microphone is open" light, and on the many themes whose accent is
     * the same blue as the cursor and the selection it would read as ordinary
     * chrome. */
    const Color fallback = { 0.93f, 0.32f, 0.33f, 1.0f };
    if (!t) return fallback;
    if (t->voice_record.a > 0.0f) return t->voice_record;

    /* Use the theme's ANSI bright red for the recording indicator only when
     * it is sufficiently saturated and red-dominant. Otherwise use the
     * canonical red fallback so the indicator remains distinguishable from
     * neutral or blue accents. */
    Color c = t->ansi[9];
    f32 max_c = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    f32 min_c = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    f32 sat = max_c > 0.001f ? (max_c - min_c) / max_c : 0.0f;
    if (max_c > 0.35f && sat > 0.35f && c.r >= c.g && c.r >= c.b) {
        c.a = 1.0f;
        return c;
    }
    return fallback;
}

Color theme_voice_accent(const Theme *t) {
    if (t && t->voice_accent.a > 0.0f) return t->voice_accent;
    return theme_ui_accent(t);
}

bool theme_apply_style_overrides(const Theme *t, AppConfig *c) {
    if (!t || !c) return false;
    bool changed = false;
    if (t->opacity_override > 0.0f) {
        f32 v = t->opacity_override;
        if (v < 0.30f) v = 0.30f;
        if (v > 1.0f)  v = 1.0f;
        if (c->opacity != v) { c->opacity = v; changed = true; }
    }
    /* Override values map: 1=block, 2=underline, 3=bar → config 0/1/2. */
    if (t->cursor_style_override > 0) {
        u8 v = (u8)(t->cursor_style_override - 1);
        if (v <= 2 && c->cursor_style != v) { c->cursor_style = v; changed = true; }
    }
    if (t->cursor_blink_override > 0) {
        bool v = (t->cursor_blink_override == 1);
        if (c->cursor_blink != v) { c->cursor_blink = v; changed = true; }
    }
    if (t->bold_is_bright_override > 0) {
        bool v = (t->bold_is_bright_override == 1);
        if (c->bold_is_bright != v) { c->bold_is_bright = v; changed = true; }
    }
    return changed;
}

/* =========================================================================
 * Config defaults + load/save
 * ========================================================================= */

Style style_default(void) {
    Style s = {
        .tab_gap               = 2.0f,
        .tab_dot_size          = 6.0f,
        .tab_close_size        = 14.0f,
        .tab_close_margin      = 6.0f,
        .tab_indicator_height  = 2.0f,
        .status_bar_height     = 22.0f,
        .sidebar_default_width = 240.0f,
        .sidebar_min_width     = 160.0f,
        .sidebar_max_width     = 500.0f,
        .sidebar_header_height = 28.0f,
        .terminal_padding      = 6.0f,
        /* 0: the terminal pane sits flush under the tab bar. Any positive gap
         * paints in the bar's bg colour, so it visually extends the bar band
         * downward and makes the tab pill's bottom margin read larger than
         * its top margin (unequal spacing around the pills). */
        .terminal_top_gap      = 0.0f,
        .active_pane_indicator_thickness = 1.0f,
        .active_pane_indicator_color     = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    return s;
}

AppConfig config_default(void) {
    AppConfig c = {0};
    c.style = style_default();
    /* Bare filename — font_atlas_create resolves it against the bundled font
     * directory (assets/fonts). Keeps config.json portable instead of baking in
     * an absolute machine-specific path. */
    snprintf(c.font_path, sizeof(c.font_path), "JetBrainsMonoNerdFontMono-Regular.ttf");
    c.fallback_font_count = 0;
    memset(c.fallback_fonts, 0, sizeof(c.fallback_fonts));
    c.font_size = CONFIG_FONT_SIZE_DEFAULT;
    c.line_height = 1.0f;
    c.font_normalize = true;
    /* Auto: Core Text smoothing dilates every outline, and at 2x the glyph shader's
     * blend correction already gives the undilated raster the intended weight (see
     * renderer.h text_blend_correct); at 1x an undilated stem is one partly lit
     * column, so it is smoothed lightly there. */
    c.font_smoothing = FONT_SMOOTHING_AUTO;
    c.font_weight = 0.0f;
    c.text_weight_terminal = 0.0f;
    c.text_weight_ui = 0.0f;
    snprintf(c.theme_name, sizeof(c.theme_name), "Liu");
    c.theme = &THEME_Liu;
    /* 1000 lines × up to cols*sizeof(Cell) is a ~1.5 MiB worst case per
     * terminal. Most TUI workflows never scroll past a few hundred lines,
     * and more can be set in config. Combined with the adaptive ring
     * growth (starts at 64 entries, doubles on demand) this keeps RSS
     * small for many concurrent panes. */
    c.scrollback_lines = CONFIG_SCROLLBACK_DEFAULT_LINES;
    c.tab_sleep_idle_minutes = 20.0f;
    c.confirm_close_agent = true;
    c.paste_protection = true;
    c.agent_hooks = true;
    c.agent_hooks_notice_seen = false;
    c.shell_integration = true;
    c.command_marks = false;
    c.secure_keyboard_entry = false;
    c.secure_input_auto = true;
    c.ssh_legacy_algorithms = false;
    c.command_history = true;
    snprintf(c.term, sizeof c.term, "%s", "xterm-256color");
    c.cursor_blink = true;
    c.cursor_style = 2;      /* 0=block, 1=underline, 2=bar */
    c.bold_is_bright = true;
    /* On by default: a drag that selects text puts it on the clipboard. */
    c.copy_on_select = true;
    c.clipboard_trim_trailing_spaces = true;
    /* Off by default because alternate-screen programs commonly handle mouse
     * selection and scrolling themselves. Shift-drag still selects in Liu;
     * enabling this option routes plain drag to Liu while clicks continue to
     * reach the program. */
    c.mouse_drag_selects = false;
    c.bidi_enabled = false;       
    c.tab_height = 36.0f;
    c.sidebar_width = 240.0f;   /* points; matches SIDEBAR_DEFAULT_PT */
    c.show_scrollbar = true;
    c.scrollbar_autohide = false;   /* the bar stays; autohide is the overlay opt-in */
    c.opacity = 1.0f;
    c.padding = 2.0f;           
    c.hide_tab_bar_single = true;
    c.show_tab_bar = true;
    c.sidebar_tabs = false;
    for (i32 i = 0; i < 4; i++) { c.sidebar_order[i] = (u8)i; c.sidebar_enabled[i] = true; }
    c.sidebar_enabled[3] = false;            /* notes: opt-in */
    c.sidebar_compact = false;
    c.sidebar_collapse_below_pt = 1100.0f;
    c.show_toolbar_icons = true;
    c.hide_title_bar = false;
    c.show_status_bar = false;
    c.show_git_badge = true;
    c.browser_enabled = true;
    c.browser_last_url[0] = '\0';
    c.browser_recents_count = 0;
    for (i32 i = 0; i < BROWSER_RECENTS_MAX; i++) c.browser_recents[i][0] = '\0';
    c.borderless = false;
    c.cell_width_scale = 1.0f;
    c.cell_height_scale = 1.0f;
    c.option_as_alt = OPTION_AS_ALT_OFF;
    c.prompt_recall = PROMPT_RECALL_BADGE;
    c.cursor_animate = false;
    c.notify_command_threshold = 10.0f;
    c.allow_osc52_write = false;

    /* Smart Vault — 15 min idle lock (1Password / Bitwarden default). */
    c.vault_auto_lock_minutes = 15;

    /* Background image defaults */
    c.background_image[0] = '\0';
    c.notes_vault_path[0] = '\0';   /* empty => app_notes_vault_path computes a default */
    c.background_opacity = 0.3f;
    c.background_mode = 3;       
    c.background_blur = false;
    c.background_blur_radius = 10.0f;
    c.enable_ligatures = true;

    c.quake_mode = false;
    snprintf(c.quake_hotkey, sizeof(c.quake_hotkey), "Ctrl+`");
    c.quake_height_ratio = 0.4f;
    c.quake_animation_duration = 0.2f;

    c.translate = translate_config_default();
    c.voice     = voice_config_default();
    return c;
}

static bool path_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0;
}

static bool dir_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

const char *config_user_dir(void) {
    static char path[ATOMIC_FILE_PATH_CAP] = {0};
    if (path[0]) return path;

    /* LIU_E2E_CONFIG_DIR overrides the config directory without changing
     * HOME. Production builds read this override only when LIU_SOAK is
     * enabled; LIU_E2E_PROBE builds read it directly. */
    const char *probe_dir = NULL;
#ifdef LIU_E2E_PROBE
    probe_dir = liu_dev_env("LIU_E2E_CONFIG_DIR");
#else
    if (liu_dev_env("LIU_SOAK")) probe_dir = liu_dev_env("LIU_E2E_CONFIG_DIR");
#endif
    if (probe_dir) {
        usize len = strlen(probe_dir);
        if (probe_dir[0] == '/' && len < sizeof(path) - 32u && dir_exists(probe_dir))
            memcpy(path, probe_dir, len + 1u);
        return path; /* An invalid override must not fall back to real state. */
    }

    const char *home = getenv("HOME");
    char legacy_dir[ATOMIC_FILE_PATH_CAP];
    char current_dir[ATOMIC_FILE_PATH_CAP];
    char legacy_cfg[ATOMIC_FILE_PATH_CAP];
    char current_cfg[ATOMIC_FILE_PATH_CAP];

    /* No $HOME (a launch agent, a stripped environment): the passwd entry is
     * the next answer. What is NOT an answer is /tmp — it is world-writable,
     * and a config, sessions.json or vault that another local user
     * pre-created there would be loaded and auto-restored as ours. With
     * neither, the directory is "" and nothing is persisted or read. */
    if (!home || !home[0]) {
        struct passwd *pw = getpwuid(geteuid());
        home = (pw && pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : NULL;
    }
    if (!home) { path[0] = '\0'; return path; }
    int a = snprintf(current_dir, sizeof(current_dir), "%s/.config/Liu", home);
    int b = snprintf(legacy_dir, sizeof(legacy_dir), "%s/.config/liu", home);
    int c = a > 0 && (usize)a < sizeof(current_dir) - 32u
          ? snprintf(current_cfg, sizeof(current_cfg), "%s/config.json", current_dir) : -1;
    int d = b > 0 && (usize)b < sizeof(legacy_dir) - 32u
          ? snprintf(legacy_cfg, sizeof(legacy_cfg), "%s/config.json", legacy_dir) : -1;
    bool paths_valid = a > 0 && b > 0 && c > 0 && d > 0 &&
                       (usize)a < sizeof(current_dir) - 32u &&
                       (usize)b < sizeof(legacy_dir) - 32u &&
                       (usize)c < sizeof(current_cfg) &&
                       (usize)d < sizeof(legacy_cfg);

    if (!paths_valid) {
        /* A home path too long to build the config paths under: refuse
         * persistence rather than fall back to a predictable /tmp name. */
        path[0] = '\0';
        return path;
    }

    /* On a case-sensitive disk the two are different folders, and the
     * notification side always writes the lowercase one (notify.conf,
     * frontmost). So a bare ~/.config/liu proves nothing about where the
     * app's own state is: an existing ~/.config/Liu wins over it even when
     * no config.json has been written there yet (the vault, sessions.json
     * and keybindings.json can be in Liu/ alone). */
    if (path_exists(current_cfg)) {
        memcpy(path, current_dir, (usize)a + 1u);
    } else if (path_exists(legacy_cfg)) {
        memcpy(path, legacy_dir, (usize)b + 1u);
    } else if (dir_exists(current_dir)) {
        memcpy(path, current_dir, (usize)a + 1u);
    } else if (dir_exists(legacy_dir)) {
        memcpy(path, legacy_dir, (usize)b + 1u);
    } else {
        memcpy(path, current_dir, (usize)a + 1u);
    }
    return path;
}

const char *config_file_path(void) {
    static char path[ATOMIC_FILE_PATH_CAP] = {0};
    if (path[0]) return path;

    const char *dir = config_user_dir();
    if (!dir[0]) { path[0] = '\0'; return path; }
    int n = snprintf(path, sizeof(path), "%s/config.json", dir);
    if (n < 0 || (usize)n >= sizeof(path)) path[0] = '\0';
    return path;
}

static void path_parent(char *path) {
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
    } else {
        snprintf(path, 4, ".");
    }
}

static bool resolve_executable_dir(char *out, usize out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';

#ifdef PLATFORM_MACOS
    u32 size = (u32)out_size;
    if (_NSGetExecutablePath(out, &size) != 0) return false;
    path_parent(out);
    return true;
#elif defined(PLATFORM_LINUX)
    ssize_t len = readlink("/proc/self/exe", out, out_size - 1);
    if (len <= 0 || (usize)len >= out_size) return false;
    out[len] = '\0';
    path_parent(out);
    return true;
#else
    return false;
#endif
}

static bool try_font_dir(char *out, usize out_size, const char *candidate) {
    if (!candidate || !candidate[0] || !dir_exists(candidate)) return false;
    snprintf(out, out_size, "%s", candidate);
    return true;
}

const char *liu_executable_dir(void) {
    static char cached[1024] = {0};
    if (cached[0]) return cached;
    if (!resolve_executable_dir(cached, sizeof(cached))) cached[0] = '\0';
    return cached;
}

const char *font_user_dir(void) {
    static char path[1024] = {0};
    if (path[0]) return path;

    const char *override = getenv("LIU_FONT_DIR");
    if (try_font_dir(path, sizeof(path), override)) return path;

#ifdef LIU_ASSETS_DIR
    /* An explicit development build path may supply bundled assets. The
     * process working directory is never an implicit font authority. */
    {
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s/fonts", LIU_ASSETS_DIR);
        if (try_font_dir(path, sizeof(path), candidate)) return path;
    }
#endif

    {
        char exe_dir[1024];
        if (resolve_executable_dir(exe_dir, sizeof(exe_dir))) {
            char candidate[1024];
            snprintf(candidate, sizeof(candidate), "%s/../assets/fonts", exe_dir);
            if (try_font_dir(path, sizeof(path), candidate)) return path;
            snprintf(candidate, sizeof(candidate), "%s/assets/fonts", exe_dir);
            if (try_font_dir(path, sizeof(path), candidate)) return path;
            /* FHS install layout (deb/rpm/PPA/COPR): binary at /usr/bin/Liu,
             * assets at /usr/share/liu/assets — neither exe-relative candidate
             * above hits it, so try it explicitly. Also covers /opt via the
             * exe-relative path already. */
            if (try_font_dir(path, sizeof(path), "/usr/share/liu/assets/fonts"))
                return path;
            if (try_font_dir(path, sizeof(path), "/usr/local/share/liu/assets/fonts"))
                return path;
        }
    }

    snprintf(path, sizeof(path), "%s/fonts", config_user_dir());
    return path;
}

const char *font_custom_dir(void) {
    static char path[1024] = {0};
    if (!path[0]) {
        liu_path_join(path, sizeof(path), font_user_dir(), "custom");
    }
    return path;
}

/* Minimal JSON parser for config — reads key:value pairs */
/* Extract "key" : "value" from a config line, unescaping the value's \" and \\
 * (the reverse of config_fputs_json_escaped), so a saved site command like npm run
 * dev -- --define "x=y" reloads whole. Keys are plain identifiers (never escaped).
 * Returns false when the value is not a quoted string, so numeric / bool lines
 * fall through to the next branch. */
static int config_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool config_parse_json_u16(const char **p, u32 *out) {
    if (!p || !*p || !out) return false;
    u32 value = 0;
    for (i32 i = 0; i < 4; i++) {
        char c = **p;
        if (!c) return false;
        int digit = config_hex_digit(c);
        if (digit < 0) return false;
        value = (value << 4) | (u32)digit;
        (*p)++;
    }
    *out = value;
    return true;
}

/* Append one code point as a whole character, or none of it. Returns false for
 * an unencodable code point; `*overflow` is set when the value no longer fits
 * (the caller then refuses the value — see config_parse_kv). */
static bool config_append_codepoint(char *out, usize cap, usize *len, u32 cp,
                                    bool *overflow) {
    if (!out || !len || cp == 0) return false;
    u8 encoded[4];
    u32 n = utf8_encode(cp, encoded);
    if (n == 0) return false;
    if (*len + n + 1u > cap) { *overflow = true; return true; }
    for (u32 i = 0; i < n; i++) out[(*len)++] = (char)encoded[i];
    return true;
}

/* The largest string field a quoted key is stored in is notes_vault_path (1024
 * bytes, NUL included). A decoded value that does not fit its buffer is
 * REFUSED, not clipped: a clipped path could end in the middle of a character,
 * and the next save would make the loss permanent. */
#define CONFIG_VALUE_MAX 1024u
/* One config line, whole: a long line is never split with its tail parsed as a
 * line of its own. A line past this is skipped as a unit — the file is
 * hand-editable, not unbounded. */
#define CONFIG_LINE_MAX  (64u * 1024u)

/* Read one line into *buf (grown as needed). False at end of file. `*too_long`
 * is set when the line exceeded CONFIG_LINE_MAX (or memory ran out): the rest of
 * it was consumed and *buf holds nothing worth parsing. */
static bool config_read_line(FILE *f, char **buf, usize *cap, bool *too_long) {
    *too_long = false;
    if (!*buf) {
        *buf = (char *)malloc(1024);
        if (!*buf) return false;
        *cap = 1024;
    }
    usize n = 0;
    int c;
    bool any = false;
    while ((c = fgetc(f)) != EOF) {
        any = true;
        /* Every byte, including LF, needs room for the terminating NUL.
         * Even when LF does not fit, it ends this line, not the next one. */
        if (!*too_long && n + 2 > *cap) {
            usize ncap = *cap * 2;
            if (ncap > CONFIG_LINE_MAX) ncap = CONFIG_LINE_MAX;
            if (n + 2 > ncap) *too_long = true;
            else {
                char *grown = (char *)realloc(*buf, ncap);
                if (!grown) *too_long = true;
                else { *buf = grown; *cap = ncap; }
            }
        }
        if (!*too_long) (*buf)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (!any) return false;
    (*buf)[n] = '\0';
    return true;
}

static bool config_parse_kv(const char *line, char *key, usize ksz, char *val, usize vsz) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    usize k = 0;
    while (*p && *p != '"') { if (k + 1 < ksz) key[k++] = *p; p++; }
    if (*p != '"') return false;
    key[k] = '\0';
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    /* Not a string value (number/bool). Returning false here is load-bearing:
     * config_load() falls through to a second, sscanf-based dispatch chain for
     * bare tokens. Accepting them here would swallow the line and silently
     * disable that whole chain — every bool and number in config.json. */
    if (*p != '"') return false;
    p++;
    usize v = 0;
    bool overflow = false;
    while (*p && *p != '"') {
        u8 c = (u8)*p++;
        if (c < 0x20) return false;          /* raw control in JSON string */
        if (c != '\\') {
            if (v + 1u < vsz) val[v++] = (char)c; else overflow = true;
            continue;
        }

        char esc = *p++;
        if (!esc) return false;
        switch (esc) {
        case '"': case '\\': case '/':
            if (v + 1u < vsz) val[v++] = esc; else overflow = true;
            break;
        case 'b': if (v + 1u < vsz) val[v++] = '\b'; else overflow = true; break;
        case 'f': if (v + 1u < vsz) val[v++] = '\f'; else overflow = true; break;
        case 'n': if (v + 1u < vsz) val[v++] = '\n'; else overflow = true; break;
        case 'r': if (v + 1u < vsz) val[v++] = '\r'; else overflow = true; break;
        case 't': if (v + 1u < vsz) val[v++] = '\t'; else overflow = true; break;
        case 'u': {
            u32 cp;
            if (!config_parse_json_u16(&p, &cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (*p != '\\') return false;
                p++;
                if (*p != 'u') return false;
                p++;
                u32 low;
                if (!config_parse_json_u16(&p, &low)) return false;
                if (low < 0xDC00 || low > 0xDFFF) return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return false;
            }
            if (!config_append_codepoint(val, vsz, &v, cp, &overflow)) return false;
            break;
        }
        default:
            return false;
        }
    }
    if (*p != '"') return false;            /* unterminated value */
    val[v] = '\0';
    if (overflow) {
        /* Not a silent clip: the key keeps whatever it held. */
        fprintf(stderr, "config: the value of \"%s\" is longer than %zu bytes; "
                "keeping the previous value\n", key, vsz - 1);
        return false;
    }
    return true;
}

const char *config_font_smoothing_name(FontSmoothing mode) {
    switch (mode) {
    case FONT_SMOOTHING_NEVER:  return "never";
    case FONT_SMOOTHING_ALWAYS: return "always";
    case FONT_SMOOTHING_AUTO:   break;
    }
    return "auto";
}

FontSmoothing config_font_smoothing_parse(const char *s) {
    if (!s) return FONT_SMOOTHING_AUTO;
    if (strcmp(s, "always") == 0 || strcmp(s, "true") == 0 || strcmp(s, "on") == 0)
        return FONT_SMOOTHING_ALWAYS;
    if (strcmp(s, "never") == 0 || strcmp(s, "off") == 0)
        return FONT_SMOOTHING_NEVER;
    /* "false" is the default an older save wrote, not a choice: it means
     * what the default means now. */
    return FONT_SMOOTHING_AUTO;
}

const char *config_option_as_alt_name(OptionAsAlt mode) {
    switch (mode) {
    case OPTION_AS_ALT_LEFT:  return "left";
    case OPTION_AS_ALT_RIGHT: return "right";
    case OPTION_AS_ALT_BOTH:  return "true";
    case OPTION_AS_ALT_OFF:   break;
    }
    return "false";
}

OptionAsAlt config_option_as_alt_parse(const char *s) {
    if (!s) return OPTION_AS_ALT_OFF;
    if (strcmp(s, "left") == 0)  return OPTION_AS_ALT_LEFT;
    if (strcmp(s, "right") == 0) return OPTION_AS_ALT_RIGHT;
    /* "true" is what older builds wrote for the bool form of this key, and
     * what the "both sides" spelling means — keep them the same value so an
     * existing config keeps working after the upgrade. */
    if (strcmp(s, "true") == 0 || strcmp(s, "both") == 0)
        return OPTION_AS_ALT_BOTH;
    if (s[0] >= '0' && s[0] <= '3' && s[1] == '\0')
        return (OptionAsAlt)(s[0] - '0');
    return OPTION_AS_ALT_OFF;
}

const char *config_prompt_recall_name(PromptRecallMode mode) {
    switch (mode) {
    case PROMPT_RECALL_ALWAYS: return "always";
    case PROMPT_RECALL_OFF:    return "off";
    case PROMPT_RECALL_BADGE:  break;
    }
    return "badge";
}

PromptRecallMode config_prompt_recall_parse(const char *s) {
    if (!s) return PROMPT_RECALL_BADGE;
    if (strcmp(s, "off")   == 0 || strcmp(s, "false") == 0) return PROMPT_RECALL_OFF;
    if (strcmp(s, "always") == 0 || strcmp(s, "open") == 0) return PROMPT_RECALL_ALWAYS;
    if (strcmp(s, "badge") == 0 || strcmp(s, "true")  == 0) return PROMPT_RECALL_BADGE;
    if (s[0] >= '0' && s[0] <= '2' && s[1] == '\0')
        return (PromptRecallMode)(s[0] - '0');
    return PROMPT_RECALL_BADGE;
}

static bool config_parse_i32(const char *s, i32 *out) {
    if (!s || !out) return false;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || errno == ERANGE || v < INT32_MIN || v > INT32_MAX || *end != '\0')
        return false;
    *out = (i32)v;
    return true;
}

static i32 config_i32_or(const char *s, i32 fallback) {
    i32 v;
    return config_parse_i32(s, &v) ? v : fallback;
}

static bool config_parse_f32(const char *s, f32 *out) {
    if (!s || !out) return false;
    errno = 0;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s || errno == ERANGE || *end != '\0' || !isfinite(v)) return false;
    *out = (f32)v;
    return true;
}

static f32 config_f32_clamped(const char *s, f32 fallback, f32 lo, f32 hi) {
    f32 v;
    if (!config_parse_f32(s, &v)) return fallback;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static bool config_parse_color(const char *s, Color *out) {
    if (!s || !out) return false;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '"') s++;
    f32 values[4] = {0, 0, 0, 1};
    i32 count = 0;
    while (count < 4) {
        while (*s == ' ' || *s == '\t') s++;
        errno = 0;
        char *end = NULL;
        float value = strtof(s, &end);
        if (end == s || errno == ERANGE || !isfinite(value)) break;
        values[count++] = CLAMP((f32)value, 0.0f, 1.0f);
        s = end;
        while (*s == ' ' || *s == '\t') s++;
        if (*s != ',') break;
        s++;
    }
    if (count < 3) return false;
    *out = (Color){values[0], values[1], values[2], values[3]};
    return true;
}

/* Parse the decimal index in either "12" or "12_field" without atoi's
 * signed-overflow UB.  When field_out is requested, an underscore and a
 * non-empty field are required. */
static bool config_parse_index(const char *s, i32 limit, i32 *out,
                               const char **field_out) {
    if (!s || !out || limit <= 0) return false;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || errno == ERANGE || v < 0 || v >= limit) return false;
    if (field_out) {
        if (*end != '_' || end[1] == '\0') return false;
        *field_out = end + 1;
    } else if (*end != '\0') {
        return false;
    }
    *out = (i32)v;
    return true;
}

/* Sidebar sections travel as one quoted list — "tabs,files,-agents,notes" —
 * because the ORDER is the setting; a leading '-' turns a section off. Names,
 * not indices, so the file stays readable and a renumbered enum cannot
 * silently reorder somebody's sidebar. A name the build does not know is
 * skipped; a section the list omits keeps its default slot at the end. */
static const char *const SIDEBAR_SECTION_NAMES[4] = { "tabs", "files", "agents", "notes" };

static void config_parse_sidebar_sections(AppConfig *cfg, const char *val) {
    u8   order[4]; bool enabled[4]; bool seen[4] = { false, false, false, false };
    i32  n = 0;
    const char *p = val;
    while (*p && n < 4) {
        while (*p == ' ' || *p == ',') p++;
        bool on = true;
        if (*p == '-') { on = false; p++; }
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        usize len = (usize)(e - p);
        for (i32 i = 0; i < 4; i++) {
            if (!seen[i] && strlen(SIDEBAR_SECTION_NAMES[i]) == len &&
                strncmp(SIDEBAR_SECTION_NAMES[i], p, len) == 0) {
                seen[i] = true; order[n] = (u8)i; enabled[i] = on; n++;
                break;
            }
        }
        p = e;
    }
    if (n == 0) return;                     /* nothing recognised: keep defaults */
    for (i32 i = 0; i < 4; i++) {
        if (seen[i]) continue;
        order[n++] = (u8)i;
        enabled[i] = cfg->sidebar_enabled[i];
    }
    for (i32 i = 0; i < 4; i++) { cfg->sidebar_order[i] = order[i]; cfg->sidebar_enabled[i] = enabled[i]; }
}

void config_format_sidebar_sections(const AppConfig *cfg, char *out, usize cap) {
    usize used = 0;
    if (cap) out[0] = '\0';
    for (i32 k = 0; k < 4; k++) {
        u8 id = cfg->sidebar_order[k];
        if (id >= 4) continue;
        int w = snprintf(out + used, cap > used ? cap - used : 0, "%s%s%s",
                         k ? "," : "", cfg->sidebar_enabled[id] ? "" : "-",
                         SIDEBAR_SECTION_NAMES[id]);
        if (w < 0) break;
        used += (usize)w;
        if (used >= cap) break;
    }
}

i32 config_sidebar_visible_sections(const AppConfig *cfg) {
    i32 n = 0;
    for (i32 i = 0; i < 4; i++) {
        if (i == 0 && !cfg->sidebar_tabs) continue;   /* SB_SEC_TABS lives on the strip */
        if (cfg->sidebar_enabled[i]) n++;
    }
    return n;
}

bool config_sidebar_ensure_section(AppConfig *cfg) {
    if (config_sidebar_visible_sections(cfg) > 0) return false;
    cfg->sidebar_enabled[1] = true;                   /* SB_SEC_FILES */
    return true;
}

/* The whole file is read up to this; a larger one is not read at all. */
#define CONFIG_FILE_MAX (16u * 1024u * 1024u)

static bool g_config_unread;

bool config_last_load_unread(void) { return g_config_unread; }

/* A config that had text and gave not one member is kept, byte for byte, as
 * `<path>.unread` beside the name it was read through (Liu's own folder for a
 * linked one): Liu runs on the defaults and its next save replaces the file. */
static void config_keep_unread(const char *path, const char *text, usize n) {
    char kept[ATOMIC_FILE_PATH_CAP];
    if ((usize)snprintf(kept, sizeof kept, "%s.unread", path) >= sizeof kept) return;
    AtomicFileWriter writer;
    if (!atomic_file_begin_private(&writer, kept)) return;
    if (fwrite(text, 1, n, writer.stream) != n) { atomic_file_abort(&writer); return; }
    (void)atomic_file_commit(&writer);
}

/* Faces Liu no longer bundles, and the Nerd Font Mono build of the same family
 * that is bundled instead — for some families another weight than Regular, the
 * one whose strokes match the other bundled faces. A config names a bundled
 * face by its file — bare, or with the fonts folder of whichever copy of Liu
 * saved it. */
static const char *const k_retired_faces[][2] = {
    {"JetBrainsMono-Regular.ttf", "JetBrainsMonoNerdFontMono-Regular.ttf"},
    {"Iosevka-Regular.ttf",       "IosevkaNerdFontMono-Medium.ttf"},
    {"GeistMono-Regular.ttf",     "GeistMonoNerdFontMono-Regular.otf"},
    {"MonaspaceNeon-Regular.otf", "MonaspiceNeNerdFontMono-Regular.otf"},
    {"MapleMono-Regular.ttf",     "MapleMono-NF-Regular.ttf"},
    {"CommitMono-Regular.ttf",    "CommitMonoNerdFontMono-Regular.otf"},
    {"FiraCode-Regular.ttf",      "FiraCodeNerdFontMono-Regular.ttf"},
    {"CascadiaCode.ttf",          "CaskaydiaCoveNerdFontMono-SemiLight.ttf"},
    {"Hack-Regular.ttf",          "HackNerdFontMono-Regular.ttf"},
    {"IBMPlexMono-Regular.ttf",   "BlexMonoNerdFontMono-Regular.ttf"},
    {"SourceCodePro-Regular.ttf", "SauceCodeProNerdFontMono-Regular.ttf"},
    {"VictorMono-Regular.ttf",    "VictorMonoNerdFontMono-SemiBold.ttf"},
    {"RobotoMono-Regular.ttf",    "RobotoMonoNerdFontMono-Regular.ttf"},
    {"Inconsolata-Regular.ttf",   "InconsolataNerdFontMono-Regular.ttf"},
    {"IosevkaNerdFontMono-Regular.ttf",       "IosevkaNerdFontMono-Medium.ttf"},
    {"CaskaydiaCoveNerdFontMono-Regular.ttf", "CaskaydiaCoveNerdFontMono-SemiLight.ttf"},
    {"VictorMonoNerdFontMono-Regular.ttf",    "VictorMonoNerdFontMono-SemiBold.ttf"},
};

/* A path naming a retired face becomes its replacement's bare file name, which
 * resolves against the bundled fonts. A path to a file that still exists is
 * the user's own copy of that face and is kept. */
void config_font_path_replace_retired(char *path, usize cap) {
    if (!path || !path[0] || cap == 0) return;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    for (usize i = 0; i < ARRAY_LEN(k_retired_faces); i++) {
        if (strcmp(base, k_retired_faces[i][0]) != 0) continue;
        if (slash && access(path, R_OK) == 0) return;
        snprintf(path, cap, "%s", k_retired_faces[i][1]);
        return;
    }
}

bool config_load(AppConfig *cfg, const char *filepath) {
    g_config_unread = false;
    usize raw_n = 0;
    char *raw = json_read_file(filepath, CONFIG_FILE_MAX, &raw_n);
    if (!raw) return false;
    usize text_n = 0;
    char *text = json_lay_out_members(raw, raw_n, &text_n);
    FILE *f = text && text_n > 0 ? fmemopen(text, text_n, "r") : NULL;
    if (text && text_n > 0 && !f) { free(text); free(raw); return false; }
    /* Text beyond blanks and the braces of an empty object: `{}` holds no
     * setting and loses nothing. */
    bool has_text = false;
    for (usize i = 0; i < raw_n && !has_text; i++)
        has_text = !strchr(" \t\r\n{}", raw[i]);
    i32 members = 0;

    /* Ensure style defaults are loaded so unset fields stay valid */
    cfg->style = style_default();

    /* Replacement semantics: removing the last rule from a watched file
     * must revoke it, including when loading into an existing live config. */
    memset(&cfg->browser_allowlist, 0, sizeof cfg->browser_allowlist);

    char *line = NULL;
    usize line_cap = 0;
    bool  line_too_long = false;
    while (f && config_read_line(f, &line, &line_cap, &line_too_long)) {
        if (line_too_long) continue;    /* one over-long line is one skipped line */
        char key[128], val[CONFIG_VALUE_MAX];
        /* Parse "key": "value" or "key": number */
        if (config_parse_kv(line, key, sizeof key, val, sizeof val)) {
            members++;
            if (strcmp(key, "font_path") == 0) snprintf(cfg->font_path, sizeof(cfg->font_path), "%s", val);
            /* Saved quoted (it is a string), so it is read in this chain.
             */
            else if (strcmp(key, "quake_hotkey") == 0) snprintf(cfg->quake_hotkey, sizeof(cfg->quake_hotkey), "%s", val);
            else if (strcmp(key, "font_features") == 0) snprintf(cfg->font_features, sizeof(cfg->font_features), "%s", val);
            else if (strcmp(key, "font_variations") == 0) snprintf(cfg->font_variations, sizeof(cfg->font_variations), "%s", val);
            else if (strcmp(key, "font_codepoint_map") == 0) snprintf(cfg->font_codepoint_map, sizeof(cfg->font_codepoint_map), "%s", val);
            else if (strncmp(key, "fallback_font_", 14) == 0) {
                i32 idx;
                if (config_parse_index(key + 14, MAX_CONFIG_FALLBACK_FONTS,
                                       &idx, NULL) && val[0]) {
                    snprintf(cfg->fallback_fonts[idx], sizeof(cfg->fallback_fonts[idx]), "%s", val);
                    if (idx >= cfg->fallback_font_count)
                        cfg->fallback_font_count = idx + 1;
                }
            }
            else if (strcmp(key, "translate_agent_id") == 0) snprintf(cfg->translate.agent_id, sizeof(cfg->translate.agent_id), "%s", val);
            else if (strcmp(key, "translate_agent_model") == 0) snprintf(cfg->translate.agent_model, sizeof(cfg->translate.agent_model), "%s", val);
            else if (strcmp(key, "translate_local_path") == 0) snprintf(cfg->translate.local_model_path, sizeof(cfg->translate.local_model_path), "%s", val);
            else if (strcmp(key, "translate_api_provider") == 0) snprintf(cfg->translate.api_provider, sizeof(cfg->translate.api_provider), "%s", val);
            else if (strcmp(key, "translate_api_model") == 0) snprintf(cfg->translate.api_model, sizeof(cfg->translate.api_model), "%s", val);
            else if (strcmp(key, "translate_api_key") == 0) snprintf(cfg->translate.api_key, sizeof(cfg->translate.api_key), "%s", val);
            else if (strcmp(key, "translate_api_base_url") == 0) snprintf(cfg->translate.api_base_url, sizeof(cfg->translate.api_base_url), "%s", val);
            else if (strcmp(key, "translate_source_lang") == 0) snprintf(cfg->translate.source_lang, sizeof(cfg->translate.source_lang), "%s", val);
            else if (strcmp(key, "translate_target_lang") == 0) snprintf(cfg->translate.target_lang, sizeof(cfg->translate.target_lang), "%s", val);
            /* Voice keys with quoted values belong in THIS chain; the bools
             * and numbers below go in the bare-value chain. A key in the
             * wrong half silently never loads. */
            else if (strcmp(key, "voice_model_path") == 0) snprintf(cfg->voice.model_path, sizeof(cfg->voice.model_path), "%s", val);
            else if (strcmp(key, "voice_lang") == 0) snprintf(cfg->voice.lang, sizeof(cfg->voice.lang), "%s", val);
            else if (strcmp(key, "voice_api_provider") == 0) snprintf(cfg->voice.api_provider, sizeof(cfg->voice.api_provider), "%s", val);
            else if (strcmp(key, "voice_api_model") == 0) snprintf(cfg->voice.api_model, sizeof(cfg->voice.api_model), "%s", val);
            else if (strcmp(key, "voice_api_key") == 0) snprintf(cfg->voice.api_key, sizeof(cfg->voice.api_key), "%s", val);
            else if (strcmp(key, "voice_api_base_url") == 0) snprintf(cfg->voice.api_base_url, sizeof(cfg->voice.api_base_url), "%s", val);
            else if (strcmp(key, "voice_input_device") == 0) snprintf(cfg->voice.input_device, sizeof(cfg->voice.input_device), "%s", val);
            else if (strcmp(key, "background_image") == 0) snprintf(cfg->background_image, sizeof(cfg->background_image), "%s", val);
            else if (strcmp(key, "browser_last_url") == 0) {
                snprintf(cfg->browser_last_url, sizeof cfg->browser_last_url, "%s", val);
            }
            else if (strncmp(key, "browser_allowlist_", 18) == 0) {
                i32 idx;
                if (config_parse_index(key + 18, BROWSER_ALLOWLIST_MAX, &idx, NULL) &&
                    val[0] && strlen(val) < BROWSER_RULE_CAP) {
                    snprintf(cfg->browser_allowlist.rules[idx], BROWSER_RULE_CAP, "%s", val);
                    if (idx + 1 > cfg->browser_allowlist.count)
                        cfg->browser_allowlist.count = idx + 1;
                }
            }
            else if (strncmp(key, "browser_recent_", 15) == 0) {
                i32 idx;
                if (config_parse_index(key + 15, BROWSER_RECENTS_MAX,
                                       &idx, NULL) && val[0]) {
                    snprintf(cfg->browser_recents[idx],
                             sizeof cfg->browser_recents[idx], "%s", val);
                    if (idx + 1 > cfg->browser_recents_count)
                        cfg->browser_recents_count = idx + 1;
                }
            }
            else if (strcmp(key, "notes_vault_path") == 0) snprintf(cfg->notes_vault_path, sizeof(cfg->notes_vault_path), "%s", val);
            else if (strcmp(key, "lang") == 0) snprintf(cfg->lang, sizeof cfg->lang, "%s", val);
            else if (strcmp(key, "term") == 0) {
                /* Only the two names Liu can stand behind: anything else
                 * would be a TERM no program here has an entry for. */
                if (strcmp(val, "xterm-liu") == 0 || strcmp(val, "xterm-256color") == 0)
                    snprintf(cfg->term, sizeof cfg->term, "%s", val);
            }
            else if (strcmp(key, "sidebar_sections") == 0) config_parse_sidebar_sections(cfg, val);
            else if (strcmp(key, "background_mode") == 0) {
                if (strcmp(val, "stretch") == 0) cfg->background_mode = 0;
                else if (strcmp(val, "center") == 0) cfg->background_mode = 1;
                else if (strcmp(val, "tile") == 0) cfg->background_mode = 2;
                else if (strcmp(val, "fill") == 0) cfg->background_mode = 3;
            }
            else if (strcmp(key, "theme") == 0) {
                snprintf(cfg->theme_name, sizeof(cfg->theme_name), "%s", val);
                cfg->theme = theme_get_by_name(val);
            }
            /* Quoted form — "false"/"left"/"right"/"true", what config_save()
             * writes. The bare form of older configs (`true` / `false`) is
             * handled by the sscanf chain below; both land on the same
             * config_option_as_alt_parse(). */
            else if (strcmp(key, "option_as_alt") == 0) {
                cfg->option_as_alt = config_option_as_alt_parse(val);
            }
            /* Quoted form, what config_save() writes; the bare true / false of
             * older configs goes through the sscanf chain to the same parse. */
            else if (strcmp(key, "font_smoothing") == 0) {
                cfg->font_smoothing = config_font_smoothing_parse(val);
            }
            else if (strcmp(key, "prompt_recall") == 0) {
                cfg->prompt_recall = config_prompt_recall_parse(val);
            }
            else if (strcmp(key, "style_active_pane_indicator_color") == 0) {
                Color color;
                if (config_parse_color(val, &color))
                    cfg->style.active_pane_indicator_color = color;
            }
            /* Profile string values: profile_N_name, profile_N_font_path, profile_N_theme */
            else if (strncmp(key, "profile_", 8) == 0) {
                i32 pi;
                const char *field;
                if (config_parse_index(key + 8, MAX_PROFILES, &pi, &field)) {
                        if (strcmp(field, "name") == 0)
                            snprintf(cfg->profiles[pi].name, sizeof(cfg->profiles[pi].name), "%s", val);
                        else if (strcmp(field, "font_path") == 0) {
                            snprintf(cfg->profiles[pi].font_path, sizeof(cfg->profiles[pi].font_path), "%s", val);
                            cfg->profiles[pi].has_font = true;
                        }
                        else if (strcmp(field, "theme") == 0) {
                            snprintf(cfg->profiles[pi].theme_name, sizeof(cfg->profiles[pi].theme_name), "%s", val);
                            cfg->profiles[pi].has_theme = true;
                        }
                }
            }
            /* Sites: site_N_name / site_N_path / site_N_command (string fields) */
            else if (strncmp(key, "site_", 5) == 0) {
                i32 si;
                const char *field;
                if (config_parse_index(key + 5, MAX_SITES, &si, &field)) {
                        if (strcmp(field, "name") == 0)
                            snprintf(cfg->sites[si].name, sizeof(cfg->sites[si].name), "%s", val);
                        else if (strcmp(field, "path") == 0)
                            snprintf(cfg->sites[si].path, sizeof(cfg->sites[si].path), "%s", val);
                        else if (strcmp(field, "command") == 0)
                            snprintf(cfg->sites[si].command, sizeof(cfg->sites[si].command), "%s", val);
                }
            }
        } else if (sscanf(line, " \"%127[^\"]\" : %511s", key, val) == 2) {
            members++;
            /* %s grabs everything up to the next whitespace, which means a
             * trailing comma from `"key": true,` is captured into val
             * itself. strcmp(val, "true") then misses, and every bool set
             * to true in the JSON silently flips to false. Trim the JSON
             * line-tail punctuation here once. */
            usize _vlen = strlen(val);
            while (_vlen > 0 && (val[_vlen-1] == ',' || val[_vlen-1] == '}' ||
                                 val[_vlen-1] == ' ' || val[_vlen-1] == '\t' ||
                                 val[_vlen-1] == '\r' || val[_vlen-1] == '\n')) {
                val[--_vlen] = '\0';
            }
            if (strcmp(key, "font_size") == 0) {
                cfg->font_size = config_f32_clamped(val, cfg->font_size, 6.0f, 96.0f);
            }
            else if (strcmp(key, "line_height") == 0) cfg->line_height = config_f32_clamped(val, cfg->line_height, 0.5f, 4.0f);
            /* Settings' "Line Height" stepper and its palette rows (they keep 0.8-2.0). */
            else if (strcmp(key, "cell_height_scale") == 0) cfg->cell_height_scale = config_f32_clamped(val, cfg->cell_height_scale, 0.5f, 2.5f);
            else if (strcmp(key, "cell_width_scale") == 0) cfg->cell_width_scale = config_f32_clamped(val, cfg->cell_width_scale, 0.5f, 2.5f);
            else if (strcmp(key, "font_normalize") == 0) cfg->font_normalize = strcmp(val, "true") == 0;
            else if (strcmp(key, "font_smoothing") == 0) cfg->font_smoothing = config_font_smoothing_parse(val);
            else if (strcmp(key, "font_weight") == 0) {
                /* Stroke-width passed straight into CGContext as a CGFloat —
                 * negative values flip the stroke direction and >2 produces
                 * pure white blobs. Clamp to the same envelope the settings
                 * panel exposes so an out-of-range JSON value can't break
                 * glyph rendering before the user can correct it. */
                cfg->font_weight = config_f32_clamped(val, cfg->font_weight, 0.0f, 2.0f);
            }
            else if (strcmp(key, "text_weight_terminal") == 0)
                cfg->text_weight_terminal = config_f32_clamped(val, cfg->text_weight_terminal,
                                                               TEXT_WEIGHT_MIN, TEXT_WEIGHT_MAX);
            else if (strcmp(key, "text_weight_ui") == 0)
                cfg->text_weight_ui = config_f32_clamped(val, cfg->text_weight_ui,
                                                         TEXT_WEIGHT_MIN, TEXT_WEIGHT_MAX);
            else if (strcmp(key, "scrollback") == 0) cfg->scrollback_lines = config_i32_or(val, cfg->scrollback_lines);
            else if (strcmp(key, "cursor_style") == 0) {
                i32 v = config_i32_or(val, (i32)cfg->cursor_style);
                if (v >= 0 && v <= 2) cfg->cursor_style = (u8)v;
            }
            else if (strcmp(key, "tab_sleep_idle_minutes") == 0) cfg->tab_sleep_idle_minutes = config_f32_clamped(val, cfg->tab_sleep_idle_minutes, 0.0f, 1440.0f);
            else if (strcmp(key, "sidebar_width") == 0) cfg->sidebar_width = config_f32_clamped(val, cfg->sidebar_width, 160.0f, 500.0f);
            else if (strcmp(key, "cursor_blink") == 0) cfg->cursor_blink = strcmp(val, "true") == 0;
            else if (strcmp(key, "visual_bell") == 0) cfg->visual_bell = strcmp(val, "true") == 0;
            else if (strcmp(key, "audible_bell") == 0) cfg->audible_bell = strcmp(val, "true") == 0;
            else if (strcmp(key, "confirm_close_agent") == 0) cfg->confirm_close_agent = strcmp(val, "true") == 0;
            else if (strcmp(key, "paste_protection") == 0) cfg->paste_protection = strcmp(val, "true") == 0;
            else if (strcmp(key, "agent_hooks") == 0) cfg->agent_hooks = strcmp(val, "true") == 0;
            else if (strcmp(key, "shell_integration") == 0) cfg->shell_integration = strcmp(val, "true") == 0;
            else if (strcmp(key, "command_marks") == 0) cfg->command_marks = strcmp(val, "true") == 0;
            else if (strcmp(key, "secure_keyboard_entry") == 0) cfg->secure_keyboard_entry = strcmp(val, "true") == 0;
            else if (strcmp(key, "secure_input_auto") == 0) cfg->secure_input_auto = strcmp(val, "true") == 0;
            else if (strcmp(key, "ssh_legacy_algorithms") == 0) cfg->ssh_legacy_algorithms = strcmp(val, "true") == 0;
            else if (strcmp(key, "command_history") == 0) cfg->command_history = strcmp(val, "true") == 0;
            else if (strcmp(key, "agent_hooks_notice_seen") == 0) cfg->agent_hooks_notice_seen = strcmp(val, "true") == 0;
            else if (strcmp(key, "bold_is_bright") == 0) cfg->bold_is_bright = strcmp(val, "true") == 0;
            else if (strcmp(key, "show_scrollbar") == 0) cfg->show_scrollbar = strcmp(val, "true") == 0;
            else if (strcmp(key, "scrollbar_autohide") == 0) cfg->scrollbar_autohide = strcmp(val, "true") == 0;
            else if (strcmp(key, "show_tab_bar") == 0) cfg->show_tab_bar = strcmp(val, "true") == 0;
            else if (strcmp(key, "sidebar_tabs") == 0) cfg->sidebar_tabs = strcmp(val, "true") == 0;
            else if (strcmp(key, "sidebar_compact") == 0) cfg->sidebar_compact = strcmp(val, "true") == 0;
            else if (strcmp(key, "sidebar_collapse_below") == 0) cfg->sidebar_collapse_below_pt = config_f32_clamped(val, cfg->sidebar_collapse_below_pt, 0.0f, 4000.0f);
            else if (strcmp(key, "show_toolbar_icons") == 0) cfg->show_toolbar_icons = strcmp(val, "true") == 0;
            else if (strcmp(key, "hide_title_bar") == 0) cfg->hide_title_bar = strcmp(val, "true") == 0;
            else if (strcmp(key, "show_status_bar") == 0) cfg->show_status_bar = strcmp(val, "true") == 0;
            else if (strcmp(key, "show_git_badge") == 0) cfg->show_git_badge = strcmp(val, "true") == 0;
            else if (strcmp(key, "browser_enabled") == 0) cfg->browser_enabled = strcmp(val, "true") == 0;
            else if (strcmp(key, "bidi_enabled") == 0) cfg->bidi_enabled = strcmp(val, "true") == 0;
            /* Clamp opacity so a bad JSON value can't render the window fully
             * transparent (and thus unrecoverable). Floor at 0.2 keeps it
             * visible; cap at 1.0 is fully opaque. */
            else if (strcmp(key, "opacity") == 0) cfg->opacity = config_f32_clamped(val, cfg->opacity, 0.2f, 1.0f);
            else if (strcmp(key, "padding") == 0) cfg->padding = config_f32_clamped(val, cfg->padding, 0.0f, 128.0f);
            else if (strcmp(key, "option_as_alt") == 0) cfg->option_as_alt = config_option_as_alt_parse(val);
            else if (strcmp(key, "prompt_recall") == 0) cfg->prompt_recall = config_prompt_recall_parse(val);
            else if (strcmp(key, "cursor_animate") == 0) cfg->cursor_animate = strcmp(val, "true") == 0;
            else if (strcmp(key, "notify_command_threshold") == 0) cfg->notify_command_threshold = config_f32_clamped(val, cfg->notify_command_threshold, 0.0f, 86400.0f);
            else if (strcmp(key, "allow_osc52_write") == 0) cfg->allow_osc52_write = strcmp(val, "true") == 0;
            else if (strcmp(key, "copy_on_select") == 0) cfg->copy_on_select = strcmp(val, "true") == 0;
            else if (strcmp(key, "clipboard_trim_trailing_spaces") == 0) cfg->clipboard_trim_trailing_spaces = strcmp(val, "true") == 0;
            /* "drag_selects", not "mouse_drag_selects": a config that carries
             * the older key holds the previous default (on) rather than a
             * choice anyone made, and the setting is off by default. The older
             * key is not read. */
            else if (strcmp(key, "drag_selects") == 0) cfg->mouse_drag_selects = strcmp(val, "true") == 0;
            else if (strcmp(key, "vault_auto_lock_minutes") == 0) {
                i32 v = config_i32_or(val, cfg->vault_auto_lock_minutes);
                if (v >= 0 && v <= 10080) cfg->vault_auto_lock_minutes = v;
            }
            else if (strcmp(key, "profile_count") == 0) {
                cfg->profile_count = config_i32_or(val, cfg->profile_count);
                if (cfg->profile_count > MAX_PROFILES) cfg->profile_count = MAX_PROFILES;
                if (cfg->profile_count < 0) cfg->profile_count = 0;
            }
            /* Profile numeric values: profile_N_font_size, profile_N_cursor_style, profile_N_opacity */
            else if (strncmp(key, "profile_", 8) == 0) {
                i32 pi;
                const char *field;
                if (config_parse_index(key + 8, MAX_PROFILES, &pi, &field)) {
                        if (strcmp(field, "font_size") == 0) {
                            cfg->profiles[pi].font_size = config_f32_clamped(
                                val, cfg->font_size, 6.0f, 96.0f);
                            cfg->profiles[pi].has_font = true;
                        } else if (strcmp(field, "cursor_style") == 0) {
                            i32 v = config_i32_or(val, -1);
                            if (v >= 0 && v <= 2) {
                                cfg->profiles[pi].cursor_style = (u8)v;
                                cfg->profiles[pi].has_cursor = true;
                            }
                        } else if (strcmp(field, "opacity") == 0) {
                            cfg->profiles[pi].opacity = config_f32_clamped(
                                val, cfg->opacity, 0.2f, 1.0f);
                            cfg->profiles[pi].has_opacity = true;
                        }
                }
            }
            else if (strcmp(key, "site_count") == 0) {
                cfg->site_count = config_i32_or(val, cfg->site_count);
                if (cfg->site_count > MAX_SITES) cfg->site_count = MAX_SITES;
                if (cfg->site_count < 0) cfg->site_count = 0;
            }
            /* Sites: site_N_port (numeric field) */
            else if (strncmp(key, "site_", 5) == 0) {
                i32 si;
                const char *field;
                if (config_parse_index(key + 5, MAX_SITES, &si, &field) &&
                    strcmp(field, "port") == 0) {
                    i32 port = config_i32_or(val, cfg->sites[si].port);
                    if (port >= 0 && port <= 65535) cfg->sites[si].port = port;
                }
            }
            /* Background image */
            else if (strcmp(key, "background_opacity") == 0) cfg->background_opacity = config_f32_clamped(val, cfg->background_opacity, 0.0f, 1.0f);
            else if (strcmp(key, "background_blur") == 0) cfg->background_blur = strcmp(val, "true") == 0;
            else if (strcmp(key, "background_blur_radius") == 0) cfg->background_blur_radius = config_f32_clamped(val, cfg->background_blur_radius, 0.0f, 50.0f);
            else if (strcmp(key, "enable_ligatures") == 0) cfg->enable_ligatures = strcmp(val, "true") == 0;
            /* Quake mode */
            else if (strcmp(key, "quake_mode") == 0) cfg->quake_mode = strcmp(val, "true") == 0;
            else if (strcmp(key, "quake_hotkey") == 0) snprintf(cfg->quake_hotkey, sizeof(cfg->quake_hotkey), "%s", val);
            else if (strcmp(key, "quake_height_ratio") == 0) cfg->quake_height_ratio = config_f32_clamped(val, cfg->quake_height_ratio, 0.1f, 1.0f);
            else if (strcmp(key, "quake_animation_duration") == 0) cfg->quake_animation_duration = config_f32_clamped(val, cfg->quake_animation_duration, 0.0f, 10.0f);
            /* Translate-on-Tab */
            else if (strcmp(key, "translate_enabled") == 0) cfg->translate.enabled = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_backend") == 0) {
                i32 b = config_i32_or(val, (i32)cfg->translate.backend);
                if (b < 0 || b > (i32)TRANSLATE_BACKEND_API) b = 0;
                cfg->translate.backend = (TranslateBackend)b;
            }
            else if (strcmp(key, "translate_tab_window_sec") == 0) cfg->translate.tab_window_sec = config_f32_clamped(val, cfg->translate.tab_window_sec, 0.05f, 10.0f);
            else if (strcmp(key, "translate_active_in_claude") == 0) cfg->translate.active_in_claude = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_codex") == 0) cfg->translate.active_in_codex = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_opencode") == 0) cfg->translate.active_in_opencode = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_grok") == 0) cfg->translate.active_in_grok = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_antigravity") == 0) cfg->translate.active_in_antigravity = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_cline") == 0) cfg->translate.active_in_cline = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_commandcode") == 0) cfg->translate.active_in_commandcode = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_pi") == 0) cfg->translate.active_in_pi = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_omp") == 0) cfg->translate.active_in_omp = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_openclaw") == 0) cfg->translate.active_in_openclaw = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_hermes") == 0) cfg->translate.active_in_hermes = strcmp(val, "true") == 0;
            else if (strcmp(key, "voice_enabled") == 0) cfg->voice.enabled = strcmp(val, "true") == 0;
            else if (strcmp(key, "voice_backend") == 0) {
                i32 b = config_i32_or(val, (i32)cfg->voice.backend);
                if (b < 0 || b > (i32)VOICE_BACKEND_API) b = 0;
                cfg->voice.backend = (VoiceBackend)b;
            }
            else if (strcmp(key, "voice_translate") == 0) cfg->voice.translate = strcmp(val, "true") == 0;
            else if (strcmp(key, "voice_auto_submit") == 0) cfg->voice.auto_submit = strcmp(val, "true") == 0;
            else if (strcmp(key, "voice_sounds") == 0) cfg->voice.sounds = strcmp(val, "true") == 0;
            else if (strcmp(key, "voice_max_seconds") == 0) cfg->voice.max_seconds = config_f32_clamped(val, cfg->voice.max_seconds, 2.0f, 900.0f);
            else if (strcmp(key, "voice_silence_stop_sec") == 0) cfg->voice.silence_stop_sec = config_f32_clamped(val, cfg->voice.silence_stop_sec, 0.0f, 30.0f);
            /* Style values. style_toolbar_height / style_tab_width are not
             * read — chrome geometry is compile-time in layout.h — so those
             * keys fall through and are ignored. */
            else if (strcmp(key, "style_tab_gap") == 0)           cfg->style.tab_gap = config_f32_clamped(val, cfg->style.tab_gap, 0.0f, 128.0f);
            else if (strcmp(key, "style_tab_dot_size") == 0)      cfg->style.tab_dot_size = config_f32_clamped(val, cfg->style.tab_dot_size, 0.0f, 128.0f);
            else if (strcmp(key, "style_tab_close_size") == 0)    cfg->style.tab_close_size = config_f32_clamped(val, cfg->style.tab_close_size, 0.0f, 128.0f);
            else if (strcmp(key, "style_tab_close_margin") == 0)  cfg->style.tab_close_margin = config_f32_clamped(val, cfg->style.tab_close_margin, 0.0f, 128.0f);
            else if (strcmp(key, "style_tab_indicator_height") == 0) cfg->style.tab_indicator_height = config_f32_clamped(val, cfg->style.tab_indicator_height, 0.0f, 128.0f);
            else if (strcmp(key, "style_status_bar_height") == 0) cfg->style.status_bar_height = config_f32_clamped(val, cfg->style.status_bar_height, 0.0f, 512.0f);
            else if (strcmp(key, "style_sidebar_default_width") == 0) cfg->style.sidebar_default_width = config_f32_clamped(val, cfg->style.sidebar_default_width, 40.0f, 4096.0f);
            else if (strcmp(key, "style_sidebar_min_width") == 0) cfg->style.sidebar_min_width = config_f32_clamped(val, cfg->style.sidebar_min_width, 40.0f, 4096.0f);
            else if (strcmp(key, "style_sidebar_max_width") == 0) cfg->style.sidebar_max_width = config_f32_clamped(val, cfg->style.sidebar_max_width, 40.0f, 4096.0f);
            else if (strcmp(key, "style_sidebar_header_height") == 0) cfg->style.sidebar_header_height = config_f32_clamped(val, cfg->style.sidebar_header_height, 0.0f, 512.0f);
            else if (strcmp(key, "style_terminal_padding") == 0)  cfg->style.terminal_padding = config_f32_clamped(val, cfg->style.terminal_padding, 0.0f, 512.0f);
            else if (strcmp(key, "style_terminal_top_gap") == 0)  cfg->style.terminal_top_gap = config_f32_clamped(val, cfg->style.terminal_top_gap, 0.0f, 512.0f);
            else if (strcmp(key, "style_active_pane_indicator_thickness") == 0) {
                cfg->style.active_pane_indicator_thickness = config_f32_clamped(
                    val, cfg->style.active_pane_indicator_thickness, 0.0f, 8.0f);
            }
            else if (strcmp(key, "style_active_pane_indicator_color") == 0) {
                Color color;
                if (config_parse_color(val, &color))
                    cfg->style.active_pane_indicator_color = color;
            }
        }
    }
    free(line);
    if (f) fclose(f);
    free(text);
    if (has_text && members == 0) {
        /* Not one member read: running on the defaults is all Liu can do, but
         * the file must not go with the next save. */
        config_keep_unread(filepath, raw, raw_n);
        g_config_unread = true;
        free(raw);
        return false;
    }
    free(raw);
    if (cfg->scrollback_lines < CONFIG_SCROLLBACK_MIN_LINES)
        cfg->scrollback_lines = CONFIG_SCROLLBACK_MIN_LINES;
    if (cfg->scrollback_lines > CONFIG_SCROLLBACK_MAX_LINES)
        cfg->scrollback_lines = CONFIG_SCROLLBACK_MAX_LINES;
    if (cfg->style.sidebar_min_width > cfg->style.sidebar_max_width) {
        Style defaults = style_default();
        cfg->style.sidebar_min_width = defaults.sidebar_min_width;
        cfg->style.sidebar_max_width = defaults.sidebar_max_width;
    }
    cfg->style.sidebar_default_width = CLAMP(cfg->style.sidebar_default_width,
                                             cfg->style.sidebar_min_width,
                                             cfg->style.sidebar_max_width);
    /* After BOTH chains: "-files,-agents,-notes" with tabs on the strip
     * would be an empty bar, and sidebar_tabs may be parsed after the list. */
    config_sidebar_ensure_section(cfg);
    config_font_path_replace_retired(cfg->font_path, sizeof cfg->font_path);
    for (i32 i = 0; i < MAX_PROFILES; i++)
        config_font_path_replace_retired(cfg->profiles[i].font_path, sizeof cfg->profiles[i].font_path);
    for (i32 i = 0; i < cfg->fallback_font_count && i < MAX_CONFIG_FALLBACK_FONTS; i++)
        config_font_path_replace_retired(cfg->fallback_fonts[i], sizeof cfg->fallback_fonts[i]);
    return true;
}

/* Emit a JSON string body with complete control-character escaping. The
 * opening/closing quotes are the caller's responsibility. */
static void config_fputs_json_escaped(FILE *f, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\b': fputs("\\b", f);  break;
        case '\f': fputs("\\f", f);  break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if (c >= 0x20) fputc((int)c, f);
            else fprintf(f, "\\u%04x", (unsigned)c);
            break;
        }
    }
}

static void config_fputs_json_string(FILE *f, const char *s) {
    fputc('"', f);
    config_fputs_json_escaped(f, s);
    fputc('"', f);
}

static void config_write_string_field(FILE *f, const char *name,
                                      const char *value) {
    fprintf(f, "  \"%s\": ", name);
    config_fputs_json_string(f, value);
    fprintf(f, ",\n");
}

/* Identity of the config file as Liu last wrote it. Compared against the file
 * on disk to tell our own atomic rename apart from a real external edit — see
 * config_file_changed_externally() in config.h for why that matters. */
static struct {
    bool  valid;
    dev_t dev;
    ino_t ino;
    off_t size;
    struct timespec mtime;
} g_config_self_write;

static void config_note_self_write(const char *path, const struct stat *st);

bool config_save(const AppConfig *cfg, const char *filepath) {
    if (!cfg || !filepath || !filepath[0] ||
        cfg->fallback_font_count < 0 ||
        cfg->fallback_font_count > MAX_CONFIG_FALLBACK_FONTS ||
        cfg->browser_recents_count < 0 ||
        cfg->browser_recents_count > BROWSER_RECENTS_MAX ||
        cfg->browser_allowlist.count < 0 ||
        cfg->browser_allowlist.count > BROWSER_ALLOWLIST_MAX ||
        cfg->profile_count < 0 || cfg->profile_count > MAX_PROFILES ||
        cfg->site_count < 0 || cfg->site_count > MAX_SITES) {
        return false;
    }

    /* Through a link to the file it names (a config kept in a dotfiles folder):
     * the link stays a link. */
    AtomicFileWriter writer;
    if (!atomic_file_begin_private_through(&writer, filepath)) return false;
    FILE *f = writer.stream;

    fprintf(f, "{\n");
    fprintf(f, "  \"font_path\": ");
    config_fputs_json_string(f, cfg->font_path);
    fprintf(f, ",\n");
    for (i32 i = 0; i < cfg->fallback_font_count; i++) {
        if (cfg->fallback_fonts[i][0]) {
            fprintf(f, "  \"fallback_font_%d\": ", i);
            config_fputs_json_string(f, cfg->fallback_fonts[i]);
            fprintf(f, ",\n");
        }
    }
    fprintf(f, "  \"font_features\": ");
    config_fputs_json_string(f, cfg->font_features);
    fprintf(f, ",\n");
    fprintf(f, "  \"font_variations\": ");
    config_fputs_json_string(f, cfg->font_variations);
    fprintf(f, ",\n");
    fprintf(f, "  \"font_codepoint_map\": ");
    config_fputs_json_string(f, cfg->font_codepoint_map);
    fprintf(f, ",\n");
    fprintf(f, "  \"font_size\": %.1f,\n", cfg->font_size);
    fprintf(f, "  \"line_height\": %.2f,\n", cfg->line_height);
    fprintf(f, "  \"cell_height_scale\": %.2f,\n", cfg->cell_height_scale);
    fprintf(f, "  \"cell_width_scale\": %.2f,\n", cfg->cell_width_scale);
    fprintf(f, "  \"font_normalize\": %s,\n", cfg->font_normalize ? "true" : "false");
    fprintf(f, "  \"font_smoothing\": \"%s\",\n", config_font_smoothing_name(cfg->font_smoothing));
    fprintf(f, "  \"font_weight\": %.2f,\n", cfg->font_weight);
    fprintf(f, "  \"text_weight_terminal\": %.2f,\n", cfg->text_weight_terminal);
    fprintf(f, "  \"text_weight_ui\": %.2f,\n", cfg->text_weight_ui);
    fprintf(f, "  \"theme\": ");
    config_fputs_json_string(f, cfg->theme_name);
    fprintf(f, ",\n");
    fprintf(f, "  \"scrollback\": %d,\n", cfg->scrollback_lines);
    fprintf(f, "  \"tab_sleep_idle_minutes\": %.1f,\n", cfg->tab_sleep_idle_minutes);
    fprintf(f, "  \"sidebar_width\": %.1f,\n", cfg->sidebar_width);
    fprintf(f, "  \"cursor_style\": %d,\n", cfg->cursor_style);
    fprintf(f, "  \"cursor_blink\": %s,\n", cfg->cursor_blink ? "true" : "false");
    fprintf(f, "  \"visual_bell\": %s,\n", cfg->visual_bell ? "true" : "false");
    fprintf(f, "  \"audible_bell\": %s,\n", cfg->audible_bell ? "true" : "false");
    fprintf(f, "  \"confirm_close_agent\": %s,\n", cfg->confirm_close_agent ? "true" : "false");
    fprintf(f, "  \"paste_protection\": %s,\n", cfg->paste_protection ? "true" : "false");
    fprintf(f, "  \"agent_hooks\": %s,\n", cfg->agent_hooks ? "true" : "false");
    fprintf(f, "  \"shell_integration\": %s,\n", cfg->shell_integration ? "true" : "false");
    fprintf(f, "  \"command_marks\": %s,\n", cfg->command_marks ? "true" : "false");
    fprintf(f, "  \"secure_keyboard_entry\": %s,\n", cfg->secure_keyboard_entry ? "true" : "false");
    fprintf(f, "  \"secure_input_auto\": %s,\n", cfg->secure_input_auto ? "true" : "false");
    fprintf(f, "  \"ssh_legacy_algorithms\": %s,\n", cfg->ssh_legacy_algorithms ? "true" : "false");
    fprintf(f, "  \"command_history\": %s,\n", cfg->command_history ? "true" : "false");
    fprintf(f, "  \"agent_hooks_notice_seen\": %s,\n", cfg->agent_hooks_notice_seen ? "true" : "false");
    fprintf(f, "  \"bold_is_bright\": %s,\n", cfg->bold_is_bright ? "true" : "false");
    fprintf(f, "  \"show_scrollbar\": %s,\n", cfg->show_scrollbar ? "true" : "false");
    fprintf(f, "  \"scrollbar_autohide\": %s,\n", cfg->scrollbar_autohide ? "true" : "false");
    fprintf(f, "  \"show_tab_bar\": %s,\n", cfg->show_tab_bar ? "true" : "false");
    fprintf(f, "  \"sidebar_tabs\": %s,\n", cfg->sidebar_tabs ? "true" : "false");
    fprintf(f, "  \"sidebar_compact\": %s,\n", cfg->sidebar_compact ? "true" : "false");
    fprintf(f, "  \"sidebar_collapse_below\": %.0f,\n", cfg->sidebar_collapse_below_pt);
    {
        char secs[64];
        config_format_sidebar_sections(cfg, secs, sizeof secs);
        fprintf(f, "  \"sidebar_sections\": \"%s\",\n", secs);
    }
    fprintf(f, "  \"show_toolbar_icons\": %s,\n", cfg->show_toolbar_icons ? "true" : "false");
    fprintf(f, "  \"hide_title_bar\": %s,\n", cfg->hide_title_bar ? "true" : "false");
    fprintf(f, "  \"show_status_bar\": %s,\n", cfg->show_status_bar ? "true" : "false");
    fprintf(f, "  \"show_git_badge\": %s,\n", cfg->show_git_badge ? "true" : "false");
    fprintf(f, "  \"browser_enabled\": %s,\n", cfg->browser_enabled ? "true" : "false");
    for (i32 i = 0; i < cfg->browser_allowlist.count; i++) {
        char name[64];
        snprintf(name, sizeof name, "browser_allowlist_%d", i);
        config_write_string_field(f, name, cfg->browser_allowlist.rules[i]);
    }
    fprintf(f, "  \"browser_last_url\": ");
    config_fputs_json_string(f, cfg->browser_last_url);
    fprintf(f, ",\n");
    for (i32 i = 0; i < cfg->browser_recents_count; i++) {
        if (cfg->browser_recents[i][0]) {
            fprintf(f, "  \"browser_recent_%d\": ", i);
            config_fputs_json_string(f, cfg->browser_recents[i]);
            fprintf(f, ",\n");
        }
    }
    fprintf(f, "  \"bidi_enabled\": %s,\n", cfg->bidi_enabled ? "true" : "false");
    fprintf(f, "  \"opacity\": %.2f,\n", cfg->opacity);
    fprintf(f, "  \"padding\": %.1f,\n", cfg->padding);
    fprintf(f, "  \"option_as_alt\": \"%s\",\n", config_option_as_alt_name(cfg->option_as_alt));
    fprintf(f, "  \"prompt_recall\": \"%s\",\n", config_prompt_recall_name(cfg->prompt_recall));
    fprintf(f, "  \"cursor_animate\": %s,\n", cfg->cursor_animate ? "true" : "false");
    fprintf(f, "  \"notify_command_threshold\": %.1f,\n", cfg->notify_command_threshold);
    fprintf(f, "  \"allow_osc52_write\": %s,\n", cfg->allow_osc52_write ? "true" : "false");
    fprintf(f, "  \"copy_on_select\": %s,\n", cfg->copy_on_select ? "true" : "false");
    fprintf(f, "  \"clipboard_trim_trailing_spaces\": %s,\n",
            cfg->clipboard_trim_trailing_spaces ? "true" : "false");
    fprintf(f, "  \"drag_selects\": %s,\n",
            cfg->mouse_drag_selects ? "true" : "false");
    fprintf(f, "  \"vault_auto_lock_minutes\": %d,\n", cfg->vault_auto_lock_minutes);
    fprintf(f, "  \"enable_ligatures\": %s,\n", cfg->enable_ligatures ? "true" : "false");
    /* Quake mode */
    fprintf(f, "  \"quake_mode\": %s,\n", cfg->quake_mode ? "true" : "false");
    fprintf(f, "  \"quake_hotkey\": ");
    config_fputs_json_string(f, cfg->quake_hotkey);
    fprintf(f, ",\n");
    fprintf(f, "  \"quake_height_ratio\": %.2f,\n", cfg->quake_height_ratio);
    fprintf(f, "  \"quake_animation_duration\": %.2f,\n", cfg->quake_animation_duration);
    /* Translate-on-Tab */
    fprintf(f, "  \"translate_enabled\": %s,\n", cfg->translate.enabled ? "true" : "false");
    fprintf(f, "  \"translate_backend\": %d,\n", (i32)cfg->translate.backend);
    config_write_string_field(f, "translate_agent_id", cfg->translate.agent_id);
    config_write_string_field(f, "translate_agent_model", cfg->translate.agent_model);
    config_write_string_field(f, "translate_local_path", cfg->translate.local_model_path);
    config_write_string_field(f, "translate_api_provider", cfg->translate.api_provider);
    config_write_string_field(f, "translate_api_model", cfg->translate.api_model);
    config_write_string_field(f, "translate_api_key", cfg->translate.api_key);
    config_write_string_field(f, "translate_api_base_url", cfg->translate.api_base_url);
    config_write_string_field(f, "translate_source_lang", cfg->translate.source_lang);
    config_write_string_field(f, "translate_target_lang", cfg->translate.target_lang);
    fprintf(f, "  \"translate_tab_window_sec\": %.2f,\n", cfg->translate.tab_window_sec);
    fprintf(f, "  \"translate_active_in_claude\": %s,\n",   cfg->translate.active_in_claude   ? "true" : "false");
    fprintf(f, "  \"translate_active_in_codex\": %s,\n",    cfg->translate.active_in_codex    ? "true" : "false");
    fprintf(f, "  \"translate_active_in_opencode\": %s,\n", cfg->translate.active_in_opencode ? "true" : "false");
    fprintf(f, "  \"translate_active_in_grok\": %s,\n",     cfg->translate.active_in_grok     ? "true" : "false");
    fprintf(f, "  \"translate_active_in_antigravity\": %s,\n", cfg->translate.active_in_antigravity ? "true" : "false");
    fprintf(f, "  \"translate_active_in_cline\": %s,\n",       cfg->translate.active_in_cline       ? "true" : "false");
    fprintf(f, "  \"translate_active_in_commandcode\": %s,\n", cfg->translate.active_in_commandcode ? "true" : "false");
    fprintf(f, "  \"translate_active_in_pi\": %s,\n",       cfg->translate.active_in_pi       ? "true" : "false");
    fprintf(f, "  \"translate_active_in_omp\": %s,\n",      cfg->translate.active_in_omp      ? "true" : "false");
    fprintf(f, "  \"translate_active_in_openclaw\": %s,\n", cfg->translate.active_in_openclaw ? "true" : "false");
    fprintf(f, "  \"translate_active_in_hermes\": %s,\n",   cfg->translate.active_in_hermes   ? "true" : "false");
    /* Voice input */
    fprintf(f, "  \"voice_enabled\": %s,\n", cfg->voice.enabled ? "true" : "false");
    fprintf(f, "  \"voice_backend\": %d,\n", (i32)cfg->voice.backend);
    config_write_string_field(f, "voice_model_path", cfg->voice.model_path);
    config_write_string_field(f, "voice_lang", cfg->voice.lang);
    config_write_string_field(f, "voice_api_provider", cfg->voice.api_provider);
    config_write_string_field(f, "voice_api_model", cfg->voice.api_model);
    config_write_string_field(f, "voice_api_key", cfg->voice.api_key);
    config_write_string_field(f, "voice_api_base_url", cfg->voice.api_base_url);
    config_write_string_field(f, "voice_input_device", cfg->voice.input_device);
    fprintf(f, "  \"voice_translate\": %s,\n", cfg->voice.translate ? "true" : "false");
    fprintf(f, "  \"voice_auto_submit\": %s,\n", cfg->voice.auto_submit ? "true" : "false");
    fprintf(f, "  \"voice_sounds\": %s,\n", cfg->voice.sounds ? "true" : "false");
    fprintf(f, "  \"voice_max_seconds\": %.1f,\n", cfg->voice.max_seconds);
    fprintf(f, "  \"voice_silence_stop_sec\": %.2f,\n", cfg->voice.silence_stop_sec);
    /* Style section — customizable UI dimensions (in points). Toolbar height
     * and tab width are compile-time (layout.h) and intentionally not emitted. */
    fprintf(f, "  \"style_tab_gap\": %.1f,\n", cfg->style.tab_gap);
    fprintf(f, "  \"style_tab_dot_size\": %.1f,\n", cfg->style.tab_dot_size);
    fprintf(f, "  \"style_tab_close_size\": %.1f,\n", cfg->style.tab_close_size);
    fprintf(f, "  \"style_tab_close_margin\": %.1f,\n", cfg->style.tab_close_margin);
    fprintf(f, "  \"style_tab_indicator_height\": %.1f,\n", cfg->style.tab_indicator_height);
    fprintf(f, "  \"style_status_bar_height\": %.1f,\n", cfg->style.status_bar_height);
    fprintf(f, "  \"style_sidebar_default_width\": %.1f,\n", cfg->style.sidebar_default_width);
    fprintf(f, "  \"style_sidebar_min_width\": %.1f,\n", cfg->style.sidebar_min_width);
    fprintf(f, "  \"style_sidebar_max_width\": %.1f,\n", cfg->style.sidebar_max_width);
    fprintf(f, "  \"style_sidebar_header_height\": %.1f,\n", cfg->style.sidebar_header_height);
    fprintf(f, "  \"style_terminal_padding\": %.1f,\n", cfg->style.terminal_padding);
    fprintf(f, "  \"style_terminal_top_gap\": %.1f,\n", cfg->style.terminal_top_gap);
    fprintf(f, "  \"style_active_pane_indicator_thickness\": %.1f,\n",
            cfg->style.active_pane_indicator_thickness);
    fprintf(f, "  \"style_active_pane_indicator_color\": \"%.3f,%.3f,%.3f,%.3f\",\n",
            cfg->style.active_pane_indicator_color.r,
            cfg->style.active_pane_indicator_color.g,
            cfg->style.active_pane_indicator_color.b,
            cfg->style.active_pane_indicator_color.a);
    /* Background image */
    if (cfg->background_image[0]) {
        fprintf(f, "  \"background_image\": ");
        config_fputs_json_string(f, cfg->background_image);
        fprintf(f, ",\n");
    }
    /* The pane's locale, when the user chose one. */
    if (cfg->lang[0]) {
        fprintf(f, "  \"lang\": ");
        config_fputs_json_string(f, cfg->lang);
        fprintf(f, ",\n");
    }
    /* TERM, when it is not the default one. */
    if (cfg->term[0] && strcmp(cfg->term, "xterm-256color") != 0) {
        fprintf(f, "  \"term\": ");
        config_fputs_json_string(f, cfg->term);
        fprintf(f, ",\n");
    }
    /* Notes/graph Vault root (empty => computed default). Escaped because it
     * is a user-picked filesystem path that may contain a quote/backslash. */
    if (cfg->notes_vault_path[0]) {
        fprintf(f, "  \"notes_vault_path\": ");
        config_fputs_json_string(f, cfg->notes_vault_path);
        fprintf(f, ",\n");
    }
    {
        const char *mode_str = "fill";
        switch (cfg->background_mode) {
            case 0: mode_str = "stretch"; break;
            case 1: mode_str = "center"; break;
            case 2: mode_str = "tile"; break;
            case 3: mode_str = "fill"; break;
        }
        fprintf(f, "  \"background_mode\": \"%s\",\n", mode_str);
    }
    fprintf(f, "  \"background_opacity\": %.2f,\n", cfg->background_opacity);
    fprintf(f, "  \"background_blur\": %s,\n", cfg->background_blur ? "true" : "false");
    fprintf(f, "  \"background_blur_radius\": %.1f,\n", cfg->background_blur_radius);
    /* Profiles */
    fprintf(f, "  \"profile_count\": %d,\n", cfg->profile_count);
    for (i32 i = 0; i < cfg->profile_count; i++) {
        const TermProfile *p = &cfg->profiles[i];
        fprintf(f, "  \"profile_%d_name\": ", i);
        config_fputs_json_string(f, p->name);
        fprintf(f, ",\n");
        if (p->has_font) {
            fprintf(f, "  \"profile_%d_font_path\": ", i);
            config_fputs_json_string(f, p->font_path);
            fprintf(f, ",\n");
            fprintf(f, "  \"profile_%d_font_size\": %.1f,\n", i, p->font_size);
        }
        if (p->has_theme) {
            fprintf(f, "  \"profile_%d_theme\": ", i);
            config_fputs_json_string(f, p->theme_name);
            fprintf(f, ",\n");
        }
        if (p->has_cursor) fprintf(f, "  \"profile_%d_cursor_style\": %d,\n", i, p->cursor_style);
        if (p->has_opacity) fprintf(f, "  \"profile_%d_opacity\": %.2f,\n", i, p->opacity);
    }
    /* Sites (dev-server manager registry) */
    fprintf(f, "  \"site_count\": %d,\n", cfg->site_count);
    for (i32 i = 0; i < cfg->site_count; i++) {
        const SiteConfig *st = &cfg->sites[i];
        fprintf(f, "  \"site_%d_name\": \"", i);
        config_fputs_json_escaped(f, st->name);
        fprintf(f, "\",\n");
        fprintf(f, "  \"site_%d_path\": \"", i);
        config_fputs_json_escaped(f, st->path);
        fprintf(f, "\",\n");
        fprintf(f, "  \"site_%d_command\": \"", i);
        config_fputs_json_escaped(f, st->command);
        fprintf(f, "\",\n");
        if (st->port) fprintf(f, "  \"site_%d_port\": %d,\n", i, st->port);
    }
    fprintf(f, "  \"_end\": 0\n");
    fprintf(f, "}\n");
    if (!atomic_file_commit(&writer)) return false;
    /* The identity comes from the descriptor Liu wrote, not from a stat()
     * of the path: a third party that replaces the file between the rename
     * and such a stat() would be recorded as Liu's own write, and the
     * watcher event carrying their edit would then compare equal and be
     * thrown away. */
    config_note_self_write(filepath,
                           writer.committed_valid ? &writer.committed_st : NULL);
    return true;
}

static bool config_stat_timespec_eq(struct timespec a, struct timespec b) {
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

static void config_note_self_write(const char *path, const struct stat *st) {
    /* Track writes only to the configured settings file. Saves to exports
     * or other paths must not affect the watcher's external-change check.
     */
    if (!path || !path[0] || strcmp(path, config_file_path()) != 0) return;
    if (!st) {
        /* No identity for a write Liu did make: fail open, so the next
         * watcher event reloads rather than silently swallowing a real edit.
         */
        g_config_self_write.valid = false;
        return;
    }
    g_config_self_write.valid = true;
    g_config_self_write.dev   = st->st_dev;
    g_config_self_write.ino   = st->st_ino;
    g_config_self_write.size  = st->st_size;
#if defined(__APPLE__)
    g_config_self_write.mtime = st->st_mtimespec;
#else
    g_config_self_write.mtime = st->st_mtim;
#endif
}

bool config_file_changed_externally(const char *path) {
    if (!g_config_self_write.valid) return true;
    struct stat st;
    if (!path || !path[0] || stat(path, &st) != 0) return true;
#if defined(__APPLE__)
    struct timespec mt = st.st_mtimespec;
#else
    struct timespec mt = st.st_mtim;
#endif
    /* Inode too: the atomic write replaces the file, so a stale inode means the
     * path now points at content Liu never wrote. */
    return !(st.st_dev == g_config_self_write.dev &&
             st.st_ino == g_config_self_write.ino &&
             st.st_size == g_config_self_write.size &&
             config_stat_timespec_eq(mt, g_config_self_write.mtime));
}

bool config_reload(AppConfig *cfg) {
    const char *path = config_file_path();
    AppConfig old = *cfg;
    AppConfig fresh = config_default();
    fresh.theme = theme_get_by_name(fresh.theme_name);
    if (!config_load(&fresh, path)) return false;
    *cfg = fresh;
    /* Check if anything actually changed */
    return memcmp(&old, cfg, sizeof(AppConfig)) != 0;
}
