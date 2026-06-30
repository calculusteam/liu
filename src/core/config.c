/*
 * Liu - configuration + built-in themes
 */
#include "core/config.h"
#include "core/string_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <unistd.h>

#ifdef PLATFORM_MACOS
    #include <mach-o/dyld.h>
#elif defined(PLATFORM_LINUX)
    #include <limits.h>
#endif

/* =========================================================================
 * Built-in themes
 * ========================================================================= */

const Theme THEME_DARK = {
    .name = "Dark",
    .fg  = { 0.94f, 0.95f, 0.97f, 1.0f },
    .bg  = { 0.03f, 0.04f, 0.05f, 1.0f },
    .cursor    = { 0.84f, 0.90f, 1.00f, 1.0f },
    .selection = { 0.16f, 0.25f, 0.40f, 0.72f },
    .ansi = {
        { 0.05f, 0.05f, 0.06f, 1.0f },  /* 0  Black */
        { 0.93f, 0.36f, 0.34f, 1.0f },  /* 1  Red */
        { 0.32f, 0.79f, 0.52f, 1.0f },  /* 2  Green */
        { 0.95f, 0.82f, 0.32f, 1.0f },  /* 3  Yellow */
        { 0.33f, 0.58f, 0.96f, 1.0f },  /* 4  Blue */
        { 0.79f, 0.50f, 0.88f, 1.0f },  /* 5  Magenta */
        { 0.30f, 0.80f, 0.86f, 1.0f },  /* 6  Cyan */
        { 0.88f, 0.90f, 0.93f, 1.0f },  /* 7  White */
        { 0.35f, 0.37f, 0.42f, 1.0f },  /* 8  Bright Black */
        { 0.98f, 0.49f, 0.47f, 1.0f },  /* 9  Bright Red */
        { 0.51f, 0.92f, 0.63f, 1.0f },  /* 10 Bright Green */
        { 0.99f, 0.92f, 0.45f, 1.0f },  /* 11 Bright Yellow */
        { 0.56f, 0.73f, 0.98f, 1.0f },  /* 12 Bright Blue */
        { 0.92f, 0.60f, 0.93f, 1.0f },  /* 13 Bright Magenta */
        { 0.49f, 0.92f, 0.96f, 1.0f },  /* 14 Bright Cyan */
        { 1.00f, 1.00f, 1.00f, 1.0f },  /* 15 Bright White */
    },
    .tab_bg         = { 0.05f, 0.06f, 0.07f, 1.0f },
    .tab_active_bg  = { 0.13f, 0.15f, 0.19f, 1.0f },
    .tab_active_fg  = { 1.00f, 1.00f, 1.00f, 1.0f },
    .tab_inactive_bg = { 0.08f, 0.09f, 0.11f, 1.0f },
    .tab_inactive_fg = { 0.58f, 0.60f, 0.66f, 1.0f },
    .sidebar_bg     = { 0.04f, 0.05f, 0.06f, 1.0f },
    .sidebar_fg     = { 0.79f, 0.81f, 0.84f, 1.0f },
    .sidebar_hover  = { 0.10f, 0.12f, 0.15f, 1.0f },
    .sidebar_active = { 0.18f, 0.27f, 0.42f, 1.0f },
    .border         = { 0.14f, 0.16f, 0.20f, 1.0f },
    .scrollbar      = { 0.08f, 0.09f, 0.11f, 1.0f },
    .scrollbar_thumb = { 0.28f, 0.31f, 0.36f, 1.0f },
    .status_bg      = { 0.05f, 0.06f, 0.07f, 1.0f },
    .status_fg      = { 0.61f, 0.64f, 0.69f, 1.0f },
};

const Theme THEME_LIGHT = {
    .name = "Light",
    .fg  = { 0.15f, 0.15f, 0.18f, 1.0f },
    .bg  = { 0.98f, 0.98f, 0.97f, 1.0f },
    .cursor    = { 0.20f, 0.20f, 0.25f, 1.0f },
    .selection = { 0.70f, 0.82f, 0.95f, 0.6f },
    .ansi = {
        { 0.10f, 0.10f, 0.12f, 1.0f },
        { 0.78f, 0.15f, 0.15f, 1.0f },
        { 0.05f, 0.55f, 0.30f, 1.0f },
        { 0.65f, 0.55f, 0.00f, 1.0f },
        { 0.10f, 0.35f, 0.70f, 1.0f },
        { 0.60f, 0.20f, 0.60f, 1.0f },
        { 0.05f, 0.50f, 0.60f, 1.0f },
        { 0.55f, 0.55f, 0.55f, 1.0f },
        { 0.35f, 0.35f, 0.40f, 1.0f },
        { 0.90f, 0.25f, 0.25f, 1.0f },
        { 0.10f, 0.65f, 0.38f, 1.0f },
        { 0.75f, 0.65f, 0.05f, 1.0f },
        { 0.18f, 0.45f, 0.80f, 1.0f },
        { 0.70f, 0.30f, 0.70f, 1.0f },
        { 0.10f, 0.60f, 0.70f, 1.0f },
        { 0.25f, 0.25f, 0.28f, 1.0f },
    },
    .tab_bg          = { 0.92f, 0.92f, 0.93f, 1.0f },
    .tab_active_bg   = { 0.98f, 0.98f, 0.97f, 1.0f },
    .tab_active_fg   = { 0.10f, 0.10f, 0.12f, 1.0f },
    .tab_inactive_bg = { 0.90f, 0.90f, 0.91f, 1.0f },
    .tab_inactive_fg = { 0.55f, 0.55f, 0.58f, 1.0f },
    .sidebar_bg      = { 0.94f, 0.94f, 0.95f, 1.0f },
    .sidebar_fg      = { 0.25f, 0.25f, 0.30f, 1.0f },
    .sidebar_hover   = { 0.88f, 0.88f, 0.90f, 1.0f },
    .sidebar_active  = { 0.70f, 0.82f, 0.95f, 1.0f },
    .border          = { 0.82f, 0.82f, 0.85f, 1.0f },
    .scrollbar       = { 0.90f, 0.90f, 0.92f, 1.0f },
    .scrollbar_thumb = { 0.70f, 0.70f, 0.75f, 1.0f },
    .status_bg       = { 0.92f, 0.92f, 0.93f, 1.0f },
    .status_fg       = { 0.40f, 0.40f, 0.45f, 1.0f },
};

const Theme THEME_SOLARIZED_DARK = {
    .name = "Solarized Dark",
    .fg  = { 0.51f, 0.58f, 0.59f, 1.0f },
    .bg  = { 0.00f, 0.17f, 0.21f, 1.0f },
    .cursor    = { 0.58f, 0.63f, 0.63f, 1.0f },
    .selection = { 0.07f, 0.26f, 0.33f, 0.8f },
    .ansi = {
        { 0.03f, 0.21f, 0.26f, 1.0f },  /* base02 */
        { 0.86f, 0.20f, 0.18f, 1.0f },  /* red */
        { 0.52f, 0.60f, 0.00f, 1.0f },  /* green */
        { 0.71f, 0.54f, 0.00f, 1.0f },  /* yellow */
        { 0.15f, 0.55f, 0.82f, 1.0f },  /* blue */
        { 0.83f, 0.21f, 0.51f, 1.0f },  /* magenta */
        { 0.16f, 0.63f, 0.60f, 1.0f },  /* cyan */
        { 0.93f, 0.91f, 0.84f, 1.0f },  /* base2 */
        { 0.00f, 0.17f, 0.21f, 1.0f },  /* base03 */
        { 0.80f, 0.29f, 0.09f, 1.0f },  /* orange */
        { 0.35f, 0.43f, 0.46f, 1.0f },  /* base01 */
        { 0.40f, 0.48f, 0.51f, 1.0f },  /* base00 */
        { 0.51f, 0.58f, 0.59f, 1.0f },  /* base0 */
        { 0.42f, 0.44f, 0.77f, 1.0f },  /* violet */
        { 0.58f, 0.63f, 0.63f, 1.0f },  /* base1 */
        { 0.99f, 0.96f, 0.89f, 1.0f },  /* base3 */
    },
    .tab_bg          = { 0.00f, 0.14f, 0.18f, 1.0f },
    .tab_active_bg   = { 0.03f, 0.21f, 0.26f, 1.0f },
    .tab_active_fg   = { 0.58f, 0.63f, 0.63f, 1.0f },
    .tab_inactive_bg = { 0.00f, 0.17f, 0.21f, 1.0f },
    .tab_inactive_fg = { 0.35f, 0.43f, 0.46f, 1.0f },
    .sidebar_bg      = { 0.00f, 0.14f, 0.18f, 1.0f },
    .sidebar_fg      = { 0.51f, 0.58f, 0.59f, 1.0f },
    .sidebar_hover   = { 0.03f, 0.21f, 0.26f, 1.0f },
    .sidebar_active  = { 0.07f, 0.26f, 0.33f, 1.0f },
    .border          = { 0.03f, 0.21f, 0.26f, 1.0f },
    .scrollbar       = { 0.00f, 0.17f, 0.21f, 1.0f },
    .scrollbar_thumb = { 0.07f, 0.26f, 0.33f, 1.0f },
    .status_bg       = { 0.00f, 0.14f, 0.18f, 1.0f },
    .status_fg       = { 0.40f, 0.48f, 0.51f, 1.0f },
};

const Theme THEME_MONOKAI = {
    .name = "Monokai",
    .fg  = { 0.97f, 0.97f, 0.94f, 1.0f },   /* #F8F8F2 */
    .bg  = { 0.16f, 0.16f, 0.13f, 1.0f },   /* #272822 */
    .cursor    = { 0.97f, 0.97f, 0.94f, 1.0f },
    .selection = { 0.29f, 0.28f, 0.25f, 0.8f },  /* #49483E */
    .ansi = {
        { 0.16f, 0.16f, 0.13f, 1.0f },  /* 0  black   #272822 */
        { 0.98f, 0.15f, 0.45f, 1.0f },  /* 1  red     #F92672 (was #F22644) */
        { 0.65f, 0.89f, 0.18f, 1.0f },  /* 2  green   #A6E22E */
        { 0.90f, 0.86f, 0.45f, 1.0f },  /* 3  yellow  #E6DB74 */
        { 0.40f, 0.85f, 0.94f, 1.0f },  /* 4  blue    #66D9EF (teal-blue) */
        { 0.68f, 0.51f, 1.00f, 1.0f },  /* 5  magenta #AE81FF */
        { 0.63f, 0.94f, 0.89f, 1.0f },  /* 6  cyan    #A1EFE4 (was green dup) */
        { 0.97f, 0.97f, 0.94f, 1.0f },  /* 7  white   #F8F8F2 */
        { 0.46f, 0.44f, 0.37f, 1.0f },  /* 8  bright black   #75715E */
        { 0.98f, 0.15f, 0.45f, 1.0f },  /* 9  bright red     #F92672 */
        { 0.65f, 0.89f, 0.18f, 1.0f },  /* 10 bright green   #A6E22E */
        { 0.90f, 0.86f, 0.45f, 1.0f },  /* 11 bright yellow  #E6DB74 */
        { 0.40f, 0.85f, 0.94f, 1.0f },  /* 12 bright blue    #66D9EF */
        { 0.68f, 0.51f, 1.00f, 1.0f },  /* 13 bright magenta #AE81FF */
        { 0.63f, 0.94f, 0.89f, 1.0f },  /* 14 bright cyan    #A1EFE4 (was green dup) */
        { 0.97f, 0.97f, 0.94f, 1.0f },  /* 15 bright white   #F8F8F2 */
    },
    .tab_bg          = { 0.13f, 0.13f, 0.12f, 1.0f },
    .tab_active_bg   = { 0.22f, 0.22f, 0.20f, 1.0f },
    .tab_active_fg   = { 0.97f, 0.97f, 0.94f, 1.0f },
    .tab_inactive_bg = { 0.16f, 0.16f, 0.15f, 1.0f },
    .tab_inactive_fg = { 0.55f, 0.53f, 0.45f, 1.0f },
    .sidebar_bg      = { 0.13f, 0.13f, 0.12f, 1.0f },
    .sidebar_fg      = { 0.80f, 0.80f, 0.76f, 1.0f },
    .sidebar_hover   = { 0.22f, 0.22f, 0.20f, 1.0f },
    .sidebar_active  = { 0.29f, 0.29f, 0.27f, 1.0f },
    .border          = { 0.22f, 0.22f, 0.20f, 1.0f },
    .scrollbar       = { 0.16f, 0.16f, 0.15f, 1.0f },
    .scrollbar_thumb = { 0.29f, 0.29f, 0.27f, 1.0f },
    .status_bg       = { 0.13f, 0.13f, 0.12f, 1.0f },
    .status_fg       = { 0.55f, 0.53f, 0.45f, 1.0f },
};

const Theme THEME_DRACULA = {
    .name = "Dracula",
    .fg  = { 0.97f, 0.97f, 0.95f, 1.0f },   /* #F8F8F2 */
    .bg  = { 0.16f, 0.16f, 0.21f, 1.0f },   /* #282A36 */
    .cursor    = { 0.97f, 0.97f, 0.95f, 1.0f },
    .selection = { 0.27f, 0.28f, 0.35f, 0.8f },  /* #44475A */
    .ansi = {
        { 0.13f, 0.13f, 0.17f, 1.0f },  /* 0  black   #21222C */
        { 1.00f, 0.33f, 0.33f, 1.0f },  /* 1  red     #FF5555 */
        { 0.31f, 0.98f, 0.48f, 1.0f },  /* 2  green   #50FA7B */
        { 0.95f, 0.98f, 0.55f, 1.0f },  /* 3  yellow  #F1FA8C */
        { 0.74f, 0.58f, 0.98f, 1.0f },  /* 4  blue    #BD93F9 (purple) */
        { 1.00f, 0.47f, 0.78f, 1.0f },  /* 5  magenta #FF79C6 (was blue 0.66) */
        { 0.55f, 0.91f, 0.99f, 1.0f },  /* 6  cyan    #8BE9FD */
        { 0.97f, 0.97f, 0.95f, 1.0f },  /* 7  white   #F8F8F2 */
        { 0.40f, 0.42f, 0.53f, 1.0f },  /* 8  bright black   #6272A4 */
        { 1.00f, 0.42f, 0.42f, 1.0f },  /* 9  bright red     #FF6E6E */
        { 0.41f, 0.98f, 0.56f, 1.0f },  /* 10 bright green   #69FF94 */
        { 0.95f, 0.98f, 0.64f, 1.0f },  /* 11 bright yellow  #FFFFA5 */
        { 0.80f, 0.66f, 0.98f, 1.0f },  /* 12 bright blue    #D6ACFF */
        { 1.00f, 0.57f, 0.87f, 1.0f },  /* 13 bright magenta #FF92DF (was blue 0.73) */
        { 0.65f, 0.93f, 0.99f, 1.0f },  /* 14 bright cyan    #A4FFFF */
        { 1.00f, 1.00f, 1.00f, 1.0f },  /* 15 bright white   #FFFFFF */
    },
    .tab_bg          = { 0.13f, 0.14f, 0.18f, 1.0f },
    .tab_active_bg   = { 0.27f, 0.28f, 0.35f, 1.0f },
    .tab_active_fg   = { 0.97f, 0.97f, 0.95f, 1.0f },
    .tab_inactive_bg = { 0.16f, 0.16f, 0.21f, 1.0f },
    .tab_inactive_fg = { 0.40f, 0.42f, 0.53f, 1.0f },
    .sidebar_bg      = { 0.13f, 0.14f, 0.18f, 1.0f },
    .sidebar_fg      = { 0.74f, 0.75f, 0.80f, 1.0f },
    .sidebar_hover   = { 0.22f, 0.23f, 0.29f, 1.0f },
    .sidebar_active  = { 0.27f, 0.28f, 0.35f, 1.0f },
    .border          = { 0.27f, 0.28f, 0.35f, 1.0f },
    .scrollbar       = { 0.16f, 0.16f, 0.21f, 1.0f },
    .scrollbar_thumb = { 0.40f, 0.42f, 0.53f, 1.0f },
    .status_bg       = { 0.13f, 0.14f, 0.18f, 1.0f },
    .status_fg       = { 0.40f, 0.42f, 0.53f, 1.0f },
};

const Theme THEME_NORD = {
    .name = "Nord",
    .fg  = { 0.85f, 0.87f, 0.91f, 1.0f },
    .bg  = { 0.18f, 0.20f, 0.25f, 1.0f },
    .cursor    = { 0.81f, 0.87f, 0.96f, 1.0f },
    .selection = { 0.26f, 0.30f, 0.37f, 0.8f },
    .ansi = {
        { 0.23f, 0.26f, 0.32f, 1.0f },
        { 0.75f, 0.38f, 0.42f, 1.0f },
        { 0.64f, 0.75f, 0.55f, 1.0f },
        { 0.92f, 0.80f, 0.55f, 1.0f },
        { 0.51f, 0.63f, 0.76f, 1.0f },
        { 0.71f, 0.56f, 0.68f, 1.0f },
        { 0.53f, 0.75f, 0.82f, 1.0f },
        { 0.91f, 0.93f, 0.94f, 1.0f },
        { 0.30f, 0.34f, 0.42f, 1.0f },
        { 0.75f, 0.38f, 0.42f, 1.0f },
        { 0.64f, 0.75f, 0.55f, 1.0f },
        { 0.92f, 0.80f, 0.55f, 1.0f },
        { 0.51f, 0.63f, 0.76f, 1.0f },
        { 0.71f, 0.56f, 0.68f, 1.0f },
        { 0.56f, 0.74f, 0.73f, 1.0f },
        { 0.93f, 0.94f, 0.96f, 1.0f },
    },
    .tab_bg          = { 0.18f, 0.20f, 0.25f, 1.0f },
    .tab_active_bg   = { 0.26f, 0.30f, 0.37f, 1.0f },
    .tab_active_fg   = { 0.85f, 0.87f, 0.91f, 1.0f },
    .tab_inactive_bg = { 0.21f, 0.24f, 0.30f, 1.0f },
    .tab_inactive_fg = { 0.49f, 0.55f, 0.65f, 1.0f },
    .sidebar_bg      = { 0.18f, 0.20f, 0.25f, 1.0f },
    .sidebar_fg      = { 0.81f, 0.83f, 0.87f, 1.0f },
    .sidebar_hover   = { 0.23f, 0.26f, 0.32f, 1.0f },
    .sidebar_active  = { 0.26f, 0.30f, 0.37f, 1.0f },
    .border          = { 0.23f, 0.26f, 0.32f, 1.0f },
    .scrollbar       = { 0.21f, 0.24f, 0.30f, 1.0f },
    .scrollbar_thumb = { 0.30f, 0.34f, 0.42f, 1.0f },
    .status_bg       = { 0.18f, 0.20f, 0.25f, 1.0f },
    .status_fg       = { 0.49f, 0.55f, 0.65f, 1.0f },
};

const Theme THEME_GRUVBOX = {
    .name = "Gruvbox",
    .fg  = { 0.92f, 0.86f, 0.70f, 1.0f },   /* #EBDBB2 */
    .bg  = { 0.16f, 0.16f, 0.16f, 1.0f },   /* #282828 (was yellow-tinted) */
    .cursor    = { 0.92f, 0.86f, 0.70f, 1.0f },
    .selection = { 0.27f, 0.26f, 0.23f, 0.8f },
    .ansi = {
        { 0.16f, 0.16f, 0.16f, 1.0f },  /* 0  black         #282828 */
        { 0.80f, 0.14f, 0.11f, 1.0f },  /* 1  red           #CC241D */
        { 0.60f, 0.59f, 0.10f, 1.0f },  /* 2  green         #98971A */
        { 0.84f, 0.60f, 0.13f, 1.0f },  /* 3  yellow        #D79921 */
        { 0.27f, 0.52f, 0.53f, 1.0f },  /* 4  blue          #458588 */
        { 0.69f, 0.38f, 0.53f, 1.0f },  /* 5  magenta       #B16286 (was blue 0.61) */
        { 0.41f, 0.62f, 0.42f, 1.0f },  /* 6  cyan          #689D6A */
        { 0.66f, 0.60f, 0.52f, 1.0f },  /* 7  white         #A89984 */
        { 0.57f, 0.51f, 0.45f, 1.0f },  /* 8  bright black  #928374 */
        { 0.98f, 0.29f, 0.20f, 1.0f },  /* 9  bright red    #FB4934 */
        { 0.72f, 0.73f, 0.15f, 1.0f },  /* 10 bright green  #B8BB26 */
        { 0.98f, 0.74f, 0.18f, 1.0f },  /* 11 bright yellow #FABD2F */
        { 0.51f, 0.65f, 0.60f, 1.0f },  /* 12 bright blue   #83A598 */
        { 0.83f, 0.53f, 0.61f, 1.0f },  /* 13 bright magenta #D3869B (was blue 0.73) */
        { 0.56f, 0.75f, 0.49f, 1.0f },  /* 14 bright cyan   #8EC07C */
        { 0.92f, 0.86f, 0.70f, 1.0f },  /* 15 bright white  #EBDBB2 */
    },
    .tab_bg          = { 0.13f, 0.12f, 0.10f, 1.0f },
    .tab_active_bg   = { 0.27f, 0.26f, 0.23f, 1.0f },
    .tab_active_fg   = { 0.92f, 0.86f, 0.70f, 1.0f },
    .tab_inactive_bg = { 0.16f, 0.15f, 0.13f, 1.0f },
    .tab_inactive_fg = { 0.57f, 0.51f, 0.45f, 1.0f },
    .sidebar_bg      = { 0.13f, 0.12f, 0.10f, 1.0f },
    .sidebar_fg      = { 0.84f, 0.78f, 0.64f, 1.0f },
    .sidebar_hover   = { 0.22f, 0.21f, 0.18f, 1.0f },
    .sidebar_active  = { 0.27f, 0.26f, 0.23f, 1.0f },
    .border          = { 0.27f, 0.26f, 0.23f, 1.0f },
    .scrollbar       = { 0.16f, 0.15f, 0.13f, 1.0f },
    .scrollbar_thumb = { 0.35f, 0.33f, 0.28f, 1.0f },
    .status_bg       = { 0.13f, 0.12f, 0.10f, 1.0f },
    .status_fg       = { 0.57f, 0.51f, 0.45f, 1.0f },
};

const Theme THEME_CATPPUCCIN_MOCHA = {
    .name = "Catppuccin Mocha",
    .fg  = { 0.80f, 0.84f, 0.96f, 1.0f },   /* Text #CDD6F4 */
    .bg  = { 0.12f, 0.12f, 0.18f, 1.0f },   /* Base #1E1E2E */
    .cursor    = { 0.95f, 0.55f, 0.66f, 1.0f },  /* Rosewater substituted w/ Red pink */
    .selection = { 0.27f, 0.28f, 0.35f, 0.8f },  /* Surface1 #45475A */
    .ansi = {
        { 0.27f, 0.28f, 0.35f, 1.0f },  /* 0  black   Surface1 #45475A */
        { 0.95f, 0.55f, 0.66f, 1.0f },  /* 1  red     #F38BA8 */
        { 0.65f, 0.89f, 0.63f, 1.0f },  /* 2  green   #A6E3A1 */
        { 0.98f, 0.89f, 0.69f, 1.0f },  /* 3  yellow  #F9E2AF (was 0.53 blue) */
        { 0.54f, 0.71f, 0.98f, 1.0f },  /* 4  blue    #89B4FA */
        { 0.80f, 0.65f, 0.97f, 1.0f },  /* 5  magenta Mauve #CBA6F7 */
        { 0.54f, 0.86f, 0.92f, 1.0f },  /* 6  cyan    Sky #89DCEB */
        { 0.73f, 0.76f, 0.87f, 1.0f },  /* 7  white   Subtext1 #BAC2DE */
        { 0.35f, 0.38f, 0.48f, 1.0f },  /* 8  bright black   Surface2 #585B70 */
        { 0.95f, 0.55f, 0.66f, 1.0f },  /* 9  bright red     #F38BA8 */
        { 0.65f, 0.89f, 0.63f, 1.0f },  /* 10 bright green   #A6E3A1 */
        { 0.98f, 0.89f, 0.69f, 1.0f },  /* 11 bright yellow  #F9E2AF */
        { 0.54f, 0.71f, 0.98f, 1.0f },  /* 12 bright blue    #89B4FA */
        { 0.80f, 0.65f, 0.97f, 1.0f },  /* 13 bright magenta #CBA6F7 */
        { 0.54f, 0.86f, 0.92f, 1.0f },  /* 14 bright cyan    #89DCEB */
        { 0.80f, 0.84f, 0.96f, 1.0f },  /* 15 bright white   #CDD6F4 */
    },
    .tab_bg          = { 0.10f, 0.10f, 0.15f, 1.0f },
    .tab_active_bg   = { 0.18f, 0.19f, 0.27f, 1.0f },
    .tab_active_fg   = { 0.81f, 0.84f, 0.95f, 1.0f },
    .tab_inactive_bg = { 0.12f, 0.12f, 0.18f, 1.0f },
    .tab_inactive_fg = { 0.44f, 0.45f, 0.56f, 1.0f },
    .sidebar_bg      = { 0.10f, 0.10f, 0.15f, 1.0f },
    .sidebar_fg      = { 0.73f, 0.74f, 0.84f, 1.0f },
    .sidebar_hover   = { 0.18f, 0.19f, 0.27f, 1.0f },
    .sidebar_active  = { 0.23f, 0.24f, 0.34f, 1.0f },
    .border          = { 0.18f, 0.19f, 0.27f, 1.0f },
    .scrollbar       = { 0.12f, 0.12f, 0.18f, 1.0f },
    .scrollbar_thumb = { 0.27f, 0.28f, 0.35f, 1.0f },
    .status_bg       = { 0.10f, 0.10f, 0.15f, 1.0f },
    .status_fg       = { 0.44f, 0.45f, 0.56f, 1.0f },
};

/* Liu theme — neutral #121212 black with a #C674F3 signature purple.
 * Dark surfaces are neutral greys derived from the #121212 background; the
 * cursor, selection, borders, active surfaces and the purple ANSI slots carry
 * the #C674F3 accent so the "main colour" reads consistently across terminal +
 * UI. The rose/mint/gold/cyan ANSI accents are unchanged. */
const Theme THEME_Liu = {
    .name = "Liu",
    .fg  = { 0.95f, 0.93f, 0.98f, 1.0f },
    .bg  = { 0.071f, 0.071f, 0.071f, 1.0f },   /* #121212 */
    .cursor    = { 0.83f, 0.62f, 0.97f, 1.0f },   /* light #C674F3 */
    .selection = { 0.34f, 0.19f, 0.42f, 0.72f },  /* dark #C674F3 wash */
    .ansi = {
        { 0.071f, 0.071f, 0.071f, 1.0f },  /* 0  black = bg #121212 */
        { 0.96f, 0.48f, 0.68f, 1.0f },     /* 1  rose */
        { 0.53f, 0.88f, 0.71f, 1.0f },     /* 2  mint */
        { 0.98f, 0.82f, 0.39f, 1.0f },     /* 3  gold */
        { 0.49f, 0.64f, 0.99f, 1.0f },     /* 4  violet-blue */
        { 0.62f, 0.30f, 0.84f, 1.0f },     /* 5  purple (accent family) */
        { 0.47f, 0.87f, 0.92f, 1.0f },     /* 6  cyan */
        { 0.92f, 0.88f, 1.00f, 1.0f },     /* 7  soft white */
        { 0.24f, 0.22f, 0.28f, 1.0f },     /* 8  bright black */
        { 0.99f, 0.64f, 0.77f, 1.0f },     /* 9  bright rose */
        { 0.70f, 0.95f, 0.82f, 1.0f },     /* 10 bright mint */
        { 1.00f, 0.91f, 0.53f, 1.0f },     /* 11 bright gold */
        { 0.71f, 0.81f, 1.00f, 1.0f },     /* 12 bright violet-blue */
        { 0.84f, 0.58f, 0.98f, 1.0f },     /* 13 bright purple ≈ #C674F3 */
        { 0.67f, 0.95f, 0.98f, 1.0f },     /* 14 bright cyan */
        { 0.98f, 0.97f, 1.00f, 1.0f },     /* 15 bright white */
    },
    .tab_bg          = { 0.071f, 0.071f, 0.071f, 1.0f },   /* #121212 */
    .tab_active_bg   = { 0.22f, 0.12f, 0.28f, 1.0f },      /* dark #C674F3 */
    .tab_active_fg   = { 0.98f, 0.97f, 1.00f, 1.0f },
    .tab_inactive_bg = { 0.102f, 0.102f, 0.102f, 1.0f },   /* #1a1a1a */
    .tab_inactive_fg = { 0.58f, 0.54f, 0.66f, 1.0f },
    .sidebar_bg      = { 0.086f, 0.086f, 0.086f, 1.0f },   /* #161616 */
    .sidebar_fg      = { 0.86f, 0.82f, 0.95f, 1.0f },
    .sidebar_hover   = { 0.15f, 0.11f, 0.19f, 1.0f },
    .sidebar_active  = { 0.26f, 0.14f, 0.33f, 1.0f },
    .border          = { 0.27f, 0.16f, 0.35f, 1.0f },      /* #C674F3-tinted */
    .scrollbar       = { 0.102f, 0.102f, 0.102f, 1.0f },
    .scrollbar_thumb = { 0.48f, 0.28f, 0.62f, 1.0f },
    .status_bg       = { 0.086f, 0.086f, 0.086f, 1.0f },   /* #161616 */
    .status_fg       = { 0.62f, 0.58f, 0.74f, 1.0f },
    /* Signature chrome accent — #C674F3. Drives every primary button, palette
     * pill, tab underline, focused-field underline, scrollbar thumb and the
     * Settings selection, so the theme's identity colour is the same one the
     * terminal palette carries. Luminance is high enough that white labels on
     * it stay legible (chrome_legible_on flips to dark text where needed). */
    .ui_accent       = { 0.776f, 0.455f, 0.953f, 1.0f },   /* #C674F3 */
};

/* A minimalist theme.
 *
 * Palette tuned to two-colour chrome: #0A0A0E for every container
 * (terminal bg, tab bar, sidebar, status bar) and #141416 for every
 * border / hover / active-selection accent. Gives the UI a flat,
 * uniform body with a single one-step-brighter shade used everywhere
 * a piece of chrome needs to read as "interactive". */
const Theme THEME_KITTY = {
    .name = "kitty",
    .fg  = { 0.87f, 0.87f, 0.87f, 1.0f },             /* #dddddd */
    .bg  = { 0.0392f, 0.0392f, 0.0549f, 1.0f },       /* #0A0A0E */
    .cursor    = { 0.90f, 0.90f, 0.90f, 1.0f },
    .selection = { 0.30f, 0.30f, 0.30f, 0.72f },
    .ansi = {
        { 0.0392f, 0.0392f, 0.0549f, 1.0f },  /* 0  black: #0A0A0E (matches bg) */
        { 0.80f, 0.00f, 0.00f, 1.0f },  /* 1  red: #cc0000 */
        { 0.30f, 0.76f, 0.02f, 1.0f },  /* 2  green: #4e9a06 */
        { 0.77f, 0.63f, 0.00f, 1.0f },  /* 3  yellow: #c4a000 */
        { 0.20f, 0.40f, 0.64f, 1.0f },  /* 4  blue: #3465a4 */
        { 0.46f, 0.31f, 0.48f, 1.0f },  /* 5  magenta: #75507b */
        { 0.02f, 0.60f, 0.60f, 1.0f },  /* 6  cyan: #06989a */
        { 0.83f, 0.83f, 0.83f, 1.0f },  /* 7  white: #d3d7cf */
        { 0.33f, 0.34f, 0.33f, 1.0f },  /* 8  bright black: #555753 */
        { 0.94f, 0.16f, 0.16f, 1.0f },  /* 9  bright red: #ef2929 */
        { 0.54f, 0.89f, 0.15f, 1.0f },  /* 10 bright green: #8ae234 */
        { 0.99f, 0.91f, 0.31f, 1.0f },  /* 11 bright yellow: #fce94f */
        { 0.45f, 0.62f, 0.82f, 1.0f },  /* 12 bright blue: #729fcf */
        { 0.68f, 0.49f, 0.66f, 1.0f },  /* 13 bright magenta: #ad7fa8 */
        { 0.20f, 0.79f, 0.79f, 1.0f },  /* 14 bright cyan: #34e2e2 */
        { 0.93f, 0.93f, 0.93f, 1.0f },  /* 15 bright white: #eeeeec */
    },
    /* Container chrome → all #0A0A0E (matches terminal bg).
     * Interactive chrome (border, active tab, active sidebar row,
     * hover) → #141416 — the single accent shade. */
    .tab_bg          = { 0.0392f, 0.0392f, 0.0549f, 1.0f },  /* #0A0A0E */
    .tab_active_bg   = { 0.0784f, 0.0784f, 0.0863f, 1.0f },  /* #141416 */
    .tab_active_fg   = { 0.87f,   0.87f,   0.87f,   1.0f },
    .tab_inactive_bg = { 0.0392f, 0.0392f, 0.0549f, 1.0f },  /* #0A0A0E */
    .tab_inactive_fg = { 0.50f,   0.50f,   0.50f,   1.0f },
    .sidebar_bg      = { 0.0392f, 0.0392f, 0.0549f, 1.0f },  /* #0A0A0E */
    .sidebar_fg      = { 0.80f,   0.80f,   0.80f,   1.0f },
    .sidebar_hover   = { 0.0784f, 0.0784f, 0.0863f, 1.0f },  /* #141416 */
    .sidebar_active  = { 0.0784f, 0.0784f, 0.0863f, 1.0f },  /* #141416 */
    .border          = { 0.0784f, 0.0784f, 0.0863f, 1.0f },  /* #141416 */
    .scrollbar       = { 0.00f, 0.00f, 0.00f, 0.0f }, /* invisible */
    .scrollbar_thumb = { 0.25f, 0.25f, 0.25f, 0.55f },
    .status_bg       = { 0.0392f, 0.0392f, 0.0549f, 1.0f },  /* #0A0A0E */
    .status_fg       = { 0.48f,   0.48f,   0.48f,   1.0f },
};
const Theme THEME_TOKYO_NIGHT = {
    /* Canonical Tokyo Night "Night" variant from folke/tokyonight.nvim */
    .name = "Tokyo Night",
    .fg = {0.75f, 0.79f, 0.96f, 1},    /* #c0caf5 (was too dim 0.66,0.70,0.84) */
    .bg = {0.10f, 0.11f, 0.15f, 1},    /* #1a1b26 */
    .cursor = {0.75f, 0.79f, 0.96f, 1},
    .selection = {0.17f, 0.21f, 0.34f, 0.7f},  /* #283457 */
    .ansi = {{0.08f,0.09f,0.12f,1},  /* 0  black   #15161E */
             {0.97f,0.46f,0.56f,1},  /* 1  red     #F7768E (blue 0.56 vs 0.52) */
             {0.62f,0.81f,0.42f,1},  /* 2  green   #9ECE6A (was 0.58,0.84,0.56) */
             {0.88f,0.69f,0.41f,1},  /* 3  yellow  #E0AF68 (was 0.88,0.77,0.49) */
             {0.48f,0.64f,0.97f,1},  /* 4  blue    #7AA2F7 (was 0.49,0.59,0.87) */
             {0.73f,0.60f,0.97f,1},  /* 5  magenta #BB9AF7 (was 0.73,0.56,0.87) */
             {0.49f,0.81f,1.00f,1},  /* 6  cyan    #7DCFFF (was 0.49,0.78,0.86) */
             {0.66f,0.70f,0.84f,1},  /* 7  white   #A9B1D6 */
             {0.25f,0.27f,0.40f,1},  /* 8  bright black   #414868 */
             {1.00f,0.50f,0.60f,1},  /* 9  bright red     #FF7A93 */
             {0.72f,0.89f,0.53f,1},  /* 10 bright green   #B9F27C */
             {1.00f,0.60f,0.31f,1},  /* 11 bright yellow  #FF9E64 */
             {0.48f,0.64f,0.97f,1},  /* 12 bright blue    #7AA2F7 */
             {0.73f,0.60f,0.97f,1},  /* 13 bright magenta #BB9AF7 */
             {0.71f,0.89f,1.00f,1},  /* 14 bright cyan    #B4F9F8 */
             {0.75f,0.79f,0.96f,1}}, /* 15 bright white   #C0CAF5 */
    .tab_bg={0.09f,0.10f,0.15f,1}, .tab_active_bg={0.16f,0.17f,0.24f,1},
    .tab_active_fg={0.66f,0.70f,0.84f,1}, .tab_inactive_bg={0.10f,0.11f,0.17f,1},
    .tab_inactive_fg={0.38f,0.40f,0.52f,1}, .sidebar_bg={0.09f,0.10f,0.15f,1},
    .sidebar_fg={0.60f,0.63f,0.76f,1}, .sidebar_hover={0.14f,0.15f,0.22f,1},
    .sidebar_active={0.20f,0.22f,0.35f,1}, .border={0.16f,0.17f,0.24f,1},
    .scrollbar={0.10f,0.11f,0.17f,1}, .scrollbar_thumb={0.27f,0.29f,0.40f,1},
    .status_bg={0.09f,0.10f,0.15f,1}, .status_fg={0.38f,0.40f,0.52f,1},
};

const Theme THEME_ONE_DARK = {
    /* Canonical Atom One Dark palette */
    .name = "One Dark",
    .fg = {0.67f, 0.70f, 0.75f, 1},   /* #ABB2BF */
    .bg = {0.16f, 0.17f, 0.20f, 1},   /* #282C34 */
    .cursor = {0.38f, 0.69f, 0.94f, 1},  /* #61AFEF */
    .selection = {0.24f, 0.27f, 0.33f, 0.7f},  /* #3E4451 */
    .ansi = {{0.16f,0.17f,0.20f,1},  /* 0  black   #282C34 */
             {0.88f,0.42f,0.46f,1},  /* 1  red     #E06C75 (blue 0.46 vs 0.42) */
             {0.60f,0.76f,0.48f,1},  /* 2  green   #98C379 */
             {0.82f,0.60f,0.40f,1},  /* 3  yellow  #D19A66 (was 0.90,0.77,0.48) */
             {0.38f,0.69f,0.94f,1},  /* 4  blue    #61AFEF (was 0.38,0.61,0.83) */
             {0.78f,0.47f,0.87f,1},  /* 5  magenta #C678DD (was blue 0.73) */
             {0.34f,0.71f,0.76f,1},  /* 6  cyan    #56B6C2 */
             {0.67f,0.70f,0.75f,1},  /* 7  white   #ABB2BF */
             {0.36f,0.39f,0.45f,1},  /* 8  bright black   #5C6370 */
             {0.88f,0.42f,0.46f,1},  /* 9  bright red     #E06C75 */
             {0.60f,0.76f,0.48f,1},  /* 10 bright green   #98C379 */
             {0.82f,0.60f,0.40f,1},  /* 11 bright yellow  #D19A66 */
             {0.38f,0.69f,0.94f,1},  /* 12 bright blue    #61AFEF */
             {0.78f,0.47f,0.87f,1},  /* 13 bright magenta #C678DD */
             {0.34f,0.71f,0.76f,1},  /* 14 bright cyan    #56B6C2 */
             {0.80f,0.84f,0.89f,1}}, /* 15 bright white   #CCD0D5 */
    .tab_bg={0.14f,0.16f,0.19f,1}, .tab_active_bg={0.21f,0.24f,0.28f,1},
    .tab_active_fg={0.68f,0.73f,0.80f,1}, .tab_inactive_bg={0.16f,0.18f,0.21f,1},
    .tab_inactive_fg={0.44f,0.48f,0.55f,1}, .sidebar_bg={0.14f,0.16f,0.19f,1},
    .sidebar_fg={0.62f,0.67f,0.74f,1}, .sidebar_hover={0.19f,0.21f,0.25f,1},
    .sidebar_active={0.24f,0.27f,0.33f,1}, .border={0.21f,0.24f,0.28f,1},
    .scrollbar={0.16f,0.18f,0.21f,1}, .scrollbar_thumb={0.30f,0.33f,0.39f,1},
    .status_bg={0.14f,0.16f,0.19f,1}, .status_fg={0.44f,0.48f,0.55f,1},
};

const Theme THEME_ROSE_PINE = {
    /* Canonical Rose Pine "Main" palette (rosepinetheme.com).
     *   love   #eb6f92   red/pink
     *   gold   #f6c177   yellow
     *   rose   #ebbcba   peach
     *   pine   #31748f   teal/blue  (used for ansi blue)
     *   foam   #9ccfd8   light cyan
     *   iris   #c4a7e7   lavender   (used for ansi magenta)
     *   text   #e0def4
     *   base   #191724   bg
     *   surface #1f1d2e  ansi black
     */
    .name = "Rose Pine",
    .fg = {0.88f, 0.87f, 0.96f, 1},   /* #E0DEF4 */
    .bg = {0.10f, 0.09f, 0.14f, 1},   /* #191724 */
    .cursor = {0.92f, 0.74f, 0.73f, 1},  /* rose #EBBCBA */
    .selection = {0.15f, 0.13f, 0.23f, 0.7f},  /* highlight-med */
    .ansi = {{0.12f,0.11f,0.18f,1},  /* 0  black   surface #1F1D2E */
             {0.92f,0.44f,0.57f,1},  /* 1  red     Love #EB6F92 (was 0.55,0.55) */
             {0.36f,0.76f,0.69f,1},  /* 2  green   Foam used as green-teal */
             {0.96f,0.76f,0.47f,1},  /* 3  yellow  Gold #F6C177 */
             {0.19f,0.45f,0.56f,1},  /* 4  blue    Pine #31748F (was 0.50,0.55,0.82) */
             {0.77f,0.65f,0.91f,1},  /* 5  magenta Iris #C4A7E7 (was 0.78,0.58,0.82) */
             {0.61f,0.81f,0.85f,1},  /* 6  cyan    Foam #9CCFD8 */
             {0.57f,0.55f,0.67f,1},  /* 7  white   subtle #908CAA */
             {0.28f,0.26f,0.38f,1},  /* 8  bright black   highlight-high */
             {0.92f,0.44f,0.57f,1},  /* 9  bright red     Love */
             {0.36f,0.76f,0.69f,1},  /* 10 bright green   Foam-green */
             {0.96f,0.76f,0.47f,1},  /* 11 bright yellow  Gold */
             {0.19f,0.45f,0.56f,1},  /* 12 bright blue    Pine */
             {0.77f,0.65f,0.91f,1},  /* 13 bright magenta Iris */
             {0.61f,0.81f,0.85f,1},  /* 14 bright cyan    Foam */
             {0.88f,0.87f,0.96f,1}}, /* 15 bright white   text */
    .tab_bg={0.08f,0.07f,0.12f,1}, .tab_active_bg={0.15f,0.14f,0.22f,1},
    .tab_active_fg={0.88f,0.85f,0.93f,1}, .tab_inactive_bg={0.10f,0.09f,0.15f,1},
    .tab_inactive_fg={0.42f,0.39f,0.52f,1}, .sidebar_bg={0.08f,0.07f,0.12f,1},
    .sidebar_fg={0.78f,0.75f,0.85f,1}, .sidebar_hover={0.13f,0.12f,0.20f,1},
    .sidebar_active={0.22f,0.20f,0.30f,1}, .border={0.15f,0.14f,0.22f,1},
    .scrollbar={0.10f,0.09f,0.15f,1}, .scrollbar_thumb={0.28f,0.26f,0.38f,1},
    .status_bg={0.08f,0.07f,0.12f,1}, .status_fg={0.42f,0.39f,0.52f,1},
    /* Rose Pine's Love (#EB6F92) — the canonical accent.  Without
     * this the chrome falls back to ansi[4] (Pine teal), which is
     * also Rose Pine palette but reads as cool rather than carrying
     * the rose identity. */
    .ui_accent = {0.92f, 0.44f, 0.57f, 1.0f},
};

const Theme THEME_KANAGAWA = {
    .name = "Kanagawa",
    .fg = {0.87f, 0.83f, 0.74f, 1}, .bg = {0.10f, 0.10f, 0.14f, 1},
    .cursor = {0.78f, 0.75f, 0.53f, 1}, .selection = {0.18f, 0.20f, 0.28f, 0.7f},
    .ansi = {{0.14f,0.14f,0.20f,1},{0.78f,0.34f,0.33f,1},{0.58f,0.71f,0.38f,1},{0.87f,0.73f,0.42f,1},
             {0.43f,0.55f,0.73f,1},{0.61f,0.46f,0.60f,1},{0.43f,0.65f,0.56f,1},{0.87f,0.83f,0.74f,1},
             {0.28f,0.28f,0.38f,1},{0.91f,0.38f,0.36f,1},{0.65f,0.78f,0.43f,1},{0.93f,0.78f,0.47f,1},
             {0.50f,0.62f,0.80f,1},{0.68f,0.53f,0.67f,1},{0.50f,0.72f,0.63f,1},{0.92f,0.89f,0.80f,1}},
    .tab_bg={0.08f,0.08f,0.12f,1}, .tab_active_bg={0.14f,0.14f,0.20f,1},
    .tab_active_fg={0.87f,0.83f,0.74f,1}, .tab_inactive_bg={0.10f,0.10f,0.14f,1},
    .tab_inactive_fg={0.40f,0.40f,0.50f,1}, .sidebar_bg={0.08f,0.08f,0.12f,1},
    .sidebar_fg={0.78f,0.74f,0.66f,1}, .sidebar_hover={0.13f,0.13f,0.18f,1},
    .sidebar_active={0.18f,0.20f,0.28f,1}, .border={0.14f,0.14f,0.20f,1},
    .scrollbar={0.10f,0.10f,0.14f,1}, .scrollbar_thumb={0.28f,0.28f,0.38f,1},
    .status_bg={0.08f,0.08f,0.12f,1}, .status_fg={0.40f,0.40f,0.50f,1},
};

/* ---- Additional themes ---- */

const Theme THEME_EVERFOREST = {
    /* Canonical Everforest Dark Medium (sainnhe/everforest). Previous values
     * had blue channels systematically too low. */
    .name = "Everforest",
    .fg={0.83f,0.78f,0.67f,1},         /* #D3C6AA */
    .bg={0.18f,0.20f,0.22f,1},         /* #2D353B (was 0.17,0.20,0.18 — green-tinted) */
    .cursor={0.66f,0.75f,0.50f,1},     /* #A7C080 */
    .selection={0.29f,0.32f,0.33f,0.8f},/* #475258 */
    .ansi={{0.30f,0.34f,0.38f,1},      /* 0  black  #475258 */
           {0.90f,0.49f,0.50f,1},      /* 1  red    #E67E80 (blue 0.50 vs 0.44) */
           {0.66f,0.75f,0.50f,1},      /* 2  green  #A7C080 (blue 0.50 vs 0.43) */
           {0.86f,0.74f,0.50f,1},      /* 3  yellow #DBBC7F (blue 0.50 vs 0.41) */
           {0.50f,0.73f,0.70f,1},      /* 4  blue   #7FBBB3 (green 0.73 vs 0.67) */
           {0.84f,0.60f,0.71f,1},      /* 5  magenta #D699B6 (g 0.60, b 0.71) */
           {0.51f,0.75f,0.57f,1},      /* 6  cyan   #83C092 (was 0.55 blue) */
           {0.83f,0.78f,0.67f,1},      /* 7  white  #D3C6AA */
           {0.50f,0.55f,0.56f,1},      /* 8  bright black  #859289 */
           {0.90f,0.49f,0.50f,1},      /* 9  bright red    #E67E80 */
           {0.66f,0.75f,0.50f,1},      /* 10 bright green  #A7C080 */
           {0.86f,0.74f,0.50f,1},      /* 11 bright yellow #DBBC7F */
           {0.50f,0.73f,0.70f,1},      /* 12 bright blue   #7FBBB3 */
           {0.84f,0.60f,0.71f,1},      /* 13 bright magenta#D699B6 */
           {0.51f,0.75f,0.57f,1},      /* 14 bright cyan   #83C092 */
           {0.90f,0.88f,0.82f,1}},     /* 15 bright white  */
    .tab_bg={0.14f,0.17f,0.15f,1}, .tab_active_bg={0.21f,0.25f,0.22f,1},
    .tab_active_fg={0.84f,0.80f,0.74f,1}, .tab_inactive_bg={0.17f,0.20f,0.18f,1},
    .tab_inactive_fg={0.50f,0.53f,0.48f,1}, .sidebar_bg={0.14f,0.17f,0.15f,1},
    .sidebar_fg={0.76f,0.73f,0.67f,1}, .sidebar_hover={0.21f,0.25f,0.22f,1},
    .sidebar_active={0.24f,0.30f,0.26f,1}, .border={0.24f,0.28f,0.24f,1},
    .scrollbar={0.17f,0.20f,0.18f,1}, .scrollbar_thumb={0.33f,0.37f,0.33f,1},
    .status_bg={0.14f,0.17f,0.15f,1}, .status_fg={0.50f,0.53f,0.48f,1},
    /* Everforest's foliage green #A7C080 — the canonical accent.  The
     * cursor sits at the same value but its saturation (~0.33) falls
     * just below the auto-fallback threshold, so we set ui_accent
     * explicitly to keep the chrome forest-green. */
    .ui_accent = {0.66f, 0.75f, 0.50f, 1.0f},
};

const Theme THEME_GITHUB_DARK = {
    .name = "GitHub Dark",
    .fg={0.89f,0.91f,0.94f,1}, .bg={0.06f,0.07f,0.09f,1},
    .cursor={0.55f,0.64f,0.96f,1}, .selection={0.16f,0.20f,0.33f,0.8f},
    .ansi={{0.29f,0.33f,0.39f,1},{1.00f,0.48f,0.41f,1},{0.24f,0.83f,0.62f,1},{0.88f,0.78f,0.26f,1},
           {0.55f,0.64f,0.96f,1},{0.74f,0.54f,0.99f,1},{0.46f,0.82f,0.96f,1},{0.89f,0.91f,0.94f,1},
           {0.40f,0.45f,0.50f,1},{1.00f,0.48f,0.41f,1},{0.24f,0.83f,0.62f,1},{0.88f,0.78f,0.26f,1},
           {0.55f,0.64f,0.96f,1},{0.74f,0.54f,0.99f,1},{0.46f,0.82f,0.96f,1},{0.95f,0.96f,0.98f,1}},
    .tab_bg={0.04f,0.05f,0.07f,1}, .tab_active_bg={0.10f,0.12f,0.15f,1},
    .tab_active_fg={0.89f,0.91f,0.94f,1}, .tab_inactive_bg={0.06f,0.07f,0.09f,1},
    .tab_inactive_fg={0.40f,0.45f,0.50f,1}, .sidebar_bg={0.04f,0.05f,0.07f,1},
    .sidebar_fg={0.80f,0.82f,0.86f,1}, .sidebar_hover={0.10f,0.12f,0.15f,1},
    .sidebar_active={0.16f,0.20f,0.33f,1}, .border={0.13f,0.16f,0.20f,1},
    .scrollbar={0.06f,0.07f,0.09f,1}, .scrollbar_thumb={0.25f,0.29f,0.35f,1},
    .status_bg={0.04f,0.05f,0.07f,1}, .status_fg={0.40f,0.45f,0.50f,1},
};

const Theme THEME_AYU_DARK = {
    .name = "Ayu Dark",
    .fg={0.73f,0.74f,0.73f,1}, .bg={0.05f,0.07f,0.10f,1},
    .cursor={0.91f,0.70f,0.25f,1}, .selection={0.14f,0.24f,0.38f,0.8f},
    .ansi={{0.03f,0.05f,0.07f,1},{0.96f,0.38f,0.31f,1},{0.63f,0.74f,0.27f,1},{0.91f,0.70f,0.25f,1},
           {0.22f,0.55f,0.99f,1},{0.85f,0.51f,0.96f,1},{0.35f,0.75f,0.80f,1},{0.73f,0.74f,0.73f,1},
           {0.18f,0.22f,0.28f,1},{0.96f,0.38f,0.31f,1},{0.63f,0.74f,0.27f,1},{0.91f,0.70f,0.25f,1},
           {0.22f,0.55f,0.99f,1},{0.85f,0.51f,0.96f,1},{0.35f,0.75f,0.80f,1},{0.86f,0.87f,0.86f,1}},
    .tab_bg={0.04f,0.05f,0.08f,1}, .tab_active_bg={0.08f,0.10f,0.14f,1},
    .tab_active_fg={0.73f,0.74f,0.73f,1}, .tab_inactive_bg={0.05f,0.07f,0.10f,1},
    .tab_inactive_fg={0.35f,0.38f,0.42f,1}, .sidebar_bg={0.04f,0.05f,0.08f,1},
    .sidebar_fg={0.65f,0.67f,0.65f,1}, .sidebar_hover={0.08f,0.10f,0.14f,1},
    .sidebar_active={0.14f,0.24f,0.38f,1}, .border={0.08f,0.10f,0.14f,1},
    .scrollbar={0.05f,0.07f,0.10f,1}, .scrollbar_thumb={0.18f,0.22f,0.28f,1},
    .status_bg={0.04f,0.05f,0.08f,1}, .status_fg={0.35f,0.38f,0.42f,1},
};

const Theme THEME_MATERIAL = {
    .name = "Material",
    .fg={0.93f,0.95f,0.96f,1}, .bg={0.16f,0.18f,0.20f,1},
    .cursor={1.00f,0.80f,0.31f,1}, .selection={0.22f,0.25f,0.29f,0.8f},
    .ansi={{0.21f,0.24f,0.27f,1},{0.95f,0.33f,0.33f,1},{0.76f,0.90f,0.38f,1},{1.00f,0.80f,0.31f,1},
           {0.51f,0.73f,0.94f,1},{0.76f,0.47f,0.86f,1},{0.53f,0.85f,0.87f,1},{0.93f,0.95f,0.96f,1},
           {0.33f,0.37f,0.41f,1},{0.95f,0.33f,0.33f,1},{0.76f,0.90f,0.38f,1},{1.00f,0.80f,0.31f,1},
           {0.51f,0.73f,0.94f,1},{0.76f,0.47f,0.86f,1},{0.53f,0.85f,0.87f,1},{1.00f,1.00f,1.00f,1}},
    .tab_bg={0.13f,0.15f,0.17f,1}, .tab_active_bg={0.20f,0.23f,0.26f,1},
    .tab_active_fg={0.93f,0.95f,0.96f,1}, .tab_inactive_bg={0.16f,0.18f,0.20f,1},
    .tab_inactive_fg={0.45f,0.50f,0.55f,1}, .sidebar_bg={0.13f,0.15f,0.17f,1},
    .sidebar_fg={0.85f,0.87f,0.88f,1}, .sidebar_hover={0.20f,0.23f,0.26f,1},
    .sidebar_active={0.22f,0.25f,0.29f,1}, .border={0.20f,0.23f,0.26f,1},
    .scrollbar={0.16f,0.18f,0.20f,1}, .scrollbar_thumb={0.33f,0.37f,0.41f,1},
    .status_bg={0.13f,0.15f,0.17f,1}, .status_fg={0.45f,0.50f,0.55f,1},
};

const Theme THEME_SOLARIZED_LIGHT = {
    .name = "Solarized Light",
    .fg={0.40f,0.48f,0.51f,1}, .bg={0.99f,0.96f,0.89f,1},
    .cursor={0.52f,0.60f,0.00f,1}, .selection={0.93f,0.91f,0.84f,0.8f},
    .ansi={{0.03f,0.21f,0.26f,1},{0.86f,0.20f,0.18f,1},{0.52f,0.60f,0.00f,1},{0.71f,0.54f,0.00f,1},
           {0.15f,0.55f,0.82f,1},{0.83f,0.21f,0.51f,1},{0.16f,0.63f,0.60f,1},{0.93f,0.91f,0.84f,1},
           {0.00f,0.17f,0.21f,1},{0.80f,0.29f,0.09f,1},{0.35f,0.43f,0.46f,1},{0.40f,0.48f,0.51f,1},
           {0.51f,0.58f,0.59f,1},{0.42f,0.44f,0.77f,1},{0.58f,0.63f,0.63f,1},{0.99f,0.96f,0.89f,1}},
    .tab_bg={0.93f,0.91f,0.84f,1}, .tab_active_bg={0.99f,0.96f,0.89f,1},
    .tab_active_fg={0.40f,0.48f,0.51f,1}, .tab_inactive_bg={0.93f,0.91f,0.84f,1},
    .tab_inactive_fg={0.58f,0.63f,0.63f,1}, .sidebar_bg={0.93f,0.91f,0.84f,1},
    .sidebar_fg={0.40f,0.48f,0.51f,1}, .sidebar_hover={0.88f,0.87f,0.80f,1},
    .sidebar_active={0.83f,0.82f,0.76f,1}, .border={0.83f,0.82f,0.76f,1},
    .scrollbar={0.93f,0.91f,0.84f,1}, .scrollbar_thumb={0.73f,0.73f,0.69f,1},
    .status_bg={0.93f,0.91f,0.84f,1}, .status_fg={0.58f,0.63f,0.63f,1},
};

const Theme THEME_CATPPUCCIN_LATTE = {
    /* Canonical Catppuccin Latte palette (catppuccin/palette). */
    .name = "Catppuccin Latte",
    .fg={0.30f,0.31f,0.41f,1},        /* Text #4C4F69 */
    .bg={0.94f,0.95f,0.96f,1},        /* Base #EFF1F5 */
    .cursor={0.53f,0.22f,0.94f,1},    /* Mauve #8839EF */
    .selection={0.80f,0.81f,0.91f,0.8f},  /* Surface1 */
    .ansi={{0.30f,0.31f,0.41f,1},     /* 0  black   Subtext0 */
           {0.82f,0.06f,0.22f,1},     /* 1  red     #D20F39 (was 0.21 green) */
           {0.25f,0.63f,0.17f,1},     /* 2  green   #40A02B (was 0.43 blue) */
           {0.87f,0.56f,0.11f,1},     /* 3  yellow  #DF8E1D */
           {0.12f,0.40f,0.96f,1},     /* 4  blue    #1E66F5 (was 0.89 blue) */
           {0.53f,0.22f,0.94f,1},     /* 5  magenta Mauve #8839EF (was 0.33 green) */
           {0.09f,0.57f,0.60f,1},     /* 6  cyan    Teal #179299 (was 0.04,0.56,0.65) */
           {0.36f,0.37f,0.47f,1},     /* 7  white   Subtext1 #5C5F77 */
           {0.43f,0.45f,0.58f,1},     /* 8  bright black   Overlay0 */
           {0.90f,0.27f,0.33f,1},     /* 9  bright red     Maroon #E64553 */
           {0.25f,0.63f,0.17f,1},     /* 10 bright green   #40A02B */
           {1.00f,0.39f,0.04f,1},     /* 11 bright yellow  Peach #FE640B */
           {0.12f,0.40f,0.96f,1},     /* 12 bright blue    #1E66F5 */
           {0.92f,0.46f,0.80f,1},     /* 13 bright magenta Pink #EA76CB */
           {0.02f,0.65f,0.90f,1},     /* 14 bright cyan    Sky #04A5E5 */
           {0.45f,0.47f,0.56f,1}},    /* 15 bright white   Overlay2 */
    .tab_bg={0.90f,0.89f,0.93f,1}, .tab_active_bg={0.94f,0.93f,0.96f,1},
    .tab_active_fg={0.30f,0.32f,0.42f,1}, .tab_inactive_bg={0.90f,0.89f,0.93f,1},
    .tab_inactive_fg={0.55f,0.57f,0.67f,1}, .sidebar_bg={0.90f,0.89f,0.93f,1},
    .sidebar_fg={0.30f,0.32f,0.42f,1}, .sidebar_hover={0.86f,0.86f,0.90f,1},
    .sidebar_active={0.80f,0.81f,0.91f,1}, .border={0.80f,0.81f,0.91f,1},
    .scrollbar={0.90f,0.89f,0.93f,1}, .scrollbar_thumb={0.67f,0.70f,0.80f,1},
    .status_bg={0.90f,0.89f,0.93f,1}, .status_fg={0.55f,0.57f,0.67f,1},
};

const Theme THEME_NIGHTFOX = {
    .name = "Nightfox",
    .fg={0.81f,0.85f,0.87f,1}, .bg={0.07f,0.13f,0.17f,1},
    .cursor={0.63f,0.83f,0.98f,1}, .selection={0.13f,0.22f,0.28f,0.8f},
    .ansi={{0.15f,0.21f,0.25f,1},{0.75f,0.38f,0.42f,1},{0.50f,0.73f,0.49f,1},{0.87f,0.76f,0.49f,1},
           {0.35f,0.61f,0.85f,1},{0.63f,0.49f,0.72f,1},{0.25f,0.67f,0.76f,1},{0.81f,0.85f,0.87f,1},
           {0.22f,0.30f,0.35f,1},{0.84f,0.46f,0.50f,1},{0.58f,0.80f,0.57f,1},{0.93f,0.83f,0.56f,1},
           {0.43f,0.68f,0.91f,1},{0.70f,0.56f,0.79f,1},{0.33f,0.74f,0.83f,1},{0.88f,0.91f,0.93f,1}},
    .tab_bg={0.05f,0.10f,0.14f,1}, .tab_active_bg={0.10f,0.17f,0.22f,1},
    .tab_active_fg={0.81f,0.85f,0.87f,1}, .tab_inactive_bg={0.07f,0.13f,0.17f,1},
    .tab_inactive_fg={0.40f,0.48f,0.53f,1}, .sidebar_bg={0.05f,0.10f,0.14f,1},
    .sidebar_fg={0.73f,0.77f,0.80f,1}, .sidebar_hover={0.10f,0.17f,0.22f,1},
    .sidebar_active={0.13f,0.22f,0.28f,1}, .border={0.13f,0.20f,0.25f,1},
    .scrollbar={0.07f,0.13f,0.17f,1}, .scrollbar_thumb={0.22f,0.30f,0.35f,1},
    .status_bg={0.05f,0.10f,0.14f,1}, .status_fg={0.40f,0.48f,0.53f,1},
};

/* Theme registry */
static const Theme *g_themes[] = {
    &THEME_Liu, &THEME_KITTY, &THEME_DARK, &THEME_LIGHT, &THEME_SOLARIZED_DARK,
    &THEME_SOLARIZED_LIGHT, &THEME_MONOKAI, &THEME_DRACULA, &THEME_NORD, &THEME_GRUVBOX,
    &THEME_CATPPUCCIN_MOCHA, &THEME_CATPPUCCIN_LATTE, &THEME_TOKYO_NIGHT, &THEME_ONE_DARK,
    &THEME_ROSE_PINE, &THEME_KANAGAWA, &THEME_EVERFOREST, &THEME_GITHUB_DARK,
    &THEME_AYU_DARK, &THEME_MATERIAL, &THEME_NIGHTFOX,
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
    return &THEME_DARK;
}

const Theme *theme_get_by_index(i32 index) {
    if (index >= 0 && index < THEME_COUNT) return g_themes[index];
    i32 ui = index - THEME_COUNT;
    if (ui >= 0 && ui < g_user_theme_count) return &g_user_themes[ui];
    return &THEME_DARK;
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

    /* Auto-fallback: vivid / branded cursors carry the theme's identity
     * (think warm-orange or rose cursors), so we use the cursor as the
     * chrome accent when it is meaningfully coloured. Threshold is
     * conservative — built-in themes whose cursor sits in the neutral
     * white/gray family stay below it and keep their legacy ansi[12]-driven
     * blue accents. */
    f32 r = t->cursor.r, g = t->cursor.g, b = t->cursor.b;
    f32 max_c = r > g ? (r > b ? r : b) : (g > b ? g : b);
    f32 min_c = r < g ? (r < b ? r : b) : (g < b ? g : b);
    f32 sat   = max_c > 0.001f ? (max_c - min_c) / max_c : 0.0f;
    if (t->cursor.a > 0.5f && sat > 0.40f) return t->cursor;
    return t->ansi[12];
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
        .terminal_top_gap      = 2.0f,
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
    snprintf(c.font_path, sizeof(c.font_path), "JetBrainsMono-Regular.ttf");
    c.fallback_font_count = 0;
    memset(c.fallback_fonts, 0, sizeof(c.fallback_fonts));
    c.font_size = 12.0f;     /* default terminal size */
    c.line_height = 1.0f;
    c.font_weight = 0.0f;
    snprintf(c.theme_name, sizeof(c.theme_name), "Liu");
    c.theme = &THEME_Liu;
    /* 1000 lines × up to cols*sizeof(Cell) is a ~1.5 MiB worst case per
     * terminal. We used to default to 2000, but most TUI workflows never
     * scroll past a few hundred lines; users who need more can bump this
     * via config. Combined with the existing adaptive ring growth (starts
     * at 64 entries, doubles on demand) this keeps RSS tight for many
     * concurrent panes. */
    c.scrollback_lines = CONFIG_SCROLLBACK_DEFAULT_LINES;
    c.tab_sleep_idle_minutes = 20.0f;
    c.confirm_close_agent = true;
    c.cursor_blink = true;
    c.cursor_style = 0;      
    c.bold_is_bright = true;
    c.copy_on_select = false;
    c.bidi_enabled = false;       
    c.tab_height = 36.0f;
    c.sidebar_width = 240.0f;   /* points; matches SIDEBAR_DEFAULT_PT */
    c.show_scrollbar = true;  /* auto-hide scrollbar after scroll activity */
    c.opacity = 1.0f;
    c.padding = 2.0f;           
    c.hide_tab_bar_single = true;
    c.show_tab_bar = true;
    c.show_toolbar_icons = true;
    c.show_status_bar = true;
    c.borderless = false;
    c.cell_width_scale = 1.0f;
    c.cell_height_scale = 1.0f;
    c.option_as_alt = false;
    c.cursor_animate = true;
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
    static char path[1024] = {0};
    if (path[0]) return path;

    const char *home = getenv("HOME");
    char legacy_dir[1024];
    char current_dir[1024];
    char legacy_cfg[1024];
    char current_cfg[1024];

    if (!home) home = "/tmp";
    snprintf(current_dir, sizeof(current_dir), "%s/.config/Liu", home);
    snprintf(legacy_dir, sizeof(legacy_dir), "%s/.config/liu", home);
    snprintf(current_cfg, sizeof(current_cfg), "%s/config.json", current_dir);
    snprintf(legacy_cfg, sizeof(legacy_cfg), "%s/config.json", legacy_dir);

    if (path_exists(current_cfg)) {
        snprintf(path, sizeof(path), "%s", current_dir);
    } else if (path_exists(legacy_cfg) || dir_exists(legacy_dir)) {
        snprintf(path, sizeof(path), "%s", legacy_dir);
    } else {
        snprintf(path, sizeof(path), "%s", current_dir);
    }
    return path;
}

const char *config_file_path(void) {
    static char path[1024] = {0};
    if (path[0]) return path;

    snprintf(path, sizeof(path), "%s/config.json", config_user_dir());
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

    {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            char candidate[1024];
            snprintf(candidate, sizeof(candidate), "%s/assets/fonts", cwd);
            if (try_font_dir(path, sizeof(path), candidate)) return path;
        }
    }

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
/* Extract  "key" : "value"  from a config line, unescaping the value's  \"  and
 * \\  (the reverse of config_fputs_json_escaped). The old  %511[^"]  scanf
 * stopped at the FIRST embedded quote and never unescaped, so a saved site
 * command like  npm run dev -- --define "x=y"  reloaded truncated/corrupted.
 * Keys are plain identifiers (never escaped). Returns false when the value is
 * not a quoted string, so numeric / bool lines fall through to the next branch. */
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
    if (*p != '"') return false;            /* not a string value (number/bool) */
    p++;
    usize v = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && (p[1] == '"' || p[1] == '\\')) { c = p[1]; p++; }
        if (v + 1 < vsz) val[v++] = c;
        p++;
    }
    if (*p != '"') return false;            /* unterminated value */
    val[v] = '\0';
    return true;
}

bool config_load(AppConfig *cfg, const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return false;

    /* Ensure style defaults are loaded so unset fields stay valid */
    cfg->style = style_default();

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char key[128], val[512];
        /* Parse "key": "value" or "key": number */
        if (config_parse_kv(line, key, sizeof key, val, sizeof val)) {
            if (strcmp(key, "font_path") == 0) snprintf(cfg->font_path, sizeof(cfg->font_path), "%s", val);
            else if (strncmp(key, "fallback_font_", 14) == 0) {
                i32 idx = atoi(key + 14);
                if (idx >= 0 && idx < MAX_CONFIG_FALLBACK_FONTS && val[0]) {
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
            else if (strcmp(key, "background_image") == 0) snprintf(cfg->background_image, sizeof(cfg->background_image), "%s", val);
            else if (strcmp(key, "notes_vault_path") == 0) snprintf(cfg->notes_vault_path, sizeof(cfg->notes_vault_path), "%s", val);
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
            /* Profile string values: profile_N_name, profile_N_font_path, profile_N_theme */
            else if (strncmp(key, "profile_", 8) == 0) {
                i32 pi = atoi(key + 8);
                if (pi >= 0 && pi < MAX_PROFILES) {
                    const char *field = strchr(key + 8, '_');
                    if (field) {
                        field++;
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
            }
            /* Sites: site_N_name / site_N_path / site_N_command (string fields) */
            else if (strncmp(key, "site_", 5) == 0) {
                i32 si = atoi(key + 5);
                if (si >= 0 && si < MAX_SITES) {
                    const char *field = strchr(key + 5, '_');
                    if (field) {
                        field++;
                        if (strcmp(field, "name") == 0)
                            snprintf(cfg->sites[si].name, sizeof(cfg->sites[si].name), "%s", val);
                        else if (strcmp(field, "path") == 0)
                            snprintf(cfg->sites[si].path, sizeof(cfg->sites[si].path), "%s", val);
                        else if (strcmp(field, "command") == 0)
                            snprintf(cfg->sites[si].command, sizeof(cfg->sites[si].command), "%s", val);
                    }
                }
            }
        } else if (sscanf(line, " \"%127[^\"]\" : %511s", key, val) == 2) {
            /* %s grabs everything up to the next whitespace, which means a
             * trailing comma from `"key": true,` is captured into val
             * itself. strcmp(val, "true") then misses, and every bool set
             * to true in the JSON silently flips to false. atoi/atof on
             * the numeric branches are already tolerant. Trim the JSON
             * line-tail punctuation here once. */
            usize _vlen = strlen(val);
            while (_vlen > 0 && (val[_vlen-1] == ',' || val[_vlen-1] == '}' ||
                                 val[_vlen-1] == ' ' || val[_vlen-1] == '\t' ||
                                 val[_vlen-1] == '\r' || val[_vlen-1] == '\n')) {
                val[--_vlen] = '\0';
            }
            if (strcmp(key, "font_size") == 0) {
                f32 v = (f32)atof(val);
                if (v < 6.0f)  v = 6.0f;
                if (v > 96.0f) v = 96.0f;
                cfg->font_size = v;
            }
            else if (strcmp(key, "line_height") == 0) cfg->line_height = (f32)atof(val);
            else if (strcmp(key, "font_weight") == 0) {
                /* Stroke-width passed straight into CGContext as a CGFloat —
                 * negative values flip the stroke direction and >2 produces
                 * pure white blobs. Clamp to the same envelope the settings
                 * panel exposes so an out-of-range JSON value can't break
                 * glyph rendering before the user can correct it. */
                f32 v = (f32)atof(val);
                if (v < 0.0f) v = 0.0f;
                if (v > 2.0f) v = 2.0f;
                cfg->font_weight = v;
            }
            else if (strcmp(key, "scrollback") == 0) cfg->scrollback_lines = atoi(val);
            else if (strcmp(key, "cursor_style") == 0) cfg->cursor_style = (u8)atoi(val);
            else if (strcmp(key, "tab_sleep_idle_minutes") == 0) cfg->tab_sleep_idle_minutes = (f32)atof(val);
            else if (strcmp(key, "sidebar_width") == 0) cfg->sidebar_width = CLAMP((f32)atof(val), 160.0f, 500.0f);
            else if (strcmp(key, "cursor_blink") == 0) cfg->cursor_blink = strcmp(val, "true") == 0;
            else if (strcmp(key, "confirm_close_agent") == 0) cfg->confirm_close_agent = strcmp(val, "true") == 0;
            else if (strcmp(key, "bold_is_bright") == 0) cfg->bold_is_bright = strcmp(val, "true") == 0;
            else if (strcmp(key, "show_scrollbar") == 0) cfg->show_scrollbar = strcmp(val, "true") == 0;
            else if (strcmp(key, "show_tab_bar") == 0) cfg->show_tab_bar = strcmp(val, "true") == 0;
            else if (strcmp(key, "show_toolbar_icons") == 0) cfg->show_toolbar_icons = strcmp(val, "true") == 0;
            else if (strcmp(key, "show_status_bar") == 0) cfg->show_status_bar = strcmp(val, "true") == 0;
            else if (strcmp(key, "bidi_enabled") == 0) cfg->bidi_enabled = strcmp(val, "true") == 0;
            /* Clamp opacity so a bad JSON value can't render the window fully
             * transparent (and thus unrecoverable). Floor at 0.2 keeps it
             * visible; cap at 1.0 is fully opaque. */
            else if (strcmp(key, "opacity") == 0) cfg->opacity = CLAMP((f32)atof(val), 0.2f, 1.0f);
            else if (strcmp(key, "padding") == 0) cfg->padding = (f32)atof(val);
            else if (strcmp(key, "option_as_alt") == 0) cfg->option_as_alt = strcmp(val, "true") == 0;
            else if (strcmp(key, "cursor_animate") == 0) cfg->cursor_animate = strcmp(val, "true") == 0;
            else if (strcmp(key, "notify_command_threshold") == 0) cfg->notify_command_threshold = (f32)atof(val);
            else if (strcmp(key, "allow_osc52_write") == 0) cfg->allow_osc52_write = strcmp(val, "true") == 0;
            else if (strcmp(key, "vault_auto_lock_minutes") == 0) cfg->vault_auto_lock_minutes = atoi(val);
            else if (strcmp(key, "profile_count") == 0) {
                cfg->profile_count = atoi(val);
                if (cfg->profile_count > MAX_PROFILES) cfg->profile_count = MAX_PROFILES;
            }
            /* Profile numeric values: profile_N_font_size, profile_N_cursor_style, profile_N_opacity */
            else if (strncmp(key, "profile_", 8) == 0) {
                i32 pi = atoi(key + 8);
                if (pi >= 0 && pi < MAX_PROFILES) {
                    const char *field = strchr(key + 8, '_');
                    if (field) {
                        field++; /* skip underscore */
                        if (strcmp(field, "font_size") == 0) {
                            cfg->profiles[pi].font_size = (f32)atof(val);
                            cfg->profiles[pi].has_font = true;
                        } else if (strcmp(field, "cursor_style") == 0) {
                            cfg->profiles[pi].cursor_style = (u8)atoi(val);
                            cfg->profiles[pi].has_cursor = true;
                        } else if (strcmp(field, "opacity") == 0) {
                            cfg->profiles[pi].opacity = (f32)atof(val);
                            cfg->profiles[pi].has_opacity = true;
                        }
                    }
                }
            }
            else if (strcmp(key, "site_count") == 0) {
                cfg->site_count = atoi(val);
                if (cfg->site_count > MAX_SITES) cfg->site_count = MAX_SITES;
                if (cfg->site_count < 0) cfg->site_count = 0;
            }
            /* Sites: site_N_port (numeric field) */
            else if (strncmp(key, "site_", 5) == 0) {
                i32 si = atoi(key + 5);
                if (si >= 0 && si < MAX_SITES) {
                    const char *field = strchr(key + 5, '_');
                    if (field) {
                        field++;
                        if (strcmp(field, "port") == 0)
                            cfg->sites[si].port = atoi(val);
                    }
                }
            }
            /* Background image */
            else if (strcmp(key, "background_opacity") == 0) cfg->background_opacity = CLAMP((f32)atof(val), 0.0f, 1.0f);
            else if (strcmp(key, "background_blur") == 0) cfg->background_blur = strcmp(val, "true") == 0;
            else if (strcmp(key, "background_blur_radius") == 0) cfg->background_blur_radius = CLAMP((f32)atof(val), 0.0f, 50.0f);
            else if (strcmp(key, "enable_ligatures") == 0) cfg->enable_ligatures = strcmp(val, "true") == 0;
            /* Quake mode */
            else if (strcmp(key, "quake_mode") == 0) cfg->quake_mode = strcmp(val, "true") == 0;
            else if (strcmp(key, "quake_hotkey") == 0) snprintf(cfg->quake_hotkey, sizeof(cfg->quake_hotkey), "%s", val);
            else if (strcmp(key, "quake_height_ratio") == 0) cfg->quake_height_ratio = (f32)atof(val);
            else if (strcmp(key, "quake_animation_duration") == 0) cfg->quake_animation_duration = (f32)atof(val);
            /* Translate-on-Tab */
            else if (strcmp(key, "translate_enabled") == 0) cfg->translate.enabled = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_backend") == 0) {
                i32 b = atoi(val);
                if (b < 0 || b > (i32)TRANSLATE_BACKEND_API) b = 0;
                cfg->translate.backend = (TranslateBackend)b;
            }
            else if (strcmp(key, "translate_tab_window_sec") == 0) cfg->translate.tab_window_sec = (f32)atof(val);
            else if (strcmp(key, "translate_active_in_claude") == 0) cfg->translate.active_in_claude = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_codex") == 0) cfg->translate.active_in_codex = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_opencode") == 0) cfg->translate.active_in_opencode = strcmp(val, "true") == 0;
            else if (strcmp(key, "translate_active_in_grok") == 0) cfg->translate.active_in_grok = strcmp(val, "true") == 0;
            /* Style values. (style_toolbar_height / style_tab_width are no
             * longer honored — chrome geometry is compile-time in layout.h;
             * stale keys in old config files fall through and are ignored.) */
            else if (strcmp(key, "style_tab_gap") == 0)           cfg->style.tab_gap = (f32)atof(val);
            else if (strcmp(key, "style_tab_dot_size") == 0)      cfg->style.tab_dot_size = (f32)atof(val);
            else if (strcmp(key, "style_tab_close_size") == 0)    cfg->style.tab_close_size = (f32)atof(val);
            else if (strcmp(key, "style_tab_close_margin") == 0)  cfg->style.tab_close_margin = (f32)atof(val);
            else if (strcmp(key, "style_tab_indicator_height") == 0) cfg->style.tab_indicator_height = (f32)atof(val);
            else if (strcmp(key, "style_status_bar_height") == 0) cfg->style.status_bar_height = (f32)atof(val);
            else if (strcmp(key, "style_sidebar_default_width") == 0) cfg->style.sidebar_default_width = (f32)atof(val);
            else if (strcmp(key, "style_sidebar_min_width") == 0) cfg->style.sidebar_min_width = (f32)atof(val);
            else if (strcmp(key, "style_sidebar_max_width") == 0) cfg->style.sidebar_max_width = (f32)atof(val);
            else if (strcmp(key, "style_sidebar_header_height") == 0) cfg->style.sidebar_header_height = (f32)atof(val);
            else if (strcmp(key, "style_terminal_padding") == 0)  cfg->style.terminal_padding = (f32)atof(val);
            else if (strcmp(key, "style_terminal_top_gap") == 0)  cfg->style.terminal_top_gap = (f32)atof(val);
            else if (strcmp(key, "style_active_pane_indicator_thickness") == 0) {
                f32 v = (f32)atof(val);
                if (v < 0.0f) v = 0.0f;
                if (v > 8.0f) v = 8.0f;
                cfg->style.active_pane_indicator_thickness = v;
            }
            else if (strcmp(key, "style_active_pane_indicator_color") == 0) {
                /* "r,g,b" or "r,g,b,a" in 0..1 floats; alpha 0 = use theme.cursor. */
                f32 cr = 0, cg = 0, cb = 0, ca = 1.0f;
                int n = sscanf(val, "\"%f,%f,%f,%f\"", &cr, &cg, &cb, &ca);
                if (n < 3) n = sscanf(val, "%f,%f,%f,%f", &cr, &cg, &cb, &ca);
                if (n >= 3) {
                    cfg->style.active_pane_indicator_color =
                        (Color){cr, cg, cb, (n >= 4 ? ca : 1.0f)};
                }
            }
        }
    }
    fclose(f);
    if (cfg->scrollback_lines < CONFIG_SCROLLBACK_MIN_LINES)
        cfg->scrollback_lines = CONFIG_SCROLLBACK_MIN_LINES;
    if (cfg->scrollback_lines > CONFIG_SCROLLBACK_MAX_LINES)
        cfg->scrollback_lines = CONFIG_SCROLLBACK_MAX_LINES;
    return true;
}

/* Emit a JSON string body with the minimal escaping the format requires:
 * backslash and double-quote get a leading backslash; control chars are
 * dropped. The opening/closing quotes are the caller's responsibility. Used
 * for user-chosen filesystem paths (notes_vault_path) that may legally
 * contain a quote or backslash and would otherwise corrupt the whole file. */
static void config_fputs_json_escaped(FILE *f, const char *s) {
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc((int)c, f); }
        else if (c >= 0x20)        { fputc((int)c, f); }
        /* else: drop raw control characters */
    }
}

bool config_save(const AppConfig *cfg, const char *filepath) {
    /* Ensure directory exists */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", filepath);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(dir, 0755);
    }

    FILE *f = fopen(filepath, "w");
    if (!f) return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"font_path\": \"%s\",\n", cfg->font_path);
    for (i32 i = 0; i < cfg->fallback_font_count && i < MAX_CONFIG_FALLBACK_FONTS; i++) {
        if (cfg->fallback_fonts[i][0])
            fprintf(f, "  \"fallback_font_%d\": \"%s\",\n", i, cfg->fallback_fonts[i]);
    }
    fprintf(f, "  \"font_size\": %.1f,\n", cfg->font_size);
    fprintf(f, "  \"line_height\": %.2f,\n", cfg->line_height);
    fprintf(f, "  \"font_weight\": %.2f,\n", cfg->font_weight);
    fprintf(f, "  \"theme\": \"%s\",\n", cfg->theme_name);
    fprintf(f, "  \"scrollback\": %d,\n", cfg->scrollback_lines);
    fprintf(f, "  \"tab_sleep_idle_minutes\": %.1f,\n", cfg->tab_sleep_idle_minutes);
    fprintf(f, "  \"sidebar_width\": %.1f,\n", cfg->sidebar_width);
    fprintf(f, "  \"cursor_style\": %d,\n", cfg->cursor_style);
    fprintf(f, "  \"cursor_blink\": %s,\n", cfg->cursor_blink ? "true" : "false");
    fprintf(f, "  \"confirm_close_agent\": %s,\n", cfg->confirm_close_agent ? "true" : "false");
    fprintf(f, "  \"bold_is_bright\": %s,\n", cfg->bold_is_bright ? "true" : "false");
    fprintf(f, "  \"show_scrollbar\": %s,\n", cfg->show_scrollbar ? "true" : "false");
    fprintf(f, "  \"show_tab_bar\": %s,\n", cfg->show_tab_bar ? "true" : "false");
    fprintf(f, "  \"show_toolbar_icons\": %s,\n", cfg->show_toolbar_icons ? "true" : "false");
    fprintf(f, "  \"show_status_bar\": %s,\n", cfg->show_status_bar ? "true" : "false");
    fprintf(f, "  \"bidi_enabled\": %s,\n", cfg->bidi_enabled ? "true" : "false");
    fprintf(f, "  \"opacity\": %.2f,\n", cfg->opacity);
    fprintf(f, "  \"padding\": %.1f,\n", cfg->padding);
    fprintf(f, "  \"option_as_alt\": %s,\n", cfg->option_as_alt ? "true" : "false");
    fprintf(f, "  \"cursor_animate\": %s,\n", cfg->cursor_animate ? "true" : "false");
    fprintf(f, "  \"notify_command_threshold\": %.1f,\n", cfg->notify_command_threshold);
    fprintf(f, "  \"allow_osc52_write\": %s,\n", cfg->allow_osc52_write ? "true" : "false");
    fprintf(f, "  \"vault_auto_lock_minutes\": %d,\n", cfg->vault_auto_lock_minutes);
    fprintf(f, "  \"enable_ligatures\": %s,\n", cfg->enable_ligatures ? "true" : "false");
    /* Quake mode */
    fprintf(f, "  \"quake_mode\": %s,\n", cfg->quake_mode ? "true" : "false");
    fprintf(f, "  \"quake_hotkey\": \"%s\",\n", cfg->quake_hotkey);
    fprintf(f, "  \"quake_height_ratio\": %.2f,\n", cfg->quake_height_ratio);
    fprintf(f, "  \"quake_animation_duration\": %.2f,\n", cfg->quake_animation_duration);
    /* Translate-on-Tab */
    fprintf(f, "  \"translate_enabled\": %s,\n", cfg->translate.enabled ? "true" : "false");
    fprintf(f, "  \"translate_backend\": %d,\n", (i32)cfg->translate.backend);
    fprintf(f, "  \"translate_agent_id\": \"%s\",\n", cfg->translate.agent_id);
    fprintf(f, "  \"translate_agent_model\": \"%s\",\n", cfg->translate.agent_model);
    fprintf(f, "  \"translate_local_path\": \"%s\",\n", cfg->translate.local_model_path);
    fprintf(f, "  \"translate_api_provider\": \"%s\",\n", cfg->translate.api_provider);
    fprintf(f, "  \"translate_api_model\": \"%s\",\n", cfg->translate.api_model);
    fprintf(f, "  \"translate_api_key\": \"%s\",\n", cfg->translate.api_key);
    fprintf(f, "  \"translate_api_base_url\": \"%s\",\n", cfg->translate.api_base_url);
    fprintf(f, "  \"translate_source_lang\": \"%s\",\n", cfg->translate.source_lang);
    fprintf(f, "  \"translate_target_lang\": \"%s\",\n", cfg->translate.target_lang);
    fprintf(f, "  \"translate_tab_window_sec\": %.2f,\n", cfg->translate.tab_window_sec);
    fprintf(f, "  \"translate_active_in_claude\": %s,\n",   cfg->translate.active_in_claude   ? "true" : "false");
    fprintf(f, "  \"translate_active_in_codex\": %s,\n",    cfg->translate.active_in_codex    ? "true" : "false");
    fprintf(f, "  \"translate_active_in_opencode\": %s,\n", cfg->translate.active_in_opencode ? "true" : "false");
    fprintf(f, "  \"translate_active_in_grok\": %s,\n",     cfg->translate.active_in_grok     ? "true" : "false");
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
        fprintf(f, "  \"background_image\": \"%s\",\n", cfg->background_image);
    }
    /* Notes/graph Vault root (empty => computed default). Escaped because it
     * is a user-picked filesystem path that may contain a quote/backslash. */
    if (cfg->notes_vault_path[0]) {
        fprintf(f, "  \"notes_vault_path\": \"");
        config_fputs_json_escaped(f, cfg->notes_vault_path);
        fprintf(f, "\",\n");
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
        fprintf(f, "  \"profile_%d_name\": \"%s\",\n", i, p->name);
        if (p->has_font) {
            fprintf(f, "  \"profile_%d_font_path\": \"%s\",\n", i, p->font_path);
            fprintf(f, "  \"profile_%d_font_size\": %.1f,\n", i, p->font_size);
        }
        if (p->has_theme) fprintf(f, "  \"profile_%d_theme\": \"%s\",\n", i, p->theme_name);
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
    fclose(f);
    return true;
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
