/* svg_internal.h - Shared render-tree builder internals. */
#ifndef SVG_INTERNAL_H
#define SVG_INTERNAL_H

#include "internal.h"

/* Hard ceiling on element nesting, independent of the configurable
 * max_depth limit: the tree builder is recursive, so this also bounds the
 * C stack.  A configurable limit may be lower but never higher. */
#define SVG2IMG_MAX_DEPTH 1024

typedef struct { char* key; Node* val; } IdEntry;
struct IdMap { IdEntry* e; int n, cap; };

void  idmap_put(IdMap* m, const char* k, Node* v);
Node* idmap_get(IdMap* m, const char* k);
void  idmap_remove(IdMap* m, const char* k, const Node* v);
void  idmap_free(IdMap* m);
void  svg_free_detached(Node* n, IdMap* ids);

Mat   parse_transform(const char* s);
Mat   transform_attr(const XMLNode* xn);
float len_attr(Ctx* ctx, const XMLNode* xn, const char* name, int axis, float def);

Path* path_from_rect(float x, float y, float w, float h, float rx, float ry);
Path* path_from_ellipse(float cx, float cy, float rx, float ry);
Path* path_from_line(float x1, float y1, float x2, float y2);
Path* path_from_points(const char* s, int close);
Path* path_from_points_lim(const char* s, int close, const svg2img_limits* lim,
                           int* limit_hit);
/* Path parsers take the resolved limits so a hostile document cannot make
 * one path allocate unbounded memory. */
Path* path_parse_lim(const char* d, const svg2img_limits* lim, int* limit_hit);

Node* build_gradient(Ctx* ctx, const XMLNode* xn, int radial);
Node* build_pattern(Ctx* ctx, const XMLNode* xn);
int   parse_stops(const XMLNode* xn, Paint* p, int max_stops);
char* gather_text(const XMLNode* xn, int max_len, int* limit_hit);

int   node_add_child(Node* parent, Node* child);
Node* build_node(Ctx* ctx, const XMLNode* xn, const Style* parent, int depth);

int   svg_parse_viewbox(const char* s, float* x, float* y, float* w, float* h);

void  compute_viewport(const XMLNode* svg, const svg2img_options* opts,
                       int* W, int* H, Mat* tm, const svg2img_limits* lim,
                       int* clamped);

#endif
