/*
 * Liu - split pane management
 * Binary tree layout for horizontal/vertical terminal splits.
 */
#include "core/types.h"
#include "terminal/terminal.h"
#include "ssh/ssh_session.h"
#include <stdlib.h>
#include <string.h>

typedef enum {
    SPLIT_NONE,
    SPLIT_HORIZONTAL,  /* side by side */
    SPLIT_VERTICAL,    /* top and bottom */
} SplitDir;

typedef struct SplitNode SplitNode;

struct SplitNode {
    SplitDir   dir;
    f32        ratio;     /* 0.0-1.0, position of divider */

    /* Leaf node data (when dir == SPLIT_NONE) */
    Terminal  *terminal;
    Session   *session;
    bool       focused;

    /* Children (when dir != SPLIT_NONE) */
    SplitNode *first;
    SplitNode *second;

    /* Computed layout rect (framebuffer coordinates) */
    f32 x, y, w, h;
};

SplitNode *split_create_leaf(Terminal *term, Session *session) {
    SplitNode *n = calloc(1, sizeof(SplitNode));
    if (!n) return NULL;
    n->dir = SPLIT_NONE;
    n->ratio = 0.5f;
    n->terminal = term;
    n->session = session;
    n->focused = true;
    return n;
}

SplitNode *split_horizontal(SplitNode *left, SplitNode *right) {
    SplitNode *n = calloc(1, sizeof(SplitNode));
    if (!n) return NULL;
    n->dir = SPLIT_HORIZONTAL;
    n->ratio = 0.5f;
    n->first = left;
    n->second = right;
    return n;
}

SplitNode *split_vertical(SplitNode *top, SplitNode *bottom) {
    SplitNode *n = calloc(1, sizeof(SplitNode));
    if (!n) return NULL;
    n->dir = SPLIT_VERTICAL;
    n->ratio = 0.5f;
    n->first = top;
    n->second = bottom;
    return n;
}

void split_destroy(SplitNode *n) {
    if (!n) return;
    if (n->dir != SPLIT_NONE) {
        split_destroy(n->first);
        split_destroy(n->second);
    }
    /* Note: terminal and session are owned by Tab, not freed here */
    free(n);
}

/* Recursively compute layout */
void split_layout(SplitNode *n, f32 x, f32 y, f32 w, f32 h, f32 cell_w, f32 cell_h) {
    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;

    if (n->dir == SPLIT_NONE) {
        /* Resize terminal to fit */
        if (n->terminal && cell_w > 0 && cell_h > 0) {
            i32 cols = (i32)(w / cell_w);
            i32 rows = (i32)(h / cell_h);
            if (cols < 1) cols = 1;
            if (rows < 1) rows = 1;
            terminal_resize(n->terminal, cols, rows);
            if (n->session) session_resize(n->session, cols, rows);
        }
    } else if (n->dir == SPLIT_HORIZONTAL) {
        f32 split_x = x + w * n->ratio;
        f32 divider = 2.0f;
        split_layout(n->first, x, y, split_x - x - divider/2, h, cell_w, cell_h);
        split_layout(n->second, split_x + divider/2, y, x + w - split_x - divider/2, h, cell_w, cell_h);
    } else if (n->dir == SPLIT_VERTICAL) {
        f32 split_y = y + h * n->ratio;
        f32 divider = 2.0f;
        split_layout(n->first, x, y, w, split_y - y - divider/2, cell_w, cell_h);
        split_layout(n->second, x, split_y + divider/2, w, y + h - split_y - divider/2, cell_w, cell_h);
    }
}

/* Find the focused leaf */
SplitNode *split_find_focused(SplitNode *n) {
    if (!n) return NULL;
    if (n->dir == SPLIT_NONE) {
        return n->focused ? n : NULL;
    }
    SplitNode *f = split_find_focused(n->first);
    if (f) return f;
    return split_find_focused(n->second);
}

/* Find leaf at screen coordinates */
SplitNode *split_find_at(SplitNode *n, f32 px, f32 py) {
    if (!n) return NULL;
    if (px < n->x || px > n->x + n->w || py < n->y || py > n->y + n->h)
        return NULL;
    if (n->dir == SPLIT_NONE) return n;
    SplitNode *f = split_find_at(n->first, px, py);
    if (f) return f;
    return split_find_at(n->second, px, py);
}

/* Count leaf nodes */
i32 split_count_leaves(SplitNode *n) {
    if (!n) return 0;
    if (n->dir == SPLIT_NONE) return 1;
    return split_count_leaves(n->first) + split_count_leaves(n->second);
}
