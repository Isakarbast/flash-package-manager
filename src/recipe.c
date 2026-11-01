/* Recipes: <repo>/<name>/recipe (key = value) plus an optional build script.
 * A recipe can split extra packages out of its build with "<sub>.files",
 * "<sub>.depends", "<sub>.description", "<sub>.provides", "<sub>.conflicts"
 * and "<sub>.replaces" keys. */
#include "fpm.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int valid_name(const char *s)
{
    if (!isalnum((unsigned char)*s))
        return 0;
    for (; *s; s++)
        if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && !strchr("+._-", *s))
            return 0;
    return 1;
}

int is_git_source(const char *src)
{
    const char *sep = strstr(src, "::");
    return starts_with(sep ? sep + 2 : src, "git+");
}

static void set_str(char **dst, const char *v)
{
    free(*dst);
    *dst = xstrdup(v);
}

static subpkg *get_sub(recipe *r, const char *name)
{
    for (size_t i = 0; i < r->nsubs; i++)
        if (!strcmp(r->subs[i].name, name))
            return &r->subs[i];
    r->subs = xrealloc(r->subs, (r->nsubs + 1) * sizeof *r->subs);
    subpkg *s = &r->subs[r->nsubs++];
    memset(s, 0, sizeof *s);
    s->name = xstrdup(name);
    return s;
}

static void on_kv(const char *k, const char *v, void *ud)
{
    recipe *r = ud;
    const char *dot = strchr(k, '.');
    if (dot) {
        char *sub = strndup(k, (size_t)(dot - k));
        subpkg *s = get_sub(r, sub);
        free(sub);
        const char *f = dot + 1;
        if (!strcmp(f, "files"))
            sv_split(&s->files, v);
        else if (!strcmp(f, "depends"))
            sv_split(&s->depends, v);
        else if (!strcmp(f, "provides"))
            sv_split(&s->provides, v);
        else if (!strcmp(f, "conflicts"))
            sv_split(&s->conflicts, v);
        else if (!strcmp(f, "replaces"))
            sv_split(&s->replaces, v);
        else if (!strcmp(f, "description"))
            set_str(&s->description, v);
        else
            warnf("%s: unknown key '%s' ignored", r->dir, k);
    } else if (!strcmp(k, "name")) {
        set_str(&r->name, v);
    } else if (!strcmp(k, "version")) {
        set_str(&r->version, v);
    } else if (!strcmp(k, "release")) {
        r->release = atoi(v);
    } else if (!strcmp(k, "description")) {
        set_str(&r->description, v);
    } else if (!strcmp(k, "license")) {
        set_str(&r->license, v);
    } else if (!strcmp(k, "url")) {
        set_str(&r->url, v);
    } else if (!strcmp(k, "arch")) {
        set_str(&r->arch, v);
    } else if (!strcmp(k, "depends")) {
        sv_split(&r->depends, v);
    } else if (!strcmp(k, "makedepends")) {
        sv_split(&r->makedepends, v);
    } else if (!strcmp(k, "provides")) {
        sv_split(&r->provides, v);
    } else if (!strcmp(k, "conflicts")) {
        sv_split(&r->conflicts, v);
    } else if (!strcmp(k, "replaces")) {
        sv_split(&r->replaces, v);
    } else if (!strcmp(k, "source")) {
        sv_split(&r->sources, v);
    } else if (!strcmp(k, "sha256")) {
        sv_split(&r->sha256, v);
    } else if (!strcmp(k, "options")) {
        sv_split(&r->options, v);
    } else if (!strcmp(k, "subpackages")) {
        strvec names = {0};
        sv_split(&names, v);
        for (size_t i = 0; i < names.n; i++)
            get_sub(r, names.v[i]);
        sv_free(&names);
    } else {
        warnf("%s: unknown key '%s' ignored", r->dir, k);
    }
}

/* Expand ${name}, ${version} and ${release} in a source URL. */
static char *expand(const recipe *r, const char *s)
{
    size_t cap = strlen(s) + 64, n = 0;
    char *out = xmalloc(cap);
    while (*s) {
        const char *val = NULL;
        size_t skip = 0;
        if (starts_with(s, "${name}"))
            val = r->name, skip = 7;
        else if (starts_with(s, "${version}"))
            val = r->version, skip = 10;
        char relbuf[16];
        if (starts_with(s, "${release}")) {
            snprintf(relbuf, sizeof relbuf, "%d", r->release);
            val = relbuf, skip = 10;
        }
        const char *add = val ? val : s;
        size_t len = val ? strlen(val) : 1;
        if (n + len + 1 > cap)
            out = xrealloc(out, cap = (n + len + 1) * 2);
        memcpy(out + n, add, len);
        n += len;
        s += val ? skip : 1;
    }
    out[n] = 0;
    return out;
}

static int check_source(const char *path, const char *src, const char *sha)
{
    if (is_git_source(src)) {
        const char *frag = strrchr(src, '#');
        if (strcmp(sha, "SKIP")) {
            warnf("%s: git source %s takes sha256 = SKIP (the commit pins it)", path, src);
            return -1;
        }
        if (!frag || !(starts_with(frag + 1, "commit=") || starts_with(frag + 1, "tag=") ||
                       starts_with(frag + 1, "branch="))) {
            warnf("%s: git source %s needs #commit=, #tag= or #branch=", path, src);
            return -1;
        }
        return 0;
    }
    if (strstr(src, "://") && strcmp(sha, "SKIP") == 0) {
        warnf("%s: remote source %s needs a real sha256", path, src);
        return -1;
    }
    return 0;
}

int recipe_load(const char *dir, recipe *r)
{
    memset(r, 0, sizeof *r);
    r->dir = xstrdup(dir);
    r->release = 1;
    char *path = xasprintf("%s/recipe", dir);
    char *text = read_file(path, NULL);
    if (!text) {
        warnf("%s: cannot read recipe", path);
        goto bad;
    }
    int line = kv_parse(text, on_kv, r);
    free(text);
    if (line) {
        warnf("%s:%d: expected 'key = value'", path, line);
        goto bad;
    }

    const char *base = strrchr(dir, '/');
    base = base ? base + 1 : dir;
    if (!r->name || !valid_name(r->name)) {
        warnf("%s: missing or invalid name (use a-z 0-9 + . _ -)", path);
        goto bad;
    }
    if (strcmp(r->name, base) != 0) {
        warnf("%s: name '%s' does not match its directory '%s'", path, r->name, base);
        goto bad;
    }
    if (!r->version || !*r->version || strpbrk(r->version, "-/ \t")) {
        warnf("%s: missing or invalid version (no '-', '/' or spaces)", path);
        goto bad;
    }
    if (r->release < 1) {
        warnf("%s: release must be a positive number", path);
        goto bad;
    }
    if (r->sources.n != r->sha256.n) {
        warnf("%s: %zu source(s) but %zu sha256 value(s)", path, r->sources.n, r->sha256.n);
        goto bad;
    }
    for (size_t i = 0; i < r->sources.n; i++) {
        char *e = expand(r, r->sources.v[i]);
        free(r->sources.v[i]);
        r->sources.v[i] = e;
        if (check_source(path, e, r->sha256.v[i]))
            goto bad;
    }
    for (size_t i = 0; i < r->nsubs; i++) {
        const subpkg *s = &r->subs[i];
        if (!valid_name(s->name) || !strcmp(s->name, r->name)) {
            warnf("%s: invalid subpackage name '%s'", path, s->name);
            goto bad;
        }
        if (!s->files.n) {
            warnf("%s: subpackage %s has no %s.files", path, s->name, s->name);
            goto bad;
        }
    }
    r->target = xstrdup(r->name);
    free(path);
    return 0;

bad:
    free(path);
    recipe_free(r);
    return -1;
}

/* ---- index of what the recipe repositories offer (built on first use) ---- */

static hmap dir_of;        /* package name (incl. subpackages) -> recipe dir */
static hmap provider_of;   /* provided name -> package name */
static int index_ready;

static void index_add(const char *name, const char *dir)
{
    if (!hm_get(&dir_of, name))
        hm_put(&dir_of, name, xstrdup(dir));
}

static void index_provides(const strvec *prov, const char *name)
{
    for (size_t i = 0; i < prov->n; i++) {
        char *pn = dep_name(prov->v[i]);
        if (!hm_get(&provider_of, pn))
            hm_put(&provider_of, pn, xstrdup(name));
        free(pn);
    }
}

static void build_index(const config *c)
{
    index_ready = 1;
    for (size_t i = 0; i < c->repos.n; i++) {
        DIR *d = opendir(c->repos.v[i]);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            char *dir = xasprintf("%s/%s", c->repos.v[i], e->d_name);
            char *rp = xasprintf("%s/recipe", dir);
            if (path_exists(rp) && !hm_get(&dir_of, e->d_name)) {
                index_add(e->d_name, dir);
                /* Only recipes that split or provide something need a full parse. */
                char *text = read_file(rp, NULL);
                recipe r;
                if (text && (strstr(text, ".files") || strstr(text, "provides")) &&
                    recipe_load(dir, &r) == 0) {
                    index_provides(&r.provides, r.name);
                    for (size_t j = 0; j < r.nsubs; j++) {
                        index_add(r.subs[j].name, dir);
                        index_provides(&r.subs[j].provides, r.subs[j].name);
                    }
                    recipe_free(&r);
                }
                free(text);
            }
            free(rp);
            free(dir);
        }
        if (d)
            closedir(d);
    }
}

int recipe_find(const config *c, const char *name, recipe *r)
{
    if (!valid_name(name))
        return -1;
    if (!index_ready)
        build_index(c);
    const char *dir = hm_get(&dir_of, name);
    if (!dir || recipe_load(dir, r))
        return -1;
    free(r->target);
    r->target = xstrdup(name);
    return 0;
}

const char *recipe_provider(const config *c, const char *virt)
{
    if (!index_ready)
        build_index(c);
    return hm_get(&provider_of, virt);
}

void recipe_names(const config *c, strvec *out)
{
    if (!index_ready)
        build_index(c);
    for (size_t i = 0; i < dir_of.cap; i++)
        if (dir_of.t[i].key && dir_of.t[i].val)
            sv_push(out, dir_of.t[i].key);
    sv_sort(out);
}

pkgview recipe_view(const recipe *r, const char *pkgname)
{
    for (size_t i = 0; i < r->nsubs; i++) {
        const subpkg *s = &r->subs[i];
        if (!strcmp(s->name, pkgname))
            return (pkgview){ s->name, s->description ? s->description : r->description,
                              &s->depends, &s->provides, &s->conflicts, &s->replaces };
    }
    return (pkgview){ r->name, r->description, &r->depends, &r->provides, &r->conflicts, &r->replaces };
}

void recipe_free(recipe *r)
{
    free(r->name);
    free(r->version);
    free(r->description);
    free(r->license);
    free(r->url);
    free(r->dir);
    free(r->arch);
    free(r->target);
    sv_free(&r->depends);
    sv_free(&r->makedepends);
    sv_free(&r->sources);
    sv_free(&r->sha256);
    sv_free(&r->options);
    sv_free(&r->provides);
    sv_free(&r->conflicts);
    sv_free(&r->replaces);
    for (size_t i = 0; i < r->nsubs; i++) {
        subpkg *s = &r->subs[i];
        free(s->name);
        free(s->description);
        sv_free(&s->depends);
        sv_free(&s->files);
        sv_free(&s->provides);
        sv_free(&s->conflicts);
        sv_free(&s->replaces);
    }
    free(r->subs);
    memset(r, 0, sizeof *r);
}

int recipe_has_option(const recipe *r, const char *opt)
{
    return sv_contains(&r->options, opt);
}

void dep_parse(const char *spec, char **name, char **op, char **ver)
{
    size_t n = strcspn(spec, "<>=");
    size_t o = strspn(spec + n, "<>=");
    *name = strndup(spec, n);
    *op = strndup(spec + n, o);
    *ver = xstrdup(spec + n + o);
    if (!*name || !*op)
        die("out of memory");
}

char *dep_name(const char *spec)
{
    char *s = strndup(spec, strcspn(spec, "<>="));
    if (!s)
        die("out of memory");
    return s;
}

int dep_satisfied(const char *op, const char *want, const char *have)
{
    if (!op || !*op)
        return 1;
    int c = vercmp(have, want);
    if (!strcmp(op, ">="))
        return c >= 0;
    if (!strcmp(op, "<="))
        return c <= 0;
    if (!strcmp(op, ">"))
        return c > 0;
    if (!strcmp(op, "<"))
        return c < 0;
    if (!strcmp(op, "=") || !strcmp(op, "=="))
        return c == 0;
    return 0;
}
