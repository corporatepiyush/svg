/* svg_alloc.c - Allocator indirection used only by the test builds.
 *
 * A normal build never compiles this file: internal.h makes svg_malloc and
 * svg_realloc `static inline` wrappers around libc, so production has no
 * indirect call and no extra state.  Test targets compile the whole library
 * with -DSVG2IMG_ALLOC_HOOKS and link this file, which lets a test replace
 * the allocator with one that fails on demand (see fuzz/alloc_fail.c).
 */
#include "internal.h"

#ifdef SVG2IMG_ALLOC_HOOKS

static svg_alloc_fn   g_alloc   = malloc;
static svg_realloc_fn g_realloc = realloc;

void svg_set_alloc_hooks(svg_alloc_fn alloc, svg_realloc_fn realloc_fn) {
    g_alloc   = alloc   ? alloc   : malloc;
    g_realloc = realloc_fn ? realloc_fn : realloc;
}

void* svg_malloc(size_t n) {
    return g_alloc(n ? n : 1);
}

void* svg_realloc(void* p, size_t n) {
    return g_realloc(p, n ? n : 1);
}

#endif /* SVG2IMG_ALLOC_HOOKS */

/* Keep this translation unit non-empty even when the hooks are compiled out,
 * so a build that lists it unconditionally still links. */
typedef int svg_alloc_unit_placeholder;