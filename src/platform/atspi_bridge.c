/*
 * Liu - AT-SPI2 bridge (X11). See atspi_bridge.h for the object tree.
 *
 * Joining. The accessibility bus is a second D-Bus daemon. Its address comes
 * from AT_SPI_BUS_ADDRESS, else the AT_SPI_BUS property on the X root window,
 * else org.a11y.Bus.GetAddress on the session bus (which starts the bus when
 * it is installed and not running). On it, Liu says Hello, embeds its root in
 * the registry (org.a11y.atspi.Socket.Embed), subscribes to the registry's
 * listener announcements and reads the current list. All of that runs on a
 * detached worker, each round trip bounded (ATSPI_BOOT_TIMEOUT_MS); the main
 * thread adopts the finished connection on a later loop turn, or forgets a
 * worker still running at quit. It starts after the first presented frame, so
 * nothing before the first frame waits for it. NO_AT_BRIDGE=1 keeps it off.
 *
 * Answering. The connection's fd sits in the loop's wait beside the X
 * connection (wake-up only: it never counts as session I/O), and
 * atspi_service reads, dispatches and writes it without blocking once per
 * loop turn. A method call is therefore answered on the turn it wakes, and
 * an idle Liu with no assistive app asking does nothing at all.
 *
 * Events. Sent only while some assistive app has registered for them (the
 * registry's list, kept current from its signals). Text, caret and selection
 * changes are found where the macOS view finds them: after a PRESENTED frame
 * (the terminal changing is what makes frames), by the provider's two
 * generations — no new wake source, and an idle window presents nothing. At
 * most one post per ATSPI_POST_INTERVAL per window; a change inside the
 * interval gets one trailing check when it ends, which shortens the loop's
 * wait (atspi_wait_limit) and paints nothing. A keystroke is the exception:
 * the frame that shows its echo posts at once, and what the keystrokes typed
 * goes out one keystroke an insertion (atspi_typing.h). A text change is sent as the
 * exact edits between the text last described and the current one
 * (atspi_text_diff: the rows scrolled off the top, then the one span that
 * differs), so a typed character is one insertion and output that scrolls is
 * "the top rows went, the new rows came", not the whole screen again.
 *
 * Offsets. The provider counts UTF-16 units; AT-SPI counts characters.
 * atspi_text.h converts once per snapshot and every answer here is in
 * characters.
 */
#if defined(PLATFORM_LINUX) && defined(LIU_HAVE_DBUS)

#include "platform/atspi_bridge.h"
#include "platform/platform.h"

#include <dbus/dbus.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ATSPI_PATH_PREFIX    "/org/a11y/atspi/accessible/"
#define ATSPI_PATH_ROOT      "/org/a11y/atspi/accessible/root"
#define ATSPI_PATH_NULL      "/org/a11y/atspi/null"
#define ATSPI_PATH_CACHE     "/org/a11y/atspi/cache"
#define ATSPI_PATH_REGISTRY  "/org/a11y/atspi/registry"
#define ATSPI_BUS_REGISTRY   "org.a11y.atspi.Registry"

#define IF_ACCESSIBLE   "org.a11y.atspi.Accessible"
#define IF_APPLICATION  "org.a11y.atspi.Application"
#define IF_COMPONENT    "org.a11y.atspi.Component"
#define IF_TEXT         "org.a11y.atspi.Text"
#define IF_CACHE        "org.a11y.atspi.Cache"
#define IF_SOCKET       "org.a11y.atspi.Socket"
#define IF_REGISTRY     "org.a11y.atspi.Registry"
#define IF_EV_OBJECT    "org.a11y.atspi.Event.Object"
#define IF_EV_WINDOW    "org.a11y.atspi.Event.Window"
#define IF_EV_FOCUS     "org.a11y.atspi.Event.Focus"
#define IF_PROPERTIES   "org.freedesktop.DBus.Properties"
#define IF_INTROSPECT   "org.freedesktop.DBus.Introspectable"

/* AtspiRole, AtspiStateType, AtspiCoordType, AtspiComponentLayer by value
 * (atspi-constants.h). */
enum { ROLE_FRAME = 23, ROLE_TERMINAL = 60, ROLE_APPLICATION = 75 };
enum {
    ST_ACTIVE = 1, ST_DEFUNCT = 6, ST_EDITABLE = 7, ST_ENABLED = 8,
    ST_FOCUSABLE = 11, ST_FOCUSED = 12, ST_MULTI_LINE = 17, ST_RESIZABLE = 21,
    ST_SENSITIVE = 24, ST_SHOWING = 25, ST_VISIBLE = 30, ST_SELECTABLE_TEXT = 38,
};
enum { COORD_SCREEN = 0, COORD_WINDOW = 1, COORD_PARENT = 2 };
enum { LAYER_WIDGET = 3, LAYER_WINDOW = 7 };

#define ATSPI_BOOT_TIMEOUT_MS     3000  /* each bootstrap round trip (worker only) */
#define ATSPI_ADDRESS_TIMEOUT_MS  5000  /* GetAddress may have to start the bus */
/* How long a snapshot answers queries without asking the provider whether
 * anything moved (the macOS view's number), and the post rate cap. The cap
 * is not the macOS view's 0.25 s: VoiceOver re-reads the whole value on each
 * post, an AT-SPI event CARRIES its change. At 0.25 s typed characters would
 * go out in chunks, which a screen reader cannot match to the keys it
 * already echoed, so it speaks them a second time. 0.05 s holds a flood at
 * 20 posts a second; typing does not wait for it (atspi_typing.h), because
 * at 0.05 s alone keys typed in quick succession still go out in pairs and
 * are spoken as pairs. */
#define ATSPI_CACHE_MAX_SEC       0.25
#define ATSPI_POST_INTERVAL       0.05
#define ATSPI_DISPATCH_MAX        256   /* messages handled per loop turn */
#define ATSPI_LISTENERS_MAX       2048

/* ---- process state ------------------------------------------------------------ */

/* One registered listener: the client and its event, split at ':' and
 * folded (lower case, no '-'), so "object:text-changed:insert" and
 * "Object:TextChanged:Insert" compare equal. */
typedef struct {
    char bus[96];
    char ev[3][40];
} AtspiListener;

enum { BOOT_RUNNING = 0, BOOT_DONE = 1, BOOT_ABANDONED = 2 };

/* What the worker is given and hands back. Owned by the worker until it
 * stores BOOT_DONE, then by the main thread; an abandoned one frees itself. */
typedef struct {
    _Atomic int     state;
    char            env_addr[1024];
    char            x_addr[1024];
    char            session_addr[1024];
    char            runtime_dir[512];
    DBusConnection *conn;
    char            desktop_bus[256];
    char            desktop_path[256];
    bool            listeners_known;
    AtspiListener  *listeners;
    i32             nlisteners;
    char            why[192];
} AtspiBoot;

static struct {
    bool            started;
    AtspiBoot      *boot;
    DBusConnection *conn;
    int             fd;
    char            unique[256];
    char            desktop_bus[256];
    char            desktop_path[256];
    i32             app_id;
    AtspiWindow    *windows;
    u32             next_window_id;
    AtspiListener  *listeners;
    i32             nlisteners, cap_listeners;
    bool            listeners_known;
    bool            want_any;    /* some assistive app listens to anything */
    bool            want_text;   /* ... to text, caret or selection changes */
    bool            probe;       /* LIU_ATSPI_PROBE=1: log to stderr */
} g_atspi = { .fd = -1 };

#define ATSPI_LOG(...) do { if (g_atspi.probe) fprintf(stderr, "[atspi] " __VA_ARGS__); } while (0)

/* ---- listeners ------------------------------------------------------------------ */

static void atspi_fold(const char *in, usize len, char *out, usize cap) {
    usize o = 0;
    for (usize i = 0; i < len && in[i] && o + 1 < cap; i++) {
        char c = in[i];
        if (c == '-' || c == '_') continue;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[o++] = c;
    }
    out[o] = '\0';
}

static void atspi_listener_parse(AtspiListener *l, const char *bus, const char *event) {
    memset(l, 0, sizeof(*l));
    snprintf(l->bus, sizeof(l->bus), "%s", bus ? bus : "");
    const char *p = event ? event : "";
    for (i32 k = 0; k < 3; k++) {
        const char *colon = strchr(p, ':');
        usize len = colon ? (usize)(colon - p) : strlen(p);
        atspi_fold(p, len, l->ev[k], sizeof(l->ev[k]));
        if (!colon) break;
        p = colon + 1;
    }
}

/* Would an assistive app hear this event? A registered event matches every
 * event its non-empty components are a prefix of ("object:" hears all object
 * events). An unknown list (an old registry) counts as "everyone listens". */
static bool atspi_listened(const char *klass, const char *major, const char *minor) {
    if (!g_atspi.listeners_known) return true;
    char e[3][40];
    atspi_fold(klass, strlen(klass), e[0], sizeof(e[0]));
    atspi_fold(major, strlen(major), e[1], sizeof(e[1]));
    atspi_fold(minor, strlen(minor), e[2], sizeof(e[2]));
    for (i32 i = 0; i < g_atspi.nlisteners; i++) {
        const AtspiListener *l = &g_atspi.listeners[i];
        bool match = true;
        for (i32 k = 0; k < 3 && l->ev[k][0]; k++)
            if (strcmp(l->ev[k], e[k]) != 0) { match = false; break; }
        if (match) return true;
    }
    return false;
}

static void atspi_wants_update(void) {
    g_atspi.want_any = !g_atspi.listeners_known || g_atspi.nlisteners > 0;
    g_atspi.want_text = atspi_listened("object", "text-changed", "insert") ||
                        atspi_listened("object", "text-changed", "delete") ||
                        atspi_listened("object", "text-caret-moved", "") ||
                        atspi_listened("object", "text-selection-changed", "");
}

static void atspi_listener_add(const char *bus, const char *event) {
    if (g_atspi.nlisteners >= ATSPI_LISTENERS_MAX) return;
    if (g_atspi.nlisteners == g_atspi.cap_listeners) {
        i32 cap = g_atspi.cap_listeners ? g_atspi.cap_listeners * 2 : 32;
        AtspiListener *n = realloc(g_atspi.listeners, (usize)cap * sizeof(*n));
        if (!n) return;
        g_atspi.listeners = n;
        g_atspi.cap_listeners = cap;
    }
    atspi_listener_parse(&g_atspi.listeners[g_atspi.nlisteners++], bus, event);
}

/* One registration goes; an EMPTY event is a client that left the bus (the
 * registry says so once, with no event), and all of its registrations go. */
static void atspi_listener_remove(const char *bus, const char *event) {
    AtspiListener key;
    atspi_listener_parse(&key, bus, event);
    bool all = !event || !event[0];
    for (i32 i = 0; i < g_atspi.nlisteners; ) {
        AtspiListener *l = &g_atspi.listeners[i];
        if (strcmp(l->bus, key.bus) == 0 &&
            (all || memcmp(l->ev, key.ev, sizeof(key.ev)) == 0)) {
            g_atspi.listeners[i] = g_atspi.listeners[--g_atspi.nlisteners];
            if (!all) return;
            continue;
        }
        i++;
    }
}

/* ---- references and paths --------------------------------------------------------- */

typedef enum { OBJ_ROOT = 1, OBJ_FRAME, OBJ_TERM } AtspiObjKind;
typedef struct { AtspiObjKind kind; AtspiWindow *w; } AtspiObj;

static void atspi_path(const AtspiObj *o, char *out, usize cap) {
    if (o->kind == OBJ_ROOT) snprintf(out, cap, "%s", ATSPI_PATH_ROOT);
    else if (o->kind == OBJ_FRAME) snprintf(out, cap, ATSPI_PATH_PREFIX "w%u", o->w->id);
    else snprintf(out, cap, ATSPI_PATH_PREFIX "w%ut%u", o->w->id, o->w->term_serial);
}

static AtspiWindow *atspi_window_by_id(u32 id) {
    for (AtspiWindow *w = g_atspi.windows; w; w = w->next)
        if (w->id == id) return w;
    return NULL;
}

static i32 atspi_window_index(const AtspiWindow *aw) {
    i32 i = 0;
    for (AtspiWindow *w = g_atspi.windows; w; w = w->next, i++)
        if (w == aw) return i;
    return -1;
}

static i32 atspi_window_count(void) {
    i32 n = 0;
    for (AtspiWindow *w = g_atspi.windows; w; w = w->next) n++;
    return n;
}

static bool atspi_parse_u32(const char **p, u32 *out) {
    const char *s = *p;
    u64 v = 0;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (u64)(*s - '0');
        if (v > 0xFFFFFFFFull) return false;
        s++;
    }
    *out = (u32)v;
    *p = s;
    return true;
}

/* The object a path names; false for a path that names nothing (a terminal
 * object whose pane is no longer the one described is gone). */
static bool atspi_resolve(const char *path, AtspiObj *o) {
    if (strcmp(path, ATSPI_PATH_ROOT) == 0) { o->kind = OBJ_ROOT; o->w = NULL; return true; }
    usize pl = sizeof(ATSPI_PATH_PREFIX) - 1;
    if (strncmp(path, ATSPI_PATH_PREFIX "w", pl + 1) != 0) return false;
    const char *p = path + pl + 1;
    u32 id = 0, serial = 0;
    if (!atspi_parse_u32(&p, &id)) return false;
    AtspiWindow *w = atspi_window_by_id(id);
    if (!w) return false;
    o->w = w;
    if (*p == '\0') { o->kind = OBJ_FRAME; return true; }
    if (*p++ != 't' || !atspi_parse_u32(&p, &serial) || *p != '\0') return false;
    if (!w->source || serial != w->term_serial) return false;
    o->kind = OBJ_TERM;
    return true;
}

static void atspi_append_ref_raw(DBusMessageIter *it, const char *bus, const char *path) {
    DBusMessageIter st;
    if (!dbus_message_iter_open_container(it, DBUS_TYPE_STRUCT, NULL, &st)) return;
    dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &bus);
    dbus_message_iter_append_basic(&st, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_close_container(it, &st);
}

static void atspi_append_ref(DBusMessageIter *it, const AtspiObj *o) {
    char path[96];
    if (!o) { atspi_append_ref_raw(it, g_atspi.unique, ATSPI_PATH_NULL); return; }
    atspi_path(o, path, sizeof(path));
    atspi_append_ref_raw(it, g_atspi.unique, path);
}

static void atspi_append_parent(DBusMessageIter *it, const AtspiObj *o) {
    if (o->kind == OBJ_ROOT) {
        if (g_atspi.desktop_bus[0])
            atspi_append_ref_raw(it, g_atspi.desktop_bus, g_atspi.desktop_path);
        else
            atspi_append_ref_raw(it, ATSPI_BUS_REGISTRY, ATSPI_PATH_ROOT);
    } else if (o->kind == OBJ_FRAME) {
        AtspiObj r = { OBJ_ROOT, NULL };
        atspi_append_ref(it, &r);
    } else {
        AtspiObj f = { OBJ_FRAME, o->w };
        atspi_append_ref(it, &f);
    }
}

/* A string as libdbus accepts it (atspi_utf8_clean), appended. */
static void atspi_append_str(DBusMessageIter *it, const char *s, u32 len) {
    char stack[512];
    u32 cap = len * 3u + 1u;
    char *buf = cap <= sizeof(stack) ? stack : malloc(cap);
    if (!buf) { const char *e = ""; dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &e); return; }
    atspi_utf8_clean(s, len, buf, cap);
    const char *p = buf;
    dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &p);
    if (buf != stack) free(buf);
}

static void atspi_append_cstr(DBusMessageIter *it, const char *s) {
    atspi_append_str(it, s ? s : "", s ? (u32)strlen(s) : 0);
}

/* ---- snapshots ------------------------------------------------------------------- */

static void atspi_gens(AtspiWindow *aw, PlatformA11yText *g) {
    memset(g, 0, sizeof(*g));
    if (aw->fn) aw->fn(aw->pw, aw->user, g, true);
}

/* The terminal's text for answering queries: the cached snapshot while it is
 * young and the provider's generations have not moved, a fresh one otherwise.
 * A rebuild never overwrites the text the last events described. */
static AtspiText *atspi_term_text(AtspiWindow *aw, bool force) {
    f64 now = platform_time_sec();
    AtspiText *cur = &aw->snap[aw->cur];
    if (!force && aw->have_cur && now - cur->built_at < ATSPI_CACHE_MAX_SEC) {
        PlatformA11yText g;
        atspi_gens(aw, &g);
        if (g.gen_value == cur->s.gen_value && g.gen_selection == cur->s.gen_selection &&
            g.source == cur->s.source)
            return cur;
    }
    u8 target = (aw->have_told && aw->told == aw->cur) ? (u8)(aw->cur ^ 1u) : aw->cur;
    PlatformA11yText s;
    memset(&s, 0, sizeof(s));
    if (aw->fn) aw->fn(aw->pw, aw->user, &s, false);
    AtspiText *t = &aw->snap[target];
    atspi_text_load(t, &s);
    t->built_at = now;
    aw->cur = target;
    aw->have_cur = true;
    return t;
}

static const char *atspi_frame_name(const AtspiWindow *aw) {
    return aw->title[0] ? aw->title : "Liu";
}

/* ---- events ----------------------------------------------------------------------- */

typedef enum { ANY_STR, ANY_INT, ANY_REF } AtspiAnyKind;

/* One AT-SPI event signal: (minor, detail1, detail2, any_data, properties). */
static void atspi_emit(const AtspiObj *src, const char *iface, const char *member,
                       const char *minor, i32 d1, i32 d2, AtspiAnyKind kind,
                       const char *s, u32 slen, const AtspiObj *ref) {
    if (!g_atspi.conn) return;
    char path[96];
    atspi_path(src, path, sizeof(path));
    DBusMessage *m = dbus_message_new_signal(path, iface, member);
    if (!m) return;
    DBusMessageIter it, var, dict;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &minor);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &d1);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &d2);
    const char *sig = kind == ANY_REF ? "(so)" : kind == ANY_INT ? "i" : "s";
    if (dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, sig, &var)) {
        if (kind == ANY_REF) atspi_append_ref(&var, ref);
        else if (kind == ANY_INT) { i32 z = 0; dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &z); }
        else atspi_append_str(&var, s ? s : "", s ? slen : 0);
        dbus_message_iter_close_container(&it, &var);
    }
    if (dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict))
        dbus_message_iter_close_container(&it, &dict);
    dbus_connection_send(g_atspi.conn, m, NULL);
    dbus_message_unref(m);
}

static void atspi_emit_state(const AtspiObj *o, const char *state, bool on) {
    if (!g_atspi.want_any) return;
    atspi_emit(o, IF_EV_OBJECT, "StateChanged", state, on ? 1 : 0, 0, ANY_INT, NULL, 0, NULL);
}

static void atspi_emit_children(const AtspiObj *parent, bool add, i32 index, const AtspiObj *child) {
    if (!g_atspi.want_any) return;
    atspi_emit(parent, IF_EV_OBJECT, "ChildrenChanged", add ? "add" : "remove", index, 0,
               ANY_REF, NULL, 0, child);
}

static void atspi_emit_name(const AtspiObj *o, const char *name) {
    if (!g_atspi.want_any) return;
    atspi_emit(o, IF_EV_OBJECT, "PropertyChange", "accessible-name", 0, 0, ANY_STR,
               name, (u32)strlen(name), NULL);
}

/* The terminal's keyboard focus, as an assistive app tracks it: the state,
 * and the focus event that older listeners still ask for. */
static void atspi_emit_term_focus(AtspiWindow *aw, bool focused) {
    if (!aw->source) return;
    AtspiObj t = { OBJ_TERM, aw };
    atspi_emit_state(&t, "focused", focused);
    if (focused && atspi_listened("focus", "focus", ""))
        atspi_emit(&t, IF_EV_FOCUS, "Focus", "", 0, 0, ANY_STR, "", 0, NULL);
}

/* The described pane moved (another tab, another split pane, or none): the
 * old terminal object goes, a new one under another path comes. */
static void atspi_source_moved(AtspiWindow *aw, u64 source) {
    AtspiObj frame = { OBJ_FRAME, aw };
    if (aw->source) {
        AtspiObj old = { OBJ_TERM, aw };
        atspi_emit_state(&old, "defunct", true);
        atspi_emit_children(&frame, false, 0, &old);
    }
    aw->source = source;
    aw->have_cur = false;
    aw->have_told = false;
    atspi_typing_clear(&aw->typing);   /* typed into the pane that went */
    if (!source) return;
    aw->term_serial++;
    AtspiObj fresh = { OBJ_TERM, aw };
    atspi_emit_children(&frame, true, 0, &fresh);
    if (aw->active) atspi_emit_term_focus(aw, true);
    ATSPI_LOG("window %u: terminal object t%u\n", aw->id, aw->term_serial);
}

/* What changed since the last post, as events. Called after a presented
 * frame (rate-capped) and for the trailing check. */
static void atspi_window_check(AtspiWindow *aw, f64 now) {
    if (!g_atspi.conn || !aw->fn || !aw->id) return;
    PlatformA11yText g;
    atspi_gens(aw, &g);
    if (g.source != aw->source) atspi_source_moved(aw, g.source);
    if (!g_atspi.want_text || !aw->source) {
        aw->have_told = false;
        return;
    }
    if (aw->have_told && g.gen_value == aw->seen_value && g.gen_selection == aw->seen_sel)
        return;
    AtspiText *t = atspi_term_text(aw, false);
    aw->seen_value = t->s.gen_value;
    aw->seen_sel = t->s.gen_selection;
    aw->last_post = now;
    if (!aw->have_told) {
        /* The base an assistive app is assumed to hold: nothing is sent for it
         * (a new terminal object is read whole by whoever looks at it). */
        aw->told = aw->cur;
        aw->have_told = true;
        return;
    }
    AtspiText *o = &aw->snap[aw->told];
    if (o == t) return;
    AtspiObj term = { OBJ_TERM, aw };

    bool text_moved = false;
    AtspiTextEdit ed[3];
    u32 ne = atspi_text_diff(o, t, ed);
    /* What the pending keystrokes typed goes out one keystroke an insertion,
     * each followed by its caret (atspi_typing.h). */
    u32 lens[ATSPI_TYPED_UNITS];
    u32 pieces = atspi_typing_split(&aw->typing, now, ed, ne, o, t, lens, ATSPI_TYPED_UNITS);
    if (pieces > 1) {
        bool ins = atspi_listened("object", "text-changed", "insert");
        bool car = atspi_listened("object", "text-caret-moved", "");
        u32 at = ed[0].at, src = ed[0].src;
        for (u32 i = 0; i < pieces; i++) {
            u32 bytes = 0;
            const char *p = atspi_text_slice(t, src, src + lens[i], &bytes);
            if (ins)
                atspi_emit(&term, IF_EV_OBJECT, "TextChanged", "insert", (i32)at, (i32)lens[i],
                           ANY_STR, p, bytes, NULL);
            at += lens[i];
            src += lens[i];
            if (car)
                atspi_emit(&term, IF_EV_OBJECT, "TextCaretMoved", "", (i32)at, 0, ANY_STR, "", 0, NULL);
        }
        ne = 0;
    }
    for (u32 i = 0; i < ne; i++) {
        const AtspiText *from = ed[i].del ? o : t;
        u32 bytes = 0;
        const char *p = atspi_text_slice(from, ed[i].src, ed[i].src + ed[i].len, &bytes);
        const char *minor = ed[i].del ? "delete" : "insert";
        if (atspi_listened("object", "text-changed", minor))
            atspi_emit(&term, IF_EV_OBJECT, "TextChanged", minor, (i32)ed[i].at, (i32)ed[i].len,
                       ANY_STR, p, bytes, NULL);
        text_moved = true;
    }
    i32 caret_o = o->s.has_cursor ? (i32)o->s.cursor16 : -1;
    i32 caret_t = t->s.has_cursor ? (i32)t->s.cursor16 : -1;
    if (pieces > 1) caret_o = caret_t;   /* each piece moved it already */
    if ((caret_t != caret_o || text_moved) && caret_t >= 0 &&
        atspi_listened("object", "text-caret-moved", ""))
        atspi_emit(&term, IF_EV_OBJECT, "TextCaretMoved", "", caret_t, 0, ANY_STR, "", 0, NULL);
    if (o->s.gen_selection != t->s.gen_selection &&
        atspi_listened("object", "text-selection-changed", ""))
        atspi_emit(&term, IF_EV_OBJECT, "TextSelectionChanged", "", 0, 0, ANY_STR, "", 0, NULL);
    if (strcmp(o->label ? o->label : "", t->label ? t->label : "") != 0)
        atspi_emit_name(&term, t->label ? t->label : "");
    if (pieces > 1) ATSPI_LOG("window %u: %u typed insertion(s)\n", aw->id, pieces);
    else ATSPI_LOG("window %u: %u edit(s), caret %d -> %d\n", aw->id, ne, caret_o, caret_t);
    aw->told = aw->cur;
}

/* ---- the bootstrap worker ------------------------------------------------------------ */

static DBusConnection *atspi_open_bus(const char *addr, char *why, usize cap) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = dbus_connection_open_private(addr, &err);
    if (!c) {
        snprintf(why, cap, "open %s: %s", addr, err.message ? err.message : "?");
        dbus_error_free(&err);
        return NULL;
    }
    dbus_connection_set_exit_on_disconnect(c, FALSE);
    /* Hello by hand: dbus_bus_register would wait libdbus's default 25 s. */
    DBusMessage *m = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
                                                  DBUS_INTERFACE_DBUS, "Hello");
    DBusMessage *r = m ? dbus_connection_send_with_reply_and_block(c, m, ATSPI_BOOT_TIMEOUT_MS, &err)
                       : NULL;
    if (m) dbus_message_unref(m);
    const char *name = NULL;
    if (!r || !dbus_message_get_args(r, &err, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID) ||
        !name || !dbus_bus_set_unique_name(c, name)) {
        snprintf(why, cap, "hello %s: %s", addr, err.message ? err.message : "no reply");
        dbus_error_free(&err);
        if (r) dbus_message_unref(r);
        dbus_connection_close(c);
        dbus_connection_unref(c);
        return NULL;
    }
    dbus_message_unref(r);
    return c;
}

static void atspi_close_bus(DBusConnection *c) {
    if (!c) return;
    dbus_connection_close(c);
    dbus_connection_unref(c);
}

/* org.a11y.Bus.GetAddress on the session bus. */
static bool atspi_session_lookup(AtspiBoot *b, char *out, usize cap) {
    char addr[1100];
    if (b->session_addr[0]) {
        snprintf(addr, sizeof(addr), "%s", b->session_addr);
    } else if (b->runtime_dir[0]) {
        /* No address in the environment: the per-user bus at its standard
         * place, if it is there. Never autolaunch one. */
        char path[600];
        struct stat st;
        snprintf(path, sizeof(path), "%s/bus", b->runtime_dir);
        if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode)) {
            snprintf(b->why, sizeof(b->why), "no session bus");
            return false;
        }
        char *esc = dbus_address_escape_value(path);
        if (!esc) return false;
        snprintf(addr, sizeof(addr), "unix:path=%s", esc);
        dbus_free(esc);
    } else {
        snprintf(b->why, sizeof(b->why), "no session bus");
        return false;
    }
    DBusConnection *c = atspi_open_bus(addr, b->why, sizeof(b->why));
    if (!c) return false;
    DBusError err;
    dbus_error_init(&err);
    bool ok = false;
    DBusMessage *m = dbus_message_new_method_call("org.a11y.Bus", "/org/a11y/bus",
                                                  "org.a11y.Bus", "GetAddress");
    DBusMessage *r = m ? dbus_connection_send_with_reply_and_block(c, m, ATSPI_ADDRESS_TIMEOUT_MS, &err)
                       : NULL;
    if (m) dbus_message_unref(m);
    const char *a = NULL;
    if (r && dbus_message_get_args(r, &err, DBUS_TYPE_STRING, &a, DBUS_TYPE_INVALID) && a && a[0]) {
        snprintf(out, cap, "%s", a);
        ok = true;
    } else {
        snprintf(b->why, sizeof(b->why), "GetAddress: %s", err.message ? err.message : "empty");
    }
    dbus_error_free(&err);
    if (r) dbus_message_unref(r);
    atspi_close_bus(c);
    return ok;
}

static bool atspi_embed(AtspiBoot *b, DBusConnection *c) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(ATSPI_BUS_REGISTRY, ATSPI_PATH_ROOT,
                                                  IF_SOCKET, "Embed");
    if (!m) return false;
    DBusMessageIter it;
    dbus_message_iter_init_append(m, &it);
    atspi_append_ref_raw(&it, dbus_bus_get_unique_name(c), ATSPI_PATH_ROOT);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, ATSPI_BOOT_TIMEOUT_MS, &err);
    dbus_message_unref(m);
    if (!r) {
        snprintf(b->why, sizeof(b->why), "Embed: %s", err.message ? err.message : "no reply");
        dbus_error_free(&err);
        return false;
    }
    DBusMessageIter ri, st;
    if (dbus_message_iter_init(r, &ri) && dbus_message_iter_get_arg_type(&ri) == DBUS_TYPE_STRUCT) {
        dbus_message_iter_recurse(&ri, &st);
        const char *bus = NULL, *path = NULL;
        if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&st, &bus);
            dbus_message_iter_next(&st);
            if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_OBJECT_PATH)
                dbus_message_iter_get_basic(&st, &path);
        }
        if (bus && path) {
            snprintf(b->desktop_bus, sizeof(b->desktop_bus), "%s", bus);
            snprintf(b->desktop_path, sizeof(b->desktop_path), "%s", path);
        }
    }
    dbus_message_unref(r);
    return true;
}

static void atspi_read_listeners(AtspiBoot *b, DBusConnection *c) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(ATSPI_BUS_REGISTRY, ATSPI_PATH_REGISTRY,
                                                  IF_REGISTRY, "GetRegisteredEvents");
    DBusMessage *r = m ? dbus_connection_send_with_reply_and_block(c, m, ATSPI_BOOT_TIMEOUT_MS, &err)
                       : NULL;
    if (m) dbus_message_unref(m);
    dbus_error_free(&err);
    if (!r) return;
    DBusMessageIter ri, arr, st;
    if (dbus_message_iter_init(r, &ri) && dbus_message_iter_get_arg_type(&ri) == DBUS_TYPE_ARRAY) {
        b->listeners_known = true;
        dbus_message_iter_recurse(&ri, &arr);
        i32 cap = 0;
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT &&
               b->nlisteners < ATSPI_LISTENERS_MAX) {
            dbus_message_iter_recurse(&arr, &st);
            const char *bus = NULL, *ev = NULL;
            if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&st, &bus);
                dbus_message_iter_next(&st);
                if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_STRING)
                    dbus_message_iter_get_basic(&st, &ev);
            }
            if (bus && ev) {
                if (b->nlisteners == cap) {
                    i32 nc = cap ? cap * 2 : 32;
                    AtspiListener *n = realloc(b->listeners, (usize)nc * sizeof(*n));
                    if (!n) break;
                    b->listeners = n;
                    cap = nc;
                }
                atspi_listener_parse(&b->listeners[b->nlisteners++], bus, ev);
            }
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(r);
}

static void *atspi_boot_main(void *arg) {
    AtspiBoot *b = arg;
    DBusConnection *c = NULL;
    char looked_up[1024] = "";
    const char *tries[3] = { b->env_addr, b->x_addr, NULL };
    for (i32 i = 0; i < 3 && !c; i++) {
        const char *addr = tries[i];
        if (i == 2) {
            if (!atspi_session_lookup(b, looked_up, sizeof(looked_up))) break;
            addr = looked_up;
        }
        if (!addr || !addr[0]) continue;
        c = atspi_open_bus(addr, b->why, sizeof(b->why));
    }
    if (c && !atspi_embed(b, c)) {
        atspi_close_bus(c);
        c = NULL;
    }
    if (c) {
        /* Subscribe before reading the list, so a registration in between is
         * not lost. No error argument: libdbus then does not wait. */
        dbus_bus_add_match(c, "type='signal',interface='" IF_REGISTRY "',"
                              "member='EventListenerRegistered'", NULL);
        dbus_bus_add_match(c, "type='signal',interface='" IF_REGISTRY "',"
                              "member='EventListenerDeregistered'", NULL);
        atspi_read_listeners(b, c);
    }
    b->conn = c;
    if (atomic_exchange(&b->state, BOOT_DONE) == BOOT_ABANDONED) {
        atspi_close_bus(c);
        free(b->listeners);
        free(b);
        return NULL;
    }
    platform_wake_event_loop();
    return NULL;
}

static void atspi_start(void) {
    if (g_atspi.started) return;
    g_atspi.started = true;
    const char *probe = getenv("LIU_ATSPI_PROBE");
    g_atspi.probe = probe && *probe && *probe != '0';
    const char *off = getenv("NO_AT_BRIDGE");
    if (off && *off && *off != '0') { ATSPI_LOG("NO_AT_BRIDGE set: off\n"); return; }

    AtspiBoot *b = calloc(1, sizeof(*b));
    if (!b) return;
    atomic_init(&b->state, BOOT_RUNNING);
    /* The environment and the X server are read here, on the main thread;
     * the worker touches neither. */
    const char *e = getenv("AT_SPI_BUS_ADDRESS");
    if (e) snprintf(b->env_addr, sizeof(b->env_addr), "%s", e);
    if (!b->env_addr[0]) linux_a11y_bus_property(b->x_addr, sizeof(b->x_addr));
    e = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (e) snprintf(b->session_addr, sizeof(b->session_addr), "%s", e);
    e = getenv("XDG_RUNTIME_DIR");
    if (e) snprintf(b->runtime_dir, sizeof(b->runtime_dir), "%s", e);

    dbus_threads_init_default();
    pthread_attr_t attr;
    pthread_t th;
    if (pthread_attr_init(&attr) != 0) { free(b); return; }
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, atspi_boot_main, b) != 0) {
        pthread_attr_destroy(&attr);
        free(b);
        return;
    }
    pthread_attr_destroy(&attr);
    g_atspi.boot = b;
    ATSPI_LOG("bootstrap started\n");
}

static DBusHandlerResult atspi_filter(DBusConnection *c, DBusMessage *m, void *user);

static void atspi_adopt(AtspiBoot *b) {
    if (!b->conn) {
        ATSPI_LOG("not joined: %s\n", b->why[0] ? b->why : "no accessibility bus");
        free(b->listeners);
        free(b);
        return;
    }
    g_atspi.conn = b->conn;
    snprintf(g_atspi.unique, sizeof(g_atspi.unique), "%s", dbus_bus_get_unique_name(b->conn));
    snprintf(g_atspi.desktop_bus, sizeof(g_atspi.desktop_bus), "%s", b->desktop_bus);
    snprintf(g_atspi.desktop_path, sizeof(g_atspi.desktop_path), "%s", b->desktop_path);
    free(g_atspi.listeners);
    g_atspi.listeners = b->listeners;
    g_atspi.nlisteners = b->nlisteners;
    g_atspi.cap_listeners = b->nlisteners;
    g_atspi.listeners_known = b->listeners_known;
    free(b);
    int fd = -1;
    if (!dbus_connection_get_unix_fd(g_atspi.conn, &fd)) fd = -1;
    g_atspi.fd = fd;
    dbus_connection_add_filter(g_atspi.conn, atspi_filter, NULL, NULL);
    atspi_wants_update();
    /* Each window's terminal object exists from here: name the pane it
     * describes, take the keyboard state as X has it now, and (when someone
     * already listens for text) the base of the first change. */
    f64 now = platform_time_sec();
    for (AtspiWindow *w = g_atspi.windows; w; w = w->next) {
        PlatformA11yText g;
        atspi_gens(w, &g);
        w->source = g.source;
        if (w->source) w->term_serial++;
        w->active = linux_a11y_window_has_focus(w->pw);
        if (g_atspi.want_text) w->post_due = now;
    }
    ATSPI_LOG("joined as %s, desktop %s %s, %d listener(s)%s\n", g_atspi.unique,
              g_atspi.desktop_bus, g_atspi.desktop_path, g_atspi.nlisteners,
              g_atspi.listeners_known ? "" : " (list unknown)");
}

static void atspi_drop_connection(void) {
    if (!g_atspi.conn) return;
    dbus_connection_remove_filter(g_atspi.conn, atspi_filter, NULL);
    atspi_close_bus(g_atspi.conn);
    g_atspi.conn = NULL;
    g_atspi.fd = -1;
    g_atspi.nlisteners = 0;
    g_atspi.want_any = g_atspi.want_text = false;
    for (AtspiWindow *w = g_atspi.windows; w; w = w->next) {
        w->have_told = false;
        w->post_due = 0.0;
    }
}

/* ---- replies ----------------------------------------------------------------------------- */

static DBusMessage *atspi_reply_basic(DBusMessage *m, int type, const void *v) {
    DBusMessage *r = dbus_message_new_method_return(m);
    if (r) {
        DBusMessageIter it;
        dbus_message_iter_init_append(r, &it);
        dbus_message_iter_append_basic(&it, type, v);
    }
    return r;
}
static DBusMessage *atspi_reply_i(DBusMessage *m, i32 v) { return atspi_reply_basic(m, DBUS_TYPE_INT32, &v); }
static DBusMessage *atspi_reply_u(DBusMessage *m, u32 v) { return atspi_reply_basic(m, DBUS_TYPE_UINT32, &v); }
static DBusMessage *atspi_reply_b(DBusMessage *m, bool v) {
    dbus_bool_t b = v ? TRUE : FALSE;
    return atspi_reply_basic(m, DBUS_TYPE_BOOLEAN, &b);
}

static DBusMessage *atspi_reply_s(DBusMessage *m, const char *s, u32 len) {
    DBusMessage *r = dbus_message_new_method_return(m);
    if (r) {
        DBusMessageIter it;
        dbus_message_iter_init_append(r, &it);
        atspi_append_str(&it, s, len);
    }
    return r;
}

static DBusMessage *atspi_reply_ints(DBusMessage *m, const i32 *v, i32 n) {
    DBusMessage *r = dbus_message_new_method_return(m);
    if (r) {
        DBusMessageIter it;
        dbus_message_iter_init_append(r, &it);
        for (i32 i = 0; i < n; i++) dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &v[i]);
    }
    return r;
}

/* (s text, i start, i end) */
static DBusMessage *atspi_reply_span(DBusMessage *m, const AtspiText *t, u32 s, u32 e) {
    DBusMessage *r = dbus_message_new_method_return(m);
    if (!r) return NULL;
    u32 bytes = 0;
    const char *p = atspi_text_slice(t, s, e, &bytes);
    DBusMessageIter it;
    dbus_message_iter_init_append(r, &it);
    atspi_append_str(&it, p, bytes);
    i32 a = (i32)s, b = (i32)e;
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &a);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &b);
    return r;
}

static DBusMessage *atspi_reply_ref(DBusMessage *m, const AtspiObj *o) {
    DBusMessage *r = dbus_message_new_method_return(m);
    if (r) {
        DBusMessageIter it;
        dbus_message_iter_init_append(r, &it);
        atspi_append_ref(&it, o);
    }
    return r;
}

static DBusMessage *atspi_reply_empty_dict(DBusMessage *m, bool with_range, i32 a, i32 b) {
    DBusMessage *r = dbus_message_new_method_return(m);
    if (!r) return NULL;
    DBusMessageIter it, dict;
    dbus_message_iter_init_append(r, &it);
    if (dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{ss}", &dict))
        dbus_message_iter_close_container(&it, &dict);
    if (with_range) {
        dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &a);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &b);
    }
    return r;
}

static DBusMessage *atspi_reply_empty_array(DBusMessage *m, const char *sig) {
    DBusMessage *r = dbus_message_new_method_return(m);
    if (!r) return NULL;
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(r, &it);
    if (dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, sig, &arr))
        dbus_message_iter_close_container(&it, &arr);
    return r;
}

static DBusMessage *atspi_reply_error(DBusMessage *m, const char *name, const char *text) {
    return dbus_message_new_error(m, name, text);
}

/* ---- geometry -------------------------------------------------------------------------------- */

/* The window's rectangle in screen pixels (an X round trip, made only when an
 * assistive app asks for screen coordinates). */
static void atspi_window_rect(AtspiWindow *aw, bool screen, i32 r[4]) {
    i32 x = 0, y = 0, w = 0, h = 0;
    linux_a11y_window_rect(aw->pw, screen ? &x : NULL, screen ? &y : NULL, &w, &h);
    r[0] = x; r[1] = y; r[2] = w; r[3] = h;
}

/* Cells [c0, c1) x rows [r0, r1] of the terminal as a pixel rectangle, in the
 * coordinates `coord` names. A terminal's parent is its frame, whose origin
 * is the window's, so PARENT and WINDOW agree. */
static void atspi_cells_rect(AtspiWindow *aw, const AtspiText *t, i32 c0, i32 r0, i32 c1, i32 r1,
                             u32 coord, i32 out[4]) {
    memset(out, 0, 4 * sizeof(i32));
    if (!t->s.has_geometry) return;
    f64 sc = linux_a11y_window_scale(aw->pw);
    f64 x = ((f64)t->s.grid_x + (f64)c0 * t->s.cell_w) * sc;
    f64 y = ((f64)t->s.grid_y + (f64)r0 * t->s.cell_h) * sc;
    f64 w = (f64)(c1 - c0) * t->s.cell_w * sc;
    f64 h = (f64)(r1 - r0 + 1) * t->s.cell_h * sc;
    if (w < 0) w = 0;
    i32 ox = 0, oy = 0;
    if (coord == COORD_SCREEN) {
        i32 wr[4];
        atspi_window_rect(aw, true, wr);
        ox = wr[0]; oy = wr[1];
    }
    out[0] = ox + (i32)lround(x);
    out[1] = oy + (i32)lround(y);
    out[2] = (i32)lround(w);
    out[3] = (i32)lround(h);
}

/* An object's extents. */
static void atspi_extents(const AtspiObj *o, u32 coord, i32 out[4]) {
    memset(out, 0, 4 * sizeof(i32));
    if (o->kind == OBJ_FRAME) {
        /* A frame's parent is the desktop: PARENT is screen coordinates. */
        atspi_window_rect(o->w, coord != COORD_WINDOW, out);
        if (coord == COORD_WINDOW) { out[0] = 0; out[1] = 0; }
    } else if (o->kind == OBJ_TERM) {
        AtspiText *t = atspi_term_text(o->w, false);
        if (t->s.has_geometry && t->s.line_count > 0 && t->s.cols > 0)
            atspi_cells_rect(o->w, t, 0, 0, t->s.cols, t->s.line_count - 1, coord, out);
        else {
            atspi_window_rect(o->w, coord == COORD_SCREEN, out);
            if (coord != COORD_SCREEN) { out[0] = 0; out[1] = 0; }
        }
    }
}

/* A point in `coord` coordinates as window pixels. */
static void atspi_point_to_window(AtspiWindow *aw, u32 coord, i32 *x, i32 *y) {
    if (coord != COORD_SCREEN) return;
    i32 wr[4];
    atspi_window_rect(aw, true, wr);
    *x -= wr[0];
    *y -= wr[1];
}

/* ---- the objects ------------------------------------------------------------------------------- */

static void atspi_state_words(const AtspiObj *o, u32 w[2]) {
    w[0] = w[1] = 0;
#define SET(s) (w[(s) / 32] |= 1u << ((s) % 32))
    if (o->kind == OBJ_ROOT) return;
    bool shown = linux_a11y_window_mapped(o->w->pw);
    SET(ST_ENABLED); SET(ST_SENSITIVE); SET(ST_VISIBLE);
    if (shown) SET(ST_SHOWING);
    if (o->kind == OBJ_FRAME) {
        SET(ST_RESIZABLE);
        if (o->w->active) SET(ST_ACTIVE);
    } else {
        /* A terminal takes typing (through the program, not through AT-SPI's
         * EditableText), spans many lines, and its text can be selected. */
        SET(ST_FOCUSABLE); SET(ST_MULTI_LINE); SET(ST_SELECTABLE_TEXT); SET(ST_EDITABLE);
        if (o->w->active) SET(ST_FOCUSED);
    }
#undef SET
}

static const char *atspi_role_name(const AtspiObj *o) {
    return o->kind == OBJ_ROOT ? "application" : o->kind == OBJ_FRAME ? "frame" : "terminal";
}

static u32 atspi_role(const AtspiObj *o) {
    return o->kind == OBJ_ROOT ? ROLE_APPLICATION : o->kind == OBJ_FRAME ? ROLE_FRAME : ROLE_TERMINAL;
}

static i32 atspi_child_count(const AtspiObj *o) {
    if (o->kind == OBJ_ROOT) return atspi_window_count();
    if (o->kind == OBJ_FRAME) return o->w->source ? 1 : 0;
    return 0;
}

static bool atspi_child(const AtspiObj *o, i32 i, AtspiObj *out) {
    if (o->kind == OBJ_ROOT) {
        i32 k = 0;
        for (AtspiWindow *w = g_atspi.windows; w; w = w->next, k++)
            if (k == i) { out->kind = OBJ_FRAME; out->w = w; return true; }
        return false;
    }
    if (o->kind == OBJ_FRAME && i == 0 && o->w->source) {
        out->kind = OBJ_TERM;
        out->w = o->w;
        return true;
    }
    return false;
}

static void atspi_locale(char *out, usize cap) {
    const char *l = setlocale(LC_MESSAGES, NULL);
    if (!l || !*l) l = "C";
    snprintf(out, cap, "%s", l);
    char *dot = strpbrk(out, ".@");
    if (dot) *dot = '\0';
}

/* The name an object is read by. */
static void atspi_name(const AtspiObj *o, DBusMessageIter *it) {
    if (o->kind == OBJ_ROOT) atspi_append_cstr(it, "Liu");
    else if (o->kind == OBJ_FRAME) atspi_append_cstr(it, atspi_frame_name(o->w));
    else {
        AtspiText *t = atspi_term_text(o->w, false);
        atspi_append_cstr(it, t->label && t->label[0] ? t->label : "Terminal");
    }
}

static const char *const k_ifaces_root[]  = { IF_ACCESSIBLE, IF_APPLICATION, NULL };
static const char *const k_ifaces_frame[] = { IF_ACCESSIBLE, IF_COMPONENT, NULL };
static const char *const k_ifaces_term[]  = { IF_ACCESSIBLE, IF_COMPONENT, IF_TEXT, NULL };

static const char *const *atspi_ifaces(const AtspiObj *o) {
    return o->kind == OBJ_ROOT ? k_ifaces_root : o->kind == OBJ_FRAME ? k_ifaces_frame : k_ifaces_term;
}

static bool atspi_has_iface(const AtspiObj *o, const char *iface) {
    for (const char *const *p = atspi_ifaces(o); *p; p++)
        if (strcmp(*p, iface) == 0) return true;
    return false;
}

/* One property's value, appended as a variant. False: no such property. */
static bool atspi_property(const AtspiObj *o, const char *iface, const char *prop, DBusMessageIter *it) {
    DBusMessageIter v;
#define VAR(sig) if (!dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, sig, &v)) return false
#define END()    dbus_message_iter_close_container(it, &v); return true
    if (strcmp(iface, IF_ACCESSIBLE) == 0) {
        if (strcmp(prop, "Name") == 0) { VAR("s"); atspi_name(o, &v); END(); }
        if (strcmp(prop, "Description") == 0 || strcmp(prop, "HelpText") == 0) {
            VAR("s"); atspi_append_cstr(&v, ""); END();
        }
        if (strcmp(prop, "Parent") == 0) { VAR("(so)"); atspi_append_parent(&v, o); END(); }
        if (strcmp(prop, "ChildCount") == 0) {
            i32 n = atspi_child_count(o);
            VAR("i"); dbus_message_iter_append_basic(&v, DBUS_TYPE_INT32, &n); END();
        }
        if (strcmp(prop, "Locale") == 0) {
            char loc[64];
            atspi_locale(loc, sizeof(loc));
            VAR("s"); atspi_append_cstr(&v, loc); END();
        }
        if (strcmp(prop, "AccessibleId") == 0) {
            VAR("s");
            atspi_append_cstr(&v, o->kind == OBJ_ROOT ? "" : o->kind == OBJ_FRAME ? "window" : "terminal");
            END();
        }
        return false;
    }
    if (strcmp(iface, IF_APPLICATION) == 0 && o->kind == OBJ_ROOT) {
        if (strcmp(prop, "ToolkitName") == 0) { VAR("s"); atspi_append_cstr(&v, "Liu"); END(); }
        if (strcmp(prop, "Version") == 0) {
#ifdef LIU_VERSION
            VAR("s"); atspi_append_cstr(&v, LIU_VERSION); END();
#else
            VAR("s"); atspi_append_cstr(&v, ""); END();
#endif
        }
        if (strcmp(prop, "AtspiVersion") == 0) { VAR("s"); atspi_append_cstr(&v, "2.1"); END(); }
        if (strcmp(prop, "Id") == 0) {
            VAR("i"); dbus_message_iter_append_basic(&v, DBUS_TYPE_INT32, &g_atspi.app_id); END();
        }
        return false;
    }
    if (strcmp(iface, IF_TEXT) == 0 && o->kind == OBJ_TERM) {
        AtspiText *t = atspi_term_text(o->w, false);
        if (strcmp(prop, "CharacterCount") == 0) {
            i32 n = (i32)t->n;
            VAR("i"); dbus_message_iter_append_basic(&v, DBUS_TYPE_INT32, &n); END();
        }
        if (strcmp(prop, "CaretOffset") == 0) {
            i32 c = t->s.has_cursor ? (i32)t->s.cursor16 : -1;
            VAR("i"); dbus_message_iter_append_basic(&v, DBUS_TYPE_INT32, &c); END();
        }
        return false;
    }
#undef VAR
#undef END
    return false;
}

static const char *const k_props_accessible[] = {
    "Name", "Description", "Parent", "ChildCount", "Locale", "AccessibleId", "HelpText", NULL };
static const char *const k_props_application[] = { "ToolkitName", "Version", "AtspiVersion", "Id", NULL };
static const char *const k_props_text[] = { "CharacterCount", "CaretOffset", NULL };

static DBusMessage *atspi_properties_call(const AtspiObj *o, DBusMessage *m, const char *member) {
    DBusError err;
    dbus_error_init(&err);
    if (strcmp(member, "Get") == 0) {
        const char *iface = NULL, *prop = NULL;
        if (!dbus_message_get_args(m, &err, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
                                   DBUS_TYPE_INVALID)) {
            dbus_error_free(&err);
            return atspi_reply_error(m, DBUS_ERROR_INVALID_ARGS, "Get takes (ss)");
        }
        DBusMessage *r = dbus_message_new_method_return(m);
        if (!r) return NULL;
        DBusMessageIter it;
        dbus_message_iter_init_append(r, &it);
        if (!atspi_property(o, iface, prop, &it)) {
            dbus_message_unref(r);
            return atspi_reply_error(m, DBUS_ERROR_UNKNOWN_PROPERTY, prop);
        }
        return r;
    }
    if (strcmp(member, "GetAll") == 0) {
        const char *iface = NULL;
        if (!dbus_message_get_args(m, &err, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID)) {
            dbus_error_free(&err);
            return atspi_reply_error(m, DBUS_ERROR_INVALID_ARGS, "GetAll takes (s)");
        }
        const char *const *names = strcmp(iface, IF_ACCESSIBLE) == 0 ? k_props_accessible
                                 : strcmp(iface, IF_APPLICATION) == 0 ? k_props_application
                                 : strcmp(iface, IF_TEXT) == 0 ? k_props_text : NULL;
        DBusMessage *r = dbus_message_new_method_return(m);
        if (!r) return NULL;
        DBusMessageIter it, dict, ent;
        dbus_message_iter_init_append(r, &it);
        if (dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict)) {
            for (i32 i = 0; names && atspi_has_iface(o, iface) && names[i]; i++) {
                if (!dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent)) break;
                dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &names[i]);
                if (!atspi_property(o, iface, names[i], &ent)) {
                    /* Never leave an entry without its value. */
                    DBusMessageIter v;
                    const char *e = "";
                    if (dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "s", &v)) {
                        dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &e);
                        dbus_message_iter_close_container(&ent, &v);
                    }
                }
                dbus_message_iter_close_container(&dict, &ent);
            }
            dbus_message_iter_close_container(&it, &dict);
        }
        return r;
    }
    if (strcmp(member, "Set") == 0) {
        /* The registry numbers the applications it embeds. Nothing else is
         * writable. */
        DBusMessageIter it, v;
        const char *iface = NULL, *prop = NULL;
        if (dbus_message_iter_init(m, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&it, &iface);
            if (dbus_message_iter_next(&it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&it, &prop);
                if (o->kind == OBJ_ROOT && strcmp(iface, IF_APPLICATION) == 0 &&
                    strcmp(prop, "Id") == 0 && dbus_message_iter_next(&it) &&
                    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
                    dbus_message_iter_recurse(&it, &v);
                    if (dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_INT32) {
                        dbus_message_iter_get_basic(&v, &g_atspi.app_id);
                        return dbus_message_new_method_return(m);
                    }
                }
            }
        }
        return atspi_reply_error(m, DBUS_ERROR_PROPERTY_READ_ONLY, "read-only");
    }
    return NULL;
}

static DBusMessage *atspi_accessible_call(const AtspiObj *o, DBusMessage *m, const char *member) {
    DBusError err;
    dbus_error_init(&err);
    if (strcmp(member, "GetChildAtIndex") == 0) {
        i32 i = 0;
        if (!dbus_message_get_args(m, &err, DBUS_TYPE_INT32, &i, DBUS_TYPE_INVALID)) {
            dbus_error_free(&err);
            return atspi_reply_error(m, DBUS_ERROR_INVALID_ARGS, "GetChildAtIndex takes (i)");
        }
        AtspiObj c;
        return atspi_reply_ref(m, atspi_child(o, i, &c) ? &c : NULL);
    }
    if (strcmp(member, "GetChildren") == 0) {
        DBusMessage *r = dbus_message_new_method_return(m);
        if (!r) return NULL;
        DBusMessageIter it, arr;
        dbus_message_iter_init_append(r, &it);
        if (dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(so)", &arr)) {
            AtspiObj c;
            for (i32 i = 0; atspi_child(o, i, &c); i++) atspi_append_ref(&arr, &c);
            dbus_message_iter_close_container(&it, &arr);
        }
        return r;
    }
    if (strcmp(member, "GetIndexInParent") == 0)
        return atspi_reply_i(m, o->kind == OBJ_ROOT ? -1 : o->kind == OBJ_FRAME ? atspi_window_index(o->w) : 0);
    if (strcmp(member, "GetRelationSet") == 0) return atspi_reply_empty_array(m, "(ua(so))");
    if (strcmp(member, "GetRole") == 0) return atspi_reply_u(m, atspi_role(o));
    if (strcmp(member, "GetRoleName") == 0 || strcmp(member, "GetLocalizedRoleName") == 0) {
        const char *n = atspi_role_name(o);
        return atspi_reply_s(m, n, (u32)strlen(n));
    }
    if (strcmp(member, "GetState") == 0) {
        u32 w[2];
        atspi_state_words(o, w);
        DBusMessage *r = dbus_message_new_method_return(m);
        if (!r) return NULL;
        DBusMessageIter it, arr;
        dbus_message_iter_init_append(r, &it);
        if (dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "u", &arr)) {
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_UINT32, &w[0]);
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_UINT32, &w[1]);
            dbus_message_iter_close_container(&it, &arr);
        }
        return r;
    }
    if (strcmp(member, "GetAttributes") == 0) {
        DBusMessage *r = dbus_message_new_method_return(m);
        if (!r) return NULL;
        DBusMessageIter it, dict, ent;
        dbus_message_iter_init_append(r, &it);
        if (dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{ss}", &dict)) {
            if (dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent)) {
                const char *k = "toolkit", *v = "Liu";
                dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k);
                dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &v);
                dbus_message_iter_close_container(&dict, &ent);
            }
            dbus_message_iter_close_container(&it, &dict);
        }
        return r;
    }
    if (strcmp(member, "GetApplication") == 0) {
        AtspiObj r = { OBJ_ROOT, NULL };
        return atspi_reply_ref(m, &r);
    }
    if (strcmp(member, "GetInterfaces") == 0) {
        DBusMessage *r = dbus_message_new_method_return(m);
        if (!r) return NULL;
        DBusMessageIter it, arr;
        dbus_message_iter_init_append(r, &it);
        if (dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr)) {
            for (const char *const *p = atspi_ifaces(o); *p; p++)
                dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, p);
            dbus_message_iter_close_container(&it, &arr);
        }
        return r;
    }
    return NULL;
}

static DBusMessage *atspi_application_call(const AtspiObj *o, DBusMessage *m, const char *member) {
    if (o->kind != OBJ_ROOT) return NULL;
    if (strcmp(member, "GetLocale") == 0) {
        char loc[64];
        atspi_locale(loc, sizeof(loc));
        return atspi_reply_s(m, loc, (u32)strlen(loc));
    }
    /* No peer-to-peer server: everything goes over the bus. */
    if (strcmp(member, "GetApplicationBusAddress") == 0) return atspi_reply_s(m, "", 0);
    return NULL;
}

static DBusMessage *atspi_component_call(const AtspiObj *o, DBusMessage *m, const char *member) {
    DBusError err;
    dbus_error_init(&err);
    if (strcmp(member, "GetExtents") == 0) {
        u32 coord = 0;
        if (!dbus_message_get_args(m, &err, DBUS_TYPE_UINT32, &coord, DBUS_TYPE_INVALID)) {
            dbus_error_free(&err);
            return atspi_reply_error(m, DBUS_ERROR_INVALID_ARGS, "GetExtents takes (u)");
        }
        i32 e[4];
        atspi_extents(o, coord, e);
        DBusMessage *r = dbus_message_new_method_return(m);
        if (!r) return NULL;
        DBusMessageIter it, st;
        dbus_message_iter_init_append(r, &it);
        if (dbus_message_iter_open_container(&it, DBUS_TYPE_STRUCT, NULL, &st)) {
            for (i32 i = 0; i < 4; i++) dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &e[i]);
            dbus_message_iter_close_container(&it, &st);
        }
        return r;
    }
    if (strcmp(member, "GetPosition") == 0) {
        u32 coord = 0;
        if (!dbus_message_get_args(m, &err, DBUS_TYPE_UINT32, &coord, DBUS_TYPE_INVALID)) {
            dbus_error_free(&err);
            return atspi_reply_error(m, DBUS_ERROR_INVALID_ARGS, "GetPosition takes (u)");
        }
        i32 e[4];
        atspi_extents(o, coord, e);
        return atspi_reply_ints(m, e, 2);
    }
    if (strcmp(member, "GetSize") == 0) {
        i32 e[4];
        atspi_extents(o, COORD_WINDOW, e);
        return atspi_reply_ints(m, e + 2, 2);
    }
    if (strcmp(member, "Contains") == 0 || strcmp(member, "GetAccessibleAtPoint") == 0) {
        i32 x = 0, y = 0;
        u32 coord = 0;
        if (!dbus_message_get_args(m, &err, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y,
                                   DBUS_TYPE_UINT32, &coord, DBUS_TYPE_INVALID)) {
            dbus_error_free(&err);
            return atspi_reply_error(m, DBUS_ERROR_INVALID_ARGS, "takes (iiu)");
        }
        i32 e[4];
        atspi_extents(o, coord, e);
        bool in = x >= e[0] && y >= e[1] && x < e[0] + e[2] && y < e[1] + e[3];
        if (member[0] == 'C') return atspi_reply_b(m, in);
        AtspiObj c;
        if (o->kind == OBJ_FRAME && in && atspi_child(o, 0, &c)) {
            i32 ce[4];
            atspi_extents(&c, coord, ce);
            if (x >= ce[0] && y >= ce[1] && x < ce[0] + ce[2] && y < ce[1] + ce[3])
                return atspi_reply_ref(m, &c);
        }
        return atspi_reply_ref(m, NULL);
    }
    if (strcmp(member, "GetLayer") == 0)
        return atspi_reply_u(m, o->kind == OBJ_FRAME ? LAYER_WINDOW : LAYER_WIDGET);
    if (strcmp(member, "GetMDIZOrder") == 0) {
        dbus_int16_t z = 0;
        return atspi_reply_basic(m, DBUS_TYPE_INT16, &z);
    }
    if (strcmp(member, "GetAlpha") == 0) {
        f64 a = 1.0;
        return atspi_reply_basic(m, DBUS_TYPE_DOUBLE, &a);
    }
    /* Liu moves no window and no focus on an assistive app's say. */
    if (strcmp(member, "GrabFocus") == 0 || strcmp(member, "SetExtents") == 0 ||
        strcmp(member, "SetPosition") == 0 || strcmp(member, "SetSize") == 0 ||
        strcmp(member, "ScrollTo") == 0 || strcmp(member, "ScrollToPoint") == 0)
        return atspi_reply_b(m, false);
    return NULL;
}

static DBusMessage *atspi_text_call(const AtspiObj *o, DBusMessage *m, const char *member) {
    if (o->kind != OBJ_TERM) return NULL;
    AtspiWindow *aw = o->w;
    DBusError err;
    dbus_error_init(&err);
    i32 a = 0, b = 0;
    u32 u = 0;
#define ARGS(...) do { if (!dbus_message_get_args(m, &err, __VA_ARGS__, DBUS_TYPE_INVALID)) { \
        dbus_error_free(&err); return atspi_reply_error(m, DBUS_ERROR_INVALID_ARGS, member); } } while (0)

    if (strcmp(member, "GetText") == 0) {
        ARGS(DBUS_TYPE_INT32, &a, DBUS_TYPE_INT32, &b);
        AtspiText *t = atspi_term_text(aw, false);
        u32 s = atspi_clamp_offset(t, a);
        u32 e = (b < 0 || (u32)b > t->n) ? t->n : (u32)b;
        if (e < s) e = s;
        u32 bytes = 0;
        const char *p = atspi_text_slice(t, s, e, &bytes);
        return atspi_reply_s(m, p, bytes);
    }
    if (strcmp(member, "GetCharacterAtOffset") == 0) {
        ARGS(DBUS_TYPE_INT32, &a);
        AtspiText *t = atspi_term_text(aw, false);
        return atspi_reply_i(m, (a >= 0 && (u32)a < t->n) ? (i32)t->cp[a] : 0);
    }
    if (strcmp(member, "GetStringAtOffset") == 0) {
        ARGS(DBUS_TYPE_INT32, &a, DBUS_TYPE_UINT32, &u);
        AtspiText *t = atspi_term_text(aw, false);
        u32 s = 0, e = 0;
        atspi_text_granule(t, a, u, &s, &e);
        return atspi_reply_span(m, t, s, e);
    }
    if (strcmp(member, "GetTextAtOffset") == 0 || strcmp(member, "GetTextBeforeOffset") == 0 ||
        strcmp(member, "GetTextAfterOffset") == 0) {
        ARGS(DBUS_TYPE_INT32, &a, DBUS_TYPE_UINT32, &u);
        AtspiText *t = atspi_term_text(aw, false);
        i32 which = member[7] == 'A' && member[8] == 't' ? 0 : member[7] == 'B' ? -1 : 1;
        u32 s = 0, e = 0;
        atspi_text_bounded(t, a, u, which, &s, &e);
        return atspi_reply_span(m, t, s, e);
    }
    if (strcmp(member, "GetNSelections") == 0) {
        AtspiText *t = atspi_term_text(aw, false);
        return atspi_reply_i(m, (t->s.has_selection && t->s.sel_len16 > 0) ? 1 : 0);
    }
    if (strcmp(member, "GetSelection") == 0) {
        ARGS(DBUS_TYPE_INT32, &a);
        AtspiText *t = atspi_term_text(aw, false);
        i32 v[2] = { 0, 0 };
        if (a == 0 && t->s.has_selection && t->s.sel_len16 > 0) {
            v[0] = (i32)t->s.sel_start16;
            v[1] = (i32)(t->s.sel_start16 + t->s.sel_len16);
        }
        return atspi_reply_ints(m, v, 2);
    }
    if (strcmp(member, "GetCharacterExtents") == 0) {
        ARGS(DBUS_TYPE_INT32, &a, DBUS_TYPE_UINT32, &u);
        AtspiText *t = atspi_term_text(aw, false);
        i32 e[4] = { 0, 0, 0, 0 };
        if (a >= 0 && (u32)a <= t->n && t->s.line_count > 0) {
            u32 s = (u32)a;
            while (s > 0 && !atspi_char_starts(t, s)) s--;
            u32 end = atspi_char_end(t, s);
            i32 c0, r0, c1, r1;
            a11y_cells_for_range(&t->s, s, end - s, &c0, &r0, &c1, &r1);
            atspi_cells_rect(aw, t, c0, r0, c1, r1, u, e);
        }
        return atspi_reply_ints(m, e, 4);
    }
    if (strcmp(member, "GetRangeExtents") == 0) {
        ARGS(DBUS_TYPE_INT32, &a, DBUS_TYPE_INT32, &b, DBUS_TYPE_UINT32, &u);
        AtspiText *t = atspi_term_text(aw, false);
        i32 e[4] = { 0, 0, 0, 0 };
        u32 s = atspi_clamp_offset(t, a), end = atspi_clamp_offset(t, b);
        if (end < s) { u32 x = s; s = end; end = x; }
        if (t->s.line_count > 0) {
            i32 c0, r0, c1, r1;
            a11y_cells_for_range(&t->s, s, end - s, &c0, &r0, &c1, &r1);
            atspi_cells_rect(aw, t, c0, r0, c1, r1, u, e);
        }
        return atspi_reply_ints(m, e, 4);
    }
    if (strcmp(member, "GetOffsetAtPoint") == 0) {
        ARGS(DBUS_TYPE_INT32, &a, DBUS_TYPE_INT32, &b, DBUS_TYPE_UINT32, &u);
        AtspiText *t = atspi_term_text(aw, false);
        if (!t->s.has_geometry || t->s.line_count <= 0 || t->s.cell_w <= 0 || t->s.cell_h <= 0)
            return atspi_reply_i(m, -1);
        atspi_point_to_window(aw, u, &a, &b);
        f64 sc = linux_a11y_window_scale(aw->pw);
        i32 col = (i32)floor(((f64)a / sc - t->s.grid_x) / t->s.cell_w);
        i32 row = (i32)floor(((f64)b / sc - t->s.grid_y) / t->s.cell_h);
        if (col < 0 || row < 0 || col >= t->s.cols || row >= t->s.line_count)
            return atspi_reply_i(m, -1);
        u32 idx = a11y_index_for_cell(&t->s, col, row);
        while (idx > 0 && idx < t->n && !atspi_char_starts(t, idx)) idx--;
        return atspi_reply_i(m, (i32)idx);
    }
    if (strcmp(member, "GetAttributes") == 0) {
        ARGS(DBUS_TYPE_INT32, &a);
        AtspiText *t = atspi_term_text(aw, false);
        return atspi_reply_empty_dict(m, true, 0, (i32)t->n);
    }
    if (strcmp(member, "GetAttributeRun") == 0) {
        dbus_bool_t inc = FALSE;
        ARGS(DBUS_TYPE_INT32, &a, DBUS_TYPE_BOOLEAN, &inc);
        AtspiText *t = atspi_term_text(aw, false);
        return atspi_reply_empty_dict(m, true, 0, (i32)t->n);
    }
    if (strcmp(member, "GetDefaultAttributes") == 0 || strcmp(member, "GetDefaultAttributeSet") == 0)
        return atspi_reply_empty_dict(m, false, 0, 0);
    if (strcmp(member, "GetAttributeValue") == 0) return atspi_reply_s(m, "", 0);
    if (strcmp(member, "GetBoundedRanges") == 0) return atspi_reply_empty_array(m, "(iisv)");
    /* The text is what the program drew: no assistive app moves the caret or
     * the selection behind the terminal's back. */
    if (strcmp(member, "SetCaretOffset") == 0 || strcmp(member, "AddSelection") == 0 ||
        strcmp(member, "RemoveSelection") == 0 || strcmp(member, "SetSelection") == 0 ||
        strcmp(member, "ScrollSubstringTo") == 0 || strcmp(member, "ScrollSubstringToPoint") == 0)
        return atspi_reply_b(m, false);
#undef ARGS
    return NULL;
}

static DBusMessage *atspi_introspect(const AtspiObj *o, DBusMessage *m) {
    char xml[1024];
    usize n = (usize)snprintf(xml, sizeof(xml),
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
        "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n"
        "  <interface name=\"" IF_PROPERTIES "\"/>\n  <interface name=\"" IF_INTROSPECT "\"/>\n");
    for (const char *const *p = atspi_ifaces(o); *p && n < sizeof(xml); p++)
        n += (usize)snprintf(xml + n, sizeof(xml) - n, "  <interface name=\"%s\"/>\n", *p);
    if (n < sizeof(xml)) snprintf(xml + n, sizeof(xml) - n, "</node>\n");
    return atspi_reply_s(m, xml, (u32)strlen(xml));
}

/* Every message the connection receives. */
static DBusHandlerResult atspi_filter(DBusConnection *c, DBusMessage *m, void *user) {
    (void)user;
    int type = dbus_message_get_type(m);
    if (type == DBUS_MESSAGE_TYPE_SIGNAL) {
        bool reg = dbus_message_is_signal(m, IF_REGISTRY, "EventListenerRegistered");
        bool dereg = !reg && dbus_message_is_signal(m, IF_REGISTRY, "EventListenerDeregistered");
        if (reg || dereg) {
            DBusMessageIter it;
            const char *bus = NULL, *ev = NULL;
            if (dbus_message_iter_init(m, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&it, &bus);
                if (dbus_message_iter_next(&it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING)
                    dbus_message_iter_get_basic(&it, &ev);
            }
            if (bus && ev) {
                bool had_text = g_atspi.want_text;
                if (reg) atspi_listener_add(bus, ev);
                else atspi_listener_remove(bus, ev);
                g_atspi.listeners_known = true;
                atspi_wants_update();
                /* Someone started listening for text: take the base the first
                 * change is measured against now (this turn's service), not
                 * at the first change itself — that one would be lost. */
                if (!had_text && g_atspi.want_text) {
                    f64 now = platform_time_sec();
                    for (AtspiWindow *w = g_atspi.windows; w; w = w->next) {
                        w->have_told = false;
                        w->post_due = now;
                    }
                }
                ATSPI_LOG("listener %s %s %s (%d)\n", reg ? "+" : "-", bus, ev, g_atspi.nlisteners);
            }
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (type != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *path = dbus_message_get_path(m);
    const char *iface = dbus_message_get_interface(m);
    const char *member = dbus_message_get_member(m);
    if (!path || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage *r = NULL;
    AtspiObj o;
    if (strcmp(path, ATSPI_PATH_CACHE) == 0 && strcmp(member, "GetItems") == 0) {
        /* Nothing is pushed into an assistive app's cache: it asks. */
        r = atspi_reply_empty_array(m, "((so)(so)(so)iiassusau)");
    } else if (strcmp(member, "GetApplicationBusAddress") == 0) {
        r = atspi_reply_s(m, "", 0);
    } else if (!atspi_resolve(path, &o)) {
        r = atspi_reply_error(m, DBUS_ERROR_UNKNOWN_OBJECT, path);
    } else if (iface && strcmp(iface, IF_PROPERTIES) == 0) {
        r = atspi_properties_call(&o, m, member);
    } else if (iface && strcmp(iface, IF_INTROSPECT) == 0 && strcmp(member, "Introspect") == 0) {
        r = atspi_introspect(&o, m);
    } else {
        if (!r && (!iface || strcmp(iface, IF_ACCESSIBLE) == 0)) r = atspi_accessible_call(&o, m, member);
        if (!r && (!iface || strcmp(iface, IF_TEXT) == 0)) r = atspi_text_call(&o, m, member);
        if (!r && (!iface || strcmp(iface, IF_COMPONENT) == 0) && o.kind != OBJ_ROOT)
            r = atspi_component_call(&o, m, member);
        if (!r && (!iface || strcmp(iface, IF_APPLICATION) == 0)) r = atspi_application_call(&o, m, member);
    }
    if (!r) r = atspi_reply_error(m, DBUS_ERROR_UNKNOWN_METHOD, member);
    ATSPI_LOG("call %s.%s %s -> %s\n", iface ? iface : "?", member, path,
              r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR ? dbus_message_get_error_name(r)
                                                                      : "ok");
    if (r) {
        if (!dbus_message_get_no_reply(m)) dbus_connection_send(c, r, NULL);
        dbus_message_unref(r);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---- the loop's side ------------------------------------------------------------------------------ */

int atspi_poll_fd(short *events) {
    if (!g_atspi.conn || g_atspi.fd < 0) return -1;
    if (events) {
        *events = POLLIN;
        if (dbus_connection_has_messages_to_send(g_atspi.conn)) *events |= POLLOUT;
    }
    return g_atspi.fd;
}

f64 atspi_wait_limit(f64 timeout) {
    if (g_atspi.boot && atomic_load(&g_atspi.boot->state) == BOOT_DONE) return 0.0;
    if (!g_atspi.conn) return timeout;
    if (dbus_connection_get_dispatch_status(g_atspi.conn) == DBUS_DISPATCH_DATA_REMAINS) return 0.0;
    f64 now = platform_time_sec();
    for (AtspiWindow *w = g_atspi.windows; w; w = w->next) {
        if (w->post_due <= 0.0) continue;
        f64 left = w->post_due - now;
        if (left < 0.0) left = 0.0;
        if (timeout < 0.0 || left < timeout) timeout = left;
    }
    return timeout;
}

static void atspi_io(void) {
    DBusConnection *c = g_atspi.conn;
    for (i32 i = 0; i < ATSPI_DISPATCH_MAX; i++) {
        if (dbus_connection_get_dispatch_status(c) == DBUS_DISPATCH_DATA_REMAINS) {
            dbus_connection_dispatch(c);
            if (!g_atspi.conn) return;
            continue;
        }
        struct pollfd p = { .fd = g_atspi.fd, .events = POLLIN };
        if (g_atspi.fd < 0 || poll(&p, 1, 0) <= 0) break;
        if (!dbus_connection_read_write(c, 0)) break;
    }
    /* Replies and events go out as far as the socket takes them now; the
     * rest when the wait sees it writable (atspi_poll_fd asks for POLLOUT). */
    for (i32 i = 0; i < 64 && dbus_connection_has_messages_to_send(c); i++) {
        struct pollfd p = { .fd = g_atspi.fd, .events = POLLOUT };
        if (g_atspi.fd < 0 || poll(&p, 1, 0) <= 0 || !(p.revents & POLLOUT)) break;
        if (!dbus_connection_read_write(c, 0)) break;
    }
    if (!dbus_connection_get_is_connected(c)) {
        ATSPI_LOG("accessibility bus gone\n");
        atspi_drop_connection();
    }
}

void atspi_service(void) {
    if (g_atspi.boot && atomic_load(&g_atspi.boot->state) == BOOT_DONE) {
        AtspiBoot *b = g_atspi.boot;
        g_atspi.boot = NULL;
        atspi_adopt(b);
    }
    if (!g_atspi.conn) return;
    atspi_io();
    if (!g_atspi.conn) return;
    f64 now = platform_time_sec();
    for (AtspiWindow *w = g_atspi.windows; w; w = w->next) {
        if (w->post_due > 0.0 && now >= w->post_due) {
            w->post_due = 0.0;
            atspi_window_check(w, now);
        }
        if (w->name_dirty) {
            w->name_dirty = false;
            /* The terminal's label carries the tab title the window title
             * follows: a changed one is announced as the object's new name. */
            if (g_atspi.want_any && w->source && w->have_cur) {
                char before[256];
                AtspiText *t = &w->snap[w->cur];
                snprintf(before, sizeof(before), "%s", t->label ? t->label : "");
                t = atspi_term_text(w, true);
                if (strcmp(before, t->label ? t->label : "") != 0) {
                    AtspiObj term = { OBJ_TERM, w };
                    atspi_emit_name(&term, t->label ? t->label : "");
                }
            }
        }
    }
    /* What the checks above queued. */
    if (dbus_connection_has_messages_to_send(g_atspi.conn)) atspi_io();
}

void atspi_shutdown(void) {
    if (g_atspi.boot) {
        /* A worker still waiting on the bus frees its own result. */
        AtspiBoot *b = g_atspi.boot;
        g_atspi.boot = NULL;
        if (atomic_exchange(&b->state, BOOT_ABANDONED) == BOOT_DONE) {
            atspi_close_bus(b->conn);
            free(b->listeners);
            free(b);
        }
    }
    if (g_atspi.conn) {
        DBusMessage *m = dbus_message_new_method_call(ATSPI_BUS_REGISTRY, ATSPI_PATH_ROOT,
                                                      IF_SOCKET, "Unembed");
        if (m) {
            DBusMessageIter it;
            AtspiObj root = { OBJ_ROOT, NULL };
            dbus_message_iter_init_append(m, &it);
            atspi_append_ref(&it, &root);
            dbus_message_set_no_reply(m, TRUE);
            dbus_connection_send(g_atspi.conn, m, NULL);
            dbus_message_unref(m);
            if (g_atspi.fd >= 0) {
                struct pollfd p = { .fd = g_atspi.fd, .events = POLLOUT };
                if (poll(&p, 1, 0) > 0 && (p.revents & POLLOUT))
                    dbus_connection_read_write(g_atspi.conn, 0);
            }
        }
        atspi_drop_connection();
    }
    free(g_atspi.listeners);
    g_atspi.listeners = NULL;
    g_atspi.cap_listeners = 0;
}

/* ---- windows ------------------------------------------------------------------------------------------ */

void atspi_window_attach(AtspiWindow *aw, PlatformWindow *pw) {
    if (!aw || aw->id) return;
    memset(aw, 0, sizeof(*aw));
    aw->pw = pw;
    aw->id = ++g_atspi.next_window_id;
    AtspiWindow **tail = &g_atspi.windows;
    while (*tail) tail = &(*tail)->next;
    *tail = aw;
    if (g_atspi.conn) {
        AtspiObj root = { OBJ_ROOT, NULL }, frame = { OBJ_FRAME, aw };
        atspi_emit_children(&root, true, atspi_window_index(aw), &frame);
    }
}

void atspi_window_detach(AtspiWindow *aw) {
    if (!aw || !aw->id) return;
    if (g_atspi.conn) {
        AtspiObj root = { OBJ_ROOT, NULL }, frame = { OBJ_FRAME, aw };
        if (aw->source) {
            AtspiObj term = { OBJ_TERM, aw };
            atspi_emit_state(&term, "defunct", true);
        }
        atspi_emit_state(&frame, "defunct", true);
        atspi_emit_children(&root, false, atspi_window_index(aw), &frame);
    }
    for (AtspiWindow **p = &g_atspi.windows; *p; p = &(*p)->next)
        if (*p == aw) { *p = aw->next; break; }
    atspi_text_free(&aw->snap[0]);
    atspi_text_free(&aw->snap[1]);
    memset(aw, 0, sizeof(*aw));
}

void atspi_window_set_provider(AtspiWindow *aw, PlatformA11yProviderFn fn, void *user) {
    if (!aw || !aw->id) return;
    aw->fn = fn;
    aw->user = user;
    aw->have_cur = aw->have_told = false;
    if (g_atspi.conn) {
        PlatformA11yText g;
        atspi_gens(aw, &g);
        if (g.source != aw->source) atspi_source_moved(aw, g.source);
    }
}

void atspi_window_set_title(AtspiWindow *aw, const char *title) {
    if (!aw || !aw->id || !title) return;
    char clean[sizeof(aw->title)];
    atspi_utf8_clean(title, (u32)strlen(title), clean, sizeof(clean));
    if (strcmp(clean, aw->title) == 0) return;
    memcpy(aw->title, clean, sizeof(clean));
    if (!g_atspi.conn) return;
    AtspiObj frame = { OBJ_FRAME, aw };
    atspi_emit_name(&frame, atspi_frame_name(aw));
    aw->name_dirty = true;
}

void atspi_window_set_active(AtspiWindow *aw, bool active) {
    if (!aw || !aw->id || aw->active == active) return;
    aw->active = active;
    if (!g_atspi.conn || !g_atspi.want_any) return;
    AtspiObj frame = { OBJ_FRAME, aw };
    const char *ev = active ? "activate" : "deactivate";
    if (atspi_listened("window", ev, ""))
        atspi_emit(&frame, IF_EV_WINDOW, active ? "Activate" : "Deactivate", "", 0, 0,
                   ANY_STR, "", 0, NULL);
    atspi_emit_state(&frame, "active", active);
    atspi_emit_term_focus(aw, active);
    ATSPI_LOG("window %u %s\n", aw->id, ev);
}

void atspi_window_set_showing(AtspiWindow *aw, bool showing) {
    if (!aw || !aw->id || !g_atspi.conn || !g_atspi.want_any) return;
    AtspiObj frame = { OBJ_FRAME, aw };
    atspi_emit_state(&frame, "showing", showing);
    if (aw->source) {
        AtspiObj term = { OBJ_TERM, aw };
        atspi_emit_state(&term, "showing", showing);
    }
}

void atspi_window_frame_presented(AtspiWindow *aw) {
    if (!aw || !aw->fn || !aw->id) return;
    if (!g_atspi.started) { atspi_start(); return; }
    if (!g_atspi.conn || !g_atspi.want_any) return;
    if (!g_atspi.want_text) {
        /* Nobody reads text changes: only which pane is described matters. */
        PlatformA11yText g;
        atspi_gens(aw, &g);
        if (g.source != aw->source) atspi_source_moved(aw, g.source);
        return;
    }
    f64 now = platform_time_sec();
    /* A keystroke's echo goes out with the first frame that can show it,
     * past the interval: once per keystroke (atspi_typing.h). */
    if (now >= aw->last_post + ATSPI_POST_INTERVAL ||
        atspi_typing_due(&aw->typing, aw->last_post, now)) {
        aw->post_due = 0.0;
        atspi_window_check(aw, now);
        return;
    }
    if (aw->post_due > 0.0) return;
    /* Inside the cap: one trailing check when it ends, but only when there is
     * something for it to find. */
    PlatformA11yText g;
    atspi_gens(aw, &g);
    if (g.source == aw->source && aw->have_told &&
        g.gen_value == aw->seen_value && g.gen_selection == aw->seen_sel)
        return;
    aw->post_due = aw->last_post + ATSPI_POST_INTERVAL;
}

void atspi_window_key(AtspiWindow *aw, u32 chars) {
    if (!aw || !aw->id || !g_atspi.conn || !g_atspi.want_text) return;
    atspi_typing_key(&aw->typing, platform_time_sec(), chars);
}

#else
/* Without D-Bus the bridge is the header's empty inlines; this keeps the unit
 * from being empty, which ISO C (and -Wpedantic) refuses. */
typedef int liu_atspi_bridge_absent;
#endif /* PLATFORM_LINUX && LIU_HAVE_DBUS */
