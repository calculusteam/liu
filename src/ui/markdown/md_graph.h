/*
 * Liu — Markdown knowledge-graph view.
 *
 * Builds a node-link graph from a folder of .md files:
 * nodes are files, edges are [[wikilinks]] / ![[embeds]] / [text](*.md)
 * links between them. A force-directed layout settles the node positions,
 * and the whole graph (edges + nodes) is CPU-rasterized into a premultiplied
 * RGBA buffer that the viewer uploads via renderer_draw_image_cached — node
 * labels are drawn separately by the caller through the glyph pipeline.
 *
 * Self-contained: no renderer/shader changes. All world<->screen transform
 * state (pan/zoom) lives in the caller; this module only needs it passed in.
 */
#ifndef UI_MARKDOWN_GRAPH_H
#define UI_MARKDOWN_GRAPH_H

#include "core/types.h"
#include "core/config.h"   /* Theme */

typedef struct MdGraph MdGraph;

/* Recursively scan `root_dir` for *.md files (bounded depth + node count),
 * parse each, and build the node/edge graph. Returns NULL on failure or if
 * the folder has no markdown files. */
MdGraph *md_graph_build(const char *root_dir);
void     md_graph_free(MdGraph *g);

i32 md_graph_node_count(const MdGraph *g);
i32 md_graph_edge_count(const MdGraph *g);

/* Advance the force-directed layout by one step (dt seconds). Returns true
 * while the layout is still moving (caller keeps redrawing); false once it
 * has settled below the motion threshold. */
bool md_graph_step(MdGraph *g, f32 dt);

/* True once the layout has settled (no further redraw needed unless the
 * caller pans/zooms/hovers/drags). */
bool md_graph_settled(const MdGraph *g);

/* Re-arm the simulation (e.g. after the user drags a node). */
void md_graph_reheat(MdGraph *g);

/* Apply the force/size multipliers (1.0 == defaults). center =
 * centering gravity, repel = node repulsion, link = edge spring, linkdist =
 * preferred edge length, node_size = rendered/hit radius scale. Cheap to call
 * every frame; reheat separately when a force changes so the layout responds. */
void md_graph_apply_params(MdGraph *g, f32 center, f32 repel, f32 link,
                           f32 linkdist, f32 node_size);

/* The radius (in world units) that encloses all nodes — used by the caller
 * to compute an initial auto-fit zoom. */
f32 md_graph_world_radius(const MdGraph *g);

/* Screen transform used everywhere: screen = (w/2,h/2) + (pan_x,pan_y) +
 * world*zoom. Compute a node's screen position + pixel radius. */
void md_graph_node_screen(const MdGraph *g, i32 idx, i32 w, i32 h,
                          f32 pan_x, f32 pan_y, f32 zoom,
                          f32 *out_sx, f32 *out_sy, f32 *out_radius);

/* Hit-test a screen point to the nearest node within its radius, or -1. */
i32 md_graph_hit(const MdGraph *g, f32 sx, f32 sy, i32 w, i32 h,
                 f32 pan_x, f32 pan_y, f32 zoom);

/* Pin/unpin and move a node to a world position (for click-drag). */
void md_graph_set_node_world(MdGraph *g, i32 idx, f32 wx, f32 wy, bool pinned);
void md_graph_screen_to_world(i32 w, i32 h, f32 pan_x, f32 pan_y, f32 zoom,
                              f32 sx, f32 sy, f32 *out_wx, f32 *out_wy);

/* Node metadata for labels / opening. */
const char *md_graph_node_label(const MdGraph *g, i32 idx);
const char *md_graph_node_path(const MdGraph *g, i32 idx);
i32         md_graph_node_degree(const MdGraph *g, i32 idx);
i32         md_graph_node_component(const MdGraph *g, i32 idx);  /* topic-cluster id, -1 if none */
i32         md_graph_node_comp_size(const MdGraph *g, i32 idx);  /* nodes in that cluster (1 = isolated) */
bool        md_graph_node_is_tag(const MdGraph *g, i32 idx);   /* tag pseudo-node */

/* Folder clustering: each file node belongs to a group = its parent directory
 * (relative to the build root). Same-folder nodes cluster together in the
 * layout; the renderer colours each cluster distinctly + labels it. */
i32         md_graph_node_group(const MdGraph *g, i32 idx);    /* -1 for tags */
i32         md_graph_group_count(const MdGraph *g);
const char *md_graph_group_name(const MdGraph *g, i32 group);  /* rel parent dir */
bool        md_graph_group_centroid(const MdGraph *g, i32 group, f32 *wx, f32 *wy);

/* Edge endpoints (node indices) and a direct-connectivity query — used by the
 * GPU renderer to draw edges and emphasize a hovered node's neighbours. */
void md_graph_edge(const MdGraph *g, i32 idx, i32 *out_a, i32 *out_b);
bool md_graph_connected(const MdGraph *g, i32 a, i32 b);

/* Find a file node by absolute path (matched on lowercased basename, so it is
 * tolerant of path normalization differences). Returns -1 if not found. */
i32 md_graph_find_by_path(const MdGraph *g, const char *abs_path);

/* Directed file→file edges (src→dst) — the undirected `edges` above can't
 * answer "who links to me", so backlinks walk these. */
i32  md_graph_dir_edge_count(const MdGraph *g);
void md_graph_dir_edge(const MdGraph *g, i32 idx, i32 *out_src, i32 *out_dst);

#endif /* UI_MARKDOWN_GRAPH_H */
