/*
 * Liu — Markdown knowledge-graph view (data + force layout). Rendering is
 * done on the GPU by the caller via md_graph accessors + renderer_draw_line /
 * rrect circles — no CPU rasterization.
 * See md_graph.h. Deliberately self-contained: no renderer/shader changes.
 */
#include "core/mem_budget.h"
#include "ui/markdown/md_graph.h"
#include "ui/markdown/md_parse.h"
#include "ui/markdown/md_ast.h"
#include "core/checked.h"
#include "core/memory.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp — glibc declares it here, not in string.h */
#ifndef PLATFORM_WIN32
#include <fnmatch.h>
#endif
#include <ctype.h>
#include <math.h>

#define MD_GRAPH_MAX_NODES      1500
#define MD_GRAPH_MAX_EDGES      8000
#define MD_GRAPH_MAX_DEPTH      10
#define MD_GRAPH_MAX_FILE_BYTES (4u * 1024u * 1024u)

typedef struct {
    char  path[1024];   /* absolute file path */
    char  label[128];   /* basename without extension (display) */
    char  key[128];     /* lowercased basename, no extension (link match) */
    f32   x, y;         /* world position */
    f32   vx, vy;       /* velocity */
    i32   degree;       /* unique edge count */
    i32   group;        /* folder cluster id (-1 = none/tag); see assign_groups */
    i32   component;    /* connected-component id (0..component_count-1); see assign_components */
    i32   comp_size;    /* node count in this component (1 = isolated note) */
    bool  pinned;
    bool  is_tag;       /* tag pseudo-node ("#name"), no file */
} MdGraphNode;

typedef struct { i32 a, b; } MdGraphEdge;   /* a < b, normalized */

/* Build-time arrays grow by doubling from these, up to the MD_GRAPH_MAX_*
 * ceilings, instead of being allocated AT the ceiling, so a small vault
 * does not pay the ceiling's peak (and, under memory pressure, its page-in)
 * for every build. Everything is indexed, never pointed at, across a growth
 * (the two `MdGraphNode *nd = g->nodes` walks are in the layout stage,
 * after the arrays are final). */
#define MD_GRAPH_NODES_INITIAL  64
#define MD_GRAPH_EDGES_INITIAL  256

struct MdGraph {
    MdGraphNode *nodes;
    i32          node_count;
    i32          node_cap;
    MdGraphEdge *edges;        /* undirected (a<b), for the graph view */
    i32          edge_count;
    /* Directed file→file edges (a=src, b=dst), un-normalized, for backlinks.
     * The undirected `edges` above lose direction, so this is kept separately. */
    MdGraphEdge *dir_edges;
    i32          dir_edge_count;
    i32          dir_edge_cap;

    /* What this graph reported to the ledger, so md_graph_free credits back
     * exactly what md_graph_build charged and a partially-built graph freed on
     * an error path credits nothing. */
    usize charged_bytes;

    f32   temperature;   /* cooling factor, 1 -> 0 */
    f32   step_accum;    /* wall-clock seconds not yet consumed by a 1/60 sub-step */
    bool  settled;
    f32   world_radius;

    /* Folder clustering: each file node is tagged with a group id = the index
     * of its parent directory (relative to the build root) in group_names[].
     * Same-folder nodes attract toward their shared centroid; different folders
     * push apart — so the layout separates into clean per-folder clusters. */
    char (*group_names)[256];   /* relative parent dir per group ("" = root) */
    i32    group_count;

    /* Cached folder centroids. The renderer asks md_graph_group_centroid()
     * once per group per frame, for labels that move only when the layout
     * does, and the force pass needs the same sums. One O(n) pass serves both,
     * recomputed at each of the four places that move a node — seed,
     * integrate+anti-overlap, and drag. */
    f32   *group_cx, *group_cy;   /* group_count entries each */
    i32   *group_n;

    /* Connected components over the undirected link graph: each topic cluster
     * (a hub + the notes linked to it) is one component, coloured distinctly in
     * the view. Isolated notes (comp_size==1) stay neutral. */
    i32    component_count;

    /* Force multipliers (1.0 == the built-in defaults). Mirrored
     * each frame from the FileBrowser's GraphSettings so the controls panel
     * tunes the live simulation; node_size scales only the rendered/hit radius. */
    f32    f_center, f_repel, f_link, f_linkdist, node_size;
};

/* ---------------------------------------------------------------- scanning */

static void str_lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

/* basename without directory or .md/.markdown extension, into out (cap). */
static void base_no_ext(const char *path, char *out, usize cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!path) return;
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    snprintf(out, cap, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot && (strcasecmp(dot, ".md") == 0 || strcasecmp(dot, ".markdown") == 0))
        *dot = '\0';
}

static bool ends_with_md(const char *name) {
    usize n = strlen(name);
    return (n > 3 && strcasecmp(name + n - 3, ".md") == 0) ||
           (n > 9 && strcasecmp(name + n - 9, ".markdown") == 0);
}

static bool ends_with_ci(const char *s, const char *suf) {
    usize ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcasecmp(s + ls - lf, suf) == 0;
}

/* Scan policy resolved ONCE per build. Path-text matching is brittle (firmlinks
 * like /System/Volumes/Data/Users/…, symlinks, NFC-vs-NFD spelling, trailing
 * slashes), so privacy-sensitive folders are pinned by (device,inode) IDENTITY
 * instead — stat the real $HOME/{Pictures,Music,…} once, then compare inodes
 * while walking. A same-device guard keeps the scan off mounted network /
 * removable / system volumes (which are TCC-prompt-protected too). */
typedef struct {
    dev_t root_dev;
    bool  have_root_dev;
    struct { dev_t dev; ino_t ino; } prot[12];
    i32   prot_n;
    char  gi[48][64];   /* <root>/.gitignore directory-skip patterns (fnmatch by name) */
    i32   gi_n;
    const _Atomic bool *cancel;   /* optional build-abort flag (see md_graph_build_ex) */
} GraphScanCtx;

/* Relaxed is enough: the flag is a one-way abort latch, no data is published
 * through it. */
static bool graph_cancelled(const _Atomic bool *cancel) {
    return cancel && atomic_load_explicit(cancel, memory_order_relaxed);
}

static void scan_ctx_pin(GraphScanCtx *c, const char *path) {
    struct stat st;
    if (path && c->prot_n < (i32)(sizeof c->prot / sizeof c->prot[0]) && stat(path, &st) == 0) {
        c->prot[c->prot_n].dev = st.st_dev;
        c->prot[c->prot_n].ino = st.st_ino;
        c->prot_n++;
    }
}
static bool scan_ctx_is_pinned(const GraphScanCtx *c, const struct stat *st) {
    for (i32 i = 0; i < c->prot_n; i++)
        if (c->prot[i].dev == st->st_dev && c->prot[i].ino == st->st_ino) return true;
    return false;
}

/* Load directory-skip patterns from <root>/.gitignore so "Graph Current Folder"
 * of a code project automatically excludes the project's OWN module / build
 * dirs (node_modules, build/, target/, dist/, build-*, cmake-build-*, …). The
 * project already declares them, so this is smarter than a fixed name list and
 * never wrongly drops a real note folder (notes aren't gitignored). Only
 * single-component, directory-ish patterns are taken (matched against an entry
 * NAME via fnmatch); file-extension globs (*.o) and negations (!keep) are
 * skipped so a directory is never dropped by a file rule. */
#define MD_GRAPH_IGNORE_BYTES KB(64)
#define MD_GRAPH_IGNORE_LINES 4096u

static void scan_ctx_load_gitignore(GraphScanCtx *c, const char *root_dir) {
    if (!c || !root_dir || graph_cancelled(c->cancel)) return;
    char path[1024];
    int path_n = snprintf(path, sizeof path, "%s/.gitignore", root_dir);
    if (path_n < 0 || (usize)path_n >= sizeof path) return;
    /* Open must itself be nonblocking: fstat after a blocking FIFO open is
     * too late. Only the regular descriptor just opened may supply rules. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (u64)st.st_size > MD_GRAPH_IGNORE_BYTES) { close(fd); return; }
    char *bytes = malloc(MD_GRAPH_IGNORE_BYTES + 1u);
    if (!bytes) { close(fd); return; }
    usize used = 0;
    bool valid = true;
    for (;;) {
        if (graph_cancelled(c->cancel)) { valid = false; break; }
        usize left = MD_GRAPH_IGNORE_BYTES + 1u - used;
        if (left > 4096u) left = 4096u;
        ssize_t n = read(fd, bytes + used, left);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { valid = false; break; }
        if (n == 0) break;
        used += (usize)n;
        if (used > MD_GRAPH_IGNORE_BYTES) { valid = false; break; }
    }
    close(fd);
    /* NULs and oversized inputs are rejected whole; strlen must not hide
     * bytes or let a partial line become an unrelated skip pattern. */
    if (!valid || memchr(bytes, '\0', used)) { free(bytes); return; }
    bytes[used] = '\0';
    u32 lines = 0;
    usize pos = 0;
    while (pos < used) {
        if (++lines > MD_GRAPH_IGNORE_LINES || graph_cancelled(c->cancel)) {
            c->gi_n = 0;
            break;
        }
        usize start = pos;
        while (pos < used && bytes[pos] != '\n') pos++;
        usize line_len = pos - start;
        if (pos < used) bytes[pos++] = '\0';
        /* Count every line, including skipped/comment/overlong lines. */
        if (line_len >= 256u) continue;
        char *s = bytes + start;
        while (*s == ' ' || *s == '\t') s++;
        usize n = strlen(s);
        while (n && (s[n-1]=='\r' || s[n-1]==' ' || s[n-1]=='\t')) s[--n] = 0;
        if (!*s || *s == '#' || *s == '!') continue;
        bool was_dir = (n && s[n-1] == '/');
        while (n && s[n-1] == '/') s[--n] = 0;
        if (*s == '/') s++;
        char *slash = strrchr(s, '/');
        if (slash) s = slash + 1;
        if (!*s || (!was_dir && strchr(s, '.')) ||
            strlen(s) >= sizeof c->gi[0]) continue;
        if (c->gi_n < (i32)(sizeof c->gi / sizeof c->gi[0]))
            snprintf(c->gi[c->gi_n++], sizeof c->gi[0], "%s", s);
    }
    free(bytes);
}

static bool scan_ctx_gitignored(const GraphScanCtx *c, const char *name) {
    for (i32 i = 0; i < c->gi_n; i++)
        if (fnmatch(c->gi[i], name, 0) == 0) return true;
    return false;
}

/* Is `name` a package-VERSION directory? This is the ecosystem-agnostic way to
 * cut a "Graph Current Folder" off from dependency caches that carry no
 * .gitignore and whose folders are named by version, not "node_modules": the Go
 * module cache (repo@v1.2.3), Homebrew Cellar (formula/1.21.4), pip/npm version
 * dirs, etc. Requires a leading (optional 'v') digit and at least one dot, and
 * the WHOLE name to be version-ish (digits/./-/_/+), so plain words, bare years
 * ("2024"), and names like "10.5-notes" are NOT skipped. */
static bool is_pkg_version_dir(const char *name) {
    if (strstr(name, "@v")) return true;                 /* go module: repo@vX.Y.Z */
    const char *p = name;
    if (*p == 'v' || *p == 'V') p++;
    if (*p < '0' || *p > '9') return false;
    bool dot = false;
    for (; *p; p++) {
        if (*p == '.') dot = true;
        else if (!((*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '+')) return false;
    }
    return dot;
}

/* Dependency / package-cache / bundle dir names (no notes; bundles are also
 * TCC-pokey). Matched anywhere — these never identify a real note folder.
 * Combined with is_pkg_version_dir + the project's .gitignore, this keeps a
 * "Graph Current Folder" of a dev tree from drowning in package READMEs. */
static bool is_skip_name(const char *name) {
    static const char *noise[] = {
        "node_modules", "bower_components", "Pods", "__pycache__", "DerivedData",
        "site-packages", "dist-packages", "Cellar", "Caskroom",
        "vendor", "Carthage", "venv", "virtualenv", "wheels", NULL
    };
    for (i32 i = 0; noise[i]; i++) if (strcmp(name, noise[i]) == 0) return true;
    if (is_pkg_version_dir(name)) return true;
    static const char *bundles[] = {
        ".app", ".bundle", ".framework", ".plugin", ".kext", ".xpc", ".pkg", ".mpkg",
        ".photoslibrary", ".photolibrary", ".musiclibrary", ".tvlibrary",
        ".imovielibrary", ".theater", ".fcpbundle", ".aplibrary", ".lrlibrary",
        ".migPlugin", NULL
    };
    for (i32 i = 0; bundles[i]; i++) if (ends_with_ci(name, bundles[i])) return true;
    return false;
}

/* Room for one more node, growing the array by doubling up to the ceiling.
 * Zero-fills the new tail so the unset fields of a partially built graph
 * read as zero. */
static bool graph_reserve_node(MdGraph *g) {
    if (g->node_count < g->node_cap) return true;
    if (g->node_cap >= MD_GRAPH_MAX_NODES) return false;
    i32 cap = g->node_cap ? g->node_cap * 2 : MD_GRAPH_NODES_INITIAL;
    if (cap > MD_GRAPH_MAX_NODES) cap = MD_GRAPH_MAX_NODES;
    MdGraphNode *next = realloc(g->nodes, (usize)cap * sizeof *next);
    if (!next) return false;
    memset(next + g->node_cap, 0, (usize)(cap - g->node_cap) * sizeof *next);
    g->nodes = next;
    g->node_cap = cap;
    return true;
}

/* The edge vectors during the build: the raw undirected list and the
 * directed backlink list. Same doubling rule as the nodes. */
typedef struct { MdGraphEdge *data; i32 len, cap; } GraphEdgeVec;

static bool graph_edge_push(GraphEdgeVec *v, i32 a, i32 b) {
    if (v->len >= MD_GRAPH_MAX_EDGES) return false;
    if (v->len >= v->cap) {
        i32 cap = v->cap ? v->cap * 2 : MD_GRAPH_EDGES_INITIAL;
        if (cap > MD_GRAPH_MAX_EDGES) cap = MD_GRAPH_MAX_EDGES;
        MdGraphEdge *next = realloc(v->data, (usize)cap * sizeof *next);
        if (!next) return false;
        v->data = next;
        v->cap = cap;
    }
    v->data[v->len].a = a;
    v->data[v->len].b = b;
    v->len++;
    return true;
}

/* Append a .md file node (path -> lowercased basename key). The caller
 * guarantees g->node_count < MD_GRAPH_MAX_NODES via the readdir while-loop. */
static bool md_graph_add_file_node(MdGraph *g, const char *full) {
    if (!g || !full || g->node_count < 0 ||
        g->node_count >= MD_GRAPH_MAX_NODES || strlen(full) >= sizeof g->nodes[0].path)
        return false;
    if (!graph_reserve_node(g)) return false;
    MdGraphNode *n = &g->nodes[g->node_count++];
    memset(n, 0, sizeof *n);
    snprintf(n->path, sizeof n->path, "%s", full);
    base_no_ext(full, n->label, sizeof n->label);
    snprintf(n->key, sizeof n->key, "%s", n->label);
    str_lower(n->key);
    return true;
}

static bool graph_join_path(char *out, usize cap, const char *dir, const char *name) {
    if (!out || cap == 0 || !dir || !name) return false;
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return n >= 0 && (usize)n < cap;
}

static void scan_dir(MdGraph *g, const char *dir, i32 depth, const GraphScanCtx *ctx) {
    if (depth > MD_GRAPH_MAX_DEPTH || g->node_count >= MD_GRAPH_MAX_NODES) return;
    if (graph_cancelled(ctx->cancel)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && g->node_count < MD_GRAPH_MAX_NODES &&
           !graph_cancelled(ctx->cancel)) {
        if (de->d_name[0] == '.') continue;   /* skip hidden + . / .. */
#ifdef DT_DIR
        /* dirent-type fast path: the scan walks the WHOLE tree (incl. build /
         * output dirs), so an lstat() syscall per entry dominated the cost.
         * Resolve the common cases from d_type and skip the syscall: symlinks
         * are never followed, and a regular .md file only needs its name.
         * Directories still take the lstat() below for the device + pinned-inode
         * (TCC/$HOME) privacy checks. DT_UNKNOWN (rare; some filesystems) falls
         * through to the unchanged lstat path, so correctness never degrades. */
        if (de->d_type == DT_LNK) continue;
        if (de->d_type == DT_REG) {
            if (ends_with_md(de->d_name)) {
                char full[1024];
                if (graph_join_path(full, sizeof full, dir, de->d_name))
                    (void)md_graph_add_file_node(g, full);
            }
            continue;
        }
        if (de->d_type == DT_DIR &&
            (is_skip_name(de->d_name) || scan_ctx_gitignored(ctx, de->d_name))) continue;
#endif
        char full[1024];
        if (!graph_join_path(full, sizeof full, dir, de->d_name)) continue;
        struct stat st;
        /* lstat: never follow a symlink — a link into ~/Library, ~/Pictures,
         * /Volumes, or an iCloud mount must not let the scan escape the skips. */
        if (lstat(full, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (ctx->have_root_dev && st.st_dev != ctx->root_dev) continue; /* other volume */
            if (scan_ctx_is_pinned(ctx, &st)) continue;                     /* $HOME media/system */
            if (is_skip_name(de->d_name) ||
                scan_ctx_gitignored(ctx, de->d_name)) continue;             /* deps / .gitignore'd / bundles */
            scan_dir(g, full, depth + 1, ctx);
        } else if (S_ISREG(st.st_mode) && ends_with_md(de->d_name)) {
            (void)md_graph_add_file_node(g, full);
        }
    }
    closedir(d);
}

/* Map a link target ("[[Page]]", "Page|Alias", "dir/Page.md", "Note#h") to a
 * node index by lowercased basename, or -1. */
static i32 resolve_target(const MdGraph *g, const u8 *data, u32 len) {
    char buf[256];
    u32 n = len < sizeof(buf) - 1 ? len : (u32)sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';
    /* strip alias (|...) and anchor (#...) */
    char *bar = strchr(buf, '|');  if (bar) *bar = '\0';
    char *hsh = strchr(buf, '#');  if (hsh) *hsh = '\0';
    /* trim trailing spaces */
    for (i32 i = (i32)strlen(buf) - 1; i >= 0 && buf[i] == ' '; i--) buf[i] = '\0';
    if (buf[0] == '\0') return -1;
    char key[128];
    base_no_ext(buf, key, sizeof key);
    str_lower(key);
    for (i32 i = 0; i < g->node_count; i++)
        if (!g->nodes[i].is_tag && strcmp(g->nodes[i].key, key) == 0) return i;
    return -1;
}

/* Find or create a tag pseudo-node ("#name"). Returns index or -1. */
static i32 graph_tag_node(MdGraph *g, const u8 *name, u32 nlen) {
    char key[128];
    u32 n = nlen < (u32)sizeof(key) - 1 ? nlen : (u32)sizeof(key) - 1;
    memcpy(key, name, n); key[n] = '\0';
    str_lower(key);
    for (i32 i = 0; i < g->node_count; i++)
        if (g->nodes[i].is_tag && strcmp(g->nodes[i].key, key) == 0) return i;
    if (g->node_count >= MD_GRAPH_MAX_NODES || !graph_reserve_node(g)) return -1;
    MdGraphNode *nd = &g->nodes[g->node_count];
    memset(nd, 0, sizeof *nd);
    nd->is_tag = true;
    snprintf(nd->label, sizeof nd->label, "#%.*s", (int)n, (const char *)name);
    snprintf(nd->key, sizeof nd->key, "%s", key);
    return g->node_count++;
}

static void walk_inlines(MdGraph *g, const MdInline *runs, u32 count,
                         i32 from, GraphEdgeVec *edges, GraphEdgeVec *dir) {
    for (u32 i = 0; i < count; i++) {
        const MdInline *in = &runs[i];
        i32 to = -1;
        if (in->kind == MD_INLINE_LINK || in->kind == MD_INLINE_WIKILINK ||
            in->kind == MD_INLINE_EMBED) {
            if (in->url.len > 0) to = resolve_target(g, in->url.data, in->url.len);
        } else if (in->kind == MD_INLINE_TAG && in->text.len > 0) {
            to = graph_tag_node(g, in->text.data, in->text.len);
        }
        if (to >= 0 && to != from) {
            i32 a = from < to ? from : to;
            i32 b = from < to ? to : from;
            if (graph_edge_push(edges, a, b)) {
                /* Directed file→file edge (src=from, dst=to) for backlinks;
                 * tag targets are excluded — backlinks are note-to-note only. */
                if (in->kind != MD_INLINE_TAG && to < g->node_count &&
                    !g->nodes[to].is_tag)
                    (void)graph_edge_push(dir, from, to);
            }
        }
        if (in->children && in->child_count)
            walk_inlines(g, in->children, in->child_count, from, edges, dir);
    }
}

static void walk_blocks(MdGraph *g, const MdBlock *blocks, u32 count,
                        i32 from, GraphEdgeVec *edges, GraphEdgeVec *dir) {
    for (u32 i = 0; i < count; i++) {
        const MdBlock *b = &blocks[i];
        if (b->inlines && b->inline_count)
            walk_inlines(g, b->inlines, b->inline_count, from, edges, dir);
        if (b->children && b->child_count)
            walk_blocks(g, b->children, b->child_count, from, edges, dir);
    }
}

static int edge_cmp(const void *pa, const void *pb) {
    const MdGraphEdge *a = pa, *b = pb;
    if (a->a != b->a) return a->a - b->a;
    return a->b - b->b;
}

static void graph_recompute_centroids(MdGraph *g);   /* defined with the accessors */

/* deterministic golden-angle spiral seed so layout is reproducible. */
static void seed_positions(MdGraph *g) {
    const f32 golden = 2.39996323f;   /* radians */
    f32 spacing = 64.0f;
    for (i32 i = 0; i < g->node_count; i++) {
        f32 r = spacing * sqrtf((f32)i + 0.5f);
        f32 a = (f32)i * golden;
        g->nodes[i].x = r * cosf(a);
        g->nodes[i].y = r * sinf(a);
    }
}

/* Parent folder of `path` relative to `root`, into out. "" for files directly
 * in root. e.g. root=/v, /v/docs/a.md -> "docs"; /v/docs/sub/b.md -> "docs/sub". */
static void rel_parent_dir(const char *path, const char *root, char *out, usize cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!path) return;
    usize rl = root ? strlen(root) : 0;
    const char *rel = path;
    if (rl && strncmp(path, root, rl) == 0) { rel = path + rl; while (*rel == '/') rel++; }
    const char *slash = strrchr(rel, '/');
    if (!slash) return;                       /* file directly in root */
    usize len = (usize)(slash - rel);
    if (len >= cap) len = cap - 1;
    memcpy(out, rel, len);
    out[len] = '\0';
}

/* Union-find with path halving. */
static i32 uf_find(i32 *p, i32 x) {
    while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
    return x;
}

/* Label each node with its connected-component id (compacted to
 * 0..component_count-1) and the component's node count. Components are computed
 * over the undirected `edges`, so a hub note and everything linked to it (even
 * transitively, including via a shared #tag node) share one id — that's the
 * "topic" the view colours. Isolated notes form size-1 components. */
static void assign_components(MdGraph *g) {
    g->component_count = 0;
    if (g->node_count <= 0) return;
    i32 n = g->node_count;
    i32 *parent = malloc((usize)n * sizeof(i32));
    i32 *remap  = malloc((usize)n * sizeof(i32));
    i32 *sizes  = calloc((usize)n, sizeof(i32));
    if (!parent || !remap || !sizes) {
        for (i32 i = 0; i < n; i++) { g->nodes[i].component = -1; g->nodes[i].comp_size = 1; }
        free(parent); free(remap); free(sizes);
        return;
    }
    for (i32 i = 0; i < n; i++) { parent[i] = i; remap[i] = -1; }
    for (i32 e = 0; e < g->edge_count; e++) {
        i32 a = g->edges[e].a, b = g->edges[e].b;
        if (a < 0 || a >= n || b < 0 || b >= n) continue;
        i32 ra = uf_find(parent, a), rb = uf_find(parent, b);
        if (ra != rb) parent[ra] = rb;
    }
    i32 cc = 0;
    for (i32 i = 0; i < n; i++) {
        i32 root = uf_find(parent, i);
        if (remap[root] < 0) remap[root] = cc++;
        g->nodes[i].component = remap[root];
        sizes[remap[root]]++;
    }
    for (i32 i = 0; i < n; i++)
        g->nodes[i].comp_size = sizes[g->nodes[i].component];
    g->component_count = cc;
    free(parent); free(remap); free(sizes);
}

/* Tag each file node with a folder-group id (dedup of its relative parent dir).
 * Tag pseudo-nodes get -1 (they link across folders, so they float free). */
static void assign_groups(MdGraph *g, const char *root) {
    g->group_count = 0;
    g->group_names = calloc((usize)(g->node_count > 0 ? g->node_count : 1),
                            sizeof(*g->group_names));
    for (i32 i = 0; i < g->node_count; i++) {
        if (g->nodes[i].is_tag || !g->group_names) { g->nodes[i].group = -1; continue; }
        char dir[256];
        rel_parent_dir(g->nodes[i].path, root, dir, sizeof dir);
        i32 gi = -1;
        for (i32 k = 0; k < g->group_count; k++)
            if (strcmp(g->group_names[k], dir) == 0) { gi = k; break; }
        if (gi < 0 && g->group_count < g->node_count) {
            gi = g->group_count++;
            snprintf(g->group_names[gi], sizeof g->group_names[gi], "%s", dir);
        }
        g->nodes[i].group = gi;
    }

    /* Centroid cache, sized once with the groups. If any of the three fails
     * to allocate the readers fall back to reporting no centroid, which only
     * costs the folder labels — never a wrong position. */
    if (g->group_count > 0) {
        usize gc = (usize)g->group_count;
        g->group_cx = calloc(gc, sizeof *g->group_cx);
        g->group_cy = calloc(gc, sizeof *g->group_cy);
        g->group_n  = calloc(gc, sizeof *g->group_n);
        if (!g->group_cx || !g->group_cy || !g->group_n) {
            free(g->group_cx); free(g->group_cy); free(g->group_n);
            g->group_cx = g->group_cy = NULL;
            g->group_n = NULL;
        }
    }
}

MdGraph *md_graph_build(const char *root_dir) {
    return md_graph_build_ex(root_dir, NULL);
}

MdGraph *md_graph_build_ex(const char *root_dir, const _Atomic bool *cancel) {
    if (!root_dir || !*root_dir) return NULL;
    MdGraph *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    if (!graph_reserve_node(g)) { free(g); return NULL; }

    /* Resolve the scan policy once: the scan root's device (so the scan does not
     * cross into mounted network/removable/system volumes) and the real inodes
     * of the privacy-sensitive $HOME folders (so they're skipped by identity,
     * immune to firmlink/symlink/NFC path-spelling differences). */
    GraphScanCtx sctx; memset(&sctx, 0, sizeof sctx);
    sctx.cancel = cancel;
    {
        struct stat rst;
        if (stat(root_dir, &rst) == 0) { sctx.root_dev = rst.st_dev; sctx.have_root_dev = true; }
        const char *home = getenv("HOME");
        if (home && *home) {
            static const char *prot[] = {
                /* macOS TCC-protected per-user folders FIRST: recursing from
                 * $HOME into ~/Documents, ~/Desktop, ~/Downloads would fire a
                 * privacy permission prompt for each. Pinned by identity so
                 * they're skipped as children — an explicit graph rooted AT one
                 * of them still scans it. */
                "Documents", "Desktop", "Downloads",
                "Pictures", "Music", "Movies", "Public", "Library", "Applications", NULL
            };
            char p[1024];
            for (i32 i = 0; prot[i]; i++) {
                if (graph_join_path(p, sizeof p, home, prot[i]))
                    scan_ctx_pin(&sctx, p);
            }
        }
    }
    /* Smart module/build-dir exclusion: honour the project's own .gitignore so a
     * "Graph Current Folder" of a code tree doesn't graph node_modules / build /
     * dist / target / build-* etc. */
    scan_ctx_load_gitignore(&sctx, root_dir);
    scan_dir(g, root_dir, 0, &sctx);
    if (graph_cancelled(cancel) || g->node_count == 0) { md_graph_free(g); return NULL; }

    /* Collect edges by parsing each file. Both vectors grow as links are
     * found; the directed one becomes the graph's backlink table. */
    GraphEdgeVec raw = {0};
    GraphEdgeVec dir = {0};
    /* Snapshot the file-node count: walk_blocks appends tag pseudo-nodes as it
     * runs, and those have no file to parse. */
    i32 file_count = g->node_count;
    for (i32 i = 0; i < file_count && raw.len < MD_GRAPH_MAX_EDGES &&
                    !graph_cancelled(cancel); i++) {
        FILE *f = fopen(g->nodes[i].path, "rb");
        if (!f) continue;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
        long sz = ftell(f);
        if (sz <= 0 || (usize)sz > MD_GRAPH_MAX_FILE_BYTES ||
            fseek(f, 0, SEEK_SET) != 0) { fclose(f); continue; }
        u8 *src = malloc((usize)sz);
        if (!src) { fclose(f); continue; }
        usize rd = fread(src, 1, (usize)sz, f);
        bool read_ok = rd == (usize)sz && !ferror(f);
        fclose(f);
        if (!read_ok) { free(src); continue; }
        usize arena_cap;
        if (!checked_mul_usize(rd, 4u, &arena_cap) ||
            !checked_add_usize(arena_cap, 64u * 1024u, &arena_cap)) {
            free(src);
            continue;
        }
        /* A parse that did not fit (MdDoc.incomplete) has silently dropped
         * blocks — and their links. Retried once with the reader's worst-case
         * formula, bounded: the graph only needs the link inlines, but it
         * needs all of them. */
        for (i32 attempt = 0; attempt < 2; attempt++) {
            if (attempt == 1) {
                usize worst = rd * 8 + (rd / 2) * (sizeof(MdInline) + sizeof(MdBlock));
                if (worst > 64u * 1024u * 1024u) worst = 64u * 1024u * 1024u;
                if (worst <= arena_cap) break;
                arena_cap = worst;
            }
            Arena arena = arena_create(arena_cap);
            if (!arena.base) break;
            MdDoc *doc = md_parse_ex(&arena, src, rd, NULL, cancel);
            bool limited = doc && doc->parse_limited;
            bool complete = doc && !doc->incomplete;
            if (complete) walk_blocks(g, doc->blocks, doc->block_count, i, &raw, &dir);
            arena_destroy(&arena);
            if (complete || !doc || limited) break;
        }
        free(src);
    }
    g->dir_edges = dir.data;
    g->dir_edge_count = dir.len;
    g->dir_edge_cap = dir.cap;

    /* Dedup edges (sort + unique) and accumulate degree. */
    i32 ne = raw.len;
    if (ne > 0) {
        qsort(raw.data, (usize)ne, sizeof(MdGraphEdge), edge_cmp);
        g->edges = calloc((usize)ne, sizeof(MdGraphEdge));
        if (g->edges) {
            for (i32 i = 0; i < ne; i++) {
                if (i > 0 && raw.data[i].a == raw.data[i-1].a && raw.data[i].b == raw.data[i-1].b) continue;
                g->edges[g->edge_count++] = raw.data[i];
                g->nodes[raw.data[i].a].degree++;
                g->nodes[raw.data[i].b].degree++;
            }
        }
    }
    free(raw.data);

    /* Tag every node with its folder cluster (after tag pseudo-nodes exist). */
    assign_groups(g, root_dir);
    /* Connected components for topic-cluster colouring (needs final edges). */
    assign_components(g);

    seed_positions(g);
    graph_recompute_centroids(g);   /* the first force pass reads these */
    g->temperature = 1.0f;
    g->settled = false;
    g->f_center = g->f_repel = g->f_link = g->f_linkdist = g->node_size = 1.0f;
    g->world_radius = 64.0f * sqrtf((f32)g->node_count) + 80.0f;

    /* Warm-start: settle the layout HERE, on the (background) build thread, so
     * the view opens to an already-arranged graph instead of playing the spring
     * expansion from the tight seed on screen, with the camera re-fitting every
     * frame. The first frame shows the settled result; md_graph_step then
     * immediately reports settled, so the per-frame re-fit never fires and the
     * camera fits exactly once. The cap is a safety bound; normal vaults settle
     * in ~200 steps. */
    for (i32 i = 0; i < 1200 && !graph_cancelled(cancel) &&
                    md_graph_step(g, 1.0f / 60.0f); i++) { }
    if (graph_cancelled(cancel)) { md_graph_free(g); return NULL; }
    /* The graph is charged to MEM_GRAPH: its node and edge arrays plus the
     * per-group folder tables. TWO graphs can be alive at once while a
     * rebuild runs behind the visible one.
     *
     * transient_adjust, not set: md_graph_build_ex runs on a WORKER (that is
     * what the cancel flag is for) while the old graph is freed on the main
     * thread, and mem_budget_set is main-thread-only. */
    /* Topology is immutable after publication. Build at bounded capacity so
     * scanners can keep node references, then retain only the finished graph.
     * Failed shrink attempts keep both the old allocation and its charge. */
    usize node_bytes = (usize)g->node_cap * sizeof(MdGraphNode);
    usize edge_bytes = (usize)ne * sizeof(MdGraphEdge);
    usize dir_bytes = (usize)g->dir_edge_cap * sizeof(MdGraphEdge);
    usize group_bytes = (usize)(g->node_count > 0 ? g->node_count : 1) * sizeof *g->group_names;
#define GRAPH_COMPACT(field, count, bytes) do { \
    usize used = (usize)(count) * sizeof *g->field; \
    if (!used) { free(g->field); g->field = NULL; (bytes) = 0; } \
    else { void *next = realloc(g->field, used); \
        if (next) { g->field = next; (bytes) = used; } } \
} while (0)
    GRAPH_COMPACT(nodes, g->node_count, node_bytes);
    GRAPH_COMPACT(edges, g->edge_count, edge_bytes);
    GRAPH_COMPACT(dir_edges, g->dir_edge_count, dir_bytes);
    GRAPH_COMPACT(group_names, g->group_count, group_bytes);
#undef GRAPH_COMPACT
    /* Topology is final: the capacities now describe the compacted arrays. */
    g->node_cap = g->nodes ? g->node_count : 0;
    g->dir_edge_cap = g->dir_edges ? g->dir_edge_count : 0;
    g->charged_bytes = node_bytes + edge_bytes + dir_bytes + group_bytes +
        (usize)g->group_count * (sizeof *g->group_cx + sizeof *g->group_cy +
                                 sizeof *g->group_n) +
        sizeof *g;
    mem_budget_transient_adjust(MEM_GRAPH, (isize)g->charged_bytes);
    return g;
}

void md_graph_free(MdGraph *g) {
    if (!g) return;
    /* Credits exactly what the build charged; a graph freed on an error path
     * before the charge credits nothing, because charged_bytes is still 0. */
    if (g->charged_bytes)
        mem_budget_transient_adjust(MEM_GRAPH, -(isize)g->charged_bytes);
    free(g->nodes);
    free(g->edges);
    free(g->dir_edges);
    free(g->group_names);
    free(g->group_cx);
    free(g->group_cy);
    free(g->group_n);
    free(g);
}

i32 md_graph_node_count(const MdGraph *g) { return g ? g->node_count : 0; }
i32 md_graph_edge_count(const MdGraph *g) { return g ? g->edge_count : 0; }

/* Find the file node for an absolute path by its match key (lowercased
 * basename without extension) — robust to the path-format (NFC vs readdir NFD)
 * and directory differences a byte-exact path compare would trip on. -1 if
 * none. */
i32 md_graph_find_by_path(const MdGraph *g, const char *abs_path) {
    if (!g || !abs_path) return -1;
    const char *base = strrchr(abs_path, '/');
    base = base ? base + 1 : abs_path;
    char key[128]; usize k = 0;
    for (const char *p = base; *p && k < sizeof key - 1; p++) {
        if (*p == '.') {  /* stop at extension if it's the trailing one */
            if (strcasecmp(p, ".md") == 0 || strcasecmp(p, ".markdown") == 0) break;
        }
        key[k++] = (char)tolower((unsigned char)*p);
    }
    key[k] = '\0';
    for (i32 i = 0; i < g->node_count; i++) {
        if (g->nodes[i].is_tag) continue;
        if (strcmp(g->nodes[i].key, key) == 0) return i;
    }
    return -1;
}

/* Directed file→file edges (src=a, dst=b) for backlinks. */
i32 md_graph_dir_edge_count(const MdGraph *g) { return g ? g->dir_edge_count : 0; }
void md_graph_dir_edge(const MdGraph *g, i32 idx, i32 *out_src, i32 *out_dst) {
    if (!g || idx < 0 || idx >= g->dir_edge_count) { if (out_src) *out_src = -1; if (out_dst) *out_dst = -1; return; }
    if (out_src) *out_src = g->dir_edges[idx].a;
    if (out_dst) *out_dst = g->dir_edges[idx].b;
}
bool md_graph_settled(const MdGraph *g)   { return g ? g->settled : true; }
f32 md_graph_world_radius(const MdGraph *g) { return g ? g->world_radius : 1.0f; }
void md_graph_reheat(MdGraph *g) { if (g) { g->temperature = 0.6f; g->settled = false; } }

void md_graph_apply_params(MdGraph *g, f32 center, f32 repel, f32 link,
                           f32 linkdist, f32 node_size) {
    if (!g) return;
    g->f_center   = isfinite(center)   ? fmaxf(0.0f, fminf(center,   4.0f)) : 1.0f;
    g->f_repel    = isfinite(repel)    ? fmaxf(0.0f, fminf(repel,    4.0f)) : 1.0f;
    g->f_link     = isfinite(link)     ? fmaxf(0.0f, fminf(link,     4.0f)) : 1.0f;
    g->f_linkdist = isfinite(linkdist) ? fmaxf(0.05f, fminf(linkdist, 4.0f)) : 1.0f;
    g->node_size  = isfinite(node_size)? fmaxf(0.05f, fminf(node_size,4.0f)) : 1.0f;
}

const char *md_graph_node_label(const MdGraph *g, i32 i) {
    return (g && i >= 0 && i < g->node_count) ? g->nodes[i].label : "";
}
const char *md_graph_node_path(const MdGraph *g, i32 i) {
    return (g && i >= 0 && i < g->node_count) ? g->nodes[i].path : "";
}
i32 md_graph_node_degree(const MdGraph *g, i32 i) {
    return (g && i >= 0 && i < g->node_count) ? g->nodes[i].degree : 0;
}
i32 md_graph_node_component(const MdGraph *g, i32 i) {
    return (g && i >= 0 && i < g->node_count) ? g->nodes[i].component : -1;
}
i32 md_graph_node_comp_size(const MdGraph *g, i32 i) {
    return (g && i >= 0 && i < g->node_count) ? g->nodes[i].comp_size : 1;
}
bool md_graph_node_is_tag(const MdGraph *g, i32 i) {
    return (g && i >= 0 && i < g->node_count) ? g->nodes[i].is_tag : false;
}

i32 md_graph_node_group(const MdGraph *g, i32 i) {
    return (g && i >= 0 && i < g->node_count) ? g->nodes[i].group : -1;
}
i32 md_graph_group_count(const MdGraph *g) { return g ? g->group_count : 0; }
const char *md_graph_group_name(const MdGraph *g, i32 gi) {
    return (g && g->group_names && gi >= 0 && gi < g->group_count) ? g->group_names[gi] : "";
}
/* One O(node_count) pass over every group. Called from the four places that
 * move a node, so the cache is never stale and the readers below are O(1). */
static void graph_recompute_centroids(MdGraph *g) {
    if (!g || g->group_count <= 0 || !g->group_cx || !g->group_cy || !g->group_n)
        return;
    usize gc = (usize)g->group_count;
    memset(g->group_cx, 0, gc * sizeof *g->group_cx);
    memset(g->group_cy, 0, gc * sizeof *g->group_cy);
    memset(g->group_n,  0, gc * sizeof *g->group_n);
    for (i32 i = 0; i < g->node_count; i++) {
        i32 gi = g->nodes[i].group;
        if (gi < 0 || gi >= g->group_count) continue;
        g->group_cx[gi] += g->nodes[i].x;
        g->group_cy[gi] += g->nodes[i].y;
        g->group_n[gi]++;
    }
    for (i32 gi = 0; gi < g->group_count; gi++) {
        if (g->group_n[gi] <= 0) continue;
        g->group_cx[gi] /= (f32)g->group_n[gi];
        g->group_cy[gi] /= (f32)g->group_n[gi];
    }
}

/* World-space centroid of a folder cluster (mean of its node positions). */
bool md_graph_group_centroid(const MdGraph *g, i32 gi, f32 *wx, f32 *wy) {
    if (!g || gi < 0 || gi >= g->group_count) return false;
    if (!g->group_n || g->group_n[gi] <= 0) return false;
    if (wx) *wx = g->group_cx[gi];
    if (wy) *wy = g->group_cy[gi];
    return true;
}

/* --------------------------------------------------------------- layout */

/* One anti-overlap sweep: a temperature-INDEPENDENT position correction so
 * node circles never interpenetrate. It edits positions, not velocities, so
 * it adds nothing to the kinetic settle test. Pinned nodes hold position but
 * still shove their neighbours aside. Returns the largest correction applied
 * (world units) so callers can iterate until the residual is small. */
typedef struct {
    f32 x;
    i32 idx;
} GraphSweepNode;

static int graph_sweep_node_cmp(const void *ap, const void *bp) {
    const GraphSweepNode *a = ap, *b = bp;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return 1;
    return a->idx - b->idx; /* deterministic tie break */
}

static i32 graph_gcd(i32 a, i32 b) {
    while (b) { i32 t = a % b; a = b; b = t; }
    return a < 0 ? -a : a;
}

static f32 graph_anti_overlap_pass(MdGraph *g, const f32 *rw, f32 nsize) {
    MdGraphNode *nd = g->nodes;
    const i32 n = g->node_count;
    f32 maxcorr = 0.0f;
    GraphSweepNode *sweep = NULL;
    f32 max_sep = 0.0f;

    /* Dense vault graphs made this second O(n^2) loop as expensive as the
     * force calculation. Sort on x and stop once two nodes are farther apart
     * than the largest possible collision radius. Up to 512 nodes this is an
     * exact sweep. Above that, inspect the nearest 32 x-neighbours per node;
     * repeated layout steps separate dense runs without restoring quadratic
     * work for a 1k-10k note vault. */
    if (n > 256) {
        sweep = malloc((usize)n * sizeof(*sweep));
        if (sweep) {
            f32 max_r = 0.0f;
            for (i32 i = 0; i < n; i++) {
                sweep[i] = (GraphSweepNode){nd[i].x, i};
                if (rw[i] > max_r) max_r = rw[i];
            }
            max_sep = max_r * 2.0f * nsize * 1.6f + 14.0f;
            qsort(sweep, (usize)n, sizeof(*sweep), graph_sweep_node_cmp);
        }
    }

    for (i32 si = 0; si < n; si++) {
        i32 i = sweep ? sweep[si].idx : si;
        i32 sj_end = sweep && n > 512 ? si + 33 : n;
        if (sj_end > n) sj_end = n;
        for (i32 sj = si + 1; sj < sj_end; sj++) {
            i32 j = sweep ? sweep[sj].idx : sj;
            if (sweep && sweep[sj].x - sweep[si].x >= max_sep) break;
            f32 dx = nd[i].x - nd[j].x, dy = nd[i].y - nd[j].y;
            f32 d2 = dx*dx + dy*dy;
            f32 sep = (rw[i] + rw[j]) * nsize * 1.6f + 14.0f;
            if (d2 >= sep*sep) continue;
            f32 ux, uy, d;
            if (d2 < 1e-6f) {
                /* Deterministic unit-vector nudge so a coincident pair always
                 * separates (no zero residual that would stick them together). */
                f32 ang = (f32)(((i*131 + j*17) & 1023)) * (6.2831853f / 1024.0f);
                ux = cosf(ang); uy = sinf(ang); d = 0.0f;
            } else {
                d = sqrtf(d2); ux = dx / d; uy = dy / d;
            }
            f32 corr = (sep - d) * 0.5f;
            if (corr > 8.0f) corr = 8.0f;        /* cap per-frame so it stays calm */
            if (corr > maxcorr) maxcorr = corr;
            if (!nd[i].pinned) { nd[i].x += ux*corr; nd[i].y += uy*corr; }
            if (!nd[j].pinned) { nd[j].x -= ux*corr; nd[j].y -= uy*corr; }
        }
    }
    free(sweep);
    return maxcorr;
}

/* One fixed 1/60 s simulation tick (forces + integration + anti-overlap).
 * Callers go through md_graph_step, which owns the dt accumulation. */
static bool md_graph_substep(MdGraph *g) {
    if (!g || g->settled || g->node_count == 0) return false;
    const i32 n = g->node_count;
    /* Force multipliers (1.0 == default); clamp to a sane envelope so the
     * controls panel can't blow the simulation up. linkdist scales the rest
     * length; repulsion keeps its own baseline so the two are independent. */
    f32 m_center = g->f_center   > 0 ? g->f_center   : 1.0f;
    f32 m_repel  = g->f_repel    > 0 ? g->f_repel    : 0.0f;  /* 0 allowed */
    f32 m_link   = g->f_link     > 0 ? g->f_link     : 0.0f;  /* 0 allowed */
    f32 m_dist   = g->f_linkdist > 0 ? g->f_linkdist : 1.0f;
    if (m_repel  > 4.0f) m_repel  = 4.0f;
    if (m_link   > 4.0f) m_link   = 4.0f;
    if (m_center > 4.0f) m_center = 4.0f;
    if (m_dist   > 4.0f) m_dist   = 4.0f;
    const f32 REST = 64.0f * m_dist; /* ideal edge length (room for labels) */
    const f32 REPULSION = (64.0f * 64.0f * 3.0f) * m_repel;
    const f32 SPRING = 0.085f * m_link;  /* edge pull → connected notes cluster */
    const f32 CENTER = 0.008f * m_center; /* gentle gravity toward origin */
    const f32 DAMP = 0.86f;
    const f32 MAXV = 64.0f * 1.5f;
    /* Drawn-radius scale (mirrors md_graph_node_screen). Folded into the
     * anti-overlap separation below so layout spacing tracks the visible node
     * size and circles do not interpenetrate at large node_size. */
    const f32 NSIZE = (g->node_size > 0) ? g->node_size : 1.0f;

    /* The scratch also carries each node's display radius (rw) for the
     * anti-overlap pass. */
    f32 *fx = calloc((usize)n * 3, sizeof(f32));
    if (!fx) { g->settled = true; return false; }
    f32 *fy = fx + n;
    f32 *rw = fx + 2 * n;            /* per-node world radius (matches node_screen) */

    MdGraphNode *nd = g->nodes;
    /* Display radius mirrors md_graph_node_screen; degree is capped at 24 so a
     * 50-link MOC doesn't fling its leaves offscreen via the repulsion weight. */
    for (i32 i = 0; i < n; i++) {
        i32 dg = nd[i].degree; if (dg > 24) dg = 24;
        rw[i] = 3.0f + 4.2f * sqrtf((f32)dg);
    }
    /* Exact all-pairs repulsion is useful for small graphs, but at vault scale
     * it is quadratic per fixed step (1500 nodes => 1.12 million pairs, before
     * overlap relaxation). Above 512 nodes sample a deterministic 64 peers per
     * node and scale their contribution. The stride is odd and derived from
     * the node index, so successive nodes do not see the same subset and the
     * result remains stable across runs. Complexity is bounded at O(n*64),
     * while small graphs retain bit-for-bit pair traversal. */
    const i32 repel_samples = (n > 512) ? 64 : n;
    for (i32 i = 0; i < n; i++) {
        /* sqrt(min(deg,24)) folded out of the j-loop: (rw-3)/4.2 == that sqrt. */
        f32 rwi = 1.0f + 0.5f * (rw[i] - 3.0f) / 4.2f;
        if (repel_samples < n) {
            i32 stride = ((i * 2 + 1) % n) | 1;
            while (graph_gcd(stride, n) != 1) {
                stride += 2;
                if (stride >= n) stride = 1;
            }
            i32 j = (i + 1) % n;
            f32 sample_scale = (f32)(n - 1) / (f32)repel_samples;
            for (i32 s = 0; s < repel_samples; s++, j = (j + stride) % n) {
                if (j == i) j = (j + 1) % n;
                f32 dx = nd[i].x - nd[j].x;
                f32 dy = nd[i].y - nd[j].y;
                f32 d2 = dx*dx + dy*dy;
                if (d2 < 0.01f) {
                    f32 ang = (f32)(((i*131 + j*17) & 1023)) * (6.2831853f / 1024.0f);
                    dx = cosf(ang); dy = sinf(ang); d2 = 1.0f;
                }
                f32 d = sqrtf(d2);
                f32 rep = REPULSION / d2;
                rep *= rwi * (1.0f + 0.5f * (rw[j] - 3.0f) / 4.2f);
                if (nd[i].group >= 0 && nd[j].group >= 0)
                    rep *= (nd[i].group == nd[j].group) ? 0.85f : 1.15f;
                fx[i] += (dx / d) * rep * sample_scale;
                fy[i] += (dy / d) * rep * sample_scale;
            }
        } else for (i32 j = i + 1; j < n; j++) {
            f32 dx = nd[i].x - nd[j].x;
            f32 dy = nd[i].y - nd[j].y;
            f32 d2 = dx*dx + dy*dy;
            if (d2 < 0.01f) {
                /* Deterministic unit-vector nudge so a coincident pair always
                 * separates (no zero residual that would stick them together). */
                f32 ang = (f32)(((i*131 + j*17) & 1023)) * (6.2831853f / 1024.0f);
                dx = cosf(ang); dy = sinf(ang); d2 = 1.0f;
            }
            f32 inv = 1.0f / d2;
            f32 d = sqrtf(d2);
            f32 rep = REPULSION * inv;
            /* Degree-weight: visually larger (higher-degree) nodes claim more
             * room, so hubs don't get buried under their leaves. */
            rep *= rwi * (1.0f + 0.5f * (rw[j] - 3.0f) / 4.2f);
            /* Sharpen folder clusters: pack same-folder nodes tighter, push
             * different folders apart (tags, group<0, stay neutral). */
            if (nd[i].group >= 0 && nd[j].group >= 0)
                rep *= (nd[i].group == nd[j].group) ? 0.85f : 1.15f;
            f32 ux = dx / d, uy = dy / d;
            fx[i] += ux*rep; fy[i] += uy*rep;
            fx[j] -= ux*rep; fy[j] -= uy*rep;
        }
        fx[i] -= nd[i].x * CENTER;
        fy[i] -= nd[i].y * CENTER;
    }
    for (i32 e = 0; e < g->edge_count; e++) {
        i32 a = g->edges[e].a, b = g->edges[e].b;
        f32 dx = nd[a].x - nd[b].x, dy = nd[a].y - nd[b].y;
        f32 d = sqrtf(dx*dx + dy*dy) + 0.01f;
        f32 att = (d - REST) * SPRING;
        f32 ux = dx / d, uy = dy / d;
        fx[a] -= ux*att; fy[a] -= uy*att;
        fx[b] += ux*att; fy[b] += uy*att;
    }

    /* Folder cohesion: pull each node toward its folder's centroid so same-dir
     * files gather into a clean cluster (combined with the group-aware
     * repulsion above, distinct folders settle into separate blobs). */
    if (g->group_count > 0 && g->group_cx && g->group_n) {
        /* The cache is current: nothing has moved since the last recompute,
         * which the previous sub-step (or the seed) ended with. This also
         * retires a calloc/free pair that ran once per SUB-STEP. */
        const f32 COHESION = 0.025f;
        for (i32 i = 0; i < n; i++) {
            i32 gi = nd[i].group;
            if (gi < 0 || gi >= g->group_count || g->group_n[gi] <= 0) continue;
            fx[i] += (g->group_cx[gi] - nd[i].x) * COHESION;
            fy[i] += (g->group_cy[gi] - nd[i].y) * COHESION;
        }
    }

    f32 temp = g->temperature;
    f32 maxke = 0.0f;
    for (i32 i = 0; i < n; i++) {
        if (nd[i].pinned) continue;
        nd[i].vx = (nd[i].vx + fx[i] * temp) * DAMP;
        nd[i].vy = (nd[i].vy + fy[i] * temp) * DAMP;
        f32 v2 = nd[i].vx*nd[i].vx + nd[i].vy*nd[i].vy;
        if (v2 > MAXV*MAXV) { f32 s = MAXV / sqrtf(v2); nd[i].vx *= s; nd[i].vy *= s; }
        nd[i].x += nd[i].vx;
        nd[i].y += nd[i].vy;
        maxke = fmaxf(maxke, v2);
    }

    /* Anti-overlap: guarantees separation without feeding the kinetic settle
     * test (it moves positions, not velocities). */
    f32 corr = graph_anti_overlap_pass(g, rw, NSIZE);

    g->temperature *= 0.985f;
    bool done = (g->temperature < 0.05f || maxke < 0.02f);
    if (done && corr > 0.3f) {
        /* The kinetic test can fire while circles still interpenetrate (the
         * per-sweep correction is capped at 8, and corrections never count
         * toward maxke), which would freeze a dense cluster mid-overlap until
         * the next reheat. Relax the residual with bounded extra sweeps so
         * "settled" also means "separated"; the bound guarantees termination
         * even if full separation is geometrically impossible. */
        i32 relax_cap = (n > 512) ? 6 : 50;
        for (i32 k = 0; k < relax_cap && corr > 0.3f; k++)
            corr = graph_anti_overlap_pass(g, rw, NSIZE);
    }
    free(fx);

    /* Positions have moved (integrate + anti-overlap), so the folder centroids
     * the label pass and the next sub-step's cohesion read are stale. */
    graph_recompute_centroids(g);

    /* Extents (and world_radius for auto-fit) recomputed AFTER anti-overlap,
     * since that pass moved nodes. */
    f32 minx=1e9f, miny=1e9f, maxx=-1e9f, maxy=-1e9f;
    for (i32 i = 0; i < n; i++) {
        minx = fminf(minx, nd[i].x); maxx = fmaxf(maxx, nd[i].x);
        miny = fminf(miny, nd[i].y); maxy = fmaxf(maxy, nd[i].y);
    }
    f32 ex = fmaxf(fabsf(minx), fabsf(maxx));
    f32 ey = fmaxf(fabsf(miny), fabsf(maxy));
    g->world_radius = fmaxf(40.0f, fmaxf(ex, ey) + REST);

    if (done) { g->settled = true; return false; }
    return true;
}

/* Advance the layout by `dt` seconds of wall-clock time, integrating in fixed
 * 1/60 s sub-steps. The accumulator makes layout speed independent of the
 * display's frame rate (one tick per display frame would run the physics
 * twice as fast on a 120 Hz panel as on a 60 Hz one); the sub-step cap keeps
 * a long stall (window drag, permission prompt) from spiralling into an
 * O(n^2) catch-up burst. */
bool md_graph_step(MdGraph *g, f32 dt) {
    if (!g || g->settled || g->node_count == 0) return false;
    const f32 STEP = 1.0f / 60.0f;
    if (!isfinite(dt) || dt <= 0.0f) dt = STEP;
    if (dt > 0.1f) dt = 0.1f;              /* clamp huge stalls */
    g->step_accum += dt;
    i32 sub = 0;
    while (g->step_accum >= STEP && sub < 4) {
        g->step_accum -= STEP;
        sub++;
        if (!md_graph_substep(g)) { g->step_accum = 0.0f; return false; }
    }
    if (sub == 4) g->step_accum = 0.0f;    /* drop backlog, don't spiral */
    return true;                           /* still moving (not settled) */
}

/* --------------------------------------------------------------- transform */

void md_graph_node_screen(const MdGraph *g, i32 idx, i32 w, i32 h,
                          f32 pan_x, f32 pan_y, f32 zoom,
                          f32 *sx, f32 *sy, f32 *radius) {
    if (!g || idx < 0 || idx >= g->node_count ||
        !isfinite(pan_x) || !isfinite(pan_y) || !isfinite(zoom)) {
        if (sx)*sx=0; if(sy)*sy=0; if(radius)*radius=0; return;
    }
    const MdGraphNode *n = &g->nodes[idx];
    if (sx) *sx = (f32)w * 0.5f + pan_x + n->x * zoom;
    if (sy) *sy = (f32)h * 0.5f + pan_y + n->y * zoom;
    if (radius) {
        /* Hub/index ("folder") notes are noticeably larger than
         * leaves. Degree drives size on a sqrt curve so a high-degree note reads
         * as a clear anchor without a 30-link note dwarfing the canvas. */
        f32 nm = (g->node_size > 0) ? g->node_size : 1.0f;
        f32 base = (3.0f + 4.2f * sqrtf((f32)n->degree)) * nm;
        *radius = fmaxf(2.5f, base * fmaxf(0.6f, fminf(zoom, 2.6f)));
    }
}

void md_graph_screen_to_world(i32 w, i32 h, f32 pan_x, f32 pan_y, f32 zoom,
                              f32 sx, f32 sy, f32 *wx, f32 *wy) {
    if (!isfinite(pan_x) || !isfinite(pan_y) || !isfinite(zoom) ||
        !isfinite(sx) || !isfinite(sy)) {
        if (wx) *wx = 0.0f;
        if (wy) *wy = 0.0f;
        return;
    }
    if (zoom < 1e-4f) zoom = 1e-4f;
    if (wx) *wx = (sx - (f32)w * 0.5f - pan_x) / zoom;
    if (wy) *wy = (sy - (f32)h * 0.5f - pan_y) / zoom;
}

i32 md_graph_hit(const MdGraph *g, f32 sx, f32 sy, i32 w, i32 h,
                 f32 pan_x, f32 pan_y, f32 zoom) {
    if (!g || !isfinite(sx) || !isfinite(sy) || !isfinite(pan_x) ||
        !isfinite(pan_y) || !isfinite(zoom)) return -1;
    i32 best = -1; f32 bestd = 1e18f;
    for (i32 i = 0; i < g->node_count; i++) {
        f32 nx, ny, r;
        md_graph_node_screen(g, i, w, h, pan_x, pan_y, zoom, &nx, &ny, &r);
        f32 dx = sx - nx, dy = sy - ny;
        f32 d2 = dx*dx + dy*dy;
        f32 hitr = r + 4.0f;
        if (d2 <= hitr*hitr && d2 < bestd) { bestd = d2; best = i; }
    }
    return best;
}

void md_graph_set_node_world(MdGraph *g, i32 idx, f32 wx, f32 wy, bool pinned) {
    if (!g || idx < 0 || idx >= g->node_count || !isfinite(wx) || !isfinite(wy)) return;
    g->nodes[idx].x = wx; g->nodes[idx].y = wy;
    g->nodes[idx].vx = g->nodes[idx].vy = 0.0f;
    g->nodes[idx].pinned = pinned;
    graph_recompute_centroids(g);   /* a dragged node moves its folder's label */
}

/* --------------------------------------------------------------- accessors */

void md_graph_edge(const MdGraph *g, i32 idx, i32 *out_a, i32 *out_b) {
    if (!g || idx < 0 || idx >= g->edge_count) {
        if (out_a) *out_a = -1;
        if (out_b) *out_b = -1;
        return;
    }
    if (out_a) *out_a = g->edges[idx].a;
    if (out_b) *out_b = g->edges[idx].b;
}
