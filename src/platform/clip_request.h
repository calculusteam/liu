/*
 * State for one X11 clipboard request. Each request owns its result and
 * matches a SelectionNotify by property or, for a refusal, by request time.
 * Only an OK request exposes text; empty, refused, timed-out, or cancelled
 * requests return an empty string. Request identity prevents a late reply from
 * being applied to a later paste.
 */
#ifndef PLATFORM_CLIP_REQUEST_H
#define PLATFORM_CLIP_REQUEST_H

#include "core/types.h"
#include <stdlib.h>
#include <string.h>

typedef enum {
    CLIP_REQ_IDLE = 0,     /* nothing asked yet */
    CLIP_REQ_PENDING,      /* asked, no answer yet */
    CLIP_REQ_OK,           /* text arrived (may be long, never empty) */
    CLIP_REQ_EMPTY,        /* the owner answered with nothing */
    CLIP_REQ_REFUSED,      /* the owner said no, or sent a form Liu does not take */
    CLIP_REQ_TIMEOUT,      /* nobody answered in time */
    CLIP_REQ_CANCELLED,    /* the window went away, or a newer request replaced it */
} ClipReqState;

#define CLIP_REQ_SLOTS 8   /* properties the platform rotates through */

typedef struct {
    ClipReqState  state;
    u32           generation;   /* bumped by every begin */
    unsigned long property;     /* the property THIS request asked for */
    unsigned long time;         /* the server time it was made with (0 = CurrentTime) */
    char         *text;         /* this request's result; NULL unless OK */
    usize         len;
    u32           stale_replies;   /* answers that belonged to no live request */
} ClipRequest;

/* Which rotating slot a generation uses. */
static inline u32 clip_request_slot(u32 generation) { return generation % CLIP_REQ_SLOTS; }

static inline void clip_request_free_text(ClipRequest *r) {
    free(r->text);
    r->text = NULL;
    r->len = 0;
}

/* Start a new request. Whatever the previous one held is gone — its text is
 * never the answer to this one — and if it was still pending it is cancelled.
 * Returns the new generation. */
static inline u32 clip_request_begin(ClipRequest *r, unsigned long property, unsigned long time) {
    clip_request_free_text(r);
    r->generation++;
    r->state = CLIP_REQ_PENDING;
    r->property = property;
    r->time = time;
    return r->generation;
}

/* Is a SelectionNotify the answer to the pending request?
 *   ours        the event's requestor is this window and its selection is the
 *               one that was asked about
 *   property    the event's property (0 = None, a refusal)
 *   time        the event's time
 * A reply naming a property is matched by the property. A refusal names none,
 * so it is matched by the request's time — and when the request was made with
 * no usable time (0), a refusal is accepted only while the request is pending,
 * which is the most that can be known. */
static inline bool clip_request_accepts(const ClipRequest *r, bool ours,
                                        unsigned long property, unsigned long time) {
    if (!ours || r->state != CLIP_REQ_PENDING) return false;
    if (property != 0) return property == r->property;
    return r->time == 0 || time == 0 || time == r->time;
}

/* Record a reply that clip_request_accepts() said is ours. `bytes` NULL with
 * `refused` false means the owner answered with nothing. */
static inline void clip_request_complete(ClipRequest *r, const void *bytes, usize n, bool refused) {
    if (r->state != CLIP_REQ_PENDING) return;
    clip_request_free_text(r);
    if (refused) { r->state = CLIP_REQ_REFUSED; return; }
    if (!bytes || n == 0) { r->state = CLIP_REQ_EMPTY; return; }
    r->text = (char *)malloc(n + 1);
    if (!r->text) { r->state = CLIP_REQ_REFUSED; return; }
    memcpy(r->text, bytes, n);
    r->text[n] = '\0';
    r->len = n;
    r->state = CLIP_REQ_OK;
}

/* A reply that is NOT ours (clip_request_accepts said no) but was addressed to
 * our window: counted, and the caller deletes the property it named. */
static inline void clip_request_stale(ClipRequest *r) { r->stale_replies++; }

static inline void clip_request_timeout(ClipRequest *r) {
    if (r->state == CLIP_REQ_PENDING) { clip_request_free_text(r); r->state = CLIP_REQ_TIMEOUT; }
}

static inline void clip_request_cancel(ClipRequest *r) {
    if (r->state == CLIP_REQ_PENDING) { clip_request_free_text(r); r->state = CLIP_REQ_CANCELLED; }
}

/* What the getter hands back: this request's text, or "" for every outcome
 * that is not OK. Never a previous request's text. */
static inline const char *clip_request_text(const ClipRequest *r) {
    return r->state == CLIP_REQ_OK && r->text ? r->text : "";
}

static inline void clip_request_reset(ClipRequest *r) {
    clip_request_free_text(r);
    r->state = CLIP_REQ_IDLE;
}

#endif /* PLATFORM_CLIP_REQUEST_H */
