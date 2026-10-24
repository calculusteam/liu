/*
 * Liu - Theme import (.itermcolors, YAML/TOML)
 * Parses external color scheme files and converts to Theme structs.
 */
#ifndef CORE_THEME_IMPORT_H
#define CORE_THEME_IMPORT_H

#include "core/types.h"
#include "core/config.h"

/* Maximum user-imported themes */
#define MAX_USER_THEMES 32

/* Global user theme storage */
extern Theme g_user_themes[MAX_USER_THEMES];
extern i32   g_user_theme_count;

/* Import a .itermcolors file (XML plist).
 * Populates a Theme struct. Returns true on success. */
bool theme_import_itermcolors(const char *filepath, Theme *out);

/* Import a YAML/TOML color scheme file.
 * Populates a Theme struct. Returns true on success. */
bool theme_import_alacritty(const char *filepath, Theme *out);

/* Auto-detect format and import. Returns true on success. */
bool theme_import_file(const char *filepath, Theme *out);

/* Save an imported theme as JSON to ~/.config/Liu/themes/<name>.json */
bool theme_save_user(const Theme *theme);

/* Guarantee a theme's tab-strip title text is legible — fills in tab_active_fg
 * / tab_inactive_fg when a user/AI theme omitted them or chose a colour that
 * vanishes into the tab background. Applied automatically when themes load. */
void theme_enforce_tab_text_legibility(Theme *t);

/* Load all user themes from ~/.config/Liu/themes/ on startup */
void theme_load_user_themes(void);

/* Get the user themes directory path */
const char *theme_user_dir(void);

/* Delete a user theme by display name. Removes the on-disk JSON and
 * reloads g_user_themes. Refuses to act when the name doesn't match a
 * known user theme (so built-ins are safe by construction). Returns
 * true when the file was successfully removed. */
bool theme_delete_user(const char *name);

#endif /* CORE_THEME_IMPORT_H */
