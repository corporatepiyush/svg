/* main.c - Command-line driver for svg2img. */
#include "cli.h"
#include "svg2img.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    svg2img_options o;
    memset(&o, 0, sizeof(o));
    o.scale = 1.0f;
    o.aa = 4;

    const char* in = NULL;
    const char* out = NULL;

    int cr = cli_parse(argc, argv, &o, &in, &out);
    if (cr == 1) return 0;
    if (cr != 0) return 1;

    /* Environment overrides are applied after the flags so a test can set a
     * limit without adding a command-line option. */
    cli_limits_from_env(&o);

    if (o.verbose) {
        fprintf(stderr, "svg2img: %s -> %s (scale %.3f, aa %d)\n",
                in, out, o.scale, o.aa);
    }

    int r = svg2img_convert_file(in, out, &o);
    if (r) {
        fprintf(stderr, "svg2img: conversion failed\n");
        return 1;
    }
    if (o.verbose) fprintf(stderr, "svg2img: wrote %s\n", out);
    return 0;
}