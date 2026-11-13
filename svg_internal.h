/* svg_internal.h - Shared render-tree builder internals. */
#ifndef SVG_INTERNAL_H
#define SVG_INTERNAL_H

#include "internal.h"

#define SVG2IMG_MAX_DEPTH 1024

typedef struct { char* key; Node* val; } IdEntry;
struct IdMap { IdEntry* e; int n, cap; };

void  idmap_put(IdMap* m, const char* k, Node* v);
Node* idmap_get(IdMap* m, const char* k);
void  idmap_free(IdMap* m);

Mat   parse_transform(const char* s);
Mat   transform_attr(const XMLNode* xn);
float len_attr(Ctx* ctx, const XMLNode* xn, const char* name, int axis, float def);

Path* path_from_rect(float x, float y, float w, float h, float rx, float ry);
Path* path_from_ellipse(float cx, float cy, float rx, float ry);
Path* path_from_line(float x1, float y1, float x2, float y2);
Path* path_from_points(const char* s, int close);

Node* build_gradient(Ctx* ctx, const XMLNode* xn, int radial);
void  parse_stops(const XMLNode* xn, Paint* p);
char* gather_text(const XMLNode* xn);

int   node_add_child(Node* parent, Node* child);
Node* build_node(Ctx* ctx, const XMLNode* xn, const Style* parent, int depth);

void  compute_viewport(const XMLNode* svg, const svg2img_options* opts,
                       int* W, int* H, Mat* tm);

#endif
