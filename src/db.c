/* Installed-package database and generations, as plain text under <root>/var/lib/fpm:
 *   installed/<name>/meta    key = value package metadata (see pkg_format)
 *   installed/<name>/files   one entry per line: "<type> <sha256 for /etc files, else -> <path>"
 *   installed/<name>/hold    present when the package is held back from upgrades
 *   generations/NNNNNN       snapshot of every installed package after a transaction
 *   current                  number of the newest generation
 * Metadata is read once per run and cached in memory. */
#include "fpm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

/* ---- package metadata ---- */

static void set(char **dst, const char *v)
{
    free(*dst);
    *dst = xstrdup(v);
}

static void pkg_kv(const char *k, const char *v, void *ud)
{
    pkg *p = ud;
    if (!strcmp(k, "name"))
        set(&p->name, v);
    else if (!strcmp(k, "version"))
        set(&p->version, v);
    else if (!strcmp(k, "release"))
        p->release = atoi(v);
    else if (!strcmp(k, "reason"))
        p->explicit_ = !strcmp(v, "explicit");
    else if (!strcmp(k, "description"))
        set(&p->description, v);
    else if (!strcmp(k, "arch"))
        set(&p->arch, v);
    else if (!strcmp(k, "sha256"))
        set(&p->sha256, v);
    else if (!strcmp(k, "size"))
        p->size = atoll(v);
    else if (!strcmp(k, "csize"))
        p->csize = atoll(v);
    else if (!strcmp(k, "depends"))
        sv_split(&p->depends, v);
    else if (!strcmp(k, "provides"))
        sv_split(&p->provides, v);
    else if (!strcmp(k, "conflicts"))
        sv_split(&p->conflicts, v);
    else if (!strcmp(k, "replaces"))
        sv_split(&p->replaces, v);
    else if (!strcmp(k, "shlib-provides"))
        sv_split(&p->shlib_provides, v);
    else if (!strcmp(k, "shlib-requires"))
        sv_split(&p->shlib_requires, v);
}

int pkg_parse(const char *text, pkg *p)
{
    memset(p, 0, sizeof *p);
    if (kv_parse(text, pkg_kv, p) || !p->name || !p->version || p->release < 1) {
        pkg_free(p);
        return -1;
    }
    return 0;
}

static void add_list(strvec *out, const char *key, const strvec *v)
{
    if (!v->n)
        return;
    char *s = sv_join(v, " ");
    sv_push_own(out, xasprintf("%s = %s", key, s));
    free(s);
}

char *pkg_format(const pkg *p, int with_reason)
{
    strvec l = {0};
    sv_push_own(&l, xasprintf("name = %s", p->name));
    sv_push_own(&l, xasprintf("version = %s", p->version));
    sv_push_own(&l, xasprintf("release = %d", p->release));
    if (with_reason)
        sv_push_own(&l, xasprintf("reason = %s", p->explicit_ ? "explicit" : "dependency"));
    if (p->arch)
        sv_push_own(&l, xasprintf("arch = %s", p->arch));
    if (p->description && *p->description)
        sv_push_own(&l, xasprintf("description = %s", p->description));
    add_list(&l, "depends", &p->depends);
    add_list(&l, "provides", &p->provides);
    add_list(&l, "conflicts", &p->conflicts);
    add_list(&l, "replaces", &p->replaces);
    add_list(&l, "shlib-provides", &p->shlib_provides);
    add_list(&l, "shlib-requires", &p->shlib_requires);
    if (p->size)
        sv_push_own(&l, xasprintf("size = %lld", p->size));
    if (p->csize)
        sv_push_own(&l, xasprintf("csize = %lld", p->csize));
    if (p->sha256)
        sv_push_own(&l, xasprintf("sha256 = %s", p->sha256));
    char *body = sv_join(&l, "\n");
    char *s = xasprintf("%s\n", body);
    free(body);
    sv_free(&l);
    return s;
}

void pkg_free(pkg *p)
{
    free(p->name);
    free(p->version);
    free(p->description);
    free(p->arch);
    free(p->repo);
    free(p->sha256);
    sv_free(&p->depends);
    sv_free(&p->provides);
    sv_free(&p->conflicts);
    sv_free(&p->replaces);
    sv_free(&p->shlib_provides);
    sv_free(&p->shlib_requires);
    memset(p, 0, sizeof *p);
}

/* ---- in-memory cache of installed metadata ---- */

static hmap meta_cache;          /* name -> meta text */
static strvec names_cache;
static int names_ready;
static hmap prov_idx, shlib_idx; /* provided name / soname -> package name */
static int idx_ready;

static void cache_forget(const char *name)
{
    free(hm_del(&meta_cache, name));
    sv_free(&names_cache);
    names_ready = 0;
    if (idx_ready) {
        hm_free(&prov_idx, free);
        hm_free(&shlib_idx, free);
        idx_ready = 0;
    }
}

/* ---- installed packages ---- */

int db_get(const config *c, const char *name, pkg *p)
{
    memset(p, 0, sizeof *p);
    if (!valid_name(name))
        return -1;
    const char *text = hm_get(&meta_cache, name);
    if (!text) {
        char *path = xasprintf("%s/installed/%s/meta", c->db, name);
        char *t = read_file(path, NULL);
        free(path);
        if (!t)
            return -1;
        hm_put(&meta_cache, name, t);
        text = t;
    }
    return pkg_parse(text, p);
}

void db_list(const config *c, strvec *names)
{
    if (!names_ready) {
        names_ready = 1;
        char *dir = xasprintf("%s/installed", c->db);
        DIR *d = opendir(dir);
        free(dir);
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.')
                sv_push(&names_cache, e->d_name);
        if (d)
            closedir(d);
        sv_sort(&names_cache);
    }
    for (size_t i = 0; i < names_cache.n; i++)
        sv_push(names, names_cache.v[i]);
}

static void build_idx(const config *c)
{
    idx_ready = 1;
    strvec names = {0};
    db_list(c, &names);
    for (size_t i = 0; i < names.n; i++) {
        pkg p;
        if (db_get(c, names.v[i], &p))
            continue;
        if (!hm_get(&prov_idx, p.name))
            hm_put(&prov_idx, p.name, xstrdup(p.name));
        for (size_t j = 0; j < p.provides.n; j++) {
            char *pn = dep_name(p.provides.v[j]);
            if (!hm_get(&prov_idx, pn))
                hm_put(&prov_idx, pn, xstrdup(p.name));
            free(pn);
        }
        for (size_t j = 0; j < p.shlib_provides.n; j++)
            if (!hm_get(&shlib_idx, p.shlib_provides.v[j]))
                hm_put(&shlib_idx, p.shlib_provides.v[j], xstrdup(p.name));
        pkg_free(&p);
    }
    sv_free(&names);
}

const char *db_provider(const config *c, const char *virt)
{
    if (!idx_ready)
        build_idx(c);
    return hm_get(&prov_idx, virt);
}

const char *db_shlib_provider(const config *c, const char *soname)
{
    if (!idx_ready)
        build_idx(c);
    return hm_get(&shlib_idx, soname);
}

int db_held(const config *c, const char *name)
{
    char *path = xasprintf("%s/installed/%s/hold", c->db, name);
    int held = path_exists(path);
    free(path);
    return held;
}

int db_set_held(const config *c, const char *name, int held)
{
    char *path = xasprintf("%s/installed/%s/hold", c->db, name);
    int rc = held ? write_file_atomic(path, "", 0) : (unlink(path) && errno != ENOENT ? -1 : 0);
    free(path);
    return rc;
}

int db_put(const config *c, const pkg *p, const manifest *m)
{
    char *dir = xasprintf("%s/installed/%s", c->db, p->name);
    if (mkdir_p(dir, 0755)) {
        free(dir);
        return -1;
    }
    size_t cap = 4096, n = 0;
    char *files = xmalloc(cap);
    for (size_t i = 0; i < m->n; i++) {
        const mentry *e = &m->e[i];
        size_t need = strlen(e->path) + 72;
        if (n + need > cap)
            files = xrealloc(files, cap = (n + need) * 2);
        n += (size_t)sprintf(files + n, "%c %s %s\n", e->type, e->sha[0] ? e->sha : "-", e->path);
    }
    char *meta = pkg_format(p, 1);
    char *mp = xasprintf("%s/meta", dir), *fp = xasprintf("%s/files", dir);
    int rc = write_file_atomic(fp, files, n);
    if (rc == 0)
        rc = write_file_atomic(mp, meta, strlen(meta));
    cache_forget(p->name);
    free(files);
    free(meta);
    free(mp);
    free(fp);
    free(dir);
    return rc;
}

int db_drop(const config *c, const char *name)
{
    char *dir = xasprintf("%s/installed/%s", c->db, name);
    int rc = rm_rf(dir);
    cache_forget(name);
    free(dir);
    return rc;
}

void db_lock(const config *c)
{
    static int fd = -1;
    if (fd >= 0)
        return;
    if (mkdir_p(c->db, 0755))
        die("cannot create %s: %s", c->db, strerror(errno));
    char *path = xasprintf("%s/lock", c->db);
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        die("cannot open %s: %s", path, strerror(errno));
    if (flock(fd, LOCK_EX | LOCK_NB))
        die("another fpm is running (lock: %s)", path);
    free(path);
}

/* ---- manifests ---- */

void manifest_push(manifest *m, char type, const char *sha, const char *path)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 64;
        m->e = xrealloc(m->e, m->cap * sizeof *m->e);
    }
    mentry *e = &m->e[m->n++];
    e->type = type;
    snprintf(e->sha, sizeof e->sha, "%s", sha && strcmp(sha, "-") ? sha : "");
    e->path = xstrdup(path);
}

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const mentry *)a)->path, ((const mentry *)b)->path);
}

void manifest_sort(manifest *m)
{
    if (m->n)
        qsort(m->e, m->n, sizeof *m->e, cmp_entry);
}

const mentry *manifest_find(const manifest *m, const char *path)
{
    mentry key = { .path = (char *)path };
    return m->n ? bsearch(&key, m->e, m->n, sizeof *m->e, cmp_entry) : NULL;
}

int manifest_read(const config *c, const char *name, manifest *m)
{
    memset(m, 0, sizeof *m);
    char *path = xasprintf("%s/installed/%s/files", c->db, name);
    char *text = read_file(path, NULL);
    free(path);
    if (!text)
        return -1;
    for (char *line = text, *nl; line && *line; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (strlen(line) < 5 || line[1] != ' ')
            continue;
        char *sha = line + 2, *sp = strchr(sha, ' ');
        if (!sp)
            continue;
        *sp = 0;
        manifest_push(m, line[0], sha, sp + 1);
    }
    free(text);
    manifest_sort(m);
    return 0;
}

void manifest_free(manifest *m)
{
    for (size_t i = 0; i < m->n; i++)
        free(m->e[i].path);
    free(m->e);
    memset(m, 0, sizeof *m);
}

/* ---- generations ---- */

char *gen_path(const config *c, int n)
{
    return xasprintf("%s/generations/%06d", c->db, n);
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

void gen_list(const config *c, int **nums, size_t *n)
{
    *nums = NULL;
    *n = 0;
    char *dir = xasprintf("%s/generations", c->db);
    DIR *d = opendir(dir);
    free(dir);
    if (!d)
        return;
    size_t cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        int v = atoi(e->d_name);
        if (v <= 0)
            continue;
        if (*n == cap)
            *nums = xrealloc(*nums, (cap = cap ? cap * 2 : 16) * sizeof **nums);
        (*nums)[(*n)++] = v;
    }
    closedir(d);
    if (*n)
        qsort(*nums, *n, sizeof **nums, cmp_int);
}

int gen_current(const config *c)
{
    char *path = xasprintf("%s/current", c->db);
    char *text = read_file(path, NULL);
    free(path);
    int n = text ? atoi(text) : 0;
    free(text);
    return n;
}

int gen_read(const config *c, int n, strvec *lines, char **header)
{
    char *path = gen_path(c, n);
    char *text = read_file(path, NULL);
    free(path);
    if (!text)
        return -1;
    if (header)
        *header = NULL;
    for (char *line = text, *nl; line && *line; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (line[0] == '#') {
            if (header && !*header)
                *header = xstrdup(line + 1);
        } else if (*line) {
            sv_push(lines, line);
        }
    }
    free(text);
    return 0;
}

/* Snapshot the installed set as a new generation (skipped if nothing changed). */
void gen_record(const config *c, const char *cmd)
{
    strvec names = {0}, lines = {0};
    db_list(c, &names);
    for (size_t i = 0; i < names.n; i++) {
        pkg p;
        if (db_get(c, names.v[i], &p))
            continue;
        sv_push_own(&lines, xasprintf("%s %s-%d %s", p.name, p.version, p.release,
                                      p.explicit_ ? "explicit" : "dependency"));
        pkg_free(&p);
    }
    char *body = sv_join(&lines, "\n");

    int cur = gen_current(c);
    strvec prev = {0};
    if (cur && gen_read(c, cur, &prev, NULL) == 0) {
        char *pb = sv_join(&prev, "\n");
        int same = strcmp(pb, body) == 0;
        free(pb);
        if (same)
            goto out;
    }

    int *nums;
    size_t n;
    gen_list(c, &nums, &n);
    int next = n ? nums[n - 1] + 1 : 1;
    free(nums);

    char *dir = xasprintf("%s/generations", c->db);
    mkdir_p(dir, 0755);
    free(dir);
    char *text = xasprintf("# time=%lld cmd=%s\n%s%s", (long long)time(NULL), cmd, body, lines.n ? "\n" : "");
    char *path = gen_path(c, next);
    char *cpath = xasprintf("%s/current", c->db);
    char *num = xasprintf("%d\n", next);
    if (write_file_atomic(path, text, strlen(text)) || write_file_atomic(cpath, num, strlen(num)))
        warnf("cannot record generation %d", next);
    free(text);
    free(path);
    free(cpath);
    free(num);
out:
    free(body);
    sv_free(&prev);
    sv_free(&lines);
    sv_free(&names);
}
