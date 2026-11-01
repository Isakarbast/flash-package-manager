/* Configuration file (default /etc/fpm.conf):
 *
 *   key = value          global settings
 *   [repo NAME]          binary repository: url, key, allow_unsigned
 *   [recipes NAME]       recipe repository: path, git, require_signed, review
 *
 * The older top-level "repos = DIR..." still works and adds recipe repositories. */
#include "fpm.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

char *rpath(const config *c, const char *rel)
{
    while (*rel == '/')
        rel++;
    if (!strcmp(c->root, "/"))
        return xasprintf("/%s", rel);
    return xasprintf("%s/%s", c->root, rel);
}

const reciperepo *recipe_repo_of(const config *c, const char *recipe_dir)
{
    for (size_t i = 0; i < c->nrrepos; i++) {
        size_t l = strlen(c->rrepos[i].path);
        if (!strncmp(recipe_dir, c->rrepos[i].path, l) && recipe_dir[l] == '/')
            return &c->rrepos[i];
    }
    return NULL;
}

static int yes(const char *v)
{
    return !strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1");
}

static void set(char **dst, const char *v)
{
    free(*dst);
    *dst = xstrdup(v);
}

static reciperepo *add_recipes(config *c, const char *name)
{
    c->rrepos = xrealloc(c->rrepos, (c->nrrepos + 1) * sizeof *c->rrepos);
    reciperepo *r = &c->rrepos[c->nrrepos++];
    memset(r, 0, sizeof *r);
    r->name = xstrdup(name);
    return r;
}

static binrepo *add_repo(config *c, const char *name)
{
    c->brepos = xrealloc(c->brepos, (c->nbrepos + 1) * sizeof *c->brepos);
    binrepo *r = &c->brepos[c->nbrepos++];
    memset(r, 0, sizeof *r);
    r->name = xstrdup(name);
    return r;
}

static int legacy_signed;
static size_t legacy_first = (size_t)-1, legacy_count;

static int global_kv(config *c, const char *k, const char *v)
{
    if (!strcmp(k, "root")) {
        if (!c->root)
            c->root = xstrdup(v);
    } else if (!strcmp(k, "arch")) {
        set(&c->arch, v);
    } else if (!strcmp(k, "repos")) {
        strvec paths = {0};
        sv_split(&paths, v);
        for (size_t i = 0; i < paths.n; i++) {
            const char *base = strrchr(paths.v[i], '/');
            if (legacy_first == (size_t)-1)
                legacy_first = c->nrrepos;
            add_recipes(c, base && base[1] ? base + 1 : paths.v[i])->path = xstrdup(paths.v[i]);
            legacy_count++;
        }
        sv_free(&paths);
    } else if (!strcmp(k, "jobs")) {
        c->jobs = atoi(v);
    } else if (!strcmp(k, "parallel")) {
        if (!c->parallel)
            c->parallel = atoi(v);
    } else if (!strcmp(k, "cflags")) {
        set(&c->cflags, v);
    } else if (!strcmp(k, "cxxflags")) {
        set(&c->cxxflags, v);
    } else if (!strcmp(k, "ldflags")) {
        set(&c->ldflags, v);
    } else if (!strcmp(k, "proxy")) {
        set(&c->proxy, v);
    } else if (!strcmp(k, "sandbox")) {
        c->sandbox = !strcmp(v, "yes") ? 1 : !strcmp(v, "no") ? 0 : -1;
    } else if (!strcmp(k, "require_signed")) {
        legacy_signed = yes(v);
    } else if (!strcmp(k, "prefer")) {
        if (strcmp(v, "binary") && strcmp(v, "source"))
            return -1;
        c->prefer_source = !strcmp(v, "source");
    } else if (!strcmp(k, "shlib_ignore")) {
        sv_split(&c->shlib_ignore, v);
    } else {
        warnf("%s: unknown setting '%s' ignored", c->conf, k);
    }
    return 0;
}

static void repo_kv(config *c, binrepo *r, const char *k, const char *v)
{
    if (!strcmp(k, "url"))
        set(&r->url, v);
    else if (!strcmp(k, "key"))
        set(&r->key, v);
    else if (!strcmp(k, "allow_unsigned"))
        r->allow_unsigned = yes(v);
    else
        warnf("%s: [repo %s]: unknown setting '%s' ignored", c->conf, r->name, k);
}

static void recipes_kv(config *c, reciperepo *r, const char *k, const char *v)
{
    if (!strcmp(k, "path"))
        set(&r->path, v);
    else if (!strcmp(k, "git"))
        set(&r->git, v);
    else if (!strcmp(k, "require_signed"))
        r->require_signed = yes(v);
    else if (!strcmp(k, "review"))
        r->review = yes(v);
    else
        warnf("%s: [recipes %s]: unknown setting '%s' ignored", c->conf, r->name, k);
}

static void parse(config *c, char *text)
{
    enum { GLOBAL, REPO, RECIPES } section = GLOBAL;
    int lineno = 0;
    for (char *p = text, *nl; p; p = nl ? nl + 1 : NULL) {
        nl = strchr(p, '\n');
        if (nl)
            *nl = 0;
        lineno++;
        char *line = trim(p);
        if (!*line || *line == '#')
            continue;
        if (*line == '[') {
            char kind[16], name[128];
            if (sscanf(line, "[%15s %127[^]]]", kind, name) != 2 || !valid_name(trim(name)))
                die("%s:%d: expected [repo NAME] or [recipes NAME]", c->conf, lineno);
            if (!strcmp(kind, "repo")) {
                section = REPO;
                add_repo(c, trim(name));
            } else if (!strcmp(kind, "recipes")) {
                section = RECIPES;
                add_recipes(c, trim(name));
            } else {
                die("%s:%d: unknown section [%s]", c->conf, lineno, kind);
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq)
            die("%s:%d: expected 'key = value'", c->conf, lineno);
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        if (section == GLOBAL) {
            if (global_kv(c, k, v))
                die("%s:%d: bad value for %s", c->conf, lineno, k);
        } else if (section == REPO) {
            repo_kv(c, &c->brepos[c->nbrepos - 1], k, v);
        } else {
            recipes_kv(c, &c->rrepos[c->nrrepos - 1], k, v);
        }
    }
}

void config_load(config *c, int conf_given)
{
    c->sandbox = -1;
    c->cflags = xstrdup("-O2 -pipe -march=native");
    c->ldflags = xstrdup("-Wl,-O1,--as-needed");
    char *text = read_file(c->conf, NULL);
    if (text) {
        parse(c, text);
        free(text);
    } else if (conf_given) {
        die("cannot read %s: %s", c->conf, strerror(errno));
    }

    for (size_t i = 0; i < legacy_count; i++)
        c->rrepos[legacy_first + i].require_signed = legacy_signed;
    for (size_t i = 0; i < c->nbrepos; i++) {
        const binrepo *r = &c->brepos[i];
        if (!r->url)
            die("%s: [repo %s] needs a url", c->conf, r->name);
        if (!r->key && !r->allow_unsigned)
            die("%s: [repo %s] needs a key (or allow_unsigned = yes)", c->conf, r->name);
    }
    if (!c->nrrepos)
        add_recipes(c, "main")->path = xstrdup("/var/db/fpm/recipes/main");
    for (size_t i = 0; i < c->nrrepos; i++) {
        if (!c->rrepos[i].path)
            die("%s: [recipes %s] needs a path", c->conf, c->rrepos[i].name);
        sv_push(&c->repos, c->rrepos[i].path);
    }

    if (!c->cxxflags)
        c->cxxflags = xstrdup(c->cflags);
    if (!c->arch) {
        struct utsname u;
        c->arch = xstrdup(uname(&u) == 0 ? u.machine : "unknown");
    }
    if (!c->root)
        c->root = xstrdup("/");
    if (c->root[0] != '/') {
        char abs[PATH_MAX];
        if (!realpath(c->root, abs))
            die("root %s: %s", c->root, strerror(errno));
        set(&c->root, abs);
    }
    size_t l = strlen(c->root);
    while (l > 1 && c->root[l - 1] == '/')
        c->root[--l] = 0;
    if (c->parallel <= 0)
        c->parallel = 2;
    if (c->jobs <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        c->jobs = n > 0 ? (int)n : 1;
    }
    c->db = rpath(c, "var/lib/fpm");
    c->cache = rpath(c, "var/cache/fpm");
    c->tmp = rpath(c, "var/tmp/fpm");
}
