/* cli.c - Command-line option parsing for svg2img. */
#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cli_usage(const char* prog) {
    fprintf(stderr,
        "svg2img %s - pure C17 SVG to PNG converter\n"
        "Usage: %s [options] input.svg output.png\n"
        "\n"
        "Options:\n"
        "  -w, --width N       Output width in pixels (0 = auto)\n"
        "  -h, --height N      Output height in pixels (0 = auto)\n"
        "  -s, --scale F       Extra uniform scale factor (default 1.0)\n"
        "  -a, --aa N          Supersampling factor 1..8 (default 4)\n"
        "  -b, --background C  Background colour RRGGBB or RRGGBBAA\n"
        "  -v, --verbose       Print progress information\n"
        "      --help          Show this help\n",
        SVG2IMG_VERSION, prog);
}

static int parse_hex_color(const char* s, unsigned int* out) {
    int n = 0;
    while (s[n] && n < 8) {
        char c = s[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) break;
        n++;
    }
    if (n != 6 && n != 8) return 0;
    unsigned int v = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else d = (unsigned)(c - 'A' + 10);
        v = (v << 4) | d;
    }
    if (n == 6) v = (v << 8) | 0xFF;
    *out = v;
    return 1;
}

int cli_parse(int argc, char** argv, svg2img_options* opts,
              const char** in, const char** out) {
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "-w") || !strcmp(a, "--width")) {
            if (++i >= argc) { cli_usage(argv[0]); return 2; }
            opts->width = atoi(argv[i]);
        } else if (!strcmp(a, "-h") || !strcmp(a, "--height")) {
            if (++i >= argc) { cli_usage(argv[0]); return 2; }
            opts->height = atoi(argv[i]);
        } else if (!strcmp(a, "-s") || !strcmp(a, "--scale")) {
            if (++i >= argc) { cli_usage(argv[0]); return 2; }
            opts->scale = (float)atof(argv[i]);
        } else if (!strcmp(a, "-a") || !strcmp(a, "--aa")) {
            if (++i >= argc) { cli_usage(argv[0]); return 2; }
            opts->aa = atoi(argv[i]);
        } else if (!strcmp(a, "-b") || !strcmp(a, "--background")) {
            if (++i >= argc || !parse_hex_color(argv[i], &opts->background)) {
                cli_usage(argv[0]); return 2;
            }
            opts->has_background = 1;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
            opts->verbose = 1;
        } else if (!strcmp(a, "--help")) {
            cli_usage(argv[0]);
            return 1;
        } else if (a[0] == '-' && a[1] != 0) {
            fprintf(stderr, "svg2img: unknown option '%s'\n", a);
            cli_usage(argv[0]);
            return 2;
        } else if (!*in) {
            *in = a;
        } else if (!*out) {
            *out = a;
        } else {
            cli_usage(argv[0]);
            return 2;
        }
    }
    if (!*in || !*out) { cli_usage(argv[0]); return 2; }
    return 0;
}
/* ------------------------------------------------------------------ */
/* Resource limits from the environment                                */
/* ------------------------------------------------------------------ */

static int env_int(const char* name, int* out) {
    const char* s = getenv(name);
    if (!s || !*s) return 0;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return 0;
    if (v < 0) v = 0;
    if (v > 0x7FFFFFFFL) v = 0x7FFFFFFFL;
    *out = (int)v;
    return 1;
}

void cli_limits_from_env(svg2img_options* opts) {
    if (!opts) return;
    svg2img_limits* l = &opts->limits;
    env_int("SVG2IMG_LIMIT_MAX_NODES", &l->max_nodes);
    env_int("SVG2IMG_LIMIT_MAX_DEPTH", &l->max_depth);
    env_int("SVG2IMG_LIMIT_MAX_PATH_POINTS", &l->max_path_points);
    env_int("SVG2IMG_LIMIT_MAX_PATH_SUBPATHS", &l->max_path_subpaths);
    env_int("SVG2IMG_LIMIT_MAX_GRADIENT_STOPS", &l->max_gradient_stops);
    env_int("SVG2IMG_LIMIT_MAX_TEXT_LENGTH", &l->max_text_length);
    env_int("SVG2IMG_LIMIT_MAX_CLIP_DEPTH", &l->max_clip_depth);
    env_int("SVG2IMG_LIMIT_MAX_LAYERS", &l->max_layers);
    env_int("SVG2IMG_LIMIT_MAX_USE_EXPANSION", &l->max_use_expansion);
    env_int("SVG2IMG_LIMIT_MAX_PIXELS", &l->max_pixels);
}
