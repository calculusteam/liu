/*
 * Liu — the diagram engine's insides, shared by its parts
 *
 * The parsed model, the parsers' line helpers and the draw list's emitters,
 * for the files that make up the engine (md_diagram*.c) and nothing else:
 * one diagram is built at a time, on one thread, into these globals.
 */
#ifndef UI_MARKDOWN_MD_DIAGRAM_INT_H
#define UI_MARKDOWN_MD_DIAGRAM_INT_H

#include "ui/markdown/md_diagram.h"

/* Hard caps. Every one of them truncates instead of growing — and anything
 * that would truncate *misleadingly* (a dropped node silently rewires the
 * graph, a dropped participant silently reassigns a message) raises g_trunc,
 * which makes the whole fence probe as NONE. Showing the reader their own
 * source is always better than showing them a diagram that is not theirs. */
#define DG_MAX_SRC      (32u * 1024u)   /* bigger fences stay code */
#define DG_MAX_LINES    2048u
#define DG_MAX_NODES    128
#define DG_MAX_EDGES    256
#define DG_MAX_PARTS    24
#define DG_MAX_MSGS     192             /* messages, notes and frame markers */
#define DG_MAX_GROUPS   16
#define DG_MAX_DEPTH    8
#define DG_MAX_BARS     48
#define DG_MAX_FRAMES   32
#define DG_MAX_DIVS     64
#define DG_MAX_SLICES   16
#define DG_MAX_LINKS    32              /* links per statement line */
#define DG_MAX_FAN      8               /* nodes on one side of `A & B -->` */
#define DG_MAX_ROWS     12              /* <br/> rows kept per label */
#define DG_MAX_RANK     127
#define DG_MIN_TEXT     3.5f            /* px cell below which labels are noise */
#define DG_MIN_SCALE    0.02f           /* fit-to-column floor; below it, NONE */
/* Baseline pitch of a stacked label row, in cells. Every box that holds text
 * reserves `rows * DG_ROW_H * g_ch`, because that is exactly what
 * em_text_box draws. */
#define DG_ROW_H        1.15f
#define DG_ID_CAP       48
#define DG_LABEL_CAP    128
#define DG_BODY_CAP     256
#define DG_ELABEL_CAP   64

typedef enum {
    SH_RECT = 0, SH_ROUND, SH_STADIUM, SH_CIRCLE, SH_DIAMOND,
    SH_SUBROUTINE, SH_CYLINDER, SH_HEX, SH_PARA, SH_DOT, SH_RING, SH_CLASS,
    SH_BAR, SH_ENTITY
} NShape;

typedef enum { ES_SOLID = 0, ES_DOTTED, ES_THICK } EStyle;

/* End decorations. ARROW is a filled head, OPEN two strokes; the UML and ER
 * shapes come from class and entity relations. */
typedef enum {
    MK_NONE = 0, MK_ARROW, MK_OPEN, MK_CROSS, MK_CIRCLE, MK_TRIANGLE,
    MK_DIAMOND_F, MK_DIAMOND_O,
    MK_ER_ONE, MK_ER_ZERO_ONE, MK_ER_MANY, MK_ER_ZERO_MANY
} EMark;

typedef enum {
    SQ_MSG = 0, SQ_NOTE_L, SQ_NOTE_R, SQ_NOTE_OVER, SQ_ACT, SQ_DEACT,
    SQ_FRAME_OPEN, SQ_FRAME_DIV, SQ_FRAME_CLOSE
} SqKind;

typedef struct {
    char id[DG_ID_CAP];
    char label[DG_LABEL_CAP];   /* '\n' separates rendered rows */
    char body[DG_BODY_CAP];     /* class attributes / entity attributes */
    char body2[DG_BODY_CAP];    /* class methods */
    u8   shape;
    bool declared;              /* given a shape or a label of its own */
    i16  group;                 /* enclosing subgraph, -1 = none */
    i16  proxy;                 /* stands for this subgraph in links, -1 = no */
    f32  x, y, w, h;            /* px, top-left + size after layout */
} DNode;

typedef struct {
    i32  from, to;
    char label[DG_ELABEL_CAP];
    u8   style;
    u8   mk_from, mk_to;
} DEdge;

typedef struct {
    char id[DG_ID_CAP];
    char label[DG_LABEL_CAP];
    i16  parent;                /* enclosing subgraph, -1 at top level */
    bool used;                  /* holds a node somewhere below it */
    f32  x, y, w, h;
} DGroup;

typedef struct {
    char id[DG_ID_CAP];
    char label[DG_LABEL_CAP];
    bool actor;
    f32  cx, w;                 /* lifeline centre + head-box width */
} DPart;

typedef struct {
    i32  from, to;              /* participant index; from == to = self call */
    i32  span;                  /* note over A,B — second participant */
    char label[DG_LABEL_CAP];
    u8   kind;
    u8   style;
    u8   mk_to, mk_from;
    i8   act;                   /* +1 activate target, -1 deactivate source */
    u16  number;                /* autonumber, 0 = none */
    f32  y, h;                  /* row top + row height */
    f32  ly;                    /* arrow baseline, below the label band */
} DMsg;

typedef struct { i32 part; i32 depth; f32 y0, y1; } DBar;

typedef struct {
    char label[DG_ELABEL_CAP];  /* "loop", "alt"… */
    char text[DG_ELABEL_CAP];   /* the condition */
    f32  x, y, w, h;
    i32  depth;
} DFrame;

typedef struct { i32 frame; char text[DG_ELABEL_CAP]; f32 y; } DDiv;

typedef struct { char label[DG_ELABEL_CAP]; f64 value; } DSlice;

typedef struct { const u8 *s; u32 len, i, lines; } DScan;

/* The parsed model. */
extern DNode  g_nodes[DG_MAX_NODES];
extern u32    g_nnodes;
extern DEdge  g_edges[DG_MAX_EDGES];
extern u32    g_nedges;
extern DGroup g_groups[DG_MAX_GROUPS];
extern u32    g_ngroups;
extern DPart  g_parts[DG_MAX_PARTS];
extern u32    g_nparts;
extern DMsg   g_msgs[DG_MAX_MSGS];
extern u32    g_nmsgs, g_nreal_msgs;
extern DBar   g_bars[DG_MAX_BARS];
extern u32    g_nbars;
extern DFrame g_frames[DG_MAX_FRAMES];
extern u32    g_nframes;
extern DDiv   g_divs[DG_MAX_DIVS];
extern u32    g_ndivs;
extern DSlice g_slices[DG_MAX_SLICES];
extern u32    g_nslices;
extern char   g_title[DG_LABEL_CAP];
extern bool   g_show_data;
extern bool   g_trunc;     /* a cap was hit — fall back to the code block */
extern u8     g_dir;       /* 0 TD/TB, 1 LR, 2 BT, 3 RL */
extern u32    g_head_rows; /* tallest participant label, in rows */
extern f32    g_cw, g_ch;  /* text cell the layout is made with */
extern f32    g_gap;
/* g_gap: the room around and between things, as a fraction of the reading layout:
 * 1 for the reader and the viewer, less where a diagram must fit rows it was
 * given (MD_DIAGRAM_COMPACT). Text sizes never change. */

#define DG_PASSES 4
enum { P_BACK = 0, P_EDGE = 1, P_NODE = 2, P_TOP = 3 };

enum {
    DI_NONE = 0xFF,
    DI_SURFACE = 0, DI_TEXT, DI_TEXT_DIM, DI_NODE_FILL, DI_NODE_STROKE,
    DI_EDGE, DI_CHIP, DI_GROUP_FILL, DI_GROUP_STROKE, DI_GROUP_TEXT,
    DI_NOTE_FILL, DI_NOTE_STROKE, DI_ACCENT, DI_ACTIVATION, DI_SOLID,
    DI_SLICE0
};

bool dg_is_space(u8 c);
bool dg_is_word(u8 c);
u32 dg_scan_id(const u8 *s, u32 len, u32 i);
void dg_strcpy(char *dst, u32 cap, const u8 *s, u32 n);
u32 dg_cells(const char *s, u32 n);
bool dg_row(const char *s, u32 k, const char **out, u32 *outlen);
void dg_text_metrics(const char *s, u32 *rows, u32 *maxcells);
void dg_set_label(char *dst, u32 cap, const u8 *s, u32 n);
u32 dg_find_close(const u8 *s, u32 len, u32 from, const char *close);
bool dg_starts(const u8 *s, u32 len, const char *kw);
bool dg_kw(const u8 *s, u32 len, const char *kw);
void dg_mark_used(i32 group);
i32 dg_intern(const u8 *id, u32 n, i16 group);
void dg_add_edge(i32 from, i32 to, u8 style, u8 mk_from, u8 mk_to, const char *label);
bool dg_next_line(DScan *sc, const u8 **out, u32 *outlen);
i32 dg_parse_noderef(const u8 *s, u32 len, u32 *pi, i16 group);
i16 dg_open_group(const u8 *line, u32 ll, u32 kwlen, i16 parent);
i32 dg_part(const u8 *s, u32 n);
DMsg *dg_add_msg(u8 kind);
void dg_parse_class(DScan *sc);
void dg_parse_state(DScan *sc);
void em_box(u8 pass, f32 x, f32 y, f32 w, f32 h, f32 radius, f32 border, u8 fill, u8 stroke);
void em_stroke(u8 pass, f32 x0, f32 y0, f32 x1, f32 y1, f32 nx0, f32 ny0, f32 nx1, f32 ny1, f32 width, u8 ink);
void em_seg(u8 pass, f32 x0, f32 y0, f32 x1, f32 y1, f32 width, u8 ink);
void em_poly(u8 pass, const f32 *px, const f32 *py, u32 n, bool closed, f32 width, u8 ink);
void em_dashed(u8 pass, const f32 *px, const f32 *py, u32 n, f32 width, f32 dash, f32 gap, u8 ink);
void em_wedge(u8 pass, f32 ax, f32 ay, f32 bx, f32 by, f32 hb, u8 ink);
void em_text(f32 x, f32 y, const char *s, u32 n, u8 ink);
void em_text_box(const char *s, f32 x, f32 y, f32 w, f32 h, u8 ink);
void em_text_rows(const char *s, f32 x, f32 y, u8 ink);
void em_path(f32 *px, f32 *py, u32 n, u8 style, u8 mk_from, u8 mk_to, u8 ink);
void em_chip(const char *label, f32 cx, f32 cy);
f32 dg_stroke_w(void);
/* How far a curve may stray from the chords it is drawn with, as a part of
 * the cell: under half a pixel at the terminal's own size. */
#define DG_FLAT_TOL 0.012f

/* The cubic (x0,y0) .. (x3,y3) as chords appended after px/py[n] (its start
 * not repeated), as many as its bend needs; the new count. */
u32 dg_bezier(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3,
              f32 *px, f32 *py, u32 n, u32 cap);
u32 dg_smooth(const f32 *qx, const f32 *qy, u32 q, f32 *px, f32 *py, u32 cap);
f32 dg_label_chip_w(const char *label);
f32 dg_label_chip_h(const char *label);
void dg_wrap_label(char *s, u32 limit);

/* Eight soft fills after the eight slices: the categorical colours washed
 * into the surface, for boxes text is drawn on. */
#define DI_SOFT0 (DI_SLICE0 + 8)
/* ...and eight fainter ones: a field behind marks and text, where the soft
 * fill would compete with what stands on it. */
#define DI_TINT0 (DI_SOFT0 + 8)

/* md_diagram_create_fit: a graph drawn at this share of its size or more in
 * its box is kept as written; turned, it has to draw this much larger to be
 * taken. */
#define DG_FIT_KEEP 0.85f
#define DG_FIT_GAIN 1.25f
/* The box the diagram being built is laid out for, 0 when none was named: a
 * chart sizes its plot to it and keeps its text's size. */
extern f32 g_fit_w, g_fit_h;

/* ---- the kinds in md_diagram_more.c: gantt, mindmap, timeline, gitGraph,
 * xychart, quadrantChart, journey ---- */
void dgm_reset(void);
/* Parse the body of a diagram of `kind` whose header line is `head`. False
 * when nothing in it can be drawn. */
bool dgm_parse(MdDiagramKind kind, DScan *sc, const u8 *head, u32 hlen);
/* Lay it out into the draw list. */
void dgm_emit(MdDiagramKind kind);

/* ---- md_diagram_lang.c: other diagram languages, as Mermaid ---- */
/* The language `src` is written in (Mermaid when it is no other). */
MdDiagramLang dgl_lang(const u8 *src, u32 len);
/* `src` in `lang` translated into the Mermaid of the same diagram, NUL
 * terminated in `out`. False when it cannot be read or does not fit. */
bool dgl_translate(const u8 *src, u32 len, MdDiagramLang lang, char *out, u32 cap, u32 *out_len);

#endif /* UI_MARKDOWN_MD_DIAGRAM_INT_H */
