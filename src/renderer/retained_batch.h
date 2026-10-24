/* Shared lifetime and row validation; GPU objects are owned by the backend. */
#ifndef LIU_RETAINED_BATCH_H
#define LIU_RETAINED_BATCH_H
#include "renderer/renderer.h"
#include "core/mem_budget.h"
#include "core/checked.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct RetainedStore RetainedStore;
typedef struct RetainedBatch RetainedBatch;
typedef struct {
    u64 version;
    usize offset;
    i32 count;
} RetainedRow;
typedef struct {
    void *buffer;
    u64 last_use_serial;
    usize capacity;
    u64 revision;
    u32 count;
    bool valid;
    RetainedRow *rows;
    i32 row_count;
    bool rows_valid;
} RetainedSlot;
struct RetainedBatch {
    RendererRetainedBatch api;
    _Atomic u32 refs;
    _Atomic bool retired;
    RetainedStore *owner;
    RetainedBatch *next;
    RetainedSlot slots[3];
    u32 current;
    u32 vao;
    bool glyphs;
    usize used_bytes;       /* newest successfully cached revision */
    f64 oversized_since;    /* maintenance clock; zero while adequately sized */
};
struct RetainedStore {
    RetainedBatch *head;
    RendererRetainedStats stats;
};

/* Keep short content/resize oscillations allocation-free. Maintenance may
 * discard oversized storage after five seconds; the next draw uploads the
 * still-owned CPU rows, even if their revision has not changed. */
#define RETAINED_SHRINK_DELAY_SEC 5.0
#define RETAINED_SHRINK_MIN_BYTES ((usize)64 * 1024)
static bool retained_slot_oversized(const RetainedBatch *b, const RetainedSlot *slot) {
    return slot->capacity && (!b->used_bytes ||
        (slot->capacity >= RETAINED_SHRINK_MIN_BYTES &&
         b->used_bytes <= slot->capacity / 4));
}
static bool retained_oversized(const RetainedBatch *b) {
    for (u32 i=0;i<3;i++) if (retained_slot_oversized(b,&b->slots[i])) return true;
    return false;
}
static void retained_note_size(RetainedBatch *b, usize bytes) {
    b->used_bytes=bytes;
    if (!retained_oversized(b)) b->oversized_since=0;
}
static bool retained_trim_due(RetainedBatch *b, f64 now) {
    if (!retained_oversized(b)) { b->oversized_since=0; return false; }
    if (b->oversized_since<=0 || now<b->oversized_since) {
        b->oversized_since=now;
        return false;
    }
    return now-b->oversized_since>=RETAINED_SHRINK_DELAY_SEC;
}

static void retained_unref(RetainedBatch *b) {
    if (atomic_fetch_sub_explicit(&b->refs, 1, memory_order_acq_rel) == 1) {
        mem_budget_transient_adjust(MEM_RENDER_CPU, -(isize)sizeof(*b));
        free(b);
    }
}
static void retained_release_ui(RendererRetainedBatch *api) {
    RetainedBatch *b = (RetainedBatch *)api;
    atomic_store_explicit(&b->retired, true, memory_order_release);
    retained_unref(b);
}
static RetainedStore *retained_store(Renderer *r) {
    if (!r->retained_store) {
        r->retained_store = calloc(1, sizeof(RetainedStore));
        if (r->retained_store)
            mem_budget_transient_adjust(MEM_RENDER_CPU, sizeof(RetainedStore));
    }
    return r->retained_store;
}
static RetainedBatch *retained_get(Renderer *r, RendererRetainedBatch **handle, bool glyphs) {
    static i32 enabled = -1;
    if (enabled < 0) {
        const char *value=getenv("LIU_RETAINED_BATCHES");
        enabled=!(value && value[0]=='0');
    }
    if (!enabled) return NULL;
    RetainedStore *s = retained_store(r);
    if (!s) return NULL;
    RetainedBatch *b = (RetainedBatch *)*handle;
    if (b && (b->owner != s || b->glyphs != glyphs)) {
        renderer_retained_release(handle);
        b = NULL;
    }
    if (!b) {
        b = calloc(1, sizeof(*b));
        if (!b) return NULL;
        b->api.release = retained_release_ui;
        atomic_init(&b->refs, 2); /* the UI handle and the renderer's list */
        atomic_init(&b->retired, false);
        b->owner = s; b->glyphs = glyphs; b->next = s->head; s->head = b;
        mem_budget_transient_adjust(MEM_RENDER_CPU, sizeof(*b));
        *handle = &b->api;
    }
    return b;
}
static const void *retained_row(const RendererRows *rows, i32 i) {
    return rows->glyphs ? (const void *)rows->data.glyph_rows[i]
                        : (const void *)rows->data.rect_rows[i];
}
static void retained_rows_free(RetainedSlot *slot) {
    if (slot->rows) {
        mem_budget_transient_adjust(MEM_RENDER_CPU,
                                   -(isize)((usize)slot->row_count*sizeof(RetainedRow)));
        free(slot->rows);
    }
    slot->rows=NULL;slot->row_count=0;slot->rows_valid=false;
}
#ifndef LIU_RETAINED_ROWS_ALLOC
#define LIU_RETAINED_ROWS_ALLOC(bytes) calloc(1,(bytes))
#endif
/* Metadata is only an optimization. Refusal keeps the full-copy path usable;
 * it must never turn an allocation failure into a skipped row update. */
static bool retained_rows_prepare(RetainedSlot *slot, const RendererRows *rows) {
    if (!rows->versions) { slot->rows_valid=false;return false; }
    if (slot->row_count==rows->rows && (slot->rows || !rows->rows)) return true;
    usize bytes;
    if (!checked_mul_usize((usize)rows->rows,sizeof(RetainedRow),&bytes)) return false;
    RetainedRow *next=bytes?LIU_RETAINED_ROWS_ALLOC(bytes):NULL;
    if (bytes && !next) { slot->rows_valid=false;return false; }
    retained_rows_free(slot);
    slot->rows=next;slot->row_count=rows->rows;
    if (bytes) mem_budget_transient_adjust(MEM_RENDER_CPU,(isize)bytes);
    return true;
}
static bool retained_size(const RendererRows *rows, u32 *count, usize *bytes) {
    if (!rows || rows->rows < 0 || !rows->counts) return false;
    usize n = 0;
    for (i32 i=0;i<rows->rows;i++) {
        if (rows->counts[i] < 0 || (rows->counts[i] && !retained_row(rows, i)) ||
            !checked_add_usize(n, (usize)rows->counts[i], &n) || n > 0x7fffffffu) return false;
    }
    *count = (u32)n;
    return checked_mul_usize(n, rows->glyphs ? sizeof(GlyphInstance) : sizeof(RectInstance), bytes);
}
static void retained_projection(const Renderer *r, f32 out[16]) {
    memcpy(out, r->projection, sizeof(f32)*16);
    for (i32 i=0;i<4;i++)
        out[12+i] += out[i]*r->draw_origin_x + out[4+i]*r->draw_origin_y;
}
static void retained_collect(Renderer *r, bool all, void (*destroy_gpu)(RetainedBatch *)) {
    RetainedStore *s = r->retained_store;
    if (!s) return;
    RetainedBatch **link = &s->head;
    while (*link) {
        RetainedBatch *b = *link;
        if (!all && !atomic_load_explicit(&b->retired, memory_order_acquire)) {
            link = &b->next; continue;
        }
        *link = b->next;
        destroy_gpu(b);
        b->owner = NULL; b->next = NULL;
        retained_unref(b);
    }
    if (all) {
        mem_budget_transient_adjust(MEM_RENDER_CPU, -(isize)sizeof(*s));
        free(s); r->retained_store = NULL;
    }
}
#endif
