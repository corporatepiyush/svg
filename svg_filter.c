/* svg_filter.c - <filter> definitions and primitive parameters (SVG 1.1 §15).
 *
 * The parser only reads parameters; nothing is evaluated here.  Each
 * primitive keeps the names of its inputs and of its result, and the
 * rasteriser resolves those names when it runs the chain.
 */
#include "svg_internal.h"
#include <ctype.h>

static float fnum(const char* s, float def) {
    if (!s) return def;
    while (isspace((unsigned char)*s)) s++;
    char* end = NULL;
    float v = strtof(s, &end);
    if (end == s) return def;
    return v;
}

/* A filter region length: in objectBoundingBox units a percentage is a
 * fraction of the bounding box, so "120%" is 1.2. */
static float region_len(const XMLNode* xn, const char* name, float def) {
    const char* s = xml_attr(xn, name);
    if (!s) return def;
    char* end = NULL;
    float v = strtof(s, &end);
    if (end == s) return def;
    if (strchr(s, '%')) v /= 100.0f;
    return v;
}

static char* attr_dup(const XMLNode* xn, const char* a, const char* b) {
    const char* v = a ? xml_attr(xn, a) : NULL;
    if (!v && b) v = xml_attr(xn, b);
    return v ? svg_strdup(v) : NULL;
}

/* A primitive length.  With primitiveUnits="objectBoundingBox" a length is a
 * fraction of the referencing element's bounding box; the rasteriser scales
 * it, so the raw value is stored.  With the default userSpaceOnUse it is a
 * user-space length and percentages resolve against the viewport. */
static float plen(Ctx* ctx, const XMLNode* xn, const char* name, int axis,
                  int bbox_units, float def) {
    const char* s = xml_attr(xn, name);
    if (!s) return def;
    if (bbox_units) {
        float v = fnum(s, def);
        if (strchr(s, '%')) v /= 100.0f;
        return v;
    }
    return length_parse(s, ctx->vw, ctx->vh, ctx->em, axis, def);
}

static void parse_floats(const char* s, float* out, int max, int* n) {
    *n = 0;
    if (!s) return;
    const char* p = s;
    while (*p && *n < max) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        char* end = NULL;
        float v = strtof(p, &end);
        if (end == p) break;
        out[(*n)++] = v;
        p = end;
    }
}

/* Split a two-number attribute ("1 2" or "1,2") into num[0] and num[1]. */
static void parse_pair(Ctx* ctx, const XMLNode* xn, const char* name, int axis,
                       int bbox_units, FilterPrim* p, float def) {
    const char* s = xml_attr(xn, name);
    if (!s) { p->num[0] = p->num[1] = def; return; }
    char buf[128];
    size_t n = strlen(s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    char* sep = strchr(buf, ',');
    if (!sep) sep = strchr(buf, ' ');
    if (sep) {
        *sep = 0;
        p->num[0] = plen(ctx, xn, name, axis, bbox_units, def);
        p->num[1] = fnum(sep + 1, p->num[0]);
        if (bbox_units && strchr(sep + 1, '%')) p->num[1] /= 100.0f;
    } else {
        p->num[0] = plen(ctx, xn, name, axis, bbox_units, def);
        p->num[1] = p->num[0];
    }
}

/* ------------------------------------------------------------------ */
/* feComponentTransfer                                                 */
/* ------------------------------------------------------------------ */

static void parse_transfer_fn(const XMLNode* fc, FilterPrim* p, int slot) {
    const char* t = xml_attr(fc, "type");
    if (!t || !strcmp(t, "identity")) p->ct_type[slot] = FE_CT_IDENTITY;
    else if (!strcmp(t, "table")) p->ct_type[slot] = FE_CT_TABLE;
    else if (!strcmp(t, "discrete")) p->ct_type[slot] = FE_CT_DISCRETE;
    else if (!strcmp(t, "linear")) p->ct_type[slot] = FE_CT_LINEAR;
    else if (!strcmp(t, "gamma")) p->ct_type[slot] = FE_CT_GAMMA;
    else p->ct_type[slot] = FE_CT_IDENTITY;
    p->ct_slope[slot] = fnum(xml_attr(fc, "slope"), 1.0f);
    p->ct_intercept[slot] = fnum(xml_attr(fc, "intercept"), 0.0f);
    p->ct_amplitude[slot] = fnum(xml_attr(fc, "amplitude"), 1.0f);
    p->ct_exponent[slot] = fnum(xml_attr(fc, "exponent"), 1.0f);
    p->ct_offset[slot] = fnum(xml_attr(fc, "offset"), 0.0f);
    p->ct_ntable[slot] = 0;
    parse_floats(xml_attr(fc, "tableValues"), p->ct_table[slot], 16,
                 &p->ct_ntable[slot]);
}

/* ------------------------------------------------------------------ */
/* Primitives                                                          */
/* ------------------------------------------------------------------ */

/* A light source child of feDiffuseLighting / feSpecularLighting. */
static void parse_light(Ctx* ctx, const XMLNode* lt, FilterPrim* p,
                        int bbox_units) {
    if (!strcmp(lt->name, "feDistantLight")) {
        p->light_type = FE_LIGHT_DISTANT;
        p->azimuth = plen(ctx, lt, "azimuth", 2, bbox_units, 0.0f);
        p->elevation = plen(ctx, lt, "elevation", 2, bbox_units, 0.0f);
    } else if (!strcmp(lt->name, "fePointLight")) {
        p->light_type = FE_LIGHT_POINT;
        p->lx = plen(ctx, lt, "x", 0, bbox_units, 0.0f);
        p->ly = plen(ctx, lt, "y", 1, bbox_units, 0.0f);
        p->lz = plen(ctx, lt, "z", 2, bbox_units, 0.0f);
    } else {
        p->light_type = FE_LIGHT_SPOT;
        p->lx = plen(ctx, lt, "x", 0, bbox_units, 0.0f);
        p->ly = plen(ctx, lt, "y", 1, bbox_units, 0.0f);
        p->lz = plen(ctx, lt, "z", 2, bbox_units, 0.0f);
        p->points_at_x = plen(ctx, lt, "pointsAtX", 0, bbox_units, 0.0f);
        p->points_at_y = plen(ctx, lt, "pointsAtY", 1, bbox_units, 0.0f);
        p->points_at_z = plen(ctx, lt, "pointsAtZ", 2, bbox_units, 0.0f);
        p->specular_exponent = plen(ctx, lt, "specularExponent", 2, bbox_units, 1.0f);
        p->limiting_cone_angle = plen(ctx, lt, "limitingConeAngle", 2, bbox_units, -1.0f);
    }
}

static FilterPrim* parse_prim(Ctx* ctx, const XMLNode* xn, int bbox_units) {
    const char* nm = xn->name;
    FilterPrim* p = (FilterPrim*)svg_malloc(sizeof(FilterPrim));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->light_color = (Color){1, 1, 1, 1};
    p->diffuse_constant = 1.0f;
    p->specular_constant = 1.0f;
    p->specular_exponent = 20.0f;
    p->divisor = -1.0f;
    p->octaves = 1;
    p->surface_scale = 1.0f;

    if (!strcmp(nm, "feBlend")) {
        p->type = FE_BLEND;
        const char* m = xml_attr(xn, "mode");
        if (m && !strcmp(m, "multiply")) p->num[0] = FE_BLEND_MULTIPLY;
        else if (m && !strcmp(m, "screen")) p->num[0] = FE_BLEND_SCREEN;
        else if (m && !strcmp(m, "darken")) p->num[0] = FE_BLEND_DARKEN;
        else if (m && !strcmp(m, "lighten")) p->num[0] = FE_BLEND_LIGHTEN;
        else p->num[0] = FE_BLEND_NORMAL;
    } else if (!strcmp(nm, "feColorMatrix")) {
        p->type = FE_COLOR_MATRIX;
        const char* t = xml_attr(xn, "type");
        if (t && !strcmp(t, "saturate")) p->num[0] = FE_CM_SATURATE;
        else if (t && !strcmp(t, "hueRotate")) p->num[0] = FE_CM_HUE_ROTATE;
        else if (t && !strcmp(t, "luminanceToAlpha"))
            p->num[0] = FE_CM_LUMINANCE_TO_ALPHA;
        else p->num[0] = FE_CM_MATRIX;
        p->nvalues = 0;
        parse_floats(xml_attr(xn, "values"), p->values, 32, &p->nvalues);
        if (p->num[0] == (float)FE_CM_MATRIX) {
            if (p->nvalues != 20) {
                static const float id[20] = {1,0,0,0,0, 0,1,0,0,0,
                                             0,0,1,0,0, 0,0,0,1,0};
                memcpy(p->values, id, sizeof id);
                p->nvalues = 20;
            }
        } else {
            p->num[1] = p->nvalues > 0 ? p->values[0]
                        : ((p->num[0] == (float)FE_CM_HUE_ROTATE) ? 0.0f : 1.0f);
        }
    } else if (!strcmp(nm, "feComponentTransfer")) {
        p->type = FE_COMPONENT_TRANSFER;
        for (int c = 0; c < 4; c++) {
            p->ct_type[c] = FE_CT_IDENTITY;
            p->ct_slope[c] = 1.0f;
            p->ct_amplitude[c] = 1.0f;
            p->ct_exponent[c] = 1.0f;
        }
        for (int k = 0; k < xn->nkids; k++) {
            const XMLNode* fc = xn->kids[k];
            int slot;
            if (!strcmp(fc->name, "feFuncR")) slot = 0;
            else if (!strcmp(fc->name, "feFuncG")) slot = 1;
            else if (!strcmp(fc->name, "feFuncB")) slot = 2;
            else if (!strcmp(fc->name, "feFuncA")) slot = 3;
            else continue;
            parse_transfer_fn(fc, p, slot);
        }
    } else if (!strcmp(nm, "feComposite")) {
        p->type = FE_COMPOSITE;
        const char* op = xml_attr(xn, "operator");
        if (op && !strcmp(op, "in")) p->num[0] = FE_COMP_IN;
        else if (op && !strcmp(op, "out")) p->num[0] = FE_COMP_OUT;
        else if (op && !strcmp(op, "atop")) p->num[0] = FE_COMP_ATOP;
        else if (op && !strcmp(op, "xor")) p->num[0] = FE_COMP_XOR;
        else if (op && !strcmp(op, "arithmetic")) p->num[0] = FE_COMP_ARITH;
        else p->num[0] = FE_COMP_OVER;
        p->num[1] = fnum(xml_attr(xn, "k1"), 0.0f);
        p->num[2] = fnum(xml_attr(xn, "k2"), 0.0f);
        p->num[3] = fnum(xml_attr(xn, "k3"), 0.0f);
        p->num[4] = fnum(xml_attr(xn, "k4"), 0.0f);
    } else if (!strcmp(nm, "feConvolveMatrix")) {
        p->type = FE_CONVOLVE_MATRIX;
        p->order_x = (int)floorf(fnum(xml_attr(xn, "orderX"), 3.0f));
        p->order_y = (int)floorf(fnum(xml_attr(xn, "orderY"), (float)p->order_x));
        if (p->order_x < 1) p->order_x = 1;
        if (p->order_y < 1) p->order_y = 1;
        if (p->order_x > 8) p->order_x = 8;
        if (p->order_y > 8) p->order_y = 8;
        p->nkernel = 0;
        parse_floats(xml_attr(xn, "kernelMatrix"), p->kernel,
                     (int)(sizeof p->kernel / sizeof p->kernel[0]), &p->nkernel);
        p->target_x = xml_attr(xn, "targetX") ? fnum(xml_attr(xn, "targetX"), 0.0f)
                                              : ((float)p->order_x - 1.0f) * 0.5f;
        p->target_y = xml_attr(xn, "targetY") ? fnum(xml_attr(xn, "targetY"), 0.0f)
                                              : ((float)p->order_y - 1.0f) * 0.5f;
        p->divisor = fnum(xml_attr(xn, "divisor"), -1.0f);
        p->bias = fnum(xml_attr(xn, "bias"), 0.0f);
        const char* em = xml_attr(xn, "edgeMode");
        if (em && !strcmp(em, "wrap")) p->edge_mode = 1;
        else if (em && !strcmp(em, "none")) p->edge_mode = 2;
        else p->edge_mode = 0;
    } else if (!strcmp(nm, "feDiffuseLighting") ||
               !strcmp(nm, "feSpecularLighting")) {
        p->type = !strcmp(nm, "feDiffuseLighting") ? FE_DIFFUSE_LIGHTING
                                                   : FE_SPECULAR_LIGHTING;
        p->surface_scale = plen(ctx, xn, "surfaceScale", 2, bbox_units, 1.0f);
        p->diffuse_constant = plen(ctx, xn, "diffuseConstant", 2, bbox_units, 1.0f);
        p->specular_constant = plen(ctx, xn, "specularConstant", 2, bbox_units, 1.0f);
        p->specular_exponent = plen(ctx, xn, "specularExponent", 2, bbox_units, 20.0f);
        const char* lc = xml_attr(xn, "lighting-color");
        char val[64];
        if (style_get_decl(xml_attr(xn, "style"), "lighting-color", val,
                           sizeof val))
            lc = val;
        if (lc) {
            Color c;
            if (color_parse(lc, &c)) p->light_color = c;
        }
        for (int k = 0; k < xn->nkids; k++) {
            const XMLNode* lt = xn->kids[k];
            if (strcmp(lt->name, "feDistantLight") &&
                strcmp(lt->name, "fePointLight") && strcmp(lt->name, "feSpotLight"))
                continue;
            parse_light(ctx, lt, p, bbox_units);
        }
    } else if (!strcmp(nm, "feDisplacementMap")) {
        p->type = FE_DISPLACEMENT_MAP;
        p->num[0] = plen(ctx, xn, "scale", 2, bbox_units, 0.0f);
        const char* xc = xml_attr(xn, "xChannelSelector");
        const char* yc = xml_attr(xn, "yChannelSelector");
        p->num[1] = (xc && xc[0] == 'G') ? FE_CHAN_G
                  : (xc && xc[0] == 'B') ? FE_CHAN_B
                  : (xc && xc[0] == 'A') ? FE_CHAN_A : FE_CHAN_R;
        p->num[2] = (yc && yc[0] == 'G') ? FE_CHAN_G
                  : (yc && yc[0] == 'B') ? FE_CHAN_B
                  : (yc && yc[0] == 'A') ? FE_CHAN_A : FE_CHAN_R;
    } else if (!strcmp(nm, "feDistantLight")) {
        p->type = FE_DISTANT_LIGHT;
        p->light_type = FE_LIGHT_DISTANT;
        p->azimuth = plen(ctx, xn, "azimuth", 2, bbox_units, 0.0f);
        p->elevation = plen(ctx, xn, "elevation", 2, bbox_units, 0.0f);
    } else if (!strcmp(nm, "fePointLight")) {
        p->type = FE_POINT_LIGHT;
        p->light_type = FE_LIGHT_POINT;
        p->lx = plen(ctx, xn, "x", 0, bbox_units, 0.0f);
        p->ly = plen(ctx, xn, "y", 1, bbox_units, 0.0f);
        p->lz = plen(ctx, xn, "z", 2, bbox_units, 0.0f);
    } else if (!strcmp(nm, "feSpotLight")) {
        p->type = FE_SPOT_LIGHT;
        p->light_type = FE_LIGHT_SPOT;
        p->lx = plen(ctx, xn, "x", 0, bbox_units, 0.0f);
        p->ly = plen(ctx, xn, "y", 1, bbox_units, 0.0f);
        p->lz = plen(ctx, xn, "z", 2, bbox_units, 0.0f);
        p->points_at_x = plen(ctx, xn, "pointsAtX", 0, bbox_units, 0.0f);
        p->points_at_y = plen(ctx, xn, "pointsAtY", 1, bbox_units, 0.0f);
        p->points_at_z = plen(ctx, xn, "pointsAtZ", 2, bbox_units, 0.0f);
        p->specular_exponent = plen(ctx, xn, "specularExponent", 2, bbox_units, 1.0f);
        p->limiting_cone_angle = plen(ctx, xn, "limitingConeAngle", 2, bbox_units, -1.0f);
    } else if (!strcmp(nm, "feFlood")) {
        p->type = FE_FLOOD;
        const char* fc = xml_attr(xn, "flood-color");
        if (fc) color_parse(fc, &p->light_color);
        else p->light_color = (Color){0, 0, 0, 1};
        p->num[0] = fnum(xml_attr(xn, "flood-opacity"), 1.0f);
        char val[64];
        if (style_get_decl(xml_attr(xn, "style"), "flood-color", val, sizeof val)) {
            Color c;
            if (color_parse(val, &c)) p->light_color = c;
        }
        if (style_get_decl(xml_attr(xn, "style"), "flood-opacity", val, sizeof val))
            p->num[0] = fnum(val, 1.0f);
    } else if (!strcmp(nm, "feGaussianBlur")) {
        p->type = FE_GAUSSIAN_BLUR;
        parse_pair(ctx, xn, "stdDeviation", 2, bbox_units, p, 0.0f);
    } else if (!strcmp(nm, "feImage")) {
        p->type = FE_IMAGE;
        p->href = attr_dup(xn, "href", "xlink:href");
        p->num[0] = plen(ctx, xn, "x", 0, bbox_units, 0.0f);
        p->num[1] = plen(ctx, xn, "y", 1, bbox_units, 0.0f);
        p->num[2] = plen(ctx, xn, "width", 0, bbox_units, 0.0f);
        p->num[3] = plen(ctx, xn, "height", 1, bbox_units, 0.0f);
    } else if (!strcmp(nm, "feMerge")) {
        p->type = FE_MERGE;
        for (int k = 0; k < xn->nkids && p->nmerge < 8; k++) {
            const XMLNode* mn = xn->kids[k];
            if (strcmp(mn->name, "feMergeNode")) continue;
            p->merge_in[p->nmerge++] = attr_dup(mn, "in", NULL);
        }
    } else if (!strcmp(nm, "feMergeNode")) {
        free(p);
        return NULL;                 /* only meaningful inside feMerge */
    } else if (!strcmp(nm, "feMorphology")) {
        p->type = FE_MORPHOLOGY;
        const char* op = xml_attr(xn, "operator");
        p->num[4] = (op && !strcmp(op, "dilate")) ? 1.0f : 0.0f;
        parse_pair(ctx, xn, "radius", 0, bbox_units, p, 0.0f);
    } else if (!strcmp(nm, "feOffset")) {
        p->type = FE_OFFSET;
        p->num[0] = plen(ctx, xn, "dx", 0, bbox_units, 0.0f);
        p->num[1] = plen(ctx, xn, "dy", 1, bbox_units, 0.0f);
    } else if (!strcmp(nm, "feTile")) {
        p->type = FE_TILE;
    } else if (!strcmp(nm, "feTurbulence")) {
        p->type = FE_TURBULENCE;
        const char* t = xml_attr(xn, "type");
        p->turb_type = (t && !strcmp(t, "fractalNoise")) ? FE_TURB_FRACTAL
                                                         : FE_TURB_TURBULENCE;
        const char* bf = xml_attr(xn, "baseFrequency");
        if (bf) {
            char buf[128];
            size_t n = strlen(bf);
            if (n >= sizeof buf) n = sizeof buf - 1;
            memcpy(buf, bf, n);
            buf[n] = 0;
            char* sep = strchr(buf, ',');
            if (!sep) sep = strchr(buf, ' ');
            if (sep) {
                *sep = 0;
                p->base_freq_x = fnum(buf, 0.0f);
                p->base_freq_y = fnum(sep + 1, p->base_freq_x);
            } else {
                p->base_freq_x = p->base_freq_y = fnum(buf, 0.0f);
            }
        }
        p->octaves = (int)fnum(xml_attr(xn, "numOctaves"), 1.0f);
        if (p->octaves < 1) p->octaves = 1;
        if (p->octaves > 8) p->octaves = 8;
        p->seed = (int)fnum(xml_attr(xn, "seed"), 0.0f);
        const char* st = xml_attr(xn, "stitchTiles");
        p->stitch = (st && !strcmp(st, "stitch")) ? 1 : 0;
    } else {
        free(p);
        return NULL;
    }

    if (strcmp(nm, "feMerge") && strcmp(nm, "feComponentTransfer"))
        p->in = attr_dup(xn, "in", NULL);
    p->in2 = attr_dup(xn, "in2", NULL);
    p->result = attr_dup(xn, "result", NULL);
    return p;
}

static void prim_free(FilterPrim* p) {
    if (!p) return;
    free(p->px);
    free(p->in);
    free(p->in2);
    free(p->result);
    free(p->href);
    for (int i = 0; i < p->nmerge; i++) free(p->merge_in[i]);
    free(p);
}

void filter_prims_free(FilterPrim* p, int n) {
    if (!p) return;
    for (int i = 0; i < n; i++) {
        free(p[i].in);
        free(p[i].in2);
        free(p[i].result);
        free(p[i].href);
        free(p[i].px);
        for (int k = 0; k < p[i].nmerge; k++) free(p[i].merge_in[k]);
    }
    free(p);
}

Node* build_filter(Ctx* ctx, const XMLNode* xn) {
    Node* n = svg_node_new(NODE_FILTERDEF);
    if (!n) return NULL;

    n->filt_units = UNITS_BBOX;
    const char* fu = xml_attr(xn, "filterUnits");
    if (fu && !strcmp(fu, "userSpaceOnUse")) n->filt_units = UNITS_USER;

    n->filt_prim_units = UNITS_USER;
    const char* pu = xml_attr(xn, "primitiveUnits");
    if (pu && !strcmp(pu, "objectBoundingBox")) n->filt_prim_units = UNITS_BBOX;

    /* Defaults: -10%, -10%, 120%, 120% of the target (SVG 1.1 §15.1). */
    float dx = (n->filt_units == UNITS_BBOX) ? -0.1f : -0.1f * ctx->vw;
    float dy = (n->filt_units == UNITS_BBOX) ? -0.1f : -0.1f * ctx->vh;
    float dw = (n->filt_units == UNITS_BBOX) ? 1.2f : 1.2f * ctx->vw;
    float dh = (n->filt_units == UNITS_BBOX) ? 1.2f : 1.2f * ctx->vh;
    if (n->filt_units == UNITS_BBOX) {
        n->filt_x = region_len(xn, "x", dx);
        n->filt_y = region_len(xn, "y", dy);
        n->filt_w = region_len(xn, "width", dw);
        n->filt_h = region_len(xn, "height", dh);
    } else {
        n->filt_x = len_attr(ctx, xn, "x", 0, dx);
        n->filt_y = len_attr(ctx, xn, "y", 1, dy);
        n->filt_w = len_attr(ctx, xn, "width", 0, dw);
        n->filt_h = len_attr(ctx, xn, "height", 1, dh);
    }

    const char* cifs = xml_attr(xn, "color-interpolation-filters");
    char val[64];
    if (style_get_decl(xml_attr(xn, "style"), "color-interpolation-filters",
                       val, sizeof val))
        cifs = val;
    n->filt_linear = !(cifs && !strcmp(cifs, "sRGB"));

    char* href = attr_dup(xn, "href", "xlink:href");
    if (href) {
        const char* h = href;
        if (*h == '#') h++;
        n->filt_href = svg_strdup(h);
        free(href);
    }

    int bbox_units = (n->filt_prim_units == UNITS_BBOX);
    for (int i = 0; i < xn->nkids; i++) {
        const XMLNode* c = xn->kids[i];
        if (strncmp(c->name, "fe", 2)) continue;
        FilterPrim* p = parse_prim(ctx, c, bbox_units);
        if (!p) continue;
        if (p->type == FE_IMAGE && p->href && strncmp(p->href, "data:", 5) &&
            ctx->base_path && *ctx->base_path) {
            unsigned char* px = NULL;
            int iw = 0, ih = 0;
            if (image_load_any(p->href, ctx->base_path, 0, &px, &iw, &ih)) {
                p->px = px;
                p->pw = iw;
                p->ph = ih;
            }
        }
        FilterPrim* np = (FilterPrim*)svg_realloc(
            n->prims, (size_t)(n->nprims + 1) * sizeof(FilterPrim));
        if (!np) { prim_free(p); continue; }
        n->prims = np;
        n->prims[n->nprims++] = *p;
        free(p);                 /* the struct was copied; only the pointee is freed */
    }
    return n;
}