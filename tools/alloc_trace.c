/* alloc_trace.c - Allocation-pattern recorder (development build only).
 *
 * Build the renderer with -DSVG2IMG_ALLOC_TRACE and link this file.  Every
 * svg_malloc / svg_calloc / svg_realloc / svg_mfree records an event with
 * the requested size, the call site, a stable allocation id and the old size
 * for reallocations.  The binary trace is analysed by
 * tools/analyze_alloc_trace.py.
 *
 * This is deliberately a single-threaded tracer: svg2img renders a document
 * synchronously, and adding atomics would change the timing being studied.
 *
 * Trace layout (little-endian, native types):
 *   char     magic[8] = "SVGTRAC1"
 *   uint32_t version  = 1
 *   uint32_t nsites
 *   uint64_t nevents
 *   sites: uint32_t len; char bytes[len]       (repeated nsites times)
 *   events: see trace_event_t                  (repeated nevents times)
 */
#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACE_MAGIC "SVGTRAC1"
#define TRACE_VERSION 1u

enum {
    TRACE_ALLOC = 0,
    TRACE_REALLOC = 1,
    TRACE_FREE = 2,
    TRACE_CALLOC = 3,
    TRACE_MARK = 4,
    TRACE_FREE_UNTRACKED = 5
};

typedef struct {
    uint32_t op;
    uint32_t site;
    uint64_t ptr;
    uint64_t old_ptr;
    uint64_t size;       /* requested new size */
    uint64_t old_size;   /* requested old size for realloc */
    uint64_t id;
    uint32_t origin;     /* site that created the allocation */
    uint32_t pad;
} trace_event_t;

typedef struct {
    const char* file;
    int line;
    char* name;
} trace_site_t;

typedef struct {
    uintptr_t ptr;       /* 0 = empty, 1 = tombstone */
    uint64_t size;
    uint64_t id;
    uint32_t origin;
    uint32_t pad;
} trace_entry_t;

#define SITE_HASH_SIZE 4096u
#define MAP_INITIAL    65536u

static trace_site_t* g_sites;
static uint32_t g_nsites, g_site_cap;
static uint32_t g_site_hash[SITE_HASH_SIZE];

static trace_entry_t* g_map;
static size_t g_map_cap, g_map_live, g_map_tomb;

static trace_event_t* g_events;
static size_t g_nevents, g_event_cap;

static uint64_t g_next_id = 1;
static uint64_t g_live_bytes, g_live_count;
static uint64_t g_peak_bytes, g_peak_count;
static uint64_t g_nalloc, g_nrealloc, g_nfree, g_nuntracked;
static int g_registered;
static int g_disabled;

static void trace_flush(void);

static void trace_fail(void) {
    g_disabled = 1;
    free(g_sites);
    free(g_map);
    free(g_events);
    g_sites = NULL;
    g_map = NULL;
    g_events = NULL;
}

static void trace_register(void) {
    if (g_registered) return;
    g_registered = 1;
    atexit(trace_flush);
}

static uint32_t site_hash(const char* file, int line) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)file; *p; p++)
        h = (h ^ *p) * 16777619u;
    h ^= (uint32_t)line + 0x9e3779b9u;
    return h;
}

static uint32_t intern_site(const char* file, int line) {
    uint32_t h = site_hash(file, line);
    uint32_t mask = SITE_HASH_SIZE - 1;
    for (uint32_t probe = 0; probe < SITE_HASH_SIZE; probe++) {
        uint32_t* slot = &g_site_hash[(h + probe) & mask];
        if (*slot == 0) {
            if (g_nsites == g_site_cap) {
                uint32_t ncap = g_site_cap ? g_site_cap * 2 : 64;
                trace_site_t* ns = (trace_site_t*)realloc(
                    g_sites, (size_t)ncap * sizeof(*ns));
                if (!ns) { trace_fail(); return 0; }
                g_sites = ns;
                g_site_cap = ncap;
            }
            char buf[512];
            if (line > 0)
                snprintf(buf, sizeof buf, "%s:%d", file, line);
            else
                snprintf(buf, sizeof buf, "%s", file);
            size_t n = strlen(buf);
            char* name = (char*)malloc(n + 1);
            if (!name) { trace_fail(); return 0; }
            memcpy(name, buf, n + 1);
            g_sites[g_nsites].file = file;
            g_sites[g_nsites].line = line;
            g_sites[g_nsites].name = name;
            *slot = ++g_nsites;
            return g_nsites - 1;
        }
        uint32_t id = *slot - 1;
        if (g_sites[id].line == line &&
            (g_sites[id].file == file || strcmp(g_sites[id].file, file) == 0))
            return id;
    }
    return 0;
}

static uint64_t ptr_hash(uintptr_t p) {
    p ^= p >> 33;
    p *= (uintptr_t)0xff51afd7ed558ccdULL;
    p ^= p >> 33;
    p *= (uintptr_t)0xc4ceb9fe1a85ec53ULL;
    p ^= p >> 33;
    return (uint64_t)p;
}

static int map_grow(void) {
    size_t ncap = g_map_cap ? g_map_cap * 2 : MAP_INITIAL;
    trace_entry_t* nm = (trace_entry_t*)calloc(ncap, sizeof(*nm));
    if (!nm) return 0;
    size_t mask = ncap - 1;
    for (size_t i = 0; i < g_map_cap; i++) {
        if (g_map[i].ptr > 1) {
            size_t j = (size_t)(ptr_hash(g_map[i].ptr) & mask);
            while (nm[j].ptr != 0) j = (j + 1) & mask;
            nm[j] = g_map[i];
        }
    }
    free(g_map);
    g_map = nm;
    g_map_cap = ncap;
    g_map_tomb = 0;
    return 1;
}

static trace_entry_t* map_find(uintptr_t p) {
    if (g_map_cap == 0) return NULL;
    size_t mask = g_map_cap - 1;
    size_t i = (size_t)(ptr_hash(p) & mask);
    for (;;) {
        if (g_map[i].ptr == 0) return NULL;
        if (g_map[i].ptr == p) return &g_map[i];
        i = (i + 1) & mask;
    }
}

static int map_insert(uintptr_t p, uint64_t size, uint64_t id, uint32_t origin) {
    if ((g_map_live + g_map_tomb + 1) * 10 >= g_map_cap * 7) {
        if (!map_grow()) return 0;
    }
    size_t mask = g_map_cap - 1;
    size_t i = (size_t)(ptr_hash(p) & mask);
    size_t tomb = SIZE_MAX;
    for (;;) {
        if (g_map[i].ptr == 0) {
            if (tomb != SIZE_MAX) i = tomb;
            g_map[i].ptr = p;
            g_map[i].size = size;
            g_map[i].id = id;
            g_map[i].origin = origin;
            g_map_live++;
            return 1;
        }
        if (g_map[i].ptr == 1) {
            if (tomb == SIZE_MAX) tomb = i;
        } else if (g_map[i].ptr == p) {
            g_map[i].size = size;
            g_map[i].id = id;
            g_map[i].origin = origin;
            return 1;
        }
        i = (i + 1) & mask;
    }
}

static void map_remove(trace_entry_t* e) {
    e->ptr = 1;
    g_map_live--;
    g_map_tomb++;
}

static int event_push(const trace_event_t* ev) {
    if (g_nevents == g_event_cap) {
        size_t ncap = g_event_cap ? g_event_cap * 2 : 65536;
        trace_event_t* ne = (trace_event_t*)realloc(
            g_events, ncap * sizeof(*ne));
        if (!ne) { trace_fail(); return 0; }
        g_events = ne;
        g_event_cap = ncap;
    }
    g_events[g_nevents++] = *ev;
    return 1;
}

static void live_add(uint64_t n) {
    g_live_count++;
    g_live_bytes += n;
    if (g_live_count > g_peak_count) g_peak_count = g_live_count;
    if (g_live_bytes > g_peak_bytes) g_peak_bytes = g_live_bytes;
}

static void live_sub(uint64_t n) {
    if (g_live_count) g_live_count--;
    if (g_live_bytes >= n) g_live_bytes -= n;
    else g_live_bytes = 0;
}

void* svg_trace_malloc(size_t n, const char* file, int line) {
    if (g_disabled) return malloc(n ? n : 1);
    trace_register();
    uint32_t site = intern_site(file, line);
    void* p = malloc(n ? n : 1);
    trace_event_t ev = {TRACE_ALLOC, site, (uint64_t)(uintptr_t)p, 0,
                        n ? n : 1, 0, 0, site, 0};
    g_nalloc++;
    if (p) {
        ev.id = g_next_id++;
        if (!map_insert((uintptr_t)p, n ? n : 1, ev.id, site)) {
            ev.id = 0;
        }
        live_add(n ? n : 1);
    }
    event_push(&ev);
    return p;
}

void* svg_trace_calloc(size_t n, const char* file, int line) {
    if (g_disabled) return calloc(1, n ? n : 1);
    trace_register();
    uint32_t site = intern_site(file, line);
    void* p = calloc(1, n ? n : 1);
    trace_event_t ev = {TRACE_CALLOC, site, (uint64_t)(uintptr_t)p, 0,
                        n ? n : 1, 0, 0, site, 0};
    if (p) {
        ev.id = g_next_id++;
        if (!map_insert((uintptr_t)p, n ? n : 1, ev.id, site)) {
            ev.id = 0;
        }
        live_add(n ? n : 1);
    }
    event_push(&ev);
    return p;
}

void* svg_trace_realloc(void* p, size_t n, const char* file, int line) {
    if (g_disabled) return realloc(p, n ? n : 1);
    trace_register();
    uint32_t site = intern_site(file, line);
    trace_entry_t* old = p ? map_find((uintptr_t)p) : NULL;
    uint64_t old_size = old ? old->size : 0;
    uint64_t old_id = old ? old->id : 0;
    uint32_t origin = old ? old->origin : site;
    if (old) map_remove(old);

    void* q = realloc(p, n ? n : 1);
    trace_event_t ev = {TRACE_REALLOC, site, (uint64_t)(uintptr_t)q,
                        (uint64_t)(uintptr_t)p, n ? n : 1, old_size,
                        old_id, origin, 0};
    g_nrealloc++;
    if (q) {
        if (old_id == 0) ev.id = g_next_id++;
        if (!map_insert((uintptr_t)q, n ? n : 1, ev.id, origin)) {
            ev.id = 0;
        }
        live_sub(old_size);
        live_add(n ? n : 1);
    } else if (p) {
        /* A failed realloc leaves the original block alive. */
        if (!map_insert((uintptr_t)p, old_size, old_id, origin)) {
            /* cannot track it any more; leave the stats conservative */
        }
    }
    event_push(&ev);
    return q;
}

void svg_trace_free(void* p, const char* file, int line) {
    if (!p) return;
    if (g_disabled) { free(p); return; }
    trace_register();
    uint32_t site = intern_site(file, line);
    trace_entry_t* e = p ? map_find((uintptr_t)p) : NULL;
    trace_event_t ev = {TRACE_FREE, site, (uint64_t)(uintptr_t)p, 0, 0, 0,
                        0, 0, 0};
    if (e) {
        ev.id = e->id;
        ev.old_size = e->size;
        ev.origin = e->origin;
        live_sub(e->size);
        map_remove(e);
        g_nfree++;
    } else if (p) {
        ev.op = TRACE_FREE_UNTRACKED;
        g_nuntracked++;
    }
    event_push(&ev);
    free(p);
}

char* svg_trace_strdup(const char* s, const char* file, int line) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* r = (char*)svg_trace_malloc(n + 1, file, line);
    if (r) memcpy(r, s, n + 1);
    return r;
}

char* svg_trace_strndup(const char* s, size_t n, const char* file, int line) {
    char* r = (char*)svg_trace_malloc(n + 1, file, line);
    if (r) { memcpy(r, s, n); r[n] = 0; }
    return r;
}

void svg_trace_mark(const char* name) {
    if (g_disabled) return;
    trace_register();
    uint32_t site = intern_site(name, 0);
    trace_event_t ev = {TRACE_MARK, site, 0, 0, 0, 0, 0, 0, 0};
    event_push(&ev);
}

static void trace_flush(void) {
    if (g_disabled) return;
    const char* path = getenv("SVG2IMG_TRACE_FILE");
    if (!path || !*path) path = "/tmp/svg2img_alloc.trace";
    FILE* f = fopen(path, "wb");
    if (!f) {
        g_disabled = 1;
        return;
    }
    uint32_t version = TRACE_VERSION;
    uint32_t nsites = g_nsites;
    uint64_t nevents = g_nevents;
    fwrite(TRACE_MAGIC, 1, 8, f);
    fwrite(&version, sizeof version, 1, f);
    fwrite(&nsites, sizeof nsites, 1, f);
    fwrite(&nevents, sizeof nevents, 1, f);
    for (uint32_t i = 0; i < g_nsites; i++) {
        uint32_t len = (uint32_t)strlen(g_sites[i].name);
        fwrite(&len, sizeof len, 1, f);
        fwrite(g_sites[i].name, 1, len, f);
    }
    if (g_nevents)
        fwrite(g_events, sizeof(*g_events), g_nevents, f);
    fclose(f);
    fprintf(stderr,
            "alloc-trace: %llu alloc, %llu realloc, %llu free, %llu untracked; "
            "peak live %llu objects / %llu bytes\n",
            (unsigned long long)g_nalloc, (unsigned long long)g_nrealloc,
            (unsigned long long)g_nfree, (unsigned long long)g_nuntracked,
            (unsigned long long)g_peak_count, (unsigned long long)g_peak_bytes);
}
