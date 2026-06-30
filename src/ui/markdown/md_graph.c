/*
 * Liu — Markdown knowledge-graph view (data + force layout). Rendering is
 * done on the GPU by the caller via md_graph accessors + renderer_draw_line /
 * rrect circles — no CPU rasterization.
 * See md_graph.h. Deliberately self-contained: no renderer/shader changes.
 */
#include "ui/markdown/md_graph.h"
#include "ui/markdown/md_parse.h"
#include "ui/markdown/md_ast.h"
#include "core/memory.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

struct MdGraph {
    MdGraphNode *nodes;
    i32          node_count;
    MdGraphEdge *edges;        /* undirected (a<b), for the graph view */
    i32          edge_count;
    /* Directed file→file edges (a=src, b=dst), un-normalized, for backlinks.
     * The undirected `edges` above lose direction, so this is kept separately. */
    MdGraphEdge *dir_edges;
    i32          dir_edge_count;

    f32   temperature;   /* cooling factor, 1 -> 0 */
    bool  settled;
    f32   world_radius;

    /* Folder clustering: each file node is tagged with a group id = the index
     * of its parent directory (relative to the build root) in group_names[].
     * Same-folder nodes attract toward their shared centroid; different folders
     * push apart — so the layout separates into clean per-folder clusters. */
    char (*group_names)[256];   /* relative parent dir per group ("" = root) */
    i32    group_count;

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
} GraphScanCtx;

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
static void scan_ctx_load_gitignore(GraphScanCtx *c, const char *root_dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/.gitignore", root_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) &&
           c->gi_n < (i32)(sizeof c->gi / sizeof c->gi[0])) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s);
        while (n && (s[n-1]=='\n' || s[n-1]=='\r' || s[n-1]==' ' || s[n-1]=='\t')) s[--n] = 0;
        if (!*s || *s == '#' || *s == '!') continue;     /* empty / comment / negation */
        bool was_dir = (n && s[n-1] == '/');
        while (n && s[n-1] == '/') s[--n] = 0;            /* strip trailing slashes */
        if (*s == '/') s++;                              /* strip leading anchor */
        char *slash = strrchr(s, '/');
        if (slash) s = slash + 1;                        /* keep the last component */
        if (!*s) continue;
        /* Bias to directory names: take a trailing-slash rule, or a bare name
         * with no '.' (so *.o / *.md file globs never skip a directory). */
        if (!was_dir && strchr(s, '.')) continue;
        if (strlen(s) >= sizeof c->gi[0]) continue;
        snprintf(c->gi[c->gi_n++], sizeof c->gi[0], "%s", s);
    }
    fclose(f);
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

/* Append a .md file node (path -> lowercased basename key). The caller
 * guarantees g->node_count < MD_GRAPH_MAX_NODES via the readdir while-loop. */
static void md_graph_add_file_node(MdGraph *g, const char *full) {
    MdGraphNode *n = &g->nodes[g->node_count++];
    memset(n, 0, sizeof *n);
    snprintf(n->path, sizeof n->path, "%s", full);
    base_no_ext(full, n->label, sizeof n->label);
    snprintf(n->key, sizeof n->key, "%s", n->label);
    str_lower(n->key);
}

static void scan_dir(MdGraph *g, const char *dir, i32 depth, const GraphScanCtx *ctx) {
    if (depth > MD_GRAPH_MAX_DEPTH || g->node_count >= MD_GRAPH_MAX_NODES) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && g->node_count < MD_GRAPH_MAX_NODES) {
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
                snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
                md_graph_add_file_node(g, full);
            }
            continue;
        }
        if (de->d_type == DT_DIR &&
            (is_skip_name(de->d_name) || scan_ctx_gitignored(ctx, de->d_name))) continue;
#endif
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
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
            md_graph_add_file_node(g, full);
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
    if (g->node_count >= MD_GRAPH_MAX_NODES) return -1;
    MdGraphNode *nd = &g->nodes[g->node_count];
    memset(nd, 0, sizeof *nd);
    nd->is_tag = true;
    snprintf(nd->label, sizeof nd->label, "#%.*s", (int)n, (const char *)name);
    snprintf(nd->key, sizeof nd->key, "%s", key);
    return g->node_count++;
}

static void walk_inlines(MdGraph *g, const MdInline *runs, u32 count,
                         i32 from, MdGraphEdge *edges, i32 *ne) {
    for (u32 i = 0; i < count; i++) {
        const MdInline *in = &runs[i];
        i32 to = -1;
        if (in->kind == MD_INLINE_LINK || in->kind == MD_INLINE_WIKILINK ||
            in->kind == MD_INLINE_EMBED) {
            if (in->url.len > 0) to = resolve_target(g, in->url.data, in->url.len);
        } else if (in->kind == MD_INLINE_TAG && in->text.len > 0) {
            to = graph_tag_node(g, in->text.data, in->text.len);
        }
        if (to >= 0 && to != from && *ne < MD_GRAPH_MAX_EDGES) {
            i32 a = from < to ? from : to;
            i32 b = from < to ? to : from;
            edges[*ne].a = a; edges[*ne].b = b; (*ne)++;
            /* Directed file→file edge (src=from, dst=to) for backlinks; tag
             * targets are excluded — backlinks are note-to-note only. */
            if (in->kind != MD_INLINE_TAG && to < g->node_count &&
                !g->nodes[to].is_tag && g->dir_edges &&
                g->dir_edge_count < MD_GRAPH_MAX_EDGES) {
                g->dir_edges[g->dir_edge_count].a = from;
                g->dir_edges[g->dir_edge_count].b = to;
                g->dir_edge_count++;
            }
        }
        if (in->children && in->child_count)
            walk_inlines(g, in->children, in->child_count, from, edges, ne);
    }
}

static void walk_blocks(MdGraph *g, const MdBlock *blocks, u32 count,
                        i32 from, MdGraphEdge *edges, i32 *ne) {
    for (u32 i = 0; i < count; i++) {
        const MdBlock *b = &blocks[i];
        if (b->inlines && b->inline_count)
            walk_inlines(g, b->inlines, b->inline_count, from, edges, ne);
        if (b->children && b->child_count)
            walk_blocks(g, b->children, b->child_count, from, edges, ne);
    }
}

static int edge_cmp(const void *pa, const void *pb) {
    const MdGraphEdge *a = pa, *b = pb;
    if (a->a != b->a) return a->a - b->a;
    return a->b - b->b;
}

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
    out[0] = '\0';
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
}

MdGraph *md_graph_build(const char *root_dir) {
    if (!root_dir || !*root_dir) return NULL;
    MdGraph *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->nodes = calloc(MD_GRAPH_MAX_NODES, sizeof(MdGraphNode));
    if (!g->nodes) { free(g); return NULL; }

    /* Resolve the scan policy once: the scan root's device (so we don't cross
     * into mounted network/removable/system volumes) and the real inodes of the
     * privacy-sensitive $HOME folders (so they're skipped by identity, immune to
     * firmlink/symlink/NFC path-spelling differences). */
    GraphScanCtx sctx; memset(&sctx, 0, sizeof sctx);
    {
        struct stat rst;
        if (stat(root_dir, &rst) == 0) { sctx.root_dev = rst.st_dev; sctx.have_root_dev = true; }
        const char *home = getenv("HOME");
        if (home && *home) {
            static const char *prot[] = {
                /* macOS TCC-protected per-user folders FIRST: a graph rooted at
                 * $HOME used to recurse into ~/Documents, ~/Desktop, ~/Downloads
                 * and fire a privacy permission prompt for each. Pinned by
                 * identity so they're skipped as children — an explicit graph
                 * rooted AT one of them still scans it. */
                "Documents", "Desktop", "Downloads",
                "Pictures", "Music", "Movies", "Public", "Library", "Applications", NULL
            };
            char p[1024];
            for (i32 i = 0; prot[i]; i++) {
                snprintf(p, sizeof p, "%s/%s", home, prot[i]);
                scan_ctx_pin(&sctx, p);
            }
        }
    }
    /* Smart module/build-dir exclusion: honour the project's own .gitignore so a
     * "Graph Current Folder" of a code tree doesn't graph node_modules / build /
     * dist / target / build-* etc. */
    scan_ctx_load_gitignore(&sctx, root_dir);
    scan_dir(g, root_dir, 0, &sctx);
    if (g->node_count == 0) { md_graph_free(g); return NULL; }

    /* Collect edges by parsing each file. */
    MdGraphEdge *raw = calloc(MD_GRAPH_MAX_EDGES, sizeof(MdGraphEdge));
    if (!raw) { md_graph_free(g); return NULL; }
    /* Directed edges accumulate straight into the graph during the walk. */
    g->dir_edges = calloc(MD_GRAPH_MAX_EDGES, sizeof(MdGraphEdge));
    g->dir_edge_count = 0;
    i32 ne = 0;
    /* Snapshot the file-node count: walk_blocks appends tag pseudo-nodes as it
     * runs, and those have no file to parse. */
    i32 file_count = g->node_count;
    for (i32 i = 0; i < file_count && ne < MD_GRAPH_MAX_EDGES; i++) {
        FILE *f = fopen(g->nodes[i].path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || (usize)sz > MD_GRAPH_MAX_FILE_BYTES) { fclose(f); continue; }
        u8 *src = malloc((usize)sz);
        if (!src) { fclose(f); continue; }
        usize rd = fread(src, 1, (usize)sz, f);
        fclose(f);
        Arena arena = arena_create((usize)rd * 4 + 64 * 1024);
        if (arena.base) {
            MdDoc *doc = md_parse(&arena, src, rd, NULL);
            if (doc) walk_blocks(g, doc->blocks, doc->block_count, i, raw, &ne);
            arena_destroy(&arena);
        }
        free(src);
    }

    /* Dedup edges (sort + unique) and accumulate degree. */
    if (ne > 0) {
        qsort(raw, (usize)ne, sizeof(MdGraphEdge), edge_cmp);
        g->edges = calloc((usize)ne, sizeof(MdGraphEdge));
        if (g->edges) {
            for (i32 i = 0; i < ne; i++) {
                if (i > 0 && raw[i].a == raw[i-1].a && raw[i].b == raw[i-1].b) continue;
                g->edges[g->edge_count++] = raw[i];
                g->nodes[raw[i].a].degree++;
                g->nodes[raw[i].b].degree++;
            }
        }
    }
    free(raw);

    /* Tag every node with its folder cluster (after tag pseudo-nodes exist). */
    assign_groups(g, root_dir);
    /* Connected components for topic-cluster colouring (needs final edges). */
    assign_components(g);

    seed_positions(g);
    g->temperature = 1.0f;
    g->settled = false;
    g->f_center = g->f_repel = g->f_link = g->f_linkdist = g->node_size = 1.0f;
    g->world_radius = 64.0f * sqrtf((f32)g->node_count) + 80.0f;

    /* Warm-start: settle the layout HERE, on the (background) build thread, so
     * the view opens to an already-arranged graph. Previously the violent spring
     * expansion from the tight seed played out in front of the user — nodes
     * flinging + the camera re-fitting every frame read as "the graph shakes
     * when it first opens". Doing it off-screen means the first frame shows the
     * settled result; md_graph_step then immediately reports settled, so the
     * per-frame re-fit never fires and the camera fits exactly once. The cap is
     * a safety bound; normal vaults settle in ~200 steps. */
    for (i32 i = 0; i < 1200 && md_graph_step(g, 1.0f / 60.0f); i++) { }
    return g;
}

void md_graph_free(MdGraph *g) {
    if (!g) return;
    free(g->nodes);
    free(g->edges);
    free(g->dir_edges);
    free(g->group_names);
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
    g->f_center   = center;
    g->f_repel    = repel;
    g->f_link     = link;
    g->f_linkdist = linkdist;
    g->node_size  = node_size;
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
/* World-space centroid of a folder cluster (mean of its node positions). */
bool md_graph_group_centroid(const MdGraph *g, i32 gi, f32 *wx, f32 *wy) {
    if (!g || gi < 0 || gi >= g->group_count) return false;
    f32 sx = 0, sy = 0; i32 c = 0;
    for (i32 i = 0; i < g->node_count; i++)
        if (g->nodes[i].group == gi) { sx += g->nodes[i].x; sy += g->nodes[i].y; c++; }
    if (c == 0) return false;
    if (wx) *wx = sx / (f32)c;
    if (wy) *wy = sy / (f32)c;
    return true;
}

/* --------------------------------------------------------------- layout */

bool md_graph_step(MdGraph *g, f32 dt) {
    (void)dt;
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
     * size — at large node_size the circles no longer interpenetrate. */
    const f32 NSIZE = (g->node_size > 0) ? g->node_size : 1.0f;

    /* O(n^2) repulsion is fine for the bounded node count. The scratch also
     * carries each node's display radius (rw) for the anti-overlap pass. */
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
    for (i32 i = 0; i < n; i++) {
        /* sqrt(min(deg,24)) folded out of the j-loop: (rw-3)/4.2 == that sqrt. */
        f32 rwi = 1.0f + 0.5f * (rw[i] - 3.0f) / 4.2f;
        for (i32 j = i + 1; j < n; j++) {
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
    if (g->group_count > 0) {
        f32 *gx = calloc((usize)g->group_count * 2, sizeof(f32));
        i32 *gc = calloc((usize)g->group_count, sizeof(i32));
        if (gx && gc) {
            f32 *gy = gx + g->group_count;
            for (i32 i = 0; i < n; i++) {
                i32 gi = nd[i].group;
                if (gi < 0) continue;
                gx[gi] += nd[i].x; gy[gi] += nd[i].y; gc[gi]++;
            }
            for (i32 gi = 0; gi < g->group_count; gi++)
                if (gc[gi] > 0) { gx[gi] /= (f32)gc[gi]; gy[gi] /= (f32)gc[gi]; }
            const f32 COHESION = 0.025f;
            for (i32 i = 0; i < n; i++) {
                i32 gi = nd[i].group;
                if (gi < 0) continue;
                fx[i] += (gx[gi] - nd[i].x) * COHESION;
                fy[i] += (gy[gi] - nd[i].y) * COHESION;
            }
        }
        free(gx); free(gc);
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

    /* Anti-overlap: a temperature-INDEPENDENT position correction so node
     * circles never interpenetrate into the "pink blob" the hairball used to
     * become. It edits positions, not velocities, so it adds nothing to maxke
     * — separation is guaranteed yet the kinetic settle test below still fires.
     * Pinned nodes hold position but still shove their neighbours aside. */
    for (i32 i = 0; i < n; i++) {
        for (i32 j = i + 1; j < n; j++) {
            f32 dx = nd[i].x - nd[j].x, dy = nd[i].y - nd[j].y;
            f32 d2 = dx*dx + dy*dy;
            f32 sep = (rw[i] + rw[j]) * NSIZE * 1.6f + 14.0f;
            if (d2 >= sep*sep) continue;
            f32 ux, uy, d;
            if (d2 < 1e-6f) {
                f32 ang = (f32)(((i*131 + j*17) & 1023)) * (6.2831853f / 1024.0f);
                ux = cosf(ang); uy = sinf(ang); d = 0.0f;
            } else {
                d = sqrtf(d2); ux = dx / d; uy = dy / d;
            }
            f32 corr = (sep - d) * 0.5f;
            if (corr > 8.0f) corr = 8.0f;        /* cap per-frame so it stays calm */
            if (!nd[i].pinned) { nd[i].x += ux*corr; nd[i].y += uy*corr; }
            if (!nd[j].pinned) { nd[j].x -= ux*corr; nd[j].y -= uy*corr; }
        }
    }
    free(fx);

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

    g->temperature *= 0.985f;
    if (g->temperature < 0.05f || maxke < 0.02f) { g->settled = true; return false; }
    return true;
}

/* --------------------------------------------------------------- transform */

void md_graph_node_screen(const MdGraph *g, i32 idx, i32 w, i32 h,
                          f32 pan_x, f32 pan_y, f32 zoom,
                          f32 *sx, f32 *sy, f32 *radius) {
    if (!g || idx < 0 || idx >= g->node_count) { if (sx)*sx=0; if(sy)*sy=0; if(radius)*radius=0; return; }
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
    if (zoom < 1e-4f) zoom = 1e-4f;
    if (wx) *wx = (sx - (f32)w * 0.5f - pan_x) / zoom;
    if (wy) *wy = (sy - (f32)h * 0.5f - pan_y) / zoom;
}

i32 md_graph_hit(const MdGraph *g, f32 sx, f32 sy, i32 w, i32 h,
                 f32 pan_x, f32 pan_y, f32 zoom) {
    if (!g) return -1;
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
    if (!g || idx < 0 || idx >= g->node_count) return;
    g->nodes[idx].x = wx; g->nodes[idx].y = wy;
    g->nodes[idx].vx = g->nodes[idx].vy = 0.0f;
    g->nodes[idx].pinned = pinned;
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

bool md_graph_connected(const MdGraph *g, i32 a, i32 b) {
    if (!g || a == b) return false;
    i32 lo = a < b ? a : b, hi = a < b ? b : a;
    for (i32 e = 0; e < g->edge_count; e++)
        if (g->edges[e].a == lo && g->edges[e].b == hi) return true;
    return false;
}
