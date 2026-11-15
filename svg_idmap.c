/* svg_idmap.c - Open-addressing id -> Node map. */
#include "svg_internal.h"

static unsigned hash_str(const char* s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

void idmap_put(IdMap* m, const char* k, Node* v) {
    if (!m->e) {
        m->cap = 32;
        m->e = (IdEntry*)svg_malloc((size_t)m->cap * sizeof(IdEntry));
        if (!m->e) { m->cap = 0; return; }
        memset(m->e, 0, (size_t)m->cap * sizeof(IdEntry));
    }
    if (m->n * 2 >= m->cap) {
        int nc = m->cap * 2;
        IdEntry* ne = (IdEntry*)svg_malloc((size_t)nc * sizeof(IdEntry));
        if (!ne) return;
        memset(ne, 0, (size_t)nc * sizeof(IdEntry));
        for (int i = 0; i < m->cap; i++) {
            if (!m->e[i].key) continue;
            unsigned h = hash_str(m->e[i].key) % (unsigned)nc;
            while (ne[h].key) h = (h + 1) % (unsigned)nc;
            ne[h] = m->e[i];
        }
        free(m->e);
        m->e = ne;
        m->cap = nc;
    }
    unsigned h = hash_str(k) % (unsigned)m->cap;
    int probes = 0;
    while (m->e[h].key) {
        if (!strcmp(m->e[h].key, k)) {
            /* Replacing an existing id: the key is shared with the old entry,
             * so keep it and only swap the value. */
            m->e[h].val = v;
            return;
        }
        if (++probes >= m->cap) return;   /* full table: do not spin */
        h = (h + 1) % (unsigned)m->cap;
    }
    m->e[h].key = svg_strdup(k);
    if (!m->e[h].key) return;             /* allocation failure: no entry */
    m->e[h].val = v;
    m->n++;
}

/* Drop the entry for `k` when it maps to `v`.  A node that is freed must be
 * unregistered first, otherwise the map keeps a dangling pointer that a later
 * <use href="#id"> would clone (use-after-free). */
void idmap_remove(IdMap* m, const char* k, const Node* v) {
    if (!m || !m->e || !m->cap || !k) return;
    unsigned h = hash_str(k) % (unsigned)m->cap;
    int probes = 0;
    while (m->e[h].key) {
        if (!strcmp(m->e[h].key, k)) {
            if (m->e[h].val == v) {
                free(m->e[h].key);
                m->e[h].key = NULL;
                m->e[h].val = NULL;
                m->n--;
            }
            return;
        }
        if (++probes >= m->cap) return;
        h = (h + 1) % (unsigned)m->cap;
    }
}

Node* idmap_get(IdMap* m, const char* k) {
    if (!m->e || m->cap == 0) return NULL;
    unsigned h = hash_str(k) % (unsigned)m->cap;
    int probes = 0;
    while (m->e[h].key) {
        if (!strcmp(m->e[h].key, k)) return m->e[h].val;
        if (++probes >= m->cap) return NULL;
        h = (h + 1) % (unsigned)m->cap;
    }
    return NULL;
}

void idmap_free(IdMap* m) {
    if (!m->e) return;
    for (int i = 0; i < m->cap; i++) free(m->e[i].key);
    free(m->e);
    m->e = NULL;
    m->n = m->cap = 0;
}
