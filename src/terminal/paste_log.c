/*
 * Liu — per-pane paste memory and placeholder expansion. See paste_log.h for
 * the placeholder shapes each agent CLI draws and why each number is checked
 * the way it is.
 */
#include "terminal/paste_log.h"
#include "terminal/terminal.h"
#include "core/mem_budget.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *text;
    usize len;
    u32   breaks;     /* line breaks: \r\n, lone \r and \n each count once  */
    usize chars;      /* code points, a line break counted as one            */
    usize chars_raw;  /* code points of the bytes as received (\r\n is two)  */
} PasteEntry;

struct PasteLog {
    PasteEntry e[PASTE_LOG_MAX_ENTRIES];   /* chronological, oldest first */
    i32   count;
    usize bytes;
};

PasteLog *paste_log_create(void) {
    return calloc(1, sizeof(PasteLog));
}

static void entry_drop(PasteLog *log, i32 i) {
    log->bytes -= log->e[i].len + 1;
    mem_budget_adjust(MEM_TERMINAL, -(isize)(log->e[i].len + 1));
    free(log->e[i].text);
    memmove(&log->e[i], &log->e[i + 1], (usize)(log->count - i - 1) * sizeof(PasteEntry));
    log->count--;
}

void paste_log_destroy(PasteLog *log) {
    if (!log) return;
    while (log->count > 0) entry_drop(log, log->count - 1);
    free(log);
}

i32   paste_log_count(const PasteLog *log) { return log ? log->count : 0; }
usize paste_log_bytes(const PasteLog *log) { return log ? log->bytes : 0; }

static void measure(PasteEntry *pe) {
    u32 br = 0; usize ch = 0, raw = 0;
    const u8 *s = (const u8 *)pe->text;
    for (usize i = 0; i < pe->len; i++) {
        u8 c = s[i];
        if ((c & 0xC0) == 0x80) continue;          /* continuation byte */
        raw++;
        if (c == '\r') {
            br++; ch++;
            if (i + 1 < pe->len && s[i + 1] == '\n') { i++; raw++; }
            continue;
        }
        if (c == '\n') br++;
        ch++;
    }
    pe->breaks = br; pe->chars = ch; pe->chars_raw = raw;
}

bool paste_log_add(PasteLog *log, const char *text, usize len) {
    if (!log || !text || len == 0 || len > PASTE_LOG_MAX_ONE) return false;
    PasteEntry pe = { .text = (char *)text, .len = len };
    measure(&pe);
    if (pe.breaks == 0 && pe.chars < PASTE_LOG_MIN_ONE_LINE) return false;
    char *copy = malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, text, len);
    copy[len] = '\0';
    pe.text = copy;
    while (log->count > 0 &&
           (log->count >= PASTE_LOG_MAX_ENTRIES || log->bytes + len + 1 > PASTE_LOG_MAX_BYTES))
        entry_drop(log, 0);
    log->e[log->count++] = pe;
    log->bytes += len + 1;
    mem_budget_adjust(MEM_TERMINAL, (isize)(len + 1));
    return true;
}

/* ---- placeholder recognition --------------------------------------------- */

typedef enum {
    PH_ONE_LINE,      /* [Pasted text #N]                                    */
    PH_LINES,         /* [Pasted text #N +L lines], [Pasted ~L lines], [… +LL] */
    PH_CHARS,         /* [Pasted text #N C chars], [Pasted Content C chars]  */
} PhKind;

typedef struct {
    usize  start, end;       /* byte span in the selection, end exclusive */
    PhKind kind;
    usize  n;                /* L or C */
    char   preview[64];      /* Command Code's head of the paste, or "" */
} Placeholder;

/* A placeholder never spans more than this many bytes of selection text —
 * the longest shape in paste_log.h is about 30, a soft wrap inside it adds
 * a newline and the agent's continuation indent. */
#define PH_MAX_SPAN 160

/* Copy the bracket's inside with every whitespace run (a wrap included)
 * folded to one space and the ends trimmed. False when there is no closing
 * bracket within reach. */
/* U+2500..U+259F — box drawing and block elements, the rails an agent draws
 * down the left of its prompt box (OpenCode's ┃). UTF-8 E2 94 xx .. E2 96 9F. */
static usize rail_len(const char *s, usize i, usize len) {
    if (i + 2 >= len || (u8)s[i] != 0xE2) return 0;
    u8 b1 = (u8)s[i + 1], b2 = (u8)s[i + 2];
    if (b1 == 0x94 || b1 == 0x95 || (b1 == 0x96 && b2 <= 0x9F)) return 3;
    return 0;
}

static bool ph_inner(const char *s, usize at, usize len, char *out, usize cap, usize *end) {
    usize o = 0;
    bool space = false;
    for (usize i = at + 1; i < len && i - at < PH_MAX_SPAN; i++) {
        char c = s[i];
        if (c == '\n') {
            /* A soft wrap: the next row starts with the agent's indent and
             * possibly its rail (OpenCode wraps "[Pasted ~12" / "┃
             * lines]"). Neither is part of the placeholder. */
            usize j = i + 1;
            for (;;) {
                usize r;
                if (j < len && (s[j] == ' ' || s[j] == '\t')) j++;
                else if ((r = rail_len(s, j, len)) > 0) j += r;
                else break;
            }
            space = o > 0;
            i = j - 1;
            continue;
        }
        if (c == ']') {
            while (o && out[o - 1] == ' ') o--;
            out[o] = '\0';
            *end = i + 1;
            return true;
        }
        if (c == '[') return false;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = o > 0; continue; }
        if (space) { if (o + 1 >= cap) return false; out[o++] = ' '; space = false; }
        if (o + 1 >= cap) return false;
        out[o++] = c;
    }
    return false;
}

/* Parse decimal digits at *p; false when there are none. */
static bool ph_num(const char **p, usize *out) {
    const char *q = *p;
    usize v = 0; i32 nd = 0;
    while (*q >= '0' && *q <= '9') {
        if (nd++ > 12) return false;
        v = v * 10 + (usize)(*q - '0');
        q++;
    }
    if (nd == 0) return false;
    *p = q; *out = v;
    return true;
}

static bool ph_lit(const char **p, const char *lit) {
    usize n = strlen(lit);
    if (strncmp(*p, lit, n) != 0) return false;
    *p += n;
    return true;
}

/* " lines" / " line" / " chars" / " char" at *p, then the end of the text. */
static bool ph_unit_end(const char *p, const char *unit) {
    if (*p != ' ') return false;
    p++;
    usize n = strlen(unit);
    if (strncmp(p, unit, n) != 0) return false;
    p += n;
    if (*p == 's') p++;
    return *p == '\0';
}

static bool ph_parse(const char *in, Placeholder *ph) {
    const char *p = in;
    ph->preview[0] = '\0';
    if (ph_lit(&p, "Pasted text #")) {
        usize num;
        if (!ph_num(&p, &num)) return false;
        if (*p == '\0') { ph->kind = PH_ONE_LINE; ph->n = 0; return true; }
        if (ph_lit(&p, " +")) {
            if (!ph_num(&p, &ph->n) || !ph_unit_end(p, "line")) return false;
            ph->kind = PH_LINES; return true;
        }
        if (*p == ' ') {
            p++;
            if (!ph_num(&p, &ph->n) || !ph_unit_end(p, "char")) return false;
            ph->kind = PH_CHARS; return true;
        }
        return false;
    }
    if (ph_lit(&p, "Pasted Content ")) {
        if (!ph_num(&p, &ph->n) || !ph_unit_end(p, "char")) return false;
        ph->kind = PH_CHARS; return true;
    }
    if (ph_lit(&p, "Pasted ~")) {
        if (!ph_num(&p, &ph->n) || !ph_unit_end(p, "line")) return false;
        ph->kind = PH_LINES; return true;
    }
    /* Command Code: "<head>… +<L>L" — U+2026 is E2 80 A6. */
    const char *ell = strstr(in, "\xE2\x80\xA6 +");
    if (ell && ell > in) {
        usize hl = (usize)(ell - in);
        if (hl >= sizeof ph->preview) return false;
        const char *q = ell + 5;
        if (!ph_num(&q, &ph->n) || strcmp(q, "L") != 0) return false;
        memcpy(ph->preview, in, hl);
        ph->preview[hl] = '\0';
        ph->kind = PH_LINES;
        return true;
    }
    return false;
}

/* Does the paste begin with `head`, comparing with whitespace folded the way
 * ph_inner folded the placeholder? Leading whitespace of the paste is skipped. */
static bool head_matches(const PasteEntry *pe, const char *head) {
    const char *t = pe->text, *end = pe->text + pe->len;
    while (t < end && (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r')) t++;
    for (const char *h = head; *h; ) {
        if (t >= end) return false;
        if (*h == ' ') {
            if (!(*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r')) return false;
            while (t < end && (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r')) t++;
            h++;
            continue;
        }
        if (*t != *h) return false;
        t++; h++;
    }
    return true;
}

static bool ph_fits(const Placeholder *ph, const PasteEntry *pe) {
    switch (ph->kind) {
    case PH_ONE_LINE:
        return pe->breaks == 0;
    case PH_LINES:
        if (!(ph->n == pe->breaks || ph->n == (usize)pe->breaks + 1)) return false;
        return ph->preview[0] ? head_matches(pe, ph->preview) : true;
    case PH_CHARS:
        return ph->n == pe->chars || ph->n == pe->chars_raw;
    }
    return false;
}

#define PH_MAX_PER_COPY 256

char *paste_log_expand(const PasteLog *log, const char *sel, i32 *n_expanded) {
    if (n_expanded) *n_expanded = 0;
    if (!log || log->count == 0 || !sel) return NULL;
    usize len = strlen(sel);

    Placeholder *ph = NULL;
    i32 nph = 0, cap = 0;
    for (usize i = 0; i < len; i++) {
        if (sel[i] != '[') continue;
        char inner[PH_MAX_SPAN];
        usize end = 0;
        if (!ph_inner(sel, i, len, inner, sizeof inner, &end)) continue;
        Placeholder cand = { .start = i, .end = end };
        if (!ph_parse(inner, &cand)) continue;
        if (nph == cap) {
            if (cap >= PH_MAX_PER_COPY) break;
            i32 ncap = cap ? cap * 2 : 8;
            Placeholder *grown = realloc(ph, (usize)ncap * sizeof *ph);
            if (!grown) break;
            ph = grown; cap = ncap;
        }
        ph[nph++] = cand;
        i = end - 1;
    }
    if (nph == 0) { free(ph); return NULL; }

    /* Last placeholder first, each taking the newest paste that fits and is
     * older than the one the placeholder after it took. */
    i32 *pick = malloc((usize)nph * sizeof *pick);
    if (!pick) { free(ph); return NULL; }
    i32 limit = log->count, hits = 0;
    usize out_len = len;
    for (i32 k = nph - 1; k >= 0; k--) {
        pick[k] = -1;
        for (i32 r = limit - 1; r >= 0; r--) {
            if (ph_fits(&ph[k], &log->e[r])) { pick[k] = r; limit = r; break; }
        }
        if (pick[k] >= 0) {
            hits++;
            out_len = out_len - (ph[k].end - ph[k].start) + log->e[pick[k]].len;
        }
    }
    char *out = NULL;
    if (hits > 0 && (out = malloc(out_len + 1))) {
        usize o = 0, from = 0;
        for (i32 k = 0; k < nph; k++) {
            if (pick[k] < 0) continue;
            memcpy(out + o, sel + from, ph[k].start - from);
            o += ph[k].start - from;
            const PasteEntry *pe = &log->e[pick[k]];
            memcpy(out + o, pe->text, pe->len);
            o += pe->len;
            from = ph[k].end;
        }
        memcpy(out + o, sel + from, len - from);
        o += len - from;
        out[o] = '\0';
        if (n_expanded) *n_expanded = hits;
    }
    free(pick);
    free(ph);
    return out;
}

/* ---- pane glue ------------------------------------------------------------ */

static void paste_log_free_opaque(void *p) { paste_log_destroy((PasteLog *)p); }

void paste_log_record(Terminal *t, const char *text, usize len) {
    if (!t || !text || len == 0) return;
    if (!t->paste_log) {
        t->paste_log = paste_log_create();
        if (!t->paste_log) return;
        t->paste_log_free = paste_log_free_opaque;
    }
    (void)paste_log_add((PasteLog *)t->paste_log, text, len);
}

char *paste_log_expand_terminal(const Terminal *t, const char *sel, i32 *n_expanded) {
    if (n_expanded) *n_expanded = 0;
    if (!t || !t->paste_log) return NULL;
    return paste_log_expand((const PasteLog *)t->paste_log, sel, n_expanded);
}
