/*
 * Liu — what a paste did, item by item, and the one line that says so.
 *
 * A single bool for the whole selection, plus a count of links the copy would
 * not follow, cannot tell these apart:
 *
 *   everything copied, except two links on purpose     (the copy is fine)
 *   one item copied minus a link, another failed       (one item is MISSING)
 *
 * Both would read "Pasted, 2 links not followed", and the second lost a file.
 *
 * So every item reports one of three outcomes (fb_paste_item_report), the
 * loop adds them up here, and the line is written from the sums — a failure
 * is named, with the first item that failed and why, whatever else went well.
 *
 * Pure: fileops.c fills a report and the paste worker keeps a tally.
 */
#ifndef UI_PASTE_REPORT_H
#define UI_PASTE_REPORT_H

#include "core/types.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    FB_PASTE_OK = 0,       /* the item is at the destination, all of it */
    FB_PASTE_LINKS,        /* it is there except entries left behind ON PURPOSE (a
                            * link to a folder, a dangling link, a device); a move
                            * therefore kept its source */
    FB_PASTE_FAILED,       /* it is not there: nothing was pasted for this item */
} FbPasteOutcome;

typedef struct {
    i32  total;            /* items the paste was asked for */
    i32  ok;               /* FB_PASTE_OK */
    i32  with_links;       /* FB_PASTE_LINKS */
    i32  failed;           /* FB_PASTE_FAILED */
    i32  links;            /* entries left behind on purpose, over all items */
    int  first_err;        /* errno of the first failure, 0 when not known */
    char first_failed[96]; /* its name */
} FbPasteTally;

static inline void fb_paste_tally_add(FbPasteTally *t, FbPasteOutcome outcome,
                                      i32 links, int err, const char *name) {
    if (!t) return;
    t->total++;
    if (links > 0) t->links += links;
    if (outcome == FB_PASTE_OK) { t->ok++; return; }
    if (outcome == FB_PASTE_LINKS) { t->with_links++; return; }
    if (t->failed++ == 0) {
        t->first_err = err;
        /* Cut on a character boundary: the name goes into a status line. */
        usize n = name ? strlen(name) : 0, keep = sizeof t->first_failed - 4;
        if (n > keep) {
            n = keep;
            while (n > 0 && ((unsigned char)name[n] & 0xC0) == 0x80) n--;
            snprintf(t->first_failed, sizeof t->first_failed, "%.*s\xe2\x80\xa6", (int)n, name);
        } else {
            snprintf(t->first_failed, sizeof t->first_failed, "%s", name ? name : "");
        }
    }
}

/* Did every item land whole? (What clears the clipboard after a cut.) */
static inline bool fb_paste_tally_clean(const FbPasteTally *t) {
    return t && t->total > 0 && t->ok == t->total;
}

/* The status line. `reason` is the caller's text for first_err (NULL or empty:
 * the item is named without one). `*is_error` (may be NULL) is true when any
 * item failed. */
static inline void fb_paste_tally_message(const FbPasteTally *t, bool is_cut,
                                          const char *reason, char *out, usize cap,
                                          bool *is_error) {
    if (is_error) *is_error = false;
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!t || t->total <= 0) return;
    const char *did = is_cut ? "Moved" : "Pasted";
    const char *plural = t->links == 1 ? "" : "s";

    if (t->failed == 0) {
        if (t->links == 0) { snprintf(out, cap, "%s", did); return; }
        /* Nothing failed; the walk left links alone on purpose. For a move
         * that means the source was therefore NOT deleted. */
        if (is_cut) snprintf(out, cap, "Copied, source kept: %d link%s not followed", t->links, plural);
        else        snprintf(out, cap, "Pasted, %d link%s not followed", t->links, plural);
        return;
    }

    if (is_error) *is_error = true;
    char why[160];
    if (reason && reason[0]) snprintf(why, sizeof why, "%s: %s", t->first_failed, reason);
    else                     snprintf(why, sizeof why, "%s", t->first_failed);
    i32 landed = t->ok + t->with_links;
    char tail[64] = "";
    if (t->links > 0) snprintf(tail, sizeof tail, ", %d link%s not followed", t->links, plural);
    if (landed == 0 && t->total == 1)
        snprintf(out, cap, "Not %s: %s", is_cut ? "moved" : "pasted", why);
    else if (landed == 0)
        snprintf(out, cap, "Not %s: all %d items failed (%s)", is_cut ? "moved" : "pasted",
                 t->total, why);
    else
        snprintf(out, cap, "%s %d of %d, %d failed (%s)%s", did, landed, t->total,
                 t->failed, why, tail);
}

#endif /* UI_PASTE_REPORT_H */
