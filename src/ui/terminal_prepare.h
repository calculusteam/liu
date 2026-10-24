/* Terminal geometry preparation: immutable cells, palette and glyph rectangles
 * enter the queue; only completed private row arrays come back. No UI pointer,
 * terminal pointer, font allocator or GPU context crosses this boundary. */
#ifndef LIU_TERMINAL_PREPARE_H
#define LIU_TERMINAL_PREPARE_H
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

/* The glyph lookup table starts small and doubles: a 6-row update of plain
 * text carries a few dozen distinct glyphs, so a fixed 2048-entry table
 * would be mostly empty in every job. Open addressing, power-of-two, grown
 * at half load; a table that cannot grow fails the capture, which falls back
 * to the synchronous rows as any other refusal. */
#define TERM_PREP_LOOKUPS_MIN 256u
#define TERM_PREP_LOOKUPS_MAX 65536u
static bool term_prepare_enabled(void) {
    const char *value=getenv("LIU_RENDER_PREPARE_THREAD");
#ifdef USE_METAL
    return !value || value[0]!='0';
#else
    return value && value[0]=='1';
#endif
}
typedef struct {
    u32 cp[4];
    FontGlyphRect rect;
    i32 width;
    u8 count;
    bool valid, got, pending, emoji;
} TermPrepLookup;
typedef struct TermPrepJob {
    struct TermPrepJob *next;
    _Atomic u32 refs;
    _Atomic bool done, cancelled;
    Terminal view;
    TermRenderCache output;
    TermRowStyle style;
    /* The captured rows, PACKED: `cells` holds `captured` rows of `cols`
     * cells, `slots[row]` maps a visible row to its packed index (-1 = not
     * captured) and `lengths[row]` is the view's per-row length. The view is
     * a sparse sync view over these (Terminal.sync_view_slots), so the
     * worker's row builder reads it through terminal_visible_row exactly as
     * it reads a full picture, and a 6-row update captures 6 rows, not the
     * whole pane. */
    Cell *cells;
    i32 *slots;
    i32 *lengths;
    u32 captured;
    TermPrepLookup *lookups;
    u32 lookup_cap, lookup_used;
    f32 decode_lut[256];
    const void *font_identity;
    u32 placement;
    u32 content, ready;
    bool has_pending;
    bool full, placeholder, allocation_failed;
    f32 x,y,cw,ch;
    usize accounted, row_bytes;
    u64 pool_generation;
    void (*wake)(void);
    u64 elapsed_ns;
    u64 submitted_ns;
} TermPrepJob;
/* One process-wide pool, not one extra viewport per pane/window. Only spare
 * CPU row storage lives here; contents, counts, glyph lookups and GPU leases
 * are never reused. The existing worker's two-second idle exit drains it. */
#define TERM_PREP_POOL_BYTES MB(1)
#define TERM_PREP_POOL_ROWS 128u
typedef struct {
    RectInstance *backgrounds;
    GlyphInstance *glyphs;
    i32 bg_cap, glyph_cap, cols;
} TermPrepStorage;
static bool term_prepare_waiting(const TermRenderCache *cache,u64 now) {
    const TermPrepJob *job=cache?cache->prepare_job:NULL;
    return job && !atomic_load_explicit(&job->done,memory_order_acquire) &&
        now-job->submitted_ns<2000000ull;
}
static struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    TermPrepJob *head,*tail;
    u32 count;
    bool running;
    TermPrepStorage pool[TERM_PREP_POOL_ROWS];
    u32 pool_count;
    usize pool_bytes;
    u64 pool_generation;
} s_term_prepare={.lock=PTHREAD_MUTEX_INITIALIZER,.ready=PTHREAD_COND_INITIALIZER};

static usize term_prepare_storage_bytes(const TermPrepStorage *s) {
    return (usize)s->bg_cap*sizeof(RectInstance)+(usize)s->glyph_cap*sizeof(GlyphInstance);
}
/* Caller owns the queue lock. Invalidating the generation also prevents a
 * completed but not yet released job from refilling a reclaimed pool. */
static void term_prepare_pool_clear_locked(void) {
    for(u32 i=0;i<s_term_prepare.pool_count;i++) {
        free(s_term_prepare.pool[i].backgrounds);free(s_term_prepare.pool[i].glyphs);
        s_term_prepare.pool[i]=(TermPrepStorage){0};
    }
    mem_budget_transient_adjust(MEM_RENDER_CPU,-(isize)s_term_prepare.pool_bytes);
    s_term_prepare.pool_count=0;s_term_prepare.pool_bytes=0;
    s_term_prepare.pool_generation++;
}
static void term_prepare_pool_reclaim(void *unused) {
    (void)unused;
    pthread_mutex_lock(&s_term_prepare.lock);
    term_prepare_pool_clear_locked();
    pthread_mutex_unlock(&s_term_prepare.lock);
}
/* Last-reference only. Worker output or replaced main-thread rows have no
 * readers by this point. A refused entry stays in output for ordinary free. */
static usize term_prepare_pool_put(TermPrepJob *job) {
    usize kept=0;
    pthread_mutex_lock(&s_term_prepare.lock);
    if(s_term_prepare.running && job->pool_generation==s_term_prepare.pool_generation) {
        TermRenderCache *c=&job->output;
        for(i32 row=0;row<c->cached_rows;row++) {
            TermPrepStorage storage={c->bg_rows[row],c->glyph_rows[row],
                                     c->bg_caps[row],c->glyph_caps[row],c->cached_cols};
            usize bytes=term_prepare_storage_bytes(&storage);
            if(!bytes || bytes>TERM_PREP_POOL_BYTES)continue;
            /* Resize must not leave the entire pool occupied by an old width
             * while the new width allocates every frame. Prefer returned rows
             * of the current width; equal-width overflow is simply freed. */
            while(bytes>TERM_PREP_POOL_BYTES-s_term_prepare.pool_bytes ||
                  s_term_prepare.pool_count==TERM_PREP_POOL_ROWS) {
                u32 i=0;
                while(i<s_term_prepare.pool_count && s_term_prepare.pool[i].cols==storage.cols)i++;
                if(i==s_term_prepare.pool_count)break;
                TermPrepStorage old=s_term_prepare.pool[i];
                usize freed=term_prepare_storage_bytes(&old);
                free(old.backgrounds);free(old.glyphs);
                s_term_prepare.pool[i]=s_term_prepare.pool[--s_term_prepare.pool_count];
                s_term_prepare.pool[s_term_prepare.pool_count]=(TermPrepStorage){0};
                s_term_prepare.pool_bytes-=freed;
                mem_budget_transient_adjust(MEM_RENDER_CPU,-(isize)freed);
            }
            if(bytes>TERM_PREP_POOL_BYTES-s_term_prepare.pool_bytes ||
               s_term_prepare.pool_count==TERM_PREP_POOL_ROWS)continue;
            s_term_prepare.pool[s_term_prepare.pool_count++]=storage;
            s_term_prepare.pool_bytes+=bytes;kept+=bytes;
            c->bg_rows[row]=NULL;c->glyph_rows[row]=NULL;
            c->bg_caps[row]=0;c->glyph_caps[row]=0;
        }
    }
    pthread_mutex_unlock(&s_term_prepare.lock);
    return kept;
}
/* Queue lock held; a single scan serves all captured rows. Width matching
 * avoids moving a wide-pane peak into an unrelated narrow pane. Ownership
 * and its transient charge move together, without a new allocation. */
static void term_prepare_pool_take_locked(TermPrepJob *job) {
    job->pool_generation=s_term_prepare.pool_generation;
    i32 row=0;
    for(u32 n=s_term_prepare.pool_count;n>0 && row<job->view.rows;n--) {
        TermPrepStorage storage=s_term_prepare.pool[n-1];
        if(storage.cols!=job->view.cols)continue;
        while(row<job->view.rows && job->slots[row]<0)row++;
        if(row==job->view.rows)break;
        TermRenderCache *c=&job->output;
        c->bg_rows[row]=storage.backgrounds;c->bg_caps[row]=storage.bg_cap;
        c->glyph_rows[row]=storage.glyphs;c->glyph_caps[row]=storage.glyph_cap;
        usize bytes=term_prepare_storage_bytes(&storage);
        s_term_prepare.pool_bytes-=bytes;job->row_bytes+=bytes;
        s_term_prepare.pool[n-1]=s_term_prepare.pool[--s_term_prepare.pool_count];
        s_term_prepare.pool[s_term_prepare.pool_count]=(TermPrepStorage){0};
        row++;
    }
}
static void term_prepare_unref(TermPrepJob *job) {
    if (atomic_fetch_sub_explicit(&job->refs,1,memory_order_acq_rel)!=1) return;
    usize kept=term_prepare_pool_put(job);
    term_render_cache_free(&job->output);
    free(job->cells);free(job->slots);free(job->lengths);free(job->lookups);
    mem_budget_transient_adjust(MEM_RENDER_CPU,-(isize)(job->accounted+job->row_bytes-kept));
    free(job);
}
static bool term_prepare_row_captured(const TermPrepJob *job,i32 row) {
    return job->slots && job->slots[row]>=0;
}
static const Cell *term_prepare_row_cells(const TermPrepJob *job,i32 row) {
    return job->cells+(usize)job->slots[row]*(usize)job->view.cols;
}
static void term_prepare_cancel(TermRenderCache *cache) {
    TermPrepJob *job=cache->prepare_job;
    if (!job) return;
    cache->prepare_job=NULL;atomic_store(&job->cancelled,true);term_prepare_unref(job);
}
static u32 term_prepare_hash(const u32 *cp,i32 count) {
    u32 hash=(u32)count;
    for(i32 k=0;k<count;k++)hash=(hash^cp[k])*16777619u;
    return hash;
}
/* Probe `table` for `cp`; with `insert` the first empty slot is claimed. The
 * caller keeps the table under half load, so the probe always terminates on
 * an empty slot. */
static TermPrepLookup *term_prepare_probe(TermPrepLookup *table,u32 cap,const u32 *cp,i32 count,bool insert) {
    u32 hash=term_prepare_hash(cp,count);
    for(u32 k=0;k<cap;k++) {
        TermPrepLookup *entry=&table[(hash+k)&(cap-1)];
        if (!entry->valid) {
            if (!insert) return NULL;
            entry->valid=true;entry->count=(u8)count;memcpy(entry->cp,cp,(usize)count*sizeof(u32));return entry;
        }
        if(entry->count==count && memcmp(entry->cp,cp,(usize)count*sizeof(u32))==0)return entry;
    }
    return NULL;
}
static bool term_prepare_lookup_grow(TermPrepJob *job) {
    u32 cap=job->lookup_cap?job->lookup_cap*2u:TERM_PREP_LOOKUPS_MIN;
    if(cap>TERM_PREP_LOOKUPS_MAX)return false;
    TermPrepLookup *next=calloc(cap,sizeof(*next));
    if(!next)return false;
    for(u32 i=0;i<job->lookup_cap;i++) {
        const TermPrepLookup *old=&job->lookups[i];
        if(!old->valid)continue;
        TermPrepLookup *entry=term_prepare_probe(next,cap,old->cp,old->count,true);
        *entry=*old;
    }
    usize was=(usize)job->lookup_cap*sizeof(*next),now=(usize)cap*sizeof(*next);
    free(job->lookups);job->lookups=next;job->lookup_cap=cap;
    job->accounted+=now-was;mem_budget_transient_adjust(MEM_RENDER_CPU,(isize)(now-was));
    return true;
}
/* Insert is main-thread only (capture); the worker only ever finds. An insert
 * may move the table, so a pointer from an earlier insert must not be held
 * across one. The FIND path is written out here rather than routed through
 * term_prepare_probe because it runs once per cell of every captured row,
 * where the extra calls cost measurable capture time in an unoptimised build.
 */
static TermPrepLookup *term_prepare_lookup(TermPrepJob *job,const u32 *cp,i32 count,bool insert) {
    u32 cap=job->lookup_cap;
    if(cap) {
        u32 hash=(u32)count;
        for(i32 k=0;k<count;k++)hash=(hash^cp[k])*16777619u;
        for(u32 k=0;k<cap;k++) {
            TermPrepLookup *entry=&job->lookups[(hash+k)&(cap-1)];
            if(!entry->valid)break;
            if(entry->count==count && memcmp(entry->cp,cp,(usize)count*sizeof(u32))==0)return entry;
        }
    }
    if(!insert)return NULL;
    if((job->lookup_used+1u)*2u>job->lookup_cap && !term_prepare_lookup_grow(job))return NULL;
    TermPrepLookup *entry=term_prepare_probe(job->lookups,job->lookup_cap,cp,count,true);
    if(entry)job->lookup_used++;
    return entry;
}
static bool term_prepare_glyph(void *context,u32 cp,FontGlyphRect *rect) {
    TermPrepJob *job=context;TermPrepLookup *entry=term_prepare_lookup(job,&cp,1,false);
    if(!entry)return false;
    *rect=entry->rect;job->placeholder|=entry->pending;return entry->got;
}
static i32 term_prepare_ligature(void *context,const u32 *cp,i32 count,FontGlyphRect *rect) {
    TermPrepLookup *entry=term_prepare_lookup(context,cp,count,false);
    if(!entry)return 0;
    *rect=entry->rect;return entry->width;
}
static bool term_prepare_emoji(void *context,u32 cp) {
    TermPrepLookup *entry=term_prepare_lookup(context,&cp,1,false);
    return entry && entry->emoji;
}
static void *term_prepare_worker(void *unused) {
    (void)unused;
#ifdef __APPLE__
    pthread_setname_np("liu.terminal.prepare");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
#endif
    pthread_mutex_lock(&s_term_prepare.lock);
    for(;;) {
        while(!s_term_prepare.head) {
            struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=2;
            if(pthread_cond_timedwait(&s_term_prepare.ready,&s_term_prepare.lock,&until)==ETIMEDOUT && !s_term_prepare.head) {
                term_prepare_pool_clear_locked();
                s_term_prepare.running=false;pthread_mutex_unlock(&s_term_prepare.lock);return NULL;
            }
        }
        TermPrepJob *job=s_term_prepare.head;s_term_prepare.head=job->next;
        if(!s_term_prepare.head)s_term_prepare.tail=NULL;
        s_term_prepare.count--;
        if(!atomic_load(&job->cancelled))term_prepare_pool_take_locked(job);
        pthread_mutex_unlock(&s_term_prepare.lock);
        u64 start=liu_render_probe_ns();
        for(i32 row=0;row<job->view.rows && !atomic_load(&job->cancelled);row++) {
            if(!term_prepare_row_captured(job,row))continue;
            job->placeholder=false;
            term_render_cache_build_row(&job->output,&job->style,&job->view,row,job->x,job->y,job->cw,job->ch);
            job->allocation_failed|=job->output.row_allocation_failed;
            /* Give capacity back when a reused row is at least 4x
             * oversized, so a reused output does not keep the size of its
             * widest line. */
            term_render_cache_shrink_row(&job->output,row);
            job->output.pending_rows[row]=job->placeholder;
        }
        job->elapsed_ns=liu_render_probe_ns()-start;
        usize row_bytes=term_render_cache_bytes(&job->output);
        mem_budget_transient_adjust(MEM_RENDER_CPU,(isize)row_bytes-(isize)job->row_bytes);
        job->row_bytes=row_bytes;
        atomic_store_explicit(&job->done,true,memory_order_release);
        if(!atomic_load(&job->cancelled) && job->wake)job->wake();
        term_prepare_unref(job);
        pthread_mutex_lock(&s_term_prepare.lock);
    }
}
static bool term_prepare_enqueue(TermPrepJob *job) {
    pthread_mutex_lock(&s_term_prepare.lock);
    if(s_term_prepare.count>=8) { pthread_mutex_unlock(&s_term_prepare.lock);return false; }
    if(!s_term_prepare.running) {
        pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr,256*1024);
        int rc=pthread_create(&thread,&attr,term_prepare_worker,NULL);pthread_attr_destroy(&attr);
        if(rc) { pthread_mutex_unlock(&s_term_prepare.lock);return false; }
        s_term_prepare.running=true;
    }
    atomic_fetch_add(&job->refs,1);
    job->submitted_ns=liu_render_probe_ns();
    if(s_term_prepare.tail)s_term_prepare.tail->next=job;else s_term_prepare.head=job;
    s_term_prepare.tail=job;s_term_prepare.count++;pthread_cond_signal(&s_term_prepare.ready);
    pthread_mutex_unlock(&s_term_prepare.lock);return true;
}
static bool term_prepare_capture_glyph(TermPrepJob *job,FontAtlas *font,u32 cp) {
    if(cp<=32)return true;
    if(term_prepare_lookup(job,&cp,1,false))return true;
    TermPrepLookup *entry=term_prepare_lookup(job,&cp,1,true);if(!entry)return false;
    /* Cluster classification reads the main-thread intern table. Capture it
     * here rather than letting geometry construction consult that table. */
    entry->emoji=utf8_is_emoji(cp&~FONT_STYLE_MASK);
    font_placeholder_reset(font);entry->got=font_get_glyph_rect(font,cp,&entry->rect);
    entry->pending=font_placeholder_seen(font);job->has_pending|=entry->pending;return true;
}
static TermPrepJob *term_prepare_capture(TermRenderCache *cache,AppState *app,Terminal *term,f32 x,f32 y,f32 cw,f32 ch) {
    usize count,bytes;
    if(!checked_mul_usize((usize)term->rows,(usize)term->cols,&count) ||
       !checked_mul_usize(count,sizeof(Cell),&bytes) || bytes>MB(8))return NULL;
    /* Only the rows the worker will build are captured; the cell image is
     * sized from THAT count, so a cursor-line update on a tall pane does not
     * copy the whole grid into the job. */
    u32 captured=0;
    for(i32 row=0;row<term->rows;row++)if(cache->all_rows_dirty || term_row_dirty(term,row))captured++;
    if(!captured)return NULL;
    usize cell_bytes=(usize)captured*(usize)term->cols*sizeof(Cell);
    TermPrepJob *job=calloc(1,sizeof(*job));if(!job)return NULL;
    atomic_init(&job->refs,1);atomic_init(&job->done,false);atomic_init(&job->cancelled,false);
    job->accounted=sizeof(*job);mem_budget_transient_adjust(MEM_RENDER_CPU,(isize)sizeof(*job));
    job->cells=calloc(1,cell_bytes);
    job->slots=malloc((usize)term->rows*sizeof(*job->slots));
    job->lengths=calloc((usize)term->rows,sizeof(*job->lengths));
    usize allocated=(job->cells?cell_bytes:0)+(job->slots?(usize)term->rows*sizeof(*job->slots):0)+
        (job->lengths?(usize)term->rows*sizeof(*job->lengths):0);
    job->accounted+=allocated;mem_budget_transient_adjust(MEM_RENDER_CPU,(isize)allocated);
    if(!job->cells || !job->slots || !job->lengths)goto fail;
    for(i32 row=0;row<term->rows;row++)job->slots[row]=-1;
    job->view.rows=term->rows;job->view.cols=term->cols;job->view.cells=job->cells;
    job->view.sync_view=true;job->view.sync_view_lengths=job->lengths;job->view.sync_view_slots=job->slots;
    job->view.cursor_attr=term->cursor_attr;job->view.bidi_enabled=term->bidi_enabled;
    job->x=x;job->y=y;job->cw=cw;job->ch=ch;job->full=cache->all_rows_dirty;
    job->font_identity=app->renderer.font.priv;
    job->placement=font_atlas_placement_generation(&app->renderer.font);
    job->wake=app->renderer.frame_ready_wake;
    if(!term_render_cache_ensure(&job->output,term->rows,term->cols,cw,ch,x,y))goto fail;
    job->output.palette_lut=malloc(256u*sizeof(Color));
    if(!job->output.palette_lut)goto fail;
    for(u32 i=0;i<256;i++)job->output.palette_lut[i]=cache_color_resolve(cache,i,i);
    job->output.palette_lut_valid=true;
    job->style=term_row_style_pane(app,cache);job->style.font=NULL;job->style.lookup=job;
    job->style.glyph=term_prepare_glyph;job->style.ligature=term_prepare_ligature;
    job->style.emoji=term_prepare_emoji;
    job->style.isolated=true;
    if(g_srgb_decode_lut_ready) {
        memcpy(job->decode_lut,g_srgb_decode_lut,sizeof(job->decode_lut));
        job->style.decode_lut=job->decode_lut;
    }
    for(i32 row=0;row<term->rows;row++) {
        if(!job->full && !term_row_dirty(term,row))continue;
        if(job->captured>=captured)goto fail; /* the dirty set grew after it was counted */
        job->slots[row]=(i32)job->captured++;job->lengths[row]=term->cols;
        i32 length=0;const Cell *source=terminal_visible_row(term,row,&length);
        Cell *target=job->cells+(usize)job->slots[row]*(usize)term->cols;
        for(i32 col=0;col<term->cols;col++)target[col]=source && col<length?source[col]:(Cell){.codepoint=' ',.attr=term->cursor_attr};
        Cell visual[1024];const Cell *lookup=target;
        if(term->bidi_enabled && term->cols<=1024 && bidi_line_has_rtl(target,term->cols)) {
            bidi_reorder_line(target,term->cols,visual,NULL);bidi_shape_arabic(visual,term->cols);lookup=visual;
        }
        /* The candidates' run, shaped once on its first cache miss
         * (font_check_ligature_run) — the same as the direct path's. */
        FontLigatureRun lrun;i32 lrun_start=0,lrun_end=0;bool lrun_cut=false;
        for(i32 col=0;col<term->cols;col++) {
            u16 sflags=lookup[col].attr.flags;
            if(!term_prepare_capture_glyph(job,&app->renderer.font,
                   term_cell_glyph_key(&lookup[col],col+1<term->cols?&lookup[col+1]:NULL)))goto fail;
            if(!job->style.ligatures || !lig_can_start(lookup[col].codepoint) ||
               (sflags&(ATTR_BOLD|ATTR_ITALIC)))continue;
            if(col>=lrun_end || (lrun_cut && col+MIN(4,term->cols-col)>lrun_end))
                lrun_start=lig_run_fill(&lrun,lookup,term->cols,col,&lrun_end,&lrun_cut);
            for(i32 n=MIN(4,term->cols-col);n>=2;n--) {
                u32 cp[4];bool uniform=true;
                for(i32 k=0;k<n;k++) {
                    const Cell *cell=&lookup[col+k];cp[k]=cell->codepoint;
                    if(cp[k]<33 || cp[k]>=127 || (cell->attr.flags&(ATTR_WIDE|ATTR_WDUMMY)) ||
                       cell->attr.fg!=lookup[col].attr.fg || cell->attr.bg!=lookup[col].attr.bg ||
                       ((cell->attr.flags^lookup[col].attr.flags)&(ATTR_INVERSE|ATTR_BOLD|ATTR_DIM|ATTR_ITALIC)))uniform=false;
                }
                if(!uniform || term_prepare_lookup(job,cp,n,false))continue;
                TermPrepLookup *entry=term_prepare_lookup(job,cp,n,true);if(!entry)goto fail;
                entry->width=font_check_ligature_run(&app->renderer.font,&lrun,col-lrun_start,n,&entry->rect);
            }
        }
    }
    if(job->placement!=font_atlas_placement_generation(&app->renderer.font))goto fail;
    job->content=font_atlas_content_version(&app->renderer.font);
    job->ready=font_atlas_gpu_ready_version(&app->renderer.font);
    return job;
fail:
    term_prepare_unref(job);return NULL;
}
/* -1: keep the frame pending; 0: synchronous fallback; 1: completed rows. */
static i32 term_prepare_rows(TermRenderCache *cache,AppState *app,Terminal *term,f32 x,f32 y,f32 cw,f32 ch) {
    if(!term_prepare_enabled()) { term_prepare_cancel(cache);return 0; }
    TermPrepJob *job=cache->prepare_job;
    if(job) {
        if(!atomic_load_explicit(&job->done,memory_order_acquire)) {
            if(liu_render_probe_ns()-job->submitted_ns<2000000ull)return -1;
            term_prepare_cancel(cache);app->prepare_expired++;
            cache->prepare_retry_after_ns=liu_render_probe_ns()+100000000ull;return 0;
        }
        TermRowStyle style=term_row_style_pane(app,cache);
        bool same=!job->allocation_failed && job->font_identity==app->renderer.font.priv &&
            job->placement==font_atlas_placement_generation(&app->renderer.font) &&
            (!job->has_pending || (job->content==font_atlas_content_version(&app->renderer.font) &&
                                   job->ready==font_atlas_gpu_ready_version(&app->renderer.font))) &&
            job->view.rows==term->rows && job->view.cols==term->cols && job->view.bidi_enabled==term->bidi_enabled &&
            job->x==x && job->y==y && job->cw==cw && job->ch==ch &&
            job->style.ligatures==style.ligatures && job->style.bold_bright==style.bold_bright &&
            job->style.opacity==style.opacity && job->style.wallpaper==style.wallpaper &&
            job->style.wallpaper_opacity==style.wallpaper_opacity &&
            (!cache->all_rows_dirty || job->full) &&
            job->output.palette_lut!=NULL;
        for(u32 i=0;same && i<256;i++) {
            Color current=cache_color_resolve(cache,i,i);
            if(memcmp(&current,&job->output.palette_lut[i],sizeof(Color)))same=false;
        }
        for(i32 row=0;same && row<term->rows;row++) {
            if(!term_prepare_row_captured(job,row)) { if(term_row_dirty(term,row))same=false;continue; }
            i32 length;const Cell *source=terminal_visible_row(term,row,&length);
            const Cell *held=term_prepare_row_cells(job,row);
            for(i32 col=0;same && col<term->cols;col++) {
                Cell value=source && col<length?source[col]:(Cell){.codepoint=' ',.attr=term->cursor_attr};
                if(memcmp(&value,&held[col],sizeof(Cell)))same=false;
            }
        }
        if(same) {
            term_render_cache_versions_ensure(cache);
            bool compare=!job->full && job->captured<=TERM_ROW_COMPARE_LIMIT &&
                         job->captured<(u32)term->rows;
            if (!compare) term_render_cache_spare_free(cache);
            for(i32 row=0;row<term->rows;row++)if(term_prepare_row_captured(job,row)) {
                bool bg_changed=!compare || !term_row_bytes_equal(cache->bg_rows[row],cache->bg_counts[row],
                    job->output.bg_rows[row],job->output.bg_counts[row],sizeof(RectInstance));
                bool glyph_changed=!compare || !term_row_bytes_equal(cache->glyph_rows[row],cache->glyph_counts[row],
                    job->output.glyph_rows[row],job->output.glyph_counts[row],sizeof(GlyphInstance));
                RectInstance *old_bg=cache->bg_rows[row];i32 old_bc=cache->bg_caps[row];
                GlyphInstance *old_glyph=cache->glyph_rows[row];i32 old_gc=cache->glyph_caps[row];
                cache->bg_rows[row]=job->output.bg_rows[row];job->output.bg_rows[row]=old_bg;
                cache->glyph_rows[row]=job->output.glyph_rows[row];job->output.glyph_rows[row]=old_glyph;
                cache->bg_counts[row]=job->output.bg_counts[row];cache->bg_caps[row]=job->output.bg_caps[row];
                cache->glyph_counts[row]=job->output.glyph_counts[row];cache->glyph_caps[row]=job->output.glyph_caps[row];
                job->output.bg_caps[row]=old_bc;job->output.glyph_caps[row]=old_gc;
                job->output.bg_counts[row]=0;job->output.glyph_counts[row]=0;
                cache->pending_rows[row]=job->output.pending_rows[row];
                term_render_cache_note_row(cache,row,bg_changed,glyph_changed);
                app->frame_rows_rebuilt++;app->frame_cells_rebuilt+=term->cols;
                app->frame_glyphs_shaped+=cache->glyph_counts[row];
#ifdef LIU_RENDER_REPLAY_PROBE
                app->renderer.replay_rows_rebuilt++;
#endif
            }
            /* The adopted arrays move to the cache inventory. The replaced
             * arrays now belong to this job and may enter the bounded pool. */
            usize row_bytes=term_render_cache_bytes(&job->output);
            mem_budget_transient_adjust(MEM_RENDER_CPU,(isize)row_bytes-(isize)job->row_bytes);
            job->row_bytes=row_bytes;
            cache->prepare_discards=0;app->prepare_adopted++;term_prepare_cancel(cache);return 1;
        }
        term_prepare_cancel(cache);
        /* Continuous output cannot starve presentation by invalidating every
         * snapshot. A mismatched result falls back to the current rows now. */
        cache->prepare_discards++;app->prepare_discarded++;
        cache->prepare_retry_after_ns=liu_render_probe_ns()+50000000ull;return 0;
    }
    if(liu_render_probe_ns()<cache->prepare_retry_after_ns)return 0;
    usize cells=0;for(i32 row=0;row<term->rows;row++)if(cache->all_rows_dirty || term_row_dirty(term,row))cells+=(usize)term->cols;
    if(cells<1024)return 0; /* no queue hop for a prompt/cursor-sized update */
    job=term_prepare_capture(cache,app,term,x,y,cw,ch);
    if(!job) { cache->prepare_retry_after_ns=liu_render_probe_ns()+100000000ull;return 0; }
    if(!term_prepare_enqueue(job)) {
        term_prepare_unref(job);cache->prepare_retry_after_ns=liu_render_probe_ns()+8000000ull;return 0;
    }
    cache->prepare_job=job;app->prepare_submitted++;return -1;
}
#endif
