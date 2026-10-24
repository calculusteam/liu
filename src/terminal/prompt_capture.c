/*
 * Liu — per-pane prompt recall. See prompt_capture.h for why the source is
 * liu's own input mirror rather than the agent's store or the screen.
 */
#include "terminal/prompt_capture.h"
#include "core/utf8.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ appending */

static bool pr_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* Append a whole unit or nothing. Splitting a codepoint at the cap is what
 * draws a replacement box at the end of a clipped prompt, so the cap is
 * enforced per sequence and never per byte. */
static bool pr_put(PromptRecall *pr, const char *b, i32 n) {
    if (n <= 0) return true;
    if (pr->len + n + 1 > (i32)sizeof pr->text) { pr->truncated = true; return false; }
    memcpy(pr->text + pr->len, b, (usize)n);
    pr->len += n;
    pr->text[pr->len] = '\0';
    return true;
}

void prompt_recall_clear(PromptRecall *pr) {
    if (!pr) return;
    pr->text[0]  = '\0';
    pr->len      = 0;
    pr->time     = 0.0;
    pr->open     = false;
    pr->truncated = false;
    prompt_recall_images_clear(pr);
}

static i32 g_imgs_live = 0;

i32 prompt_recall_imgs_live(void) { return g_imgs_live; }

void prompt_recall_images_clear(PromptRecall *pr) {
    if (!pr) return;
    if (pr->imgs) { free(pr->imgs); g_imgs_live--; }
    pr->imgs = NULL;
    pr->img_count = 0;
    pr->img_extra = 0;
}

const char *prompt_recall_img(const PromptRecall *pr, i32 k) {
    if (!pr || !pr->imgs || k < 0 || k >= pr->img_count) return NULL;
    return pr->imgs->path[k];
}

void prompt_recall_move(PromptRecall *dst, PromptRecall *src) {
    if (!dst || !src || dst == src) return;
    prompt_recall_images_clear(dst);
    *dst = *src;                      /* the block's ownership goes with it */
    src->imgs = NULL;
    src->img_count = 0;
    src->img_extra = 0;
    prompt_recall_clear(src);
}

bool prompt_recall_ready(const PromptRecall *pr) {
    return pr && !pr->open && (pr->len > 0 || pr->img_count > 0);
}

bool prompt_recall_attach(PromptRecall *pr, const char *path) {
    if (!pr || !path || !path[0]) return false;
    usize n = strlen(path);
    if (n + 1 > PROMPT_RECALL_IMG_PATH) return false;   /* longer than a slot */

    /* The same picture reaches here twice as a matter of course: a drop stages
     * the path, and for the CLIs that leave it as literal text the commit scan
     * then finds it again in the prompt. Silently ignoring the repeat is what
     * keeps the card from showing one screenshot four times. */
    for (i32 i = 0; i < pr->img_count; i++)
        if (strcmp(pr->imgs->path[i], path) == 0) return false;

    if (pr->img_count >= PROMPT_RECALL_MAX_IMGS) { pr->img_extra++; return false; }
    if (!pr->imgs) {
        pr->imgs = calloc(1, sizeof *pr->imgs);
        if (!pr->imgs) { pr->img_extra++; return false; }   /* counted, not lost */
        g_imgs_live++;
    }
    memcpy(pr->imgs->path[pr->img_count], path, n + 1);
    pr->img_count++;
    return true;
}

void prompt_recall_line(PromptRecall *pr, const char *line,
                        bool continuation, f64 now) {
    if (!pr) return;

    if (!pr->open) {
        /* A committed prompt is replaced wholesale by the next one. */
        pr->text[0]   = '\0';
        pr->len       = 0;
        pr->truncated = false;
    } else if (pr->len > 0) {
        pr_put(pr, "\n", 1);
    }

    /* Fold the raw mirror in, one codepoint at a time.
     *
     * CR and LF both mean "line break" here — a pasted prompt arrives with
     * whichever the source used, and CRLF must not become two. Tabs flatten to
     * a space: the panel is not a text editor and a tab in a prompt is nearly
     * always an artefact of the paste. Every other control byte is dropped
     * outright, which matters most for CR: to an agent CLI a stray CR in
     * injected text is not whitespace, it is SUBMIT (see app_voice_clean_text),
     * so one reaching a recall buffer means it very nearly reached the pane. */
    const i32 line_n = line ? (i32)strlen(line) : 0;
    for (i32 i = 0; i < line_n; ) {
        unsigned char c = (unsigned char)line[i];
        if (c == '\r') {
            if (line[i + 1] == '\n') i++;          /* CRLF is one break */
            pr_put(pr, "\n", 1);
            i++;
            continue;
        }
        if (c == '\n') { pr_put(pr, "\n", 1); i++; continue; }
        if (c == '\t') { pr_put(pr, " ", 1);  i++; continue; }
        if (c < 0x20 || c == 0x7f) { i++; continue; }
        if (c < 0x80) {
            if (!pr_put(pr, line + i, 1)) break;
            i++;
            continue;
        }
        u32 cp = 0;
        u32 adv = utf8_decode((const u8 *)line + i, (usize)(line_n - i), &cp);
        if (adv == 0) { i++; continue; }           /* malformed: drop the byte */
        if (!pr_put(pr, line + i, (i32)adv)) break;
        i += (i32)adv;
    }

    pr->open = continuation;
    pr->time = now;

    if (!pr->open) {
        while (pr->len > 0 && pr_is_space(pr->text[pr->len - 1])) pr->len--;
        pr->text[pr->len] = '\0';
    }
}

bool prompt_recall_close(PromptRecall *pr) {
    if (!pr || !pr->open) return false;
    pr->open = false;
    while (pr->len > 0 && pr_is_space(pr->text[pr->len - 1])) pr->len--;
    pr->text[pr->len] = '\0';
    return true;
}

/* -------------------------------------------------------------- summary */

i32 prompt_recall_summary(const PromptRecall *pr, char *out, usize cap,
                          i32 max_cols) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!pr || pr->len <= 0 || max_cols < 1) return 0;

    i32 i = 0;
    while (i < pr->len && pr_is_space(pr->text[i])) i++;   /* leading blanks */

    i32 n = 0;          /* bytes written */
    i32 w = 0;          /* columns written */
    i32 safe_n = 0;     /* bytes at the last point that leaves room for "…" */
    bool pending_space = false;
    bool clipped = false;

    for (; i < pr->len; ) {
        if (pr_is_space(pr->text[i])) {
            /* Runs of whitespace — including the line breaks of a multi-line
             * prompt — collapse to one space: the badge is one line. */
            while (i < pr->len && pr_is_space(pr->text[i])) i++;
            if (i < pr->len) pending_space = true;
            continue;
        }
        u32 cp = 0;
        i32 gc = 0;
        u32 adv = utf8_next_grapheme((const u8 *)pr->text + i,
                                     (usize)(pr->len - i), &cp, &gc);
        if (adv == 0) { adv = 1; gc = 1; }
        if (gc < 0) gc = 0;

        i32 need_w = gc + (pending_space ? 1 : 0);
        i32 need_n = (i32)adv + (pending_space ? 1 : 0);
        if (w + need_w > max_cols || n + need_n + 1 > (i32)cap) {
            clipped = true;
            break;
        }
        if (pending_space) { out[n++] = ' '; w += 1; pending_space = false; }
        memcpy(out + n, pr->text + i, adv);
        n += (i32)adv;
        w += gc;
        i += (i32)adv;
        /* Everything up to here still leaves a column for the ellipsis. */
        if (w <= max_cols - 1 && n + 3 + 1 <= (i32)cap) safe_n = n;
    }
    if (!clipped && pr->truncated) clipped = true;

    if (clipped) {
        n = safe_n;
        if (n + 3 + 1 <= (i32)cap) { memcpy(out + n, "\xE2\x80\xA6", 3); n += 3; }
    }
    out[n] = '\0';
    return n;
}

/* ----------------------------------------------------------------- wrap */

i32 prompt_recall_wrap(const PromptRecall *pr, i32 cols, PromptLine *out,
                       i32 max_lines) {
    if (!pr || !out || max_lines <= 0 || cols <= 0 || pr->len <= 0) return 0;

    i32 n = 0;
    i32 i = 0;
    const i32 end = pr->len;

    while (i < end && n < max_lines) {
        const i32 start = i;
        i32 w = 0;
        i32 j = i;
        i32 break_at = -1;      /* byte index of the last space on this line */
        bool hard = false;

        while (j < end) {
            if (pr->text[j] == '\n') { hard = true; break; }
            u32 cp = 0;
            i32 gc = 0;
            u32 adv = utf8_next_grapheme((const u8 *)pr->text + j,
                                         (usize)(end - j), &cp, &gc);
            if (adv == 0) { adv = 1; gc = 1; }
            if (gc < 0) gc = 0;
            if (w + gc > cols) break;
            if (pr->text[j] == ' ' && j > start) break_at = j;
            w += gc;
            j += (i32)adv;
        }

        /* Nothing fit at all: one grapheme is wider than the whole box.
         *
         * Without this the loop makes NO progress — `i = j = start` — and
         * spends its whole line budget emitting empty descriptors: at cols=1,
         * "👍 ok" would come out as eight blank lines instead of one line
         * holding the emoji. Overflowing the box by a column is the standard
         * answer; reporting a line the caller then draws as nothing is not.
         */
        if (!hard && j == start && j < end) {
            u32 cp = 0;
            i32 gc = 0;
            u32 adv = utf8_next_grapheme((const u8 *)pr->text + j,
                                         (usize)(end - j), &cp, &gc);
            j += (i32)(adv ? adv : 1);
        }

        i32 line_end;
        if (hard || j >= end) {
            line_end = j;
            i = hard ? j + 1 : end;
        } else if (break_at > start) {
            line_end = break_at;               /* break at the space… */
            i = break_at;
            while (i < end && pr->text[i] == ' ') i++;   /* …and swallow it */
        } else {
            line_end = j;                      /* one word longer than the box */
            i = j;
            /* Swallow the separator here too, exactly as the break_at branch
             * does. Leaving it made the next line OPEN with a space, which
             * costs a display column: at cols=2 "👍 ok" wrapped to
             * ["👍"][" o"]["k"] — three lines where two fit, because the
             * space ate the first column of line 2. A soft break never wants
             * its separator; only a hard '\n' preserves indentation, and that
             * branch is above. */
            while (i < end && pr->text[i] == ' ') i++;
        }

        while (line_end > start && pr->text[line_end - 1] == ' ') line_end--;
        out[n].off = start;
        out[n].len = line_end - start;
        n++;
    }
    return n;
}
