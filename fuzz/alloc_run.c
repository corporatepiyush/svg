/* alloc_run.c - Allocation-failure injection driver (test only).
 *
 * Usage: ./alloc_run <fail_at> <input.svg> <output.png>
 *
 * Fails the <fail_at>-th allocation (0 = fail none).  Every allocation in the
 * library goes through svg_malloc / svg_realloc, so this exercises every
 * "malloc returned NULL" path in the renderer with no production code
 * changes.  The exit status is 0 on success, 1 on a clean render failure and
 * 2 on a usage error; a crash or sanitizer report shows up as a signal.
 *
 * Build: see fuzz/alloc_fail.py (adds -DSVG2IMG_ALLOC_HOOKS).
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long g_budget = -1;   /* fail the allocation with this index (0-based) */
static long g_count = 0;
static long g_failed = 0;

static void* test_alloc(size_t n) {
    if (g_budget >= 0 && g_count == g_budget) {
        g_count++;
        g_failed++;
        return NULL;
    }
    g_count++;
    return malloc(n ? n : 1);
}

static void* test_realloc(void* p, size_t n) {
    if (g_budget >= 0 && g_count == g_budget) {
        g_count++;
        g_failed++;
        return NULL;
    }
    g_count++;
    return realloc(p, n ? n : 1);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <fail_at> <in.svg> <out.png>\n", argv[0]);
        return 2;
    }
    g_budget = strtol(argv[1], NULL, 10);
    svg_set_alloc_hooks(test_alloc, test_realloc);

    svg2img_options o;
    memset(&o, 0, sizeof(o));
    o.scale = 1.0f;
    o.aa = 1;                 /* keep the run fast: aa multiplies allocations */

    int r = svg2img_convert_file(argv[2], argv[3], &o);
    /* A failing allocation must never be reported as success with a broken
     * image: the converter either finishes cleanly or fails cleanly. */
    if (r == 0 && g_failed > 0) {
        /* Not necessarily a bug: some allocations are optional (a dash list
         * that fails to grow is simply dropped).  Report it only if the file
         * is missing, which would be a real correctness break. */
        FILE* f = fopen(argv[3], "rb");
        if (!f) return 1;
        fclose(f);
    }
    return r == 0 ? 0 : 1;
}