#ifndef LIU_X11_IDENTITY_H
#define LIU_X11_IDENTITY_H

/* What an X11 window says about the application that owns it.
 *
 * A window manager's own dialogs ("not responding"), a taskbar and an
 * alt-tab list name the application from these, and find its icon through the
 * installed .desktop file. Window managers read the desktop file's id from
 * _KDE_NET_WM_DESKTOP_FILE or _GTK_APPLICATION_ID and show that file's
 * Name= ("Liu"); a window that carries neither is named after its WM_CLASS
 * alone ("liu"). The three strings below are one identity, and
 * packaging/linux/liu.desktop (also written by scripts/install.sh) carries
 * the same: StartupWMClass is the class instance, the file is <id>.desktop. */

#define X11_DESKTOP_FILE_ID  "liu"   /* liu.desktop, without the suffix */
#define X11_WM_CLASS_NAME    "liu"   /* WM_CLASS res_name = StartupWMClass */
#define X11_WM_CLASS_CLASS   "Liu"   /* WM_CLASS res_class */

#endif
