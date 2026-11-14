/* svg_api.c - Public entry points and render-tree teardown. */
#include "svg_internal.h"

void svg_free(Node* n) {
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) svg_free(n->kids[i]);
    free(n->kids);
    path_free(n->path);
    free(n->fill.stops);
    free(n->fill.ref);
    free(n->stroke.stops);
    free(n->stroke.ref);
    free(n->dash);
    free(n->text);
    free(n->id);
    free(n->clip_ref);
    free(n->mask_ref);
    free(n->use_ref);
    free(n);
}

/* Free a subtree that is being discarded while `ids` is still alive.  Every
 * id registered inside the subtree must be dropped first: leaving it behind
 * lets a later <use href="#id"> clone freed memory. */
void svg_free_detached(Node* n, IdMap* ids) {
    if (!n) return;
    if (ids && n->id) idmap_remove(ids, n->id, n);
    for (int i = 0; i < n->nkids; i++) svg_free_detached(n->kids[i], ids);
    free(n->kids);
    path_free(n->path);
    free(n->fill.stops); free(n->fill.ref);
    free(n->stroke.stops); free(n->stroke.ref);
    free(n->dash); free(n->text); free(n->id); free(n->clip_ref);
    free(n->mask_ref); free(n->use_ref);
    free(n);
}

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

void svg2img_default_limits(svg2img_limits* out) {
    if (!out) return;
    out->max_nodes         = SVG2IMG_LIMIT_MAX_NODES;
    out->max_depth         = SVG2IMG_LIMIT_MAX_DEPTH;
    out->max_path_points   = SVG2IMG_LIMIT_MAX_PATH_POINTS;
    out->max_path_subpaths = SVG2IMG_LIMIT_MAX_PATH_SUBPATHS;
    out->max_gradient_stops= SVG2IMG_LIMIT_MAX_GRADIENT_STOPS;
    out->max_text_length   = SVG2IMG_LIMIT_MAX_TEXT_LENGTH;
    out->max_clip_depth    = SVG2IMG_LIMIT_MAX_CLIP_DEPTH;
    out->max_layers        = SVG2IMG_LIMIT_MAX_LAYERS;
    out->max_use_expansion = SVG2IMG_LIMIT_MAX_USE_EXPANSION;
    out->max_pixels        = SVG2IMG_LIMIT_MAX_PIXELS;
}

static int lim_or(int v, int def) { return v > 0 ? v : def; }

/* Resolve every limit field to a positive value.  A non-positive field
 * selects the compiled-in default, so a zero-initialised options struct is
 * always valid. */
void limits_resolve(const svg2img_limits* in, svg2img_limits* out) {
    svg2img_limits d;
    svg2img_default_limits(&d);
    if (!in) { *out = d; return; }
    out->max_nodes         = lim_or(in->max_nodes, d.max_nodes);
    out->max_depth         = lim_or(in->max_depth, d.max_depth);
    out->max_path_points   = lim_or(in->max_path_points, d.max_path_points);
    out->max_path_subpaths = lim_or(in->max_path_subpaths, d.max_path_subpaths);
    out->max_gradient_stops= lim_or(in->max_gradient_stops, d.max_gradient_stops);
    out->max_text_length   = lim_or(in->max_text_length, d.max_text_length);
    out->max_clip_depth    = lim_or(in->max_clip_depth, d.max_clip_depth);
    out->max_layers        = lim_or(in->max_layers, d.max_layers);
    out->max_use_expansion = lim_or(in->max_use_expansion, d.max_use_expansion);
    out->max_pixels        = lim_or(in->max_pixels, d.max_pixels);
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

static void options_normalize(svg2img_options* o) {
    if (o->aa < 1) o->aa = 1;
    if (o->aa > 8) o->aa = 8;
    if (!(o->scale > 0.0f)) o->scale = 1.0f;
}

Node* svg_parse(const char* data, size_t len, const svg2img_options* opts,
                int* limit_hit) {
    if (limit_hit) *limit_hit = 0;
    svg2img_options o;
    if (opts) o = *opts;
    else { memset(&o, 0, sizeof(o)); o.scale = 1.0f; o.aa = 4; }
    options_normalize(&o);

    XMLNode* doc = xml_parse(data, len);
    if (!doc) return NULL;
    XMLNode* root = xml_child(doc, "svg");
    if (!root) { xml_free(doc); return NULL; }

    int W = 0, H = 0;
    Mat view = mat_identity();
    svg2img_limits lim;
    limits_resolve(&o.limits, &lim);
    int clamped = 0;

    IdMap ids;
    memset(&ids, 0, sizeof(ids));

    compute_viewport(root, &o, &W, &H, &view, &lim, &clamped);
    /* A canvas that had to be scaled down to fit max_pixels cannot produce a
     * faithful image, so report it as a limit error rather than returning a
     * silently smaller image. */
    if (clamped) {
        if (limit_hit) *limit_hit = 1;
        idmap_free(&ids);
        xml_free(doc);
        return NULL;
    }

    Ctx ctx;
    ctx.vw = (float)W;
    ctx.vh = (float)H;
    ctx.em = 16.0f;
    ctx.ids = &ids;
    ctx.lim = &lim;
    ctx.nodes = 0;
    ctx.limit_hit = 0;

    Style parent = style_default();
    Node* root_node = build_node(&ctx, root, &parent, 0);
    style_free(&parent);

    if (root_node) {
        if (svg_resolve(root_node, &ids, &lim)) ctx.limit_hit = 1;
        root_node->vp.w = W;
        root_node->vp.h = H;
        root_node->vp.tm = view;
        root_node->vp.valid = 1;
    }

    idmap_free(&ids);
    xml_free(doc);
    /* A hard limit was hit while building: the caller maps this to
     * SVG2IMG_ERR_LIMIT.  NULL also means "nothing was built" or an
     * allocation failure, which the caller reports as a format error. */
    if (ctx.limit_hit) { if (limit_hit) *limit_hit = 1; return NULL; }
    return root_node;
}

int svg2img_convert(const char* svg_data, size_t len, const char* out_path,
                    const svg2img_options* opts) {
    if (!svg_data || !out_path) return SVG2IMG_ERR_ARG;

    svg2img_options o;
    if (opts) o = *opts;
    else { memset(&o, 0, sizeof(o)); o.scale = 1.0f; o.aa = 4; }
    options_normalize(&o);
    svg2img_limits lim;
    limits_resolve(&o.limits, &lim);

    int hit_limit = 0;
    Node* root = svg_parse(svg_data, len, &o, &hit_limit);
    if (!root) {
        svg_free(root);
        return hit_limit ? SVG2IMG_ERR_LIMIT : SVG2IMG_ERR_FORMAT;
    }
    if (!root->vp.valid) { svg_free(root); return SVG2IMG_ERR_FORMAT; }

    int W = root->vp.w, H = root->vp.h;
    if ((int64_t)W * (int64_t)H > (int64_t)lim.max_pixels) {
        svg_free(root);
        return SVG2IMG_ERR_LIMIT;
    }
    Canvas* c = canvas_create(W, H);
    if (!c) { svg_free(root); return SVG2IMG_ERR_NOMEM; }

    Color bg = {0, 0, 0, 0};
    if (o.has_background) {
        unsigned int v = o.background;
        bg.r = (float)((v >> 24) & 0xFF) / 255.0f;
        bg.g = (float)((v >> 16) & 0xFF) / 255.0f;
        bg.b = (float)((v >> 8) & 0xFF) / 255.0f;
        bg.a = (float)(v & 0xFF) / 255.0f;
    }
    canvas_clear(c, bg);
    render_document(c, root, mat_identity(), &o, &lim);
    int ok = png_save(out_path, c->px, W, H);

    canvas_free(c);
    svg_free(root);
    return ok ? SVG2IMG_OK : SVG2IMG_ERR_IO;
}

int svg2img_convert_file(const char* in_path, const char* out_path,
                         const svg2img_options* opts) {
    if (!in_path || !out_path) return SVG2IMG_ERR_ARG;
    FILE* f = fopen(in_path, "rb");
    if (!f) return SVG2IMG_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return SVG2IMG_ERR_IO; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return SVG2IMG_ERR_IO; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return SVG2IMG_ERR_IO; }

    char* buf = (char*)svg_malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return SVG2IMG_ERR_NOMEM; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    int r = svg2img_convert(buf, rd, out_path, opts);
    free(buf);
    return r;
}