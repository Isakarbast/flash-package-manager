/* String-keyed hash map: open addressing, FNV-1a, tombstones on delete. */
#include "fpm.h"

#include <stdlib.h>
#include <string.h>

static char TOMB;

static size_t hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return (size_t)h;
}

static hent *find(const hmap *m, const char *key)
{
    if (!m->cap)
        return NULL;
    for (size_t i = hash(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        hent *e = &m->t[i];
        if (!e->key)
            return NULL;
        if (e->key != &TOMB && strcmp(e->key, key) == 0)
            return e;
    }
}

static void grow(hmap *m)
{
    hent *old = m->t;
    size_t oldcap = m->cap;
    m->cap = oldcap ? oldcap * 2 : 64;
    m->t = calloc(m->cap, sizeof *m->t);
    if (!m->t)
        die("out of memory");
    m->used = m->live = 0;
    for (size_t i = 0; i < oldcap; i++) {
        if (!old[i].key || old[i].key == &TOMB)
            continue;
        size_t j = hash(old[i].key) & (m->cap - 1);
        while (m->t[j].key)
            j = (j + 1) & (m->cap - 1);
        m->t[j] = old[i];
        m->used++;
        m->live++;
    }
    free(old);
}

void *hm_get(const hmap *m, const char *key)
{
    hent *e = find(m, key);
    return e ? e->val : NULL;
}

void hm_put(hmap *m, const char *key, void *val)
{
    hent *e = find(m, key);
    if (e) {
        e->val = val;
        return;
    }
    if ((m->used + 1) * 4 > m->cap * 3)
        grow(m);
    size_t i = hash(key) & (m->cap - 1);
    while (m->t[i].key && m->t[i].key != &TOMB)
        i = (i + 1) & (m->cap - 1);
    if (!m->t[i].key)
        m->used++;
    m->t[i].key = xstrdup(key);
    m->t[i].val = val;
    m->live++;
}

void *hm_del(hmap *m, const char *key)
{
    hent *e = find(m, key);
    if (!e)
        return NULL;
    void *val = e->val;
    free(e->key);
    e->key = &TOMB;
    e->val = NULL;
    m->live--;
    return val;
}

void hm_free(hmap *m, void (*free_val)(void *))
{
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->t[i].key || m->t[i].key == &TOMB)
            continue;
        free(m->t[i].key);
        if (free_val)
            free_val(m->t[i].val);
    }
    free(m->t);
    memset(m, 0, sizeof *m);
}
