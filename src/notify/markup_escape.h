/*
 * Liu — text that is about to be shown by a desktop notification daemon.
 *
 * The Desktop Notifications spec lets a daemon render a body as MARKUP — <b>, <i>, <u> and,
 * on GNOME Shell, KDE Plasma, dunst and mako, <a href="..."> links (some also <img>). The
 * banner text of liu-notify is an agent's own summary, which an injected instruction or a
 * web page the agent just read can shape, so unescaped it can put a clickable link in a
 * notification that looks like Liu's own. Plain text has to be escaped to stay plain text:
 * & < > become entities, which a markup-capable daemon draws as the characters they stand
 * for and a plain-text one shows literally (harmless). Control bytes other than TAB and LF
 * are dropped.
 *
 * Never splits an entity: a replacement that does not fit ends the output, so the result is
 * always well-formed. Returns the bytes written, excluding the NUL.
 */
#ifndef NOTIFY_MARKUP_ESCAPE_H
#define NOTIFY_MARKUP_ESCAPE_H

#include <stddef.h>

static inline size_t notify_escape_markup(const char *in, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; p++) {
        const char *rep = NULL;
        size_t rl = 0;
        if (*p == '&')      { rep = "&amp;"; rl = 5; }
        else if (*p == '<') { rep = "&lt;";  rl = 4; }
        else if (*p == '>') { rep = "&gt;";  rl = 4; }
        else if ((*p < 0x20 && *p != '\t' && *p != '\n') || *p == 0x7f) continue;
        if (rep) {
            if (o + rl + 1 > cap) break;
            for (size_t i = 0; i < rl; i++) out[o++] = rep[i];
        } else {
            if (o + 2 > cap) break;
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
    return o;
}

#endif /* NOTIFY_MARKUP_ESCAPE_H */
