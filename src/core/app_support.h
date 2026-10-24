/*
 * Liu — where Liu keeps its own data under $HOME: downloaded models, the notes
 * vault. macOS keeps it in ~/Library/Application Support/Liu; on X11 the
 * desktop's place for an application's data is ~/.local/share (XDG), and a
 * ~/Library folder appearing in a Linux home is a stranger's path.
 */
#ifndef LIU_CORE_APP_SUPPORT_H
#define LIU_CORE_APP_SUPPORT_H

#ifdef PLATFORM_MACOS
#define LIU_APP_SUPPORT_SUBDIR "Library/Application Support/Liu"
#else
#define LIU_APP_SUPPORT_SUBDIR ".local/share/liu"
#endif

#endif
