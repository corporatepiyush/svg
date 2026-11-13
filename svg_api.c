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
    free(n->use_ref);
    free(n);
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

static void options_normalize(svg2img_options* o) {
    if (o->aa < 1) o->aa = 1;
    if (o->aa > 8) o->aa = 8;
    if (!(o->scale > 0.0f)) o->scale = 1.0f;
}

Node* svg_parse(const char* data, size_t len, const svg2img_options* opts) {
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
    compute_viewport(root, &o, &W, &H, &view);

    IdMap ids;
    memset(&ids, 0, sizeof(ids));

    Ctx ctx;
    ctx.vw = (float)W;
    ctx.vh = (float)H;
    ctx.em = 16.0f;
    ctx.ids = &ids;

    Style parent = style_default();
    Node* root_node = build_node(&ctx, root, &parent, 0);
    style_free(&parent);

    if (root_node) {
        svg_resolve(root_node, &ids);
        root_node->vp.w = W;
        root_node->vp.h = H;
        root_node->vp.tm = view;
        root_node->vp.valid = 1;
    }

    idmap_free(&ids);
    xml_free(doc);
    return root_node;
}

int svg2img_convert(const char* svg_data, size_t len, const char* out_path,
                    const svg2img_options* opts) {
    svg2img_options o;
    if (opts) o = *opts;
    else { memset(&o, 0, sizeof(o)); o.scale = 1.0f; o.aa = 4; }
    options_normalize(&o);

    Node* root = svg_parse(svg_data, len, &o);
    if (!root || !root->vp.valid) { svg_free(root); return 1; }

    int W = root->vp.w, H = root->vp.h;
    Canvas* c = canvas_create(W, H);
    if (!c) { svg_free(root); return 1; }

    Color bg = {0, 0, 0, 0};
    if (o.has_background) {
        unsigned int v = o.background;
        bg.r = (float)((v >> 24) & 0xFF) / 255.0f;
        bg.g = (float)((v >> 16) & 0xFF) / 255.0f;
        bg.b = (float)((v >> 8) & 0xFF) / 255.0f;
        bg.a = (float)(v & 0xFF) / 255.0f;
    }
    canvas_clear(c, bg);
    render_document(c, root, root->vp.tm, &o);
    int ok = png_save(out_path, c->px, W, H);

    canvas_free(c);
    svg_free(root);
    return ok ? 0 : 1;
}

int svg2img_convert_file(const char* in_path, const char* out_path,
                         const svg2img_options* opts) {
    FILE* f = fopen(in_path, "rb");
    if (!f) return 1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return 1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 1; }

    char* buf = (char*)svg_malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    int r = svg2img_convert(buf, rd, out_path, opts);
    free(buf);
    return r;
}