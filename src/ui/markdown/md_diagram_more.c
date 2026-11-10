/*
 * Liu — the diagram engine's other Mermaid kinds: gantt, mindmap, timeline,
 * gitGraph, xychart, quadrantChart and journey.
 *
 * Each parses its body into a model of its own, kept here, and lays itself
 * out into the engine's draw list with the engine's emitters (see
 * md_diagram_int.h): boxes, ribbons and glyphs in diagram space at scale 1.
 * Colours are roles — a category's soft fill and vivid ink, the node and
 * group inks — resolved at draw time from the theme.
 *
 * Bounded like the rest of the engine: every list has a cap, and a cap that
 * would drop something the reader wrote raises g_trunc, so the source is
 * shown instead of a diagram that is not theirs.
 */
#include "ui/markdown/md_diagram_int.h"
#include "core/utf8.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DGM_MAX_ITEMS    160     /* tasks, events, nodes, commits, points */
#define DGM_MAX_GROUPS   32      /* sections, periods, branches */
#define DGM_MAX_SERIES   8
#define DGM_MAX_VALUES   64
#define DGM_MAX_ACTORS   12

/* ---- small helpers ---- */

static u8 soft(u32 i)  { return (u8)(DI_SOFT0 + (i % 8u)); }
static u8 vivid(u32 i) { return (u8)(DI_SLICE0 + (i % 8u)); }
static u8 tint(u32 i)  { return (u8)(DI_TINT0 + (i % 8u)); }

static void trim(const u8 **s, u32 *n) {
    while (*n && (dg_is_space((*s)[0]) || (*s)[0] == '\r')) { (*s)++; (*n)--; }
    while (*n && (dg_is_space((*s)[*n - 1]) || (*s)[*n - 1] == '\r')) (*n)--;
}

static void unquote(const u8 **s, u32 *n) {
    trim(s, n);
    if (*n >= 2 && (*s)[0] == '"' && (*s)[*n - 1] == '"') { (*s)++; *n -= 2; }
}

/* `kw` and what follows it on the line, trimmed and unquoted. */
static bool after_kw(const u8 *line, u32 ll, const char *kw, const u8 **rest, u32 *rn) {
    if (!dg_kw(line, ll, kw)) return false;
    u32 k = (u32)strlen(kw);
    *rest = line + k;
    *rn = ll - k;
    unquote(rest, rn);
    return true;
}

static u32 find_byte(const u8 *s, u32 n, u8 c) {
    for (u32 i = 0; i < n; i++) if (s[i] == c) return i;
    return n;
}

static f32 text_w(const char *s) { u32 r, c; dg_text_metrics(s, &r, &c); return (f32)c * g_cw; }
static f32 text_h(const char *s) { u32 r, c; dg_text_metrics(s, &r, &c); return (f32)r * g_ch * DG_ROW_H; }

static void set_label(char *dst, u32 cap, const u8 *s, u32 n, u32 wrap) {
    unquote(&s, &n);
    dg_set_label(dst, cap, s, n);
    if (wrap) dg_wrap_label(dst, wrap);
}

/* The title, centred over [x0, x1] at the top; the height it takes. */
static f32 title_h(void) { return g_title[0] ? text_h(g_title) + g_ch * 0.9f : 0.0f; }
static void emit_title(f32 x0, f32 x1) {
    if (!g_title[0]) return;
    f32 w = text_w(g_title);
    em_text_box(g_title, x0 + (x1 - x0 - w) * 0.5f, 0.0f, w, text_h(g_title), DI_TEXT);
}

static void title_line(const u8 *line, u32 ll) {
    const u8 *r; u32 rn;
    if (after_kw(line, ll, "title", &r, &rn)) set_label(g_title, sizeof g_title, r, rn, 60);
}

/* A filled circle. */
static void dot(f32 cx, f32 cy, f32 r, u8 fill, u8 stroke, f32 border) {
    em_box(P_NODE, cx - r, cy - r, r * 2.0f, r * 2.0f, r, border, fill, stroke);
}

/* A filled rhombus centred on (cx, cy), drawn as two wide-angled wedges. */
static void rhombus(f32 cx, f32 cy, f32 w, f32 h, u8 fill) {
    f32 o = 0.5f;
    em_wedge(P_NODE, cx, cy - h * 0.5f, cx, cy + o, w * 0.5f, fill);
    em_wedge(P_NODE, cx, cy + h * 0.5f, cx, cy - o, w * 0.5f, fill);
}

/* A number as an axis prints it: no trailing zeros, no exponent for the
 * sizes charts hold. */
static void fmt_num(f64 v, char *out, u32 cap) {
    if (fabs(v) < 1e-9) v = 0;
    if (fabs(v) >= 1e6 || (fabs(v) < 1e-3 && v != 0)) { snprintf(out, cap, "%g", v); return; }
    snprintf(out, cap, "%.3f", v);
    u32 n = (u32)strlen(out);
    while (n && out[n - 1] == '0') out[--n] = 0;
    if (n && out[n - 1] == '.') out[--n] = 0;
}

/* A step of 1, 2 or 5 times a power of ten near `x`. */
static f64 nice_step(f64 x) {
    if (!(x > 0) || !isfinite(x)) return 1.0;
    f64 e = floor(log10(x)), f = x / pow(10.0, e);
    f64 nf = f < 1.5 ? 1.0 : f < 3.0 ? 2.0 : f < 7.0 ? 5.0 : 10.0;
    return nf * pow(10.0, e);
}

/* `[a, "b c", 3]` into up to `cap` items, each through `put`. */
static u32 list_items(const u8 *s, u32 n, void (*put)(void *ctx, u32 i, const u8 *s, u32 n), void *ctx,
                      u32 cap) {
    u32 a = find_byte(s, n, '[');
    if (a >= n) return 0;
    u32 b = a + 1, count = 0;
    while (b < n && count < cap) {
        while (b < n && (dg_is_space(s[b]) || s[b] == ',')) b++;
        if (b >= n || s[b] == ']') break;
        u32 e = b;
        if (s[b] == '"') {
            e = b + 1;
            while (e < n && s[e] != '"') e++;
            if (e < n) e++;
        } else {
            while (e < n && s[e] != ',' && s[e] != ']') e++;
        }
        const u8 *it = s + b;
        u32 il = e - b;
        trim(&it, &il);
        put(ctx, count++, it, il);
        b = e;
    }
    return count;
}

/* ---- dates ---- */

/* Days a date or a duration may reach, about 8,000 years: any day the chart
 * adds up stays far inside i64 when it is floored for a label. */
#define GANTT_DAYS_MAX 3.0e6

static i64 days_from_civil(i64 y, i64 m, i64 d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(i64 z, i64 *y, i64 *m, i64 *d) {
    z += 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

/* Read up to `max` digits (at least one). */
static bool digits(const u8 *s, u32 n, u32 *i, u32 max, i64 *out) {
    u32 k = 0;
    i64 v = 0;
    while (*i < n && k < max && s[*i] >= '0' && s[*i] <= '9') { v = v * 10 + (s[*i] - '0'); (*i)++; k++; }
    *out = v;
    return k > 0;
}

/* A date in `fmt`'s spelling (YYYY YY MM M DD D HH H mm ss, X unix seconds,
 * x milliseconds; anything else is a literal) as days since 1970-01-01. */
static bool parse_date(const u8 *s, u32 n, const char *fmt, f64 *out) {
    trim(&s, &n);
    if (!n) return false;
    if (!strcmp(fmt, "X") || !strcmp(fmt, "x")) {
        char buf[32];
        if (n >= sizeof buf) return false;
        memcpy(buf, s, n); buf[n] = 0;
        char *end;
        f64 v = strtod(buf, &end);
        if (end == buf || *end || !isfinite(v)) return false;
        v /= fmt[0] == 'X' ? 86400.0 : 86400000.0;
        if (fabs(v) > GANTT_DAYS_MAX) return false;
        *out = v;
        return true;
    }
    i64 y = 1970, mo = 1, d = 1, h = 0, mi = 0, se = 0;
    u32 i = 0;
    for (const char *f = fmt; *f; ) {
        i64 v;
        if (!strncmp(f, "YYYY", 4)) { if (!digits(s, n, &i, 4, &v)) return false; y = v; f += 4; }
        else if (!strncmp(f, "YY", 2)) { if (!digits(s, n, &i, 2, &v)) return false; y = 2000 + v; f += 2; }
        else if (!strncmp(f, "MM", 2) || f[0] == 'M') { if (!digits(s, n, &i, 2, &v)) return false; mo = v; f += f[1] == 'M' ? 2 : 1; }
        else if (!strncmp(f, "DD", 2) || f[0] == 'D') { if (!digits(s, n, &i, 2, &v)) return false; d = v; f += f[1] == 'D' ? 2 : 1; }
        else if (!strncmp(f, "HH", 2) || f[0] == 'H') { if (!digits(s, n, &i, 2, &v)) return false; h = v; f += f[1] == 'H' ? 2 : 1; }
        else if (!strncmp(f, "mm", 2)) { if (!digits(s, n, &i, 2, &v)) return false; mi = v; f += 2; }
        else if (!strncmp(f, "ss", 2)) { if (!digits(s, n, &i, 2, &v)) return false; se = v; f += 2; }
        else { if (i < n && s[i] == (u8)*f) i++; else return false; f++; }
    }
    if (i != n || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    *out = (f64)days_from_civil(y, mo, d) + ((f64)h + (f64)mi / 60.0 + (f64)se / 3600.0) / 24.0;
    return true;
}

/* `3d`, `1.5w`, `12h` as days. */
static bool parse_duration(const u8 *s, u32 n, f64 *out) {
    trim(&s, &n);
    char buf[32];
    if (!n || n >= sizeof buf) return false;
    memcpy(buf, s, n); buf[n] = 0;
    char *end;
    f64 v = strtod(buf, &end);
    if (end == buf || !isfinite(v) || v < 0) return false;
    const char *u = end;
    f64 k = !strcmp(u, "ms") ? 1.0 / 86400000.0 : !strcmp(u, "s") ? 1.0 / 86400.0 :
            !strcmp(u, "m") ? 1.0 / 1440.0 : !strcmp(u, "h") ? 1.0 / 24.0 : !strcmp(u, "d") ? 1.0 :
            !strcmp(u, "w") ? 7.0 : !strcmp(u, "M") ? 30.0 : !strcmp(u, "y") ? 365.0 : -1.0;
    if (k < 0 || v * k > GANTT_DAYS_MAX) return false;
    *out = v * k;
    return true;
}

/* A day as an axis label in a strftime-like `fmt` (%Y %y %m %d %e %b %H %M). */
static void fmt_date(f64 day, const char *fmt, char *out, u32 cap) {
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    i64 z = (i64)floor(day), y, m, d;
    civil_from_days(z, &y, &m, &d);
    f64 frac = day - (f64)z;
    i64 hh = (i64)(frac * 24.0), mi = (i64)(frac * 1440.0) % 60;
    u32 o = 0;
    for (const char *f = fmt; *f && o + 8 < cap; f++) {
        if (*f != '%' || !f[1]) { out[o++] = *f; continue; }
        f++;
        int w = 0;
        switch (*f) {
        case 'Y': w = snprintf(out + o, cap - o, "%lld", (long long)y); break;
        case 'y': w = snprintf(out + o, cap - o, "%02lld", (long long)(y % 100)); break;
        case 'm': w = snprintf(out + o, cap - o, "%02lld", (long long)m); break;
        case 'd': w = snprintf(out + o, cap - o, "%02lld", (long long)d); break;
        case 'e': w = snprintf(out + o, cap - o, "%lld", (long long)d); break;
        case 'b': w = snprintf(out + o, cap - o, "%s", mon[(m - 1) % 12]); break;
        case 'H': w = snprintf(out + o, cap - o, "%02lld", (long long)hh); break;
        case 'M': w = snprintf(out + o, cap - o, "%02lld", (long long)mi); break;
        default: out[o++] = *f; break;
        }
        if (w > 0) o += (u32)w;
    }
    out[o < cap ? o : cap - 1] = 0;
}

/* =========================================================================
 * gantt
 * ========================================================================= */

enum { GT_DONE = 1, GT_ACTIVE = 2, GT_CRIT = 4, GT_MILE = 8 };

typedef struct {
    char name[DG_LABEL_CAP];
    char id[DG_ID_CAP];
    char after[DG_LABEL_CAP];   /* ids it starts after, space separated */
    char until[DG_ID_CAP];
    i32  section;
    u8   flags;
    bool has_start, has_end, has_dur;
    f64  start, end, dur;       /* days */
} GTask;

static GTask gt[DGM_MAX_ITEMS];
static u32   ngt;
static char  gsec[DGM_MAX_GROUPS][DG_LABEL_CAP];
static u32   ngsec;
static char  g_datefmt[32], g_axisfmt[32];

static void gantt_spec(GTask *t, const u8 *s, u32 n) {
    const u8 *tok[8]; u32 tl[8]; u32 nt = 0;
    u32 i = 0;
    while (i <= n && nt < 8) {
        u32 e = i;
        while (e < n && s[e] != ',') e++;
        const u8 *p = s + i; u32 pl = e - i;
        trim(&p, &pl);
        if (pl) { tok[nt] = p; tl[nt] = pl; nt++; }
        i = e + 1;
    }
    u32 k = 0;
    for (; k < nt; k++) {
        if (dg_kw(tok[k], tl[k], "done")) t->flags |= GT_DONE;
        else if (dg_kw(tok[k], tl[k], "active")) t->flags |= GT_ACTIVE;
        else if (dg_kw(tok[k], tl[k], "crit")) t->flags |= GT_CRIT;
        else if (dg_kw(tok[k], tl[k], "milestone")) t->flags |= GT_MILE;
        else break;
    }
    u32 left = nt - k;
    const u8 *start = NULL, *end = NULL; u32 sl = 0, el = 0;
    if (left >= 3) {
        dg_strcpy(t->id, sizeof t->id, tok[k], tl[k]);
        start = tok[k + 1]; sl = tl[k + 1]; end = tok[k + 2]; el = tl[k + 2];
    } else if (left == 2) {
        start = tok[k]; sl = tl[k]; end = tok[k + 1]; el = tl[k + 1];
    } else if (left == 1) {
        end = tok[k]; el = tl[k];
    }
    if (start) {
        f64 d;
        if (dg_starts(start, sl, "after ")) {
            dg_strcpy(t->after, sizeof t->after, start + 6, sl - 6);
        } else if (parse_date(start, sl, g_datefmt, &d) || parse_date(start, sl, "YYYY-MM-DD", &d)) {
            t->start = d; t->has_start = true;
        } else {
            /* not a start: an id, the task starting where the last one ended */
            dg_strcpy(t->id, sizeof t->id, start, sl);
        }
    }
    if (end) {
        f64 d;
        if (dg_starts(end, el, "until ")) dg_strcpy(t->until, sizeof t->until, end + 6, el - 6);
        else if (parse_duration(end, el, &d)) { t->dur = d; t->has_dur = true; }
        else if (parse_date(end, el, g_datefmt, &d) || parse_date(end, el, "YYYY-MM-DD", &d)) {
            t->end = d; t->has_end = true;
        }
    }
}

static bool gantt_parse(DScan *sc) {
    snprintf(g_datefmt, sizeof g_datefmt, "YYYY-MM-DD");
    snprintf(g_axisfmt, sizeof g_axisfmt, "%%Y-%%m-%%d");
    i32 section = -1;
    const u8 *line; u32 ll;
    while (dg_next_line(sc, &line, &ll)) {
        const u8 *r; u32 rn;
        if (dg_kw(line, ll, "title")) { title_line(line, ll); continue; }
        if (after_kw(line, ll, "dateFormat", &r, &rn)) { dg_strcpy(g_datefmt, sizeof g_datefmt, r, rn); continue; }
        if (after_kw(line, ll, "axisFormat", &r, &rn)) { dg_strcpy(g_axisfmt, sizeof g_axisfmt, r, rn); continue; }
        if (after_kw(line, ll, "section", &r, &rn)) {
            if (ngsec >= DGM_MAX_GROUPS) { g_trunc = true; return false; }
            set_label(gsec[ngsec], DG_LABEL_CAP, r, rn, 18);
            section = (i32)ngsec++;
            continue;
        }
        static const char *skip[] = { "excludes", "includes", "todayMarker", "tickInterval", "weekday",
                                       "inclusiveEndDates", "topAxis", "displayMode", "accTitle",
                                       "accDescr", "weekend", "click", NULL };
        bool skipped = false;
        for (u32 k = 0; skip[k] && !skipped; k++) skipped = dg_kw(line, ll, skip[k]);
        if (skipped) continue;
        u32 c = find_byte(line, ll, ':');
        if (c >= ll) continue;
        if (ngt >= DGM_MAX_ITEMS) { g_trunc = true; return false; }
        GTask *t = &gt[ngt++];
        memset(t, 0, sizeof *t);
        set_label(t->name, sizeof t->name, line, c, 0);
        t->section = section;
        gantt_spec(t, line + c + 1, ll - c - 1);
    }
    if (!ngt) return false;
    /* Times, in order: a start that is not written is where the previous task
     * (or the tasks it comes after) ended; an end that is not written is a
     * duration on, the start of the task it runs until, or a day. */
    f64 first = INFINITY;
    for (u32 i = 0; i < ngt; i++) {
        if (gt[i].has_start && gt[i].start < first) first = gt[i].start;
        if (gt[i].has_end && gt[i].end < first) first = gt[i].end;
    }
    if (!isfinite(first)) first = (f64)days_from_civil(2024, 1, 1);
    f64 prev = first;
    for (u32 i = 0; i < ngt; i++) {
        GTask *t = &gt[i];
        if (!t->has_start) {
            t->start = prev;
            if (t->after[0]) {
                f64 best = -INFINITY;
                const char *p = t->after;
                while (*p) {
                    while (*p == ' ') p++;
                    const char *q = p;
                    while (*q && *q != ' ') q++;
                    for (u32 j = 0; j < i; j++)
                        if (strlen(gt[j].id) == (size_t)(q - p) && !strncmp(gt[j].id, p, (size_t)(q - p)) &&
                            gt[j].end > best)
                            best = gt[j].end;
                    p = q;
                }
                if (isfinite(best)) t->start = best;
            }
        }
        if (!t->has_end) {
            if (t->has_dur) t->end = t->start + t->dur;
            else if (t->until[0]) {
                t->end = t->start + 1.0;
                for (u32 j = 0; j < ngt; j++)
                    if (!strcmp(gt[j].id, t->until) && gt[j].has_start) t->end = gt[j].start;
            } else t->end = (t->flags & GT_MILE) ? t->start : t->start + 1.0;
        }
        if (t->end < t->start) t->end = t->start;
        prev = t->end;
    }
    return true;
}

/* Tick spacing in days for a range, and whether ticks fall on month starts. */
static f64 gantt_tick(f64 range, f32 width, f32 label_w, bool *months) {
    static const f64 steps[] = { 1.0 / 24, 1.0 / 12, 0.25, 0.5, 1, 2, 7, 14, 30, 91, 182, 365, 730, 1825 };
    *months = false;
    for (u32 i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        f64 n = range / steps[i];
        if (n * (f64)(label_w + g_cw * 2.0f) <= (f64)width) { *months = steps[i] >= 30; return steps[i]; }
    }
    *months = true;
    return 3650;
}

static void gantt_emit(void) {
    f64 lo = INFINITY, hi = -INFINITY;
    for (u32 i = 0; i < ngt; i++) {
        if (gt[i].start < lo) lo = gt[i].start;
        if (gt[i].end > hi) hi = gt[i].end;
    }
    f64 range = hi - lo;
    if (!(range > 0)) range = 1.0;
    f32 gutter = 0;
    for (u32 s = 0; s < ngsec; s++) gutter = fmaxf(gutter, text_w(gsec[s]) + g_cw * 2.0f);
    f32 chart = g_cw * 88.0f;
    f32 ppd = (f32)((f64)chart / range);
    f32 row = g_ch * (1.45f + 0.35f * g_gap), bar = g_ch * 1.15f;
    f32 top = title_h();
    f32 x0 = gutter;
    f32 y = top;
    f32 t = dg_stroke_w();

    /* section bands, the rows they hold */
    f32 ys[DGM_MAX_ITEMS];
    i32 cur = -2;
    f32 band_y0 = y;
    for (u32 i = 0; i <= ngt; i++) {
        i32 s = i < ngt ? gt[i].section : -3;
        if (s != cur) {
            if (cur != -2 && y > band_y0) {
                if (cur >= 0 && (cur & 1) == 0)
                    em_box(P_BACK, 0, band_y0, x0 + chart, y - band_y0, 0, 0, DI_GROUP_FILL, DI_NONE);
                if (cur >= 0) {
                    f32 lh = text_h(gsec[cur]);
                    em_text_box(gsec[cur], g_cw * 0.6f, band_y0 + (y - band_y0 - lh) * 0.5f,
                                text_w(gsec[cur]), lh, DI_TEXT_DIM);
                }
            }
            cur = s;
            band_y0 = y;
        }
        if (i < ngt) { ys[i] = y; y += row; }
    }
    f32 y_end = y;

    /* the time axis: grid lines and labels under the rows */
    char sample[48];
    fmt_date(lo, g_axisfmt, sample, sizeof sample);
    bool months;
    f64 step = gantt_tick(range, chart, text_w(sample), &months);
    i64 sy, sm, sd;
    civil_from_days((i64)floor(lo), &sy, &sm, &sd);
    for (u32 k = 0; k < 400; k++) {
        f64 day;
        if (months) {
            i64 mstep = (i64)(step / 30.0 + 0.5);
            if (mstep < 1) mstep = 1;
            i64 mm = sm - 1 + (i64)k * mstep;
            day = (f64)days_from_civil(sy + mm / 12, mm % 12 + 1, 1);
            if (day < lo - 0.01) continue;
        } else {
            f64 base = floor(lo / step) * step;
            if (step == 7) base = floor((lo - 4.0) / 7.0) * 7.0 + 4.0;   /* Mondays */
            day = base + (f64)k * step;
            if (day < lo - 1e-6) continue;
        }
        if (day > hi + 1e-6) break;
        f32 x = x0 + (f32)(day - lo) * ppd;
        em_seg(P_BACK, x, top, x, y_end, fmaxf(1.0f, t * 0.6f), DI_GROUP_STROKE);
        char lab[48];
        fmt_date(day, g_axisfmt, lab, sizeof lab);
        f32 lw = text_w(lab);
        em_text(x - lw * 0.5f, y_end + g_ch * 0.35f, lab, (u32)strlen(lab), DI_TEXT_DIM);
    }
    em_seg(P_BACK, x0, y_end, x0 + chart, y_end, t, DI_GROUP_STROKE);

    /* the tasks */
    for (u32 i = 0; i < ngt; i++) {
        const GTask *k = &gt[i];
        f32 bx = x0 + (f32)(k->start - lo) * ppd, bw = (f32)(k->end - k->start) * ppd;
        f32 by = ys[i] + (row - bar) * 0.5f;
        f32 nw = text_w(k->name);
        u8 ink = DI_TEXT;
        if (k->flags & GT_MILE) {
            f32 cx = bx + bw * 0.5f;
            rhombus(cx, by + bar * 0.5f, bar * 1.2f, bar * 1.2f, (k->flags & GT_CRIT) ? vivid(5) : DI_ACCENT);
            em_text(cx + bar * 0.9f, by + (bar - g_ch) * 0.5f, k->name, (u32)strlen(k->name), DI_TEXT);
            continue;
        }
        if (bw < 2.0f) bw = 2.0f;
        u8 fill = DI_NODE_FILL, edge = DI_NODE_STROKE;
        if (k->flags & GT_CRIT)        { fill = soft(5); edge = vivid(5); }
        else if (k->flags & GT_ACTIVE) { fill = DI_ACTIVATION; edge = DI_ACCENT; }
        else if (k->flags & GT_DONE)   { fill = DI_GROUP_STROKE; edge = DI_GROUP_TEXT; ink = DI_TEXT; }
        em_box(P_NODE, bx, by, bw, bar, g_ch * 0.22f, t, fill, edge);
        f32 ty = by + (bar - g_ch) * 0.5f;
        /* a finished task carries a tick before its name */
        char name[DG_LABEL_CAP + 8];
        snprintf(name, sizeof name, "%s%s", (k->flags & GT_DONE) ? "\xE2\x9C\x93 " : "", k->name);
        nw = text_w(name);
        u32 nl = (u32)strlen(name);
        if (nw + g_cw * 1.2f <= bw) em_text(bx + (bw - nw) * 0.5f, ty, name, nl, ink);
        else if (bx + bw + g_cw * 0.6f + nw <= x0 + chart) em_text(bx + bw + g_cw * 0.6f, ty, name, nl, DI_TEXT);
        else em_text(bx - g_cw * 0.6f - nw, ty, name, nl, DI_TEXT);
    }
    emit_title(x0, x0 + chart);
}

/* =========================================================================
 * timeline
 * ========================================================================= */

typedef struct { char label[DG_LABEL_CAP]; i32 section; u32 ev0, nev; } TPeriod;

static TPeriod tp[DGM_MAX_GROUPS * 2];
static u32     ntp;
static char    tev[DGM_MAX_ITEMS][DG_LABEL_CAP];
static u32     ntev;
static char    tsec[DGM_MAX_GROUPS][DG_LABEL_CAP];
static u32     ntsec;

static bool tl_event(const u8 *s, u32 n) {
    trim(&s, &n);
    if (!n || !ntp) return true;
    if (ntev >= DGM_MAX_ITEMS) { g_trunc = true; return false; }
    set_label(tev[ntev++], DG_LABEL_CAP, s, n, 20);
    tp[ntp - 1].nev++;
    return true;
}

static bool timeline_parse(DScan *sc) {
    i32 section = -1;
    const u8 *line; u32 ll;
    while (dg_next_line(sc, &line, &ll)) {
        const u8 *r; u32 rn;
        if (dg_kw(line, ll, "title")) { title_line(line, ll); continue; }
        if (dg_kw(line, ll, "accTitle") || dg_kw(line, ll, "accDescr")) continue;
        if (after_kw(line, ll, "section", &r, &rn)) {
            if (ntsec >= DGM_MAX_GROUPS) { g_trunc = true; return false; }
            set_label(tsec[ntsec], DG_LABEL_CAP, r, rn, 24);
            section = (i32)ntsec++;
            continue;
        }
        u32 p = 0;
        if (line[0] != ':') {
            if (ntp >= sizeof tp / sizeof tp[0]) { g_trunc = true; return false; }
            u32 c = find_byte(line, ll, ':');
            TPeriod *pd = &tp[ntp++];
            memset(pd, 0, sizeof *pd);
            set_label(pd->label, sizeof pd->label, line, c, 18);
            pd->section = section;
            pd->ev0 = ntev;
            p = c;
        }
        while (p < ll) {
            u32 e = p + 1;
            while (e < ll && line[e] != ':') e++;
            if (!tl_event(line + p + 1, e - p - 1)) return false;
            p = e;
        }
    }
    return ntp > 0;
}

static void timeline_emit(void) {
    f32 pad = g_cw * 1.2f, gap = g_cw * (0.8f + 0.6f * g_gap), t = dg_stroke_w();
    /* A box named: the rows close up, and the text keeps its size. */
    bool fitted = g_fit_h > 0.0f;
    f32 ev_pad = g_ch * 0.7f, ev_step = g_ch * (0.35f + 0.25f * g_gap), ev_side = g_cw * 0.9f;
    f32 colw[DGM_MAX_GROUPS * 2], roww[DGM_MAX_GROUPS * 2];
    f32 head_h = 0, stack_h = 0, row_h = 0, stack_w = 0, row_w = 0;
    for (u32 i = 0; i < ntp; i++) {
        f32 w = text_w(tp[i].label), rw = 0, sh = 0;
        for (u32 k = 0; k < tp[i].nev; k++) {
            const char *ev = tev[tp[i].ev0 + k];
            w = fmaxf(w, text_w(ev));
            rw += text_w(ev) + pad * 1.4f + (k ? ev_side : 0.0f);
            sh += text_h(ev) + ev_pad + (k ? ev_step : 0.0f);
            row_h = fmaxf(row_h, text_h(ev) + ev_pad);
        }
        colw[i] = fmaxf(w + pad * 2.0f, g_cw * 14.0f);
        roww[i] = fmaxf(fmaxf(text_w(tp[i].label) + pad * 2.0f, rw), g_cw * 8.0f);
        stack_w += colw[i] + (i ? gap : 0.0f);
        row_w += roww[i] + (i ? gap : 0.0f);
        stack_h = fmaxf(stack_h, sh);
        head_h = fmaxf(head_h, text_h(tp[i].label));
    }
    f32 top = g_title[0] ? text_h(g_title) + g_ch * (fitted ? 0.5f : 0.9f) : 0.0f;
    f32 sec_h = ntsec ? g_ch * (fitted ? 1.4f : 1.8f) : 0.0f;
    f32 py = top + sec_h + (ntsec ? g_ch * (fitted ? 0.3f : 0.4f) : 0.0f);
    f32 ph = head_h + g_ch * (fitted ? 0.6f : 0.9f);
    f32 line_y = py + ph + g_ch * (fitted ? 0.8f : 1.1f);
    f32 ev_y = line_y + g_ch * (fitted ? 0.9f : 1.2f);
    /* A period's events stand side by side under it where the box is too low
     * for them one under another and that draws the whole larger. */
    bool side = false;
    if (fitted && g_fit_w > 0.0f && stack_h > row_h && ev_y + stack_h > g_fit_h) {
        f32 stacked = fminf(1.0f, fminf(g_fit_w / fmaxf(1.0f, stack_w), g_fit_h / (ev_y + stack_h)));
        f32 beside = fminf(1.0f, fminf(g_fit_w / fmaxf(1.0f, row_w), g_fit_h / (ev_y + row_h)));
        side = beside > stacked * 1.1f;
    }
    if (side) memcpy(colw, roww, (usize)ntp * sizeof colw[0]);
    f32 xs[DGM_MAX_GROUPS * 2];
    f32 x = 0;
    for (u32 i = 0; i < ntp; i++) { xs[i] = x; x += colw[i] + gap; }
    f32 total = x - gap;

    /* sections over the periods they hold */
    for (u32 s = 0; s < ntsec; s++) {
        f32 a = 1e30f, b = -1e30f;
        for (u32 i = 0; i < ntp; i++)
            if (tp[i].section == (i32)s) { a = fminf(a, xs[i]); b = fmaxf(b, xs[i] + colw[i]); }
        if (b < a) continue;
        em_box(P_NODE, a, top, b - a, sec_h, g_ch * 0.25f, 0, soft(s), DI_NONE);
        em_text_box(tsec[s], a, top, b - a, sec_h, DI_TEXT);
    }
    /* the line the periods hang on, with an arrow at its end */
    em_seg(P_EDGE, -g_cw, line_y, total + g_cw * 1.5f, line_y, t * 1.6f, DI_EDGE);
    em_wedge(P_EDGE, total + g_cw * 2.6f, line_y, total + g_cw * 1.4f, line_y, g_ch * 0.3f, DI_EDGE);
    for (u32 i = 0; i < ntp; i++) {
        u32 c = ntsec && tp[i].section >= 0 ? (u32)tp[i].section : i;
        f32 cx = xs[i] + colw[i] * 0.5f;
        f32 stem = fmaxf(1.0f, t * 0.8f);
        em_box(P_NODE, xs[i], py, colw[i], ph, g_ch * 0.25f, t, soft(c), vivid(c));
        em_text_box(tp[i].label, xs[i], py, colw[i], ph, DI_TEXT);
        em_seg(P_EDGE, cx, py + ph, cx, line_y, t, vivid(c));
        if (side && tp[i].nev) {
            /* one row: a rail under the line, a drop to each event */
            f32 rw = 0;
            for (u32 k = 0; k < tp[i].nev; k++) rw += text_w(tev[tp[i].ev0 + k]) + pad * 1.4f + (k ? ev_side : 0.0f);
            f32 ex = xs[i] + (colw[i] - rw) * 0.5f;
            f32 rail = (line_y + ev_y) * 0.5f, first = 0, last = 0;
            for (u32 k = 0; k < tp[i].nev; k++) {
                const char *ev = tev[tp[i].ev0 + k];
                f32 ew = text_w(ev) + pad * 1.4f, mid = ex + ew * 0.5f;
                em_box(P_NODE, ex, ev_y, ew, row_h, g_ch * 0.2f, t, DI_NODE_FILL, vivid(c));
                em_text_box(ev, ex, ev_y, ew, row_h, DI_TEXT);
                if (tp[i].nev > 1) em_seg(P_BACK, mid, rail, mid, ev_y, stem, vivid(c));
                if (!k) first = mid;
                last = mid;
                ex += ew + ev_side;
            }
            if (tp[i].nev > 1) {
                em_seg(P_BACK, first, rail, last, rail, stem, vivid(c));
                em_seg(P_BACK, cx, line_y, cx, rail, stem, vivid(c));
            } else {
                em_seg(P_BACK, cx, line_y, cx, ev_y, stem, vivid(c));
            }
        } else {
            f32 ey = ev_y, last = line_y;
            for (u32 k = 0; k < tp[i].nev; k++) {
                const char *ev = tev[tp[i].ev0 + k];
                f32 eh = text_h(ev) + ev_pad;
                em_box(P_NODE, xs[i] + g_cw * 0.4f, ey, colw[i] - g_cw * 0.8f, eh, g_ch * 0.2f, t, DI_NODE_FILL, vivid(c));
                em_text_box(ev, xs[i] + g_cw * 0.4f, ey, colw[i] - g_cw * 0.8f, eh, DI_TEXT);
                last = ey;
                ey += eh + ev_step;
            }
            if (tp[i].nev) em_seg(P_BACK, cx, line_y, cx, last, stem, vivid(c));
        }
        dot(cx, line_y, g_ch * 0.32f, vivid(c), DI_NONE, 0);
    }
    emit_title(0, total);
}

/* =========================================================================
 * journey
 * ========================================================================= */

typedef struct { char name[DG_LABEL_CAP]; i32 section; i32 score; u32 actors; } JTask;

static JTask jt[DGM_MAX_ITEMS];
static u32   njt;
static char  jsec[DGM_MAX_GROUPS][DG_LABEL_CAP];
static u32   njsec;
static char  jact[DGM_MAX_ACTORS][DG_ID_CAP];
static u32   njact;

static i32 journey_actor(const u8 *s, u32 n) {
    unquote(&s, &n);
    if (!n) return -1;
    for (u32 i = 0; i < njact; i++)
        if (strlen(jact[i]) == n && !memcmp(jact[i], s, n)) return (i32)i;
    if (njact >= DGM_MAX_ACTORS) return -1;
    dg_strcpy(jact[njact], DG_ID_CAP, s, n);
    return (i32)njact++;
}

static bool journey_parse(DScan *sc) {
    i32 section = -1;
    const u8 *line; u32 ll;
    while (dg_next_line(sc, &line, &ll)) {
        const u8 *r; u32 rn;
        if (dg_kw(line, ll, "title")) { title_line(line, ll); continue; }
        if (dg_kw(line, ll, "accTitle") || dg_kw(line, ll, "accDescr")) continue;
        if (after_kw(line, ll, "section", &r, &rn)) {
            if (njsec >= DGM_MAX_GROUPS) { g_trunc = true; return false; }
            set_label(jsec[njsec], DG_LABEL_CAP, r, rn, 22);
            section = (i32)njsec++;
            continue;
        }
        u32 c1 = find_byte(line, ll, ':');
        if (c1 >= ll) continue;
        if (njt >= DGM_MAX_ITEMS) { g_trunc = true; return false; }
        JTask *t = &jt[njt++];
        memset(t, 0, sizeof *t);
        set_label(t->name, sizeof t->name, line, c1, 16);
        t->section = section;
        u32 c2 = c1 + 1 + find_byte(line + c1 + 1, ll - c1 - 1, ':');
        char num[16];
        const u8 *sv = line + c1 + 1; u32 sn = (c2 < ll ? c2 : ll) - c1 - 1;
        trim(&sv, &sn);
        if (sn >= sizeof num) sn = sizeof num - 1;
        memcpy(num, sv, sn); num[sn] = 0;
        t->score = atoi(num);
        if (t->score < 1) t->score = 1;
        if (t->score > 5) t->score = 5;
        u32 p = c2 + 1;
        while (p < ll) {
            u32 e = p;
            while (e < ll && line[e] != ',') e++;
            i32 a = journey_actor(line + p, e - p);
            if (a >= 0) t->actors |= 1u << a;
            p = e + 1;
        }
    }
    return njt > 0;
}

static void journey_emit(void) {
    f32 t = dg_stroke_w(), gap = g_cw * (0.6f + 0.6f * g_gap);
    f32 legend = 0;
    for (u32 a = 0; a < njact; a++) legend = fmaxf(legend, text_w(jact[a]) + g_ch * 1.3f);
    if (legend > 0) legend += g_cw * 2.0f;
    f32 top = title_h();
    f32 sec_h = njsec ? g_ch * 1.8f : 0.0f;
    f32 box_y = top + sec_h + (njsec ? g_ch * 0.4f : 0.0f);
    f32 box_h = 0;
    f32 colw[DGM_MAX_ITEMS], xs[DGM_MAX_ITEMS];
    f32 x = legend;
    for (u32 i = 0; i < njt; i++) {
        colw[i] = fmaxf(text_w(jt[i].name) + g_cw * 2.4f, g_cw * 12.0f);
        box_h = fmaxf(box_h, text_h(jt[i].name));
        xs[i] = x;
        x += colw[i] + gap;
    }
    f32 total = x - gap;
    box_h += g_ch * (njact ? 1.9f : 1.0f);
    for (u32 s = 0; s < njsec; s++) {
        f32 a = 1e30f, b = -1e30f;
        for (u32 i = 0; i < njt; i++)
            if (jt[i].section == (i32)s) { a = fminf(a, xs[i]); b = fmaxf(b, xs[i] + colw[i]); }
        if (b < a) continue;
        em_box(P_NODE, a, top, b - a, sec_h, g_ch * 0.25f, 0, soft(s), DI_NONE);
        em_text_box(jsec[s], a, top, b - a, sec_h, DI_TEXT);
    }
    /* the score band: five levels under the tasks, 5 on top */
    f32 band_y = box_y + box_h + g_ch * 1.4f, lvl = g_ch * 1.25f;
    f32 fx[DGM_MAX_ITEMS], fy[DGM_MAX_ITEMS];
    for (u32 i = 0; i < njt; i++) {
        const JTask *k = &jt[i];
        u32 c = k->section >= 0 ? (u32)k->section : 0;
        em_box(P_NODE, xs[i], box_y, colw[i], box_h, g_ch * 0.25f, t, DI_NODE_FILL, vivid(c));
        f32 th = text_h(k->name);
        em_text_box(k->name, xs[i], box_y + g_ch * 0.4f, colw[i], th, DI_TEXT);
        u32 na = 0;
        for (u32 a = 0; a < njact; a++) if (k->actors & (1u << a)) na++;
        f32 ax = xs[i] + (colw[i] - (f32)na * g_ch * 0.7f) * 0.5f + g_ch * 0.3f;
        for (u32 a = 0; a < njact; a++)
            if (k->actors & (1u << a)) { dot(ax, box_y + box_h - g_ch * 0.6f, g_ch * 0.26f, vivid(a + 2), DI_NONE, 0); ax += g_ch * 0.7f; }
        fx[i] = xs[i] + colw[i] * 0.5f;
        fy[i] = band_y + (f32)(5 - k->score) * lvl;
        f32 dash[2] = { box_y + box_h, fy[i] - g_ch * 0.55f };
        f32 dx[2] = { fx[i], fx[i] };
        if (dash[1] > dash[0]) em_dashed(P_BACK, dx, dash, 2, fmaxf(1.0f, t * 0.8f), g_ch * 0.25f, g_ch * 0.2f, DI_GROUP_STROKE);
    }
    if (njt >= 2) {
        static f32 px[4096], py[4096];
        u32 n = dg_smooth(fx, fy, njt, px, py, 4096);
        em_poly(P_EDGE, px, py, n, false, t * 1.3f, DI_EDGE);
    }
    for (u32 i = 0; i < njt; i++) {
        i32 sc = jt[i].score;
        u32 c = sc >= 4 ? 2u : sc == 3 ? 3u : 5u;
        dot(fx[i], fy[i], g_ch * 0.55f, soft(c), vivid(c), t);
        char d[4] = { (char)('0' + sc), 0 };
        em_text_box(d, fx[i] - g_ch * 0.55f, fy[i] - g_ch * 0.55f, g_ch * 1.1f, g_ch * 1.1f, DI_TEXT);
    }
    /* actors */
    for (u32 a = 0; a < njact; a++) {
        f32 ly = box_y + (f32)a * g_ch * 1.3f;
        dot(g_ch * 0.4f, ly + g_ch * 0.5f, g_ch * 0.3f, vivid(a + 2), DI_NONE, 0);
        em_text(g_ch * 1.0f, ly, jact[a], (u32)strlen(jact[a]), DI_TEXT);
    }
    emit_title(legend, total);
}

/* =========================================================================
 * xychart
 * ========================================================================= */

typedef struct { bool line; f64 v[DGM_MAX_VALUES]; u32 n; } XSeries;

static XSeries xs_[DGM_MAX_SERIES];
static u32     nxs;
static char    xcat[DGM_MAX_VALUES][DG_ELABEL_CAP];
static u32     nxcat;
static bool    x_numeric, y_given, xy_horizontal;
static f64     x_lo, x_hi, y_lo, y_hi;
static char    x_title[DG_ELABEL_CAP], y_title[DG_ELABEL_CAP];

static void put_cat(void *ctx, u32 i, const u8 *s, u32 n) {
    (void)ctx;
    if (i < DGM_MAX_VALUES) { unquote(&s, &n); dg_set_label(xcat[i], DG_ELABEL_CAP, s, n); }
}

static void put_val(void *ctx, u32 i, const u8 *s, u32 n) {
    XSeries *se = (XSeries *)ctx;
    char b[48];
    if (n >= sizeof b) n = sizeof b - 1;
    memcpy(b, s, n); b[n] = 0;
    if (i < DGM_MAX_VALUES) se->v[i] = strtod(b, NULL);
}

/* `"title" lo --> hi`, `lo --> hi`, `"title" [..]` or `"title"`. */
static void xy_axis(const u8 *s, u32 n, char *title, bool *range, f64 *lo, f64 *hi) {
    trim(&s, &n);
    if (n && s[0] == '"') {
        u32 e = 1;
        while (e < n && s[e] != '"') e++;
        dg_set_label(title, DG_ELABEL_CAP, s + 1, e - 1);
        s += e < n ? e + 1 : n;
        n -= e < n ? e + 1 : n;
        trim(&s, &n);
    } else if (n && s[0] != '[' && !(s[0] >= '0' && s[0] <= '9') && s[0] != '-') {
        u32 e = 0;
        while (e < n && !dg_is_space(s[e])) e++;
        dg_set_label(title, DG_ELABEL_CAP, s, e);
        s += e; n -= e;
        trim(&s, &n);
    }
    char b[96];
    u32 bn = n < sizeof b - 1 ? n : (u32)sizeof b - 1;
    memcpy(b, s, bn); b[bn] = 0;
    char *arrow = strstr(b, "-->");
    if (arrow) {
        *arrow = 0;
        *lo = strtod(b, NULL);
        *hi = strtod(arrow + 3, NULL);
        *range = *hi != *lo;
    }
}

#define XY_ABS_MAX 1e15

static bool xy_number_ok(f64 v) { return isfinite(v) && fabs(v) <= XY_ABS_MAX; }

static bool xychart_parse(DScan *sc, const u8 *head, u32 hlen) {
    for (u32 i = 0; i + 10 <= hlen; i++)
        if (!memcmp(head + i, "horizontal", 10)) xy_horizontal = true;
    const u8 *line; u32 ll;
    while (dg_next_line(sc, &line, &ll)) {
        const u8 *r; u32 rn;
        if (dg_kw(line, ll, "title")) { title_line(line, ll); continue; }
        if (dg_kw(line, ll, "x-axis")) {
            r = line + 6; rn = ll - 6;
            if (find_byte(r, rn, '[') < rn) {
                xy_axis(r, find_byte(r, rn, '['), x_title, &x_numeric, &x_lo, &x_hi);
                x_numeric = false;
                nxcat = list_items(r, rn, put_cat, NULL, DGM_MAX_VALUES);
            } else xy_axis(r, rn, x_title, &x_numeric, &x_lo, &x_hi);
            continue;
        }
        if (dg_kw(line, ll, "y-axis")) { xy_axis(line + 6, ll - 6, y_title, &y_given, &y_lo, &y_hi); continue; }
        bool is_line = dg_kw(line, ll, "line");
        if (is_line || dg_kw(line, ll, "bar")) {
            if (nxs >= DGM_MAX_SERIES) { g_trunc = true; return false; }
            XSeries *se = &xs_[nxs++];
            memset(se, 0, sizeof *se);
            se->line = is_line;
            se->n = list_items(line, ll, put_val, se, DGM_MAX_VALUES);
            if (!se->n) nxs--;
        }
    }
    /* Every number the axes are worked out from is finite and of a size whose
     * sums stay finite; a chart with any other is not drawn. */
    for (u32 k = 0; k < nxs; k++)
        for (u32 i = 0; i < xs_[k].n; i++)
            if (!xy_number_ok(xs_[k].v[i])) return false;
    if (y_given && (!xy_number_ok(y_lo) || !xy_number_ok(y_hi))) return false;
    if (x_numeric && (!xy_number_ok(x_lo) || !xy_number_ok(x_hi))) return false;
    return nxs > 0;
}

static void xychart_emit(void) {
    u32 n = nxcat;
    for (u32 s = 0; s < nxs; s++) if (xs_[s].n > n) n = xs_[s].n;
    if (n == 0) return;
    f64 lo = 0, hi = 0;
    bool first = true;
    for (u32 s = 0; s < nxs; s++)
        for (u32 i = 0; i < xs_[s].n; i++) {
            f64 v = xs_[s].v[i];
            if (first || v < lo) lo = v;
            if (first || v > hi) hi = v;
            first = false;
        }
    if (y_given) { lo = fmin(y_lo, y_hi); hi = fmax(y_lo, y_hi); }
    else { if (lo > 0) lo = 0; if (hi < 0) hi = 0; }
    if (!(hi > lo)) hi = lo + 1.0;
    /* category labels */
    char labs[DGM_MAX_VALUES][DG_ELABEL_CAP];
    for (u32 i = 0; i < n; i++) {
        if (i < nxcat) snprintf(labs[i], sizeof labs[i], "%s", xcat[i]);
        else if (x_numeric) fmt_num(x_lo + (x_hi - x_lo) * (n > 1 ? (f64)i / (f64)(n - 1) : 0.0), labs[i], sizeof labs[i]);
        else snprintf(labs[i], sizeof labs[i], "%u", i + 1);
    }
    f32 lab_w = 0;
    for (u32 i = 0; i < n; i++) lab_w = fmaxf(lab_w, text_w(labs[i]));
    /* the band each category takes along its axis, and the value axis's length */
    f32 band = xy_horizontal ? g_ch * 1.9f : fmaxf(lab_w + g_cw * 1.4f, g_cw * 5.0f);
    f32 along = band * (f32)n;
    f32 across = xy_horizontal ? g_cw * 60.0f : g_ch * 14.0f;
    /* A box named: the plot takes what it leaves between the title and the
     * category row, down to a few rows, and the text keeps its size. The
     * value axis's name then stands on the title's row, left of it, where
     * the two do not meet. */
    bool y_on_title = false;
    if (!xy_horizontal && g_fit_h > 0.0f) {
        y_on_title = g_title[0] && y_title[0] && (along - text_w(g_title)) * 0.5f >= text_w(y_title);
        f32 above = title_h() + (y_title[0] && !y_on_title ? g_ch * 1.4f : 0.0f);
        f32 below = g_ch * 1.5f + (x_title[0] ? g_ch * 1.4f : 0.0f);
        across = fminf(across, fmaxf(g_ch * 3.5f, g_fit_h - above - below));
    }
    /* as many divisions as the value axis has room to name */
    f64 divs = xy_horizontal ? 5.0 : fmin(5.0, fmax(1.0, floor((f64)(across / (g_ch * 1.5f)))));
    f64 step = nice_step((hi - lo) / divs);
    if (!y_given) { lo = floor(lo / step) * step; hi = ceil(hi / step) * step; }
    /* The value ticks by count, not by adding up a step: near a large bound a
     * small step can be below the spacing of doubles there. */
    f64 span = (hi - lo) / step;
    u32 nticks = isfinite(span) && span > 0.0 ? (u32)fmin(span + 0.5, 64.0) : 1u;
    char vl[32];
    f32 val_w = 0;
    for (u32 k = 0; k <= nticks; k++) { fmt_num(lo + step * (f64)k, vl, sizeof vl); val_w = fmaxf(val_w, text_w(vl)); }
    f32 top = title_h() + (y_title[0] && !xy_horizontal && !y_on_title ? g_ch * 1.4f : 0.0f);
    f32 t = dg_stroke_w();
    f32 ox, oy, pw, ph;                            /* the plot box */
    if (!xy_horizontal) { ox = val_w + g_cw * 1.5f; oy = top; pw = along; ph = across; }
    else { ox = lab_w + g_cw * 1.5f; oy = top; pw = across; ph = along; }
    /* value grid and labels */
    for (u32 k = 0; k <= nticks; k++) {
        f64 v = lo + step * (f64)k;
        f32 f = (f32)((v - lo) / (hi - lo));
        fmt_num(v, vl, sizeof vl);
        f32 w = text_w(vl);
        if (!xy_horizontal) {
            f32 y = oy + ph - f * ph;
            em_seg(P_BACK, ox, y, ox + pw, y, fmaxf(1.0f, t * 0.6f), DI_GROUP_STROKE);
            em_text(ox - g_cw * 0.6f - w, y - g_ch * 0.5f, vl, (u32)strlen(vl), DI_TEXT_DIM);
        } else {
            f32 x = ox + f * pw;
            em_seg(P_BACK, x, oy, x, oy + ph, fmaxf(1.0f, t * 0.6f), DI_GROUP_STROKE);
            em_text(x - w * 0.5f, oy + ph + g_ch * 0.3f, vl, (u32)strlen(vl), DI_TEXT_DIM);
        }
    }
    /* category labels */
    for (u32 i = 0; i < n; i++) {
        f32 c = (f32)i * band + band * 0.5f, w = text_w(labs[i]);
        if (!xy_horizontal) em_text(ox + c - w * 0.5f, oy + ph + g_ch * 0.3f, labs[i], (u32)strlen(labs[i]), DI_TEXT_DIM);
        else em_text(ox - g_cw * 0.6f - w, oy + c - g_ch * 0.5f, labs[i], (u32)strlen(labs[i]), DI_TEXT_DIM);
    }
    em_seg(P_BACK, ox, oy + ph, ox + pw, oy + ph, t, DI_GROUP_STROKE);
    em_seg(P_BACK, ox, oy, ox, oy + ph, t, DI_GROUP_STROKE);
    /* bars side by side in each band, then lines over them */
    u32 nbar = 0;
    for (u32 s = 0; s < nxs; s++) if (!xs_[s].line) nbar++;
    f32 zero = (f32)((fmax(lo, fmin(0.0, hi)) - lo) / (hi - lo));
    u32 bi = 0, color = 0;
    for (u32 s = 0; s < nxs; s++, color++) {
        const XSeries *se = &xs_[s];
        if (se->line) continue;
        f32 group = band * 0.72f, bw = group / (f32)(nbar ? nbar : 1);
        for (u32 i = 0; i < se->n && i < n; i++) {
            f32 f = (f32)((fmax(lo, fmin(hi, se->v[i])) - lo) / (hi - lo));
            f32 a = fminf(f, zero), b = fmaxf(f, zero);
            f32 c0 = (f32)i * band + (band - group) * 0.5f + bw * (f32)bi;
            if (!xy_horizontal) em_box(P_NODE, ox + c0 + bw * 0.08f, oy + ph - b * ph, bw * 0.84f, (b - a) * ph, g_ch * 0.12f, 0, vivid(color), DI_NONE);
            else em_box(P_NODE, ox + a * pw, oy + c0 + bw * 0.08f, (b - a) * pw, bw * 0.84f, g_ch * 0.12f, 0, vivid(color), DI_NONE);
        }
        bi++;
    }
    color = 0;
    for (u32 s = 0; s < nxs; s++, color++) {
        const XSeries *se = &xs_[s];
        if (!se->line) continue;
        f32 lx[DGM_MAX_VALUES], ly[DGM_MAX_VALUES];
        u32 m = se->n < n ? se->n : n;
        for (u32 i = 0; i < m; i++) {
            f32 f = (f32)((fmax(lo, fmin(hi, se->v[i])) - lo) / (hi - lo));
            f32 c = (f32)i * band + band * 0.5f;
            if (!xy_horizontal) { lx[i] = ox + c; ly[i] = oy + ph - f * ph; }
            else { lx[i] = ox + f * pw; ly[i] = oy + c; }
        }
        em_poly(P_TOP, lx, ly, m, false, t * 2.0f, vivid(color + 4));
        for (u32 i = 0; i < m; i++) dot(lx[i], ly[i], g_ch * 0.22f, vivid(color + 4), DI_NONE, 0);
    }
    /* axis titles */
    if (y_title[0]) {
        if (!xy_horizontal) em_text(ox - val_w - g_cw * 0.6f, y_on_title ? (text_h(g_title) - g_ch) * 0.5f : oy - g_ch * 1.4f,
                                    y_title, (u32)strlen(y_title), DI_TEXT_DIM);
        else { f32 w = text_w(y_title); em_text(ox + (pw - w) * 0.5f, oy + ph + g_ch * 1.5f, y_title, (u32)strlen(y_title), DI_TEXT_DIM); }
    }
    if (x_title[0]) {
        f32 w = text_w(x_title);
        if (!xy_horizontal) em_text(ox + (pw - w) * 0.5f, oy + ph + g_ch * 1.6f, x_title, (u32)strlen(x_title), DI_TEXT_DIM);
        else em_text(0, oy - g_ch * 1.4f, x_title, (u32)strlen(x_title), DI_TEXT_DIM);
    }
    emit_title(ox, ox + pw);
}

/* =========================================================================
 * quadrantChart
 * ========================================================================= */

typedef struct { char label[DG_ELABEL_CAP]; f32 x, y; } QPoint;

static QPoint qp[DGM_MAX_ITEMS];
static u32    nqp;
static char   q_label[4][DG_LABEL_CAP];
static char   qx_lo[DG_ELABEL_CAP], qx_hi[DG_ELABEL_CAP], qy_lo[DG_ELABEL_CAP], qy_hi[DG_ELABEL_CAP];

static void quad_axis(const u8 *s, u32 n, char *lo, char *hi) {
    trim(&s, &n);
    u32 a = 0;
    while (a + 3 <= n && memcmp(s + a, "-->", 3) != 0) a++;
    if (a + 3 <= n) {
        set_label(lo, DG_ELABEL_CAP, s, a, 0);
        set_label(hi, DG_ELABEL_CAP, s + a + 3, n - a - 3, 0);
    } else set_label(lo, DG_ELABEL_CAP, s, n, 0);
}

static bool quadrant_parse(DScan *sc) {
    const u8 *line; u32 ll;
    while (dg_next_line(sc, &line, &ll)) {
        if (dg_kw(line, ll, "title")) { title_line(line, ll); continue; }
        if (dg_kw(line, ll, "x-axis")) { quad_axis(line + 6, ll - 6, qx_lo, qx_hi); continue; }
        if (dg_kw(line, ll, "y-axis")) { quad_axis(line + 6, ll - 6, qy_lo, qy_hi); continue; }
        if (dg_starts(line, ll, "quadrant-") && ll > 10 && line[9] >= '1' && line[9] <= '4') {
            set_label(q_label[line[9] - '1'], DG_LABEL_CAP, line + 10, ll - 10, 22);
            continue;
        }
        if (dg_kw(line, ll, "classDef") || dg_kw(line, ll, "accTitle") || dg_kw(line, ll, "accDescr")) continue;
        u32 c = find_byte(line, ll, ':');
        u32 b = find_byte(line, ll, '[');
        if (c >= ll || b >= ll || b < c) continue;
        if (nqp >= DGM_MAX_ITEMS) { g_trunc = true; return false; }
        QPoint *p = &qp[nqp++];
        u32 nl = c;
        for (u32 k = 0; k + 3 <= c; k++) if (!memcmp(line + k, ":::", 3)) { nl = k; break; }
        set_label(p->label, sizeof p->label, line, nl, 0);
        char buf[64];
        u32 e = b + 1;
        while (e < ll && line[e] != ']') e++;
        u32 bl = e - b - 1 < sizeof buf - 1 ? e - b - 1 : (u32)sizeof buf - 1;
        memcpy(buf, line + b + 1, bl); buf[bl] = 0;
        char *comma = strchr(buf, ',');
        p->x = (f32)strtod(buf, NULL);
        p->y = comma ? (f32)strtod(comma + 1, NULL) : 0.0f;
        p->x = fminf(1.0f, fmaxf(0.0f, p->x));
        p->y = fminf(1.0f, fmaxf(0.0f, p->y));
    }
    return nqp > 0 || q_label[0][0] || q_label[1][0];
}

static void quadrant_emit(void) {
    f32 t = dg_stroke_w();
    f32 side = g_ch * 22.0f;
    /* a box named: the square is as tall as it leaves under the title and
     * over the axis's names, down to one that still holds its points apart */
    if (g_fit_h > 0.0f) side = fminf(side, fmaxf(g_ch * 9.0f, g_fit_h - title_h() - g_ch * 1.6f));
    f32 left = fmaxf(text_w(qy_lo), text_w(qy_hi));
    if (left > 0) left += g_cw * 1.5f;
    f32 top = title_h();
    f32 x0 = left, y0 = top, h = side * 0.5f;
    /* quadrant 1 top right, 2 top left, 3 bottom left, 4 bottom right */
    const f32 qx[4] = { x0 + h, x0, x0, x0 + h }, qy[4] = { y0, y0, y0 + h, y0 + h };
    const u32 col[4] = { 0, 4, 2, 3 };
    for (u32 q = 0; q < 4; q++) {
        em_box(P_BACK, qx[q], qy[q], h, h, 0, 0, tint(col[q]), DI_NONE);
        if (q_label[q][0]) {
            f32 lh = text_h(q_label[q]);
            em_text_box(q_label[q], qx[q], qy[q] + g_ch * 0.5f, h, lh, DI_TEXT_DIM);
        }
    }
    em_seg(P_EDGE, x0 + h, y0, x0 + h, y0 + side, t, DI_GROUP_STROKE);
    em_seg(P_EDGE, x0, y0 + h, x0 + side, y0 + h, t, DI_GROUP_STROKE);
    f32 bx[4] = { x0, x0 + side, x0 + side, x0 }, by[4] = { y0, y0, y0 + side, y0 + side };
    em_poly(P_EDGE, bx, by, 4, true, t, DI_GROUP_STROKE);
    for (u32 i = 0; i < nqp; i++) {
        f32 px = x0 + qp[i].x * side, py = y0 + (1.0f - qp[i].y) * side;
        dot(px, py, g_ch * 0.32f, DI_ACCENT, DI_SURFACE, t);
        f32 w = text_w(qp[i].label);
        f32 lx = px + g_ch * 0.7f;
        if (lx + w + g_cw * 0.4f > x0 + side) lx = px - g_ch * 0.7f - w;
        f32 ly = py - g_ch * 0.5f;
        em_box(P_NODE, lx - g_cw * 0.4f, ly - g_ch * 0.08f, w + g_cw * 0.8f, g_ch * 1.16f, g_ch * 0.22f, 0,
               DI_CHIP, DI_NONE);
        em_text(lx, ly, qp[i].label, (u32)strlen(qp[i].label), DI_TEXT);
    }
    f32 ay = y0 + side + g_ch * 0.4f;
    if (qx_lo[0]) em_text(x0, ay, qx_lo, (u32)strlen(qx_lo), DI_TEXT_DIM);
    if (qx_hi[0]) em_text(x0 + side - text_w(qx_hi), ay, qx_hi, (u32)strlen(qx_hi), DI_TEXT_DIM);
    if (qy_lo[0]) em_text(x0 - g_cw * 0.8f - text_w(qy_lo), y0 + side - g_ch * 1.2f, qy_lo, (u32)strlen(qy_lo), DI_TEXT_DIM);
    if (qy_hi[0]) em_text(x0 - g_cw * 0.8f - text_w(qy_hi), y0 + g_ch * 0.2f, qy_hi, (u32)strlen(qy_hi), DI_TEXT_DIM);
    emit_title(x0, x0 + side);
}

/* =========================================================================
 * mindmap
 * ========================================================================= */

typedef struct {
    char label[DG_LABEL_CAP];
    i32  parent, depth, branch, side;   /* side: +1 right of the root, -1 left */
    u8   shape;                         /* 0 rounded, 1 square, 2 circle, 3 cloud */
    f32  x, y, w, h, sub;
} MNode;

static MNode mn[DGM_MAX_ITEMS];
static u32   nmn;

static void mind_node(MNode *m, const u8 *s, u32 n) {
    /* `id[text]`, `id(text)`, `id((text))`, `id))text((`, `id)text(`, `id{{text}}` or text */
    for (u32 k = 0; k + 3 <= n; k++) if (!memcmp(s + k, ":::", 3)) { n = k; break; }
    trim(&s, &n);
    static const struct { const char *o, *c; u8 shape; } forms[] = {
        { "((", "))", 2 }, { "))", "((", 3 }, { "{{", "}}", 0 }, { "[", "]", 1 },
        { "(", ")", 0 }, { ")", "(", 3 }, { NULL, NULL, 0 }
    };
    u32 i = 0;
    while (i < n && dg_is_word(s[i])) i++;
    for (u32 k = 0; forms[k].o; k++) {
        u32 ol = (u32)strlen(forms[k].o), cl = (u32)strlen(forms[k].c);
        if (i + ol + cl <= n && !memcmp(s + i, forms[k].o, ol) && !memcmp(s + n - cl, forms[k].c, cl)) {
            set_label(m->label, sizeof m->label, s + i + ol, n - i - ol - cl, m->depth == 0 ? 16 : 22);
            m->shape = forms[k].shape;
            return;
        }
    }
    set_label(m->label, sizeof m->label, s, n, m->depth == 0 ? 16 : 22);
    m->shape = 0;
}

static bool mindmap_parse(DScan *sc) {
    i32 stack[64]; u32 indent[64]; u32 sp = 0;
    while (sc->i < sc->len) {
        if (sc->lines++ >= DG_MAX_LINES) break;
        u32 ls = sc->i;
        while (sc->i < sc->len && sc->s[sc->i] != '\n') sc->i++;
        u32 le = sc->i;
        if (sc->i < sc->len) sc->i++;
        u32 ind = 0, p = ls;
        while (p < le && (sc->s[p] == ' ' || sc->s[p] == '\t')) { ind += sc->s[p] == '\t' ? 4u : 1u; p++; }
        const u8 *s = sc->s + p; u32 n = le - p;
        trim(&s, &n);
        if (!n || (n >= 2 && s[0] == '%' && s[1] == '%') || dg_starts(s, n, "::icon(")) continue;
        if (nmn >= DGM_MAX_ITEMS) { g_trunc = true; return false; }
        MNode *m = &mn[nmn];
        memset(m, 0, sizeof *m);
        while (sp > 0 && indent[sp - 1] >= ind) sp--;
        m->parent = nmn == 0 ? -1 : (sp ? stack[sp - 1] : 0);
        m->depth = m->parent < 0 ? 0 : mn[m->parent].depth + 1;
        mind_node(m, s, n);
        if (sp < 64) { stack[sp] = (i32)nmn; indent[sp] = ind; sp++; }
        nmn++;
    }
    return nmn > 0;
}

static f32 mind_vgap(i32 depth) { return g_ch * (depth <= 1 ? 0.9f : 0.45f) * (0.5f + 0.5f * g_gap); }

/* Height a node's subtree takes, its children stacked. */
static f32 mind_measure(u32 v) {
    f32 sum = 0;
    u32 kids = 0;
    for (u32 c = v + 1; c < nmn; c++) if (mn[c].parent == (i32)v) { sum += mind_measure(c); kids++; }
    if (kids > 1) sum += mind_vgap(mn[v].depth + 1) * (f32)(kids - 1);
    mn[v].sub = fmaxf(mn[v].h, sum);
    return mn[v].sub;
}

/* Place v's children on `side`, centred on v. */
static void mind_place(u32 v, i32 side) {
    f32 hgap = g_cw * (mn[v].depth == 0 ? 5.0f : 3.0f) * (0.6f + 0.4f * g_gap);
    f32 sum = 0;
    u32 kids = 0;
    for (u32 c = v + 1; c < nmn; c++)
        if (mn[c].parent == (i32)v && (mn[v].depth > 0 || mn[c].side == side)) { sum += mn[c].sub; kids++; }
    if (!kids) return;
    sum += mind_vgap(mn[v].depth + 1) * (f32)(kids - 1);
    f32 y = mn[v].y + mn[v].h * 0.5f - sum * 0.5f;
    for (u32 c = v + 1; c < nmn; c++) {
        if (mn[c].parent != (i32)v || (mn[v].depth == 0 && mn[c].side != side)) continue;
        mn[c].side = side;
        mn[c].x = side > 0 ? mn[v].x + mn[v].w + hgap : mn[v].x - hgap - mn[c].w;
        mn[c].y = y + (mn[c].sub - mn[c].h) * 0.5f;
        mind_place(c, side);
        y += mn[c].sub + mind_vgap(mn[v].depth + 1);
    }
}

static void mindmap_emit(void) {
    f32 t = dg_stroke_w();
    for (u32 i = 0; i < nmn; i++) {
        MNode *m = &mn[i];
        f32 pw = g_cw * (m->depth == 0 ? 3.0f : m->depth == 1 ? 2.0f : 1.4f);
        f32 ph = g_ch * (m->depth == 0 ? 1.4f : m->depth == 1 ? 0.8f : 0.5f);
        m->w = text_w(m->label) + pw;
        m->h = text_h(m->label) + ph;
        if (m->shape == 2) { f32 d = fmaxf(m->w, m->h); m->w = m->h = d; }
        /* each first-level child starts a branch, and its colour */
        if (m->depth == 1) m->branch = (i32)i;
        else if (m->depth > 1) m->branch = mn[m->parent].branch;
    }
    for (u32 i = 0; i < nmn; i++) if (mn[i].depth == 1) mind_measure(i);
    /* the root's children shared out: each to the lighter side so far */
    f32 heavy[2] = { 0, 0 };
    for (u32 i = 1; i < nmn; i++) {
        if (mn[i].depth != 1) continue;
        i32 s = heavy[0] <= heavy[1] ? 1 : -1;
        mn[i].side = s;
        heavy[s > 0 ? 0 : 1] += mn[i].sub + mind_vgap(1);
    }
    mn[0].x = 0; mn[0].y = 0;
    mind_place(0, 1);
    mind_place(0, -1);
    u32 bno[DGM_MAX_ITEMS] = { 0 };
    u32 nb = 0;
    for (u32 i = 0; i < nmn; i++) if (mn[i].depth == 1) bno[i] = nb++;
    for (u32 i = 1; i < nmn; i++) {
        const MNode *m = &mn[i], *p = &mn[m->parent];
        u32 b = bno[m->branch];
        f32 ax = m->side > 0 ? p->x + p->w : p->x, ay = p->y + p->h * 0.5f;
        f32 bx = m->side > 0 ? m->x : m->x + m->w, by = m->y + m->h * 0.5f;
        f32 mx = (ax + bx) * 0.5f;
        f32 qx[4] = { ax, mx, mx, bx }, qy[4] = { ay, ay, by, by };
        static f32 px[64], py[64];
        u32 n = 0;
        px[n] = ax; py[n] = ay; n++;
        n = dg_bezier(qx[0], qy[0], qx[1], qy[1], qx[2], qy[2], qx[3], qy[3], px, py, n, 64);
        em_poly(P_EDGE, px, py, n, false, t * (m->depth == 1 ? 2.4f : 1.5f), vivid(b));
    }
    for (u32 i = 0; i < nmn; i++) {
        const MNode *m = &mn[i];
        u32 b = m->depth ? bno[m->branch] : 0;
        f32 r = m->shape == 2 ? m->w * 0.5f : m->shape == 1 ? g_ch * 0.15f : m->shape == 3 ? m->h * 0.5f : g_ch * 0.45f;
        if (m->depth == 0) em_box(P_NODE, m->x, m->y, m->w, m->h, r, t * 1.5f, DI_ACTIVATION, DI_ACCENT);
        else if (m->depth == 1) em_box(P_NODE, m->x, m->y, m->w, m->h, r, t, soft(b), vivid(b));
        else em_box(P_NODE, m->x, m->y, m->w, m->h, r, 0, DI_CHIP, DI_NONE);
        em_text_box(m->label, m->x, m->y, m->w, m->h, DI_TEXT);
    }
}

/* =========================================================================
 * gitGraph
 * ========================================================================= */

enum { GC_NORMAL = 0, GC_REVERSE, GC_HIGHLIGHT };

typedef struct { char name[DG_ID_CAP]; i32 head, order, made, lane; } GBranch;
typedef struct { char id[DG_ID_CAP]; char tag[DG_ID_CAP]; i32 branch, p0, p1; u8 type; bool merge, cherry; } GCommit;

static GBranch gb[DGM_MAX_GROUPS];
static u32     ngb;
static GCommit gc[DGM_MAX_ITEMS];
static u32     ngc;
static bool    git_vertical;

/* The quoted value of `key:` on the line, or "". */
static void git_attr(const u8 *s, u32 n, const char *key, char *out, u32 cap) {
    out[0] = 0;
    u32 kl = (u32)strlen(key);
    for (u32 i = 0; i + kl < n; i++) {
        if (memcmp(s + i, key, kl) != 0 || (i > 0 && dg_is_word(s[i - 1]))) continue;
        u32 p = i + kl;
        while (p < n && dg_is_space(s[p])) p++;
        if (p < n && s[p] == ':') p++;
        while (p < n && dg_is_space(s[p])) p++;
        if (p < n && s[p] == '"') {
            u32 e = p + 1;
            while (e < n && s[e] != '"') e++;
            dg_set_label(out, cap, s + p + 1, e - p - 1);
        } else {
            u32 e = p;
            while (e < n && !dg_is_space(s[e])) e++;
            dg_strcpy(out, cap, s + p, e - p);
        }
        return;
    }
}

static i32 git_branch(const u8 *s, u32 n, bool make) {
    trim(&s, &n);
    u32 e = 0;
    while (e < n && !dg_is_space(s[e])) e++;
    unquote(&s, &e);
    for (u32 i = 0; i < ngb; i++) if (strlen(gb[i].name) == e && !memcmp(gb[i].name, s, e)) return (i32)i;
    if (!make || ngb >= DGM_MAX_GROUPS) return -1;
    GBranch *b = &gb[ngb];
    memset(b, 0, sizeof *b);
    dg_strcpy(b->name, sizeof b->name, s, e);
    b->head = -1;
    b->order = (i32)ngb;
    b->made = (i32)ngc;
    return (i32)ngb++;
}

static GCommit *git_commit(i32 branch) {
    if (ngc >= DGM_MAX_ITEMS) { g_trunc = true; return NULL; }
    GCommit *c = &gc[ngc];
    memset(c, 0, sizeof *c);
    c->branch = branch;
    c->p0 = gb[branch].head;
    c->p1 = -1;
    gb[branch].head = (i32)ngc++;
    return c;
}

static i32 git_find(const char *id) {
    for (u32 i = 0; i < ngc; i++) if (gc[i].id[0] && !strcmp(gc[i].id, id)) return (i32)i;
    return -1;
}

static bool gitgraph_parse(DScan *sc, const u8 *head, u32 hlen) {
    for (u32 i = 0; i + 2 <= hlen; i++)
        if (!memcmp(head + i, "TB", 2) || !memcmp(head + i, "BT", 2)) git_vertical = true;
    ngb = 0;
    i32 cur = git_branch((const u8 *)"main", 4, true);
    const u8 *line; u32 ll;
    char type[24], order[16];
    while (dg_next_line(sc, &line, &ll)) {
        if (dg_kw(line, ll, "commit")) {
            GCommit *c = git_commit(cur);
            if (!c) return false;
            git_attr(line, ll, "id", c->id, sizeof c->id);
            git_attr(line, ll, "tag", c->tag, sizeof c->tag);
            git_attr(line, ll, "type", type, sizeof type);
            c->type = !strcmp(type, "REVERSE") ? GC_REVERSE : !strcmp(type, "HIGHLIGHT") ? GC_HIGHLIGHT : GC_NORMAL;
            continue;
        }
        if (dg_kw(line, ll, "branch")) {
            i32 b = git_branch(line + 6, ll - 6, true);
            if (b < 0) { g_trunc = true; return false; }
            gb[b].head = gb[cur].head;
            git_attr(line, ll, "order", order, sizeof order);
            if (order[0]) gb[b].order = atoi(order);
            gb[b].made = (i32)ngc;
            cur = b;
            continue;
        }
        if (dg_kw(line, ll, "checkout") || dg_kw(line, ll, "switch")) {
            u32 k = dg_kw(line, ll, "checkout") ? 8u : 6u;
            i32 b = git_branch(line + k, ll - k, true);
            if (b >= 0) cur = b;
            continue;
        }
        if (dg_kw(line, ll, "merge")) {
            i32 b = git_branch(line + 5, ll - 5, false);
            GCommit *c = git_commit(cur);
            if (!c) return false;
            c->merge = true;
            c->p1 = b >= 0 ? gb[b].head : -1;
            git_attr(line, ll, "id", c->id, sizeof c->id);
            git_attr(line, ll, "tag", c->tag, sizeof c->tag);
            continue;
        }
        if (dg_kw(line, ll, "cherry-pick")) {
            char src[DG_ID_CAP];
            git_attr(line, ll, "id", src, sizeof src);
            GCommit *c = git_commit(cur);
            if (!c) return false;
            c->cherry = true;
            c->p1 = git_find(src);
            snprintf(c->tag, sizeof c->tag, "cherry-pick: %.28s", src);
            continue;
        }
    }
    return ngc > 0;
}

/* Lanes from the branches' order, then where each was made; a branch with no
 * commit of its own takes no lane. */
static void git_lanes(void) {
    i32 idx[DGM_MAX_GROUPS];
    u32 n = 0;
    for (u32 b = 0; b < ngb; b++) {
        bool used = false;
        for (u32 c = 0; c < ngc && !used; c++) used = gc[c].branch == (i32)b;
        gb[b].lane = -1;
        if (used) idx[n++] = (i32)b;
    }
    for (u32 i = 1; i < n; i++) {
        i32 v = idx[i]; u32 j = i;
        while (j > 0 && (gb[idx[j - 1]].order > gb[v].order ||
                         (gb[idx[j - 1]].order == gb[v].order && gb[idx[j - 1]].made > gb[v].made))) {
            idx[j] = idx[j - 1]; j--;
        }
        idx[j] = v;
    }
    for (u32 i = 0; i < n; i++) gb[idx[i]].lane = (i32)i;
}

static void git_xy(f32 along, f32 lane, f32 *x, f32 *y) {
    if (git_vertical) { *x = lane; *y = along; } else { *x = along; *y = lane; }
}

static void git_link(i32 from, i32 to, f32 step, f32 lane_h, f32 a0, f32 l0, u8 ink, bool dashed) {
    const GCommit *f = &gc[from], *c = &gc[to];
    f32 fa = a0 + (f32)from * step, ca = a0 + (f32)to * step;
    f32 fl = l0 + (f32)gb[f->branch].lane * lane_h, cl = l0 + (f32)gb[c->branch].lane * lane_h;
    f32 qx[4], qy[4];
    if (fl == cl) {
        git_xy(fa, fl, &qx[0], &qy[0]);
        git_xy(ca, cl, &qx[1], &qy[1]);
        if (dashed) em_dashed(P_EDGE, qx, qy, 2, dg_stroke_w() * 1.6f, g_ch * 0.3f, g_ch * 0.2f, ink);
        else em_seg(P_EDGE, qx[0], qy[0], qx[1], qy[1], dg_stroke_w() * 2.2f, ink);
        return;
    }
    /* off one lane onto another: an S that leaves and arrives along the lanes */
    f32 ma = fa + fminf(step, ca - fa) * 0.5f;
    git_xy(fa, fl, &qx[0], &qy[0]);
    git_xy(ma, fl, &qx[1], &qy[1]);
    git_xy(ma, cl, &qx[2], &qy[2]);
    git_xy(ca, cl, &qx[3], &qy[3]);
    static f32 px[64], py[64];
    u32 n = 0;
    px[n] = qx[0]; py[n] = qy[0]; n++;
    n = dg_bezier(qx[0], qy[0], qx[1], qy[1], qx[2], qy[2], qx[3], qy[3], px, py, n, 64);
    if (dashed) em_dashed(P_EDGE, px, py, n, dg_stroke_w() * 1.6f, g_ch * 0.3f, g_ch * 0.2f, ink);
    else em_poly(P_EDGE, px, py, n, false, dg_stroke_w() * 2.2f, ink);
}

static void gitgraph_emit(void) {
    git_lanes();
    f32 t = dg_stroke_w();
    f32 name_w = 0, lab_w = 0;
    u32 lanes = 0;
    for (u32 b = 0; b < ngb; b++) if (gb[b].lane >= 0) { name_w = fmaxf(name_w, text_w(gb[b].name)); lanes++; }
    for (u32 c = 0; c < ngc; c++) {
        lab_w = fmaxf(lab_w, text_w(gc[c].id));
        lab_w = fmaxf(lab_w, text_w(gc[c].tag) + g_cw);
    }
    f32 step = git_vertical ? g_ch * (2.2f + 0.6f * g_gap)
                            : fmaxf(g_cw * 5.0f, fminf(lab_w + g_cw * 1.2f, g_cw * 16.0f));
    f32 lane_h = git_vertical ? fmaxf(name_w + g_cw * 2.0f, g_cw * 12.0f) : g_ch * (2.6f + 0.8f * g_gap);
    f32 top = title_h();
    f32 a0 = git_vertical ? top + g_ch * 2.4f : name_w + g_cw * 4.0f;
    f32 l0 = git_vertical ? lane_h * 0.5f : top + g_ch * 1.8f;
    f32 r = g_ch * 0.5f;
    /* branch names at the start of their lanes, and the lanes */
    for (u32 b = 0; b < ngb; b++) {
        if (gb[b].lane < 0) continue;
        f32 ln = l0 + (f32)gb[b].lane * lane_h;
        f32 last = 0;
        for (u32 c = 0; c < ngc; c++) if (gc[c].branch == (i32)b) last = (f32)c;
        f32 x0, y0, x1, y1;
        git_xy(a0 - step * 0.4f, ln, &x0, &y0);
        git_xy(a0 + last * step, ln, &x1, &y1);
        em_seg(P_BACK, x0, y0, x1, y1, fmaxf(1.0f, t * 0.8f), DI_GROUP_STROKE);
        f32 w = text_w(gb[b].name) + g_cw * 1.4f, h = g_ch * 1.3f;
        f32 bx, by;
        if (git_vertical) { bx = ln - w * 0.5f; by = top; }
        else { bx = g_cw * 0.5f; by = ln - h * 0.5f; }
        em_box(P_NODE, bx, by, w, h, g_ch * 0.25f, t, soft((u32)gb[b].lane), vivid((u32)gb[b].lane));
        em_text_box(gb[b].name, bx, by, w, h, DI_TEXT);
    }
    for (u32 c = 0; c < ngc; c++) {
        const GCommit *k = &gc[c];
        u8 ink = vivid((u32)gb[k->branch].lane);
        if (k->p0 >= 0) git_link(k->p0, (i32)c, step, lane_h, a0, l0, ink, false);
        if (k->p1 >= 0) git_link(k->p1, (i32)c, step, lane_h, a0, l0, vivid((u32)gb[gc[k->p1].branch].lane), k->cherry);
    }
    for (u32 c = 0; c < ngc; c++) {
        const GCommit *k = &gc[c];
        u32 lane = (u32)gb[k->branch].lane;
        f32 x, y;
        git_xy(a0 + (f32)c * step, l0 + (f32)lane * lane_h, &x, &y);
        if (k->type == GC_HIGHLIGHT) em_box(P_NODE, x - r, y - r, r * 2, r * 2, g_ch * 0.1f, t, vivid(lane), DI_TEXT);
        else if (k->merge) {
            dot(x, y, r, DI_SURFACE, vivid(lane), t * 2.4f);
            dot(x, y, r * 0.45f, vivid(lane), DI_NONE, 0);
        } else dot(x, y, r, vivid(lane), DI_NONE, 0);
        if (k->type == GC_REVERSE) {
            em_seg(P_TOP, x - r * 0.5f, y - r * 0.5f, x + r * 0.5f, y + r * 0.5f, t * 1.5f, DI_SURFACE);
            em_seg(P_TOP, x - r * 0.5f, y + r * 0.5f, x + r * 0.5f, y - r * 0.5f, t * 1.5f, DI_SURFACE);
        }
        if (k->id[0]) {
            f32 w = text_w(k->id);
            f32 tx = git_vertical ? x + r * 1.6f : x - w * 0.5f;
            f32 ty = git_vertical ? y - g_ch * 0.5f : y + r * 1.4f;
            em_box(P_NODE, tx - g_cw * 0.35f, ty, w + g_cw * 0.7f, g_ch, g_ch * 0.2f, 0, DI_SURFACE, DI_NONE);
            em_text(tx, ty, k->id, (u32)strlen(k->id), DI_TEXT_DIM);
        }
        if (k->tag[0]) {
            f32 w = text_w(k->tag) + g_cw, h = g_ch * 1.15f;
            f32 bx = git_vertical ? x - r * 1.6f - w : x - w * 0.5f;
            f32 by = git_vertical ? y - h * 0.5f : y - r * 1.5f - h;
            em_box(P_NODE, bx, by, w, h, g_ch * 0.2f, t, DI_CHIP, DI_GROUP_STROKE);
            em_text_box(k->tag, bx, by, w, h, DI_TEXT);
        }
    }
    f32 extent = a0 + (f32)(ngc ? ngc - 1 : 0) * step;
    if (git_vertical) emit_title(0, l0 + (f32)(lanes ? lanes - 1 : 0) * lane_h + lane_h * 0.5f);
    else emit_title(0, extent);
}

/* =========================================================================
 * the entry points
 * ========================================================================= */

void dgm_reset(void) {
    ngt = ngsec = 0;
    ntp = ntev = ntsec = 0;
    njt = njsec = njact = 0;
    nxs = nxcat = 0;
    x_numeric = y_given = xy_horizontal = false;
    x_lo = x_hi = y_lo = y_hi = 0;
    x_title[0] = y_title[0] = 0;
    nqp = 0;
    for (u32 i = 0; i < 4; i++) q_label[i][0] = 0;
    qx_lo[0] = qx_hi[0] = qy_lo[0] = qy_hi[0] = 0;
    nmn = 0;
    ngb = ngc = 0;
    git_vertical = false;
}

bool dgm_parse(MdDiagramKind kind, DScan *sc, const u8 *head, u32 hlen) {
    switch (kind) {
    case MD_DIAGRAM_GANTT:    return gantt_parse(sc);
    case MD_DIAGRAM_TIMELINE: return timeline_parse(sc);
    case MD_DIAGRAM_JOURNEY:  return journey_parse(sc);
    case MD_DIAGRAM_XYCHART:  return xychart_parse(sc, head, hlen);
    case MD_DIAGRAM_QUADRANT: return quadrant_parse(sc);
    case MD_DIAGRAM_MINDMAP:  return mindmap_parse(sc);
    case MD_DIAGRAM_GITGRAPH: return gitgraph_parse(sc, head, hlen);
    default:                  return false;
    }
}

void dgm_emit(MdDiagramKind kind) {
    switch (kind) {
    case MD_DIAGRAM_GANTT:    gantt_emit(); break;
    case MD_DIAGRAM_TIMELINE: timeline_emit(); break;
    case MD_DIAGRAM_JOURNEY:  journey_emit(); break;
    case MD_DIAGRAM_XYCHART:  xychart_emit(); break;
    case MD_DIAGRAM_QUADRANT: quadrant_emit(); break;
    case MD_DIAGRAM_MINDMAP:  mindmap_emit(); break;
    case MD_DIAGRAM_GITGRAPH: gitgraph_emit(); break;
    default: break;
    }
}
