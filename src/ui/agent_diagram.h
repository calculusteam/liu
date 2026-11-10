/*
 * Liu — diagrams, formulas and pictures drawn over an agent's output
 *
 * When an agent in a pane prints a diagram (Mermaid, Graphviz DOT,
 * PlantUML), display math or an SVG document (ui/mermaid_scan.h finds it in
 * the cells), the block's rows become a card with it drawn on it: the card
 * covers exactly the rows the source took, so nothing of the agent's own
 * layout moves. What is drawn is made once, when the block is first seen
 * whole (ui/agent_visual.h: a diagram laid out by the native engine, a
 * formula by the math engine, an SVG drawn to pixels on a thread of its
 * own), and each painted frame only draws it. A click on the card opens it large
 * in the window's viewer (pan, zoom, copy the source); the card's Source
 * button gives the rows back to the agent's text until Diagram is clicked.
 *
 * Only panes running an agent are looked at, and only while the
 * `agent_diagrams` setting is on. A pane's state lives in its row cache and
 * goes with it: the rows' summaries, kept on the dirty signal the glyph rows
 * use, the tape of the agent's output the pane has shown (ui/mermaid_scan.h),
 * so a block taller than the pane is found whole, and the blocks it has seen
 * whole — remembered by the hashes of their rows, so a block scrolled part
 * way off keeps its card where its rows are.
 */
#ifndef UI_AGENT_DIAGRAM_H
#define UI_AGENT_DIAGRAM_H

#include "core/types.h"
#include "ui/anim.h"
#include "ui/markdown/md_diagram.h"

typedef struct AppState AppState;
typedef struct AgentDiagramPane AgentDiagramPane;
struct Terminal;

/* Every pane state begins with this, so the row cache that holds one can let
 * it go (agent_diagram_pane_release) without linking this module: the test
 * targets that build a row cache know nothing of diagrams. */
typedef struct {
    void (*free)(AgentDiagramPane **state);
} AgentDiagramPaneHead;

static inline void agent_diagram_pane_release(AgentDiagramPane **state) {
    if (state && *state) {
        void (*f)(AgentDiagramPane **) = ((const AgentDiagramPaneHead *)(const void *)*state)->free;
        if (f) f(state);
    }
}

/* What a published rect does when clicked. */
typedef enum {
    ADG_HIT_CARD = 0,        /* the diagram: open it in the viewer */
    ADG_HIT_EXPAND,          /* the card's expand button: the same */
    ADG_HIT_SOURCE,          /* show the agent's text instead */
    ADG_HIT_DIAGRAM          /* the pill over shown source: the diagram again */
} AdgHitKind;

#define ADG_MAX_HITS 96

typedef struct {
    const struct Terminal *term;    /* compared, never dereferenced */
    u64  src_hash;
    u8   kind;                      /* AdgHitKind */
    f32  x, y, w, h;
    f32  scale;                     /* a card's: what it holds is drawn at, of its own size */
} AgentDiagramHit;

struct AgentVisual;

/* The window's large view of one block. It owns a copy of the source and a
 * visual built for it, so the pane may scroll or close under it. */
typedef struct {
    bool  open;
    char *src;
    u32   len;
    struct AgentVisual *vis;
    f32   cw, ch;                   /* the cell it was built with */
    f32   zoom;                     /* 1 = fit */
    f32   pan_x, pan_y;             /* pixels, from centre */
    bool  dragging;
    f32   drag_x, drag_y;
    Anim  open_anim, close_anim;
    bool  was_open;
} DiagramViewer;

/* The pane pass, from the terminal pane draw: keeps the pane's row summaries
 * current (rows that are dirty, every row when `full`), finds and places the
 * cards when a summary changed, and when `draw` draws them and publishes
 * their controls. `source` is the pane's terminal, `view` what is drawn. */
void agent_diagram_pane_pass(AppState *app, struct Terminal *source, struct Terminal *view,
                             AgentDiagramPane **state, f32 origin_x, f32 origin_y,
                             bool full, bool draw);
void agent_diagram_pane_free(AgentDiagramPane **state);
usize agent_diagram_pane_bytes(const AgentDiagramPane *state);

/* Published controls: index or -1, the click, and whether the pointer is on
 * one (the pointer cursor). */
i32  agent_diagram_hit(const AppState *app, f32 x, f32 y);
bool agent_diagram_click(AppState *app, i32 hit);

/* ---- the viewer ---- */
bool diagram_viewer_open(AppState *app, const char *src, u32 len);
void diagram_viewer_close(AppState *app);
void diagram_viewer_free(DiagramViewer *v);
/* Draw. Called every frame, last; it releases the diagram the frame after
 * the close has drained. */
void diagram_viewer_render(AppState *app);
/* A curve is running: the loop owes frames. */
bool diagram_viewer_animating(const DiagramViewer *v);
/* The viewer's panel in pixels, as drawn (the driver's state). */
void diagram_viewer_panel_rect(const AppState *app, f32 *x, f32 *y, f32 *w, f32 *h);
/* The line under the viewer's bar: its gestures, with the platform's app
 * modifier. A button under the pointer shows its own name and key there. */
const char *diagram_viewer_hint(void);
/* The bar's buttons, left to right. The size the drawing is shown at stands
 * between the two zoom buttons and is the way back to the fitted size. */
enum { DGV_BTN_ZOOM_OUT, DGV_BTN_FIT, DGV_BTN_ZOOM_IN, DGV_BTN_COPY, DGV_BTN_CLOSE, DGV_BTN_COUNT };
/* A button's rect in pixels while the viewer is up, and its name (the
 * driver's state). */
bool diagram_viewer_button_rect(const AppState *app, i32 button, f32 *x, f32 *y, f32 *w, f32 *h);
const char *diagram_viewer_button_name(i32 button);

bool diagram_viewer_key(AppState *app, i32 key, u32 mods);
bool diagram_viewer_char(AppState *app, u32 codepoint);
/* +1 larger, -1 smaller, 0 back to fit. */
void diagram_viewer_zoom(AppState *app, i32 dir);
bool diagram_viewer_mouse_down(AppState *app, f32 x, f32 y);
bool diagram_viewer_mouse_up(AppState *app);
bool diagram_viewer_mouse_move(AppState *app, f32 x, f32 y);
bool diagram_viewer_scroll(AppState *app, f32 dx, f32 dy, bool precise, u32 mods);

#endif /* UI_AGENT_DIAGRAM_H */
