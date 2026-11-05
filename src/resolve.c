/* Dependency resolution.
 *
 * A package comes from a candidate: a binary repository entry or a recipe to
 * build. When both exist the newer version wins; on a tie, binary unless
 * prefer = source (or --source). A dependency may name a package or anything a
 * package provides. Binary packages also pull in whatever provides the shared
 * libraries they need. The walk is depth-first, emits packages in install
 * order (dependencies first), and detects cycles. Build-time dependencies are
 * only pulled in for packages that actually need building. */
#include "fpm.h"

#include <stdlib.h>
#include <string.h>

/* ---- candidates ---- */

/* What the planner chose for each package, so later lookups in the same run
 * (scheduling, installing) get the same version and source. */
static hmap pins;
static char SOURCE_PIN;

static int cand_pick(const config *c, const char *name, const char *op, const char *want, cand *x)
{
    memset(x, 0, sizeof *x);
    void *pin = hm_get(&pins, name);
    const pkg *bp = pin == &SOURCE_PIN ? NULL
                  : pin                ? pin
                  : op && *op          ? repo_find_match(c, name, op, want)
                                       : repo_find(c, name);
    int have_r = pin && pin != &SOURCE_PIN ? 0 : recipe_find(c, name, &x->r) == 0;
    if (have_r && op && *op && !dep_satisfied(op, want, x->r.version)) {
        recipe_free(&x->r);   /* the recipe's version doesn't fit the constraint */
        have_r = 0;
    }
    if (!bp && !have_r)
        return -1;
    int binary;
    if (!have_r)
        binary = 1;
    else if (!bp || c->source_only)
        binary = 0;
    else {
        int cmp = vrcmp(bp->version, bp->release, x->r.version, x->r.release);
        binary = cmp > 0 || (cmp == 0 && !c->prefer_source);
    }
    if (binary) {
        if (have_r)
            recipe_free(&x->r);
        x->binary = 1;
        x->rp = bp;
        x->name = xstrdup(bp->name);
        x->version = xstrdup(bp->version);
        x->release = bp->release;
    } else {
        x->name = xstrdup(name);
        x->version = xstrdup(x->r.version);
        x->release = x->r.release;
    }
    return 0;
}

int cand_find(const config *c, const char *name, cand *x)
{
    return cand_pick(c, name, NULL, NULL, x);
}

pkgview cand_view(const cand *x)
{
    if (x->binary)
        return (pkgview){ x->rp->name, x->rp->description, &x->rp->depends,
                          &x->rp->provides, &x->rp->conflicts, &x->rp->replaces };
    return recipe_view(&x->r, x->name);
}

void cand_free(cand *x)
{
    free(x->name);
    free(x->version);
    if (!x->binary)
        recipe_free(&x->r);
    memset(x, 0, sizeof *x);
}

/* Is the soname present: ignored by config, provided by an installed package,
 * or (for systems not entirely managed by fpm, and packages built before 32-bit
 * names were tagged) a file in a library dir of the right word size? */
int shlib_satisfied(const config *c, const char *soname)
{
    if (db_shlib_provider(c, soname))
        return 1;
    static const char *dirs64[] = { "usr/lib", "usr/lib64", "lib", "lib64", NULL };
    static const char *dirs32[] = { "usr/lib32", "lib32", NULL };
    size_t n = strlen(soname), tn = strlen(SHLIB32_TAG);
    int is32 = n > tn && !strcmp(soname + n - tn, SHLIB32_TAG);
    char *name = is32 ? strndup(soname, n - tn) : xstrdup(soname);
    const char **dirs = is32 ? dirs32 : dirs64;
    int ok = sv_contains(&c->shlib_ignore, soname) || sv_contains(&c->shlib_ignore, name);
    for (int i = 0; !ok && dirs[i]; i++) {
        char *rel = xasprintf("%s/%s", dirs[i], name);
        char *p = rpath(c, rel);
        ok = path_exists(p);
        free(rel);
        free(p);
    }
    free(name);
    return ok;
}

/* ---- planning ---- */

enum { UNSEEN, VISITING, DONE };

typedef struct {
    const config *c;
    strvec *order;
    hmap state;
} planner;

static int visit(planner *pl, const char *spec, int is_target);

static void mark(planner *pl, const char *name, int st)
{
    hm_put(&pl->state, name, (void *)(intptr_t)st);
}

static int need_shlib(planner *pl, const char *owner, const char *so)
{
    if (shlib_satisfied(pl->c, so))
        return 0;
    const pkg *prov = repo_shlib_provider(pl->c, so);
    /* A library from a package already being planned (gcc's libstdc++ for
     * binutils, while gcc itself depends on binutils) is not a cycle: it is
     * part of this transaction, and the install waits for it. */
    if (prov && (intptr_t)hm_get(&pl->state, prov->name) == VISITING)
        return 0;
    if (prov)
        return visit(pl, prov->name, 0);
    warnf("%s needs %s, but no package provides it", owner, so);
    return -1;
}

static int visit(planner *pl, const char *spec, int is_target)
{
    const config *c = pl->c;
    char *name, *op, *ver;
    dep_parse(spec, &name, &op, &ver);
    int ret = 0, installed = 0, have = 0;
    pkg inst = {0};
    cand x;

    intptr_t st = (intptr_t)hm_get(&pl->state, name);
    if (st == VISITING) {
        warnf("dependency cycle involving %s", name);
        ret = -1;
        goto out;
    }
    if (st == DONE)
        goto out;

    installed = db_get(c, name, &inst) == 0;
    if (!is_target) {
        if (installed && dep_satisfied(op, ver, inst.version)) {
            mark(pl, name, DONE);
            goto out;
        }
        if (!installed && !*op && db_provider(c, name)) {
            mark(pl, name, DONE);   /* an installed package provides it */
            goto out;
        }
    }

    have = cand_pick(c, name, op, ver, &x) == 0;
    if (!have && *op && cand_find(c, name, &x) == 0) {
        warnf("%s%s%s is required, but the repositories have no such version (newest: %s-%d)",
              name, op, ver, x.version, x.release);
        cand_free(&x);
        ret = -1;
        goto out;
    }
    if (!have) {
        if (!is_target) {
            const pkg *rp = repo_provider(c, name);
            const char *pn = rp ? rp->name : recipe_provider(c, name);
            if (pn) {
                mark(pl, name, DONE);
                ret = visit(pl, pn, 0);
                goto out;
            }
        }
        warnf("%s: not found in any repository", name);
        ret = -1;
        goto out;
    }

    int need;
    if (is_target)
        need = !installed || c->reinstall ||
               vrcmp(x.version, x.release, inst.version, inst.release) != 0;
    else
        need = !installed || !dep_satisfied(op, ver, inst.version);

    if (need && !dep_satisfied(op, ver, x.version)) {
        warnf("%s%s%s is required, but the repositories have %s %s", name, op, ver, name, x.version);
        ret = -1;
        goto out;
    }

    mark(pl, name, VISITING);
    if (need) {
        hm_put(&pins, name, x.binary ? (void *)x.rp : &SOURCE_PIN);
        pkgview v = cand_view(&x);
        for (size_t i = 0; i < v.depends->n && !ret; i++)
            ret = visit(pl, v.depends->v[i], 0);
        if (x.binary) {
            for (size_t i = 0; i < x.rp->shlib_requires.n && !ret; i++)
                ret = need_shlib(pl, name, x.rp->shlib_requires.v[i]);
        } else if (c->rebuild || !recipe_built(c, &x.r)) {
            /* Building needs the whole recipe's dependencies, not just this package's. */
            for (size_t i = 0; i < x.r.depends.n && !ret; i++)
                ret = visit(pl, x.r.depends.v[i], 0);
            for (size_t i = 0; i < x.r.makedepends.n && !ret; i++)
                ret = visit(pl, x.r.makedepends.v[i], 0);
        }
        if (!ret)
            sv_push(pl->order, name);
    } else if (is_target) {
        step("%s %s-%d is up to date", name, inst.version, inst.release);
    }
    mark(pl, name, DONE);

out:
    if (have)
        cand_free(&x);
    if (installed)
        pkg_free(&inst);
    free(name);
    free(op);
    free(ver);
    return ret;
}

/* Replaced packages are scheduled for removal; anything else that conflicts
 * with the plan stops it (unless --force). */
static int check_conflicts(const config *c, const strvec *order, strvec *removals)
{
    int bad = 0;
    for (size_t i = 0; i < order->n; i++) {
        cand x;
        if (cand_find(c, order->v[i], &x))
            continue;
        pkgview v = cand_view(&x);
        for (size_t j = 0; j < v.replaces->n; j++) {
            char *rn = dep_name(v.replaces->v[j]);
            pkg q;
            if (strcmp(rn, x.name) && !sv_contains(order, rn) && !sv_contains(removals, rn) &&
                db_get(c, rn, &q) == 0) {
                sv_push(removals, rn);
                pkg_free(&q);
            }
            free(rn);
        }
        cand_free(&x);
    }

    for (size_t i = 0; i < order->n; i++) {
        cand x;
        if (cand_find(c, order->v[i], &x))
            continue;
        pkgview v = cand_view(&x);
        for (size_t j = 0; j < v.conflicts->n; j++) {
            char *dn, *op, *ver;
            dep_parse(v.conflicts->v[j], &dn, &op, &ver);
            pkg q;
            if (!strcmp(dn, x.name)) {
                /* a package never conflicts with itself */
            } else if (sv_contains(order, dn)) {
                warnf("%s and %s conflict; they can't be installed together", x.name, dn);
                bad = 1;
            } else if (!sv_contains(removals, dn) && db_get(c, dn, &q) == 0) {
                if (dep_satisfied(op, ver, q.version)) {
                    warnf("%s conflicts with the installed %s; remove it first", x.name, dn);
                    bad = 1;
                }
                pkg_free(&q);
            }
            free(dn);
            free(op);
            free(ver);
        }
        cand_free(&x);
    }

    strvec names = {0};
    db_list(c, &names);
    for (size_t i = 0; i < names.n; i++) {
        pkg q;
        if (sv_contains(order, names.v[i]) || sv_contains(removals, names.v[i]) ||
            db_get(c, names.v[i], &q))
            continue;
        for (size_t j = 0; j < q.conflicts.n; j++) {
            char *dn = dep_name(q.conflicts.v[j]);
            if (sv_contains(order, dn)) {
                warnf("the installed %s conflicts with %s", q.name, dn);
                bad = 1;
            }
            free(dn);
        }
        pkg_free(&q);
    }
    sv_free(&names);
    return bad && !c->force ? -1 : 0;
}

int plan_install(const config *c, const strvec *targets, strvec *order, strvec *removals)
{
    planner pl = { .c = c, .order = order };
    int ret = 0;
    for (size_t i = 0; i < targets->n && !ret; i++)
        ret = visit(&pl, targets->v[i], 1);
    hm_free(&pl.state, NULL);
    if (!ret)
        ret = check_conflicts(c, order, removals);
    return ret;
}

/* ---- reverse dependencies ---- */

static int provides_name(const strvec *provides, const char *what)
{
    for (size_t i = 0; i < provides->n; i++) {
        char *pn = dep_name(provides->v[i]);
        int hit = !strcmp(pn, what);
        free(pn);
        if (hit)
            return 1;
    }
    return 0;
}

/* Does an installed package other than `except` (and those in ignore) provide what? */
static int provided_elsewhere(const config *c, const char *what, int shlib,
                              const char *except, const strvec *ignore)
{
    strvec names = {0};
    db_list(c, &names);
    int found = 0;
    for (size_t i = 0; i < names.n && !found; i++) {
        pkg q;
        if (!strcmp(names.v[i], except) || (ignore && sv_contains(ignore, names.v[i])) ||
            db_get(c, names.v[i], &q))
            continue;
        found = shlib ? sv_contains(&q.shlib_provides, what)
                      : !strcmp(q.name, what) || provides_name(&q.provides, what);
        pkg_free(&q);
    }
    sv_free(&names);
    return found;
}

/* Installed packages (other than those in ignore) that would break without
 * name: they depend on it, on something only it provides, or on a shared
 * library only it ships. */
void required_by(const config *c, const char *name, const strvec *ignore, strvec *out)
{
    pkg p;
    if (db_get(c, name, &p))
        return;
    strvec names = {0};
    db_list(c, &names);
    for (size_t i = 0; i < names.n; i++) {
        pkg q;
        if (!strcmp(names.v[i], name) || (ignore && sv_contains(ignore, names.v[i])) ||
            db_get(c, names.v[i], &q))
            continue;
        int hit = 0;
        for (size_t j = 0; j < q.depends.n && !hit; j++) {
            char *dn = dep_name(q.depends.v[j]);
            hit = !strcmp(dn, name) ||
                  (provides_name(&p.provides, dn) && !provided_elsewhere(c, dn, 0, name, ignore));
            free(dn);
        }
        for (size_t j = 0; j < q.shlib_requires.n && !hit; j++)
            hit = sv_contains(&p.shlib_provides, q.shlib_requires.v[j]) &&
                  !provided_elsewhere(c, q.shlib_requires.v[j], 1, name, ignore);
        if (hit)
            sv_push(out, q.name);
        pkg_free(&q);
    }
    sv_free(&names);
    pkg_free(&p);
}

/* ---- orphans ---- */

static void provide(hmap *prov, const char *kind, const char *what, const char *name)
{
    char *key = xasprintf("%s:%s", kind, what);
    strvec *v = hm_get(prov, key);
    if (!v) {
        v = calloc(1, sizeof *v);
        if (!v)
            die("out of memory");
        hm_put(prov, key, v);
    }
    sv_push(v, name);
    free(key);
}

static void free_strvec(void *v)
{
    sv_free(v);
    free(v);
}

/* Dependencies nothing explicitly installed reaches anymore: one pass over the
 * dependency graph (also catches orphaned dependency cycles). */
void find_orphans(const config *c, strvec *out)
{
    strvec names = {0};
    db_list(c, &names);
    size_t n = names.n;
    pkg *ps = calloc(n ? n : 1, sizeof *ps);
    char *reach = calloc(n ? n : 1, 1);
    size_t *stack = xmalloc((n ? n : 1) * sizeof *stack), sp = 0;
    hmap idx = {0}, prov = {0};
    if (!ps || !reach)
        die("out of memory");

    for (size_t i = 0; i < n; i++) {
        if (db_get(c, names.v[i], &ps[i]))
            continue;
        hm_put(&idx, names.v[i], (void *)(i + 1));
        for (size_t j = 0; j < ps[i].provides.n; j++) {
            char *pn = dep_name(ps[i].provides.v[j]);
            provide(&prov, "v", pn, names.v[i]);
            free(pn);
        }
        for (size_t j = 0; j < ps[i].shlib_provides.n; j++)
            provide(&prov, "s", ps[i].shlib_provides.v[j], names.v[i]);
        if (ps[i].explicit_) {
            reach[i] = 1;   /* marked when queued, so each package is queued once */
            stack[sp++] = i;
        }
    }

    strvec next = {0};
    while (sp) {
        size_t i = stack[--sp];
        sv_free(&next);
        for (size_t j = 0; j < ps[i].depends.n; j++) {
            char *dn = dep_name(ps[i].depends.v[j]);
            if (hm_get(&idx, dn)) {
                sv_push(&next, dn);
            } else {
                char *key = xasprintf("v:%s", dn);
                strvec *v = hm_get(&prov, key);
                for (size_t k = 0; v && k < v->n; k++)
                    sv_push(&next, v->v[k]);
                free(key);
            }
            free(dn);
        }
        for (size_t j = 0; j < ps[i].shlib_requires.n; j++) {
            char *key = xasprintf("s:%s", ps[i].shlib_requires.v[j]);
            strvec *v = hm_get(&prov, key);
            for (size_t k = 0; v && k < v->n; k++)
                sv_push(&next, v->v[k]);
            free(key);
        }
        for (size_t j = 0; j < next.n; j++) {
            size_t k = (size_t)hm_get(&idx, next.v[j]);
            if (k && !reach[k - 1]) {
                reach[k - 1] = 1;
                stack[sp++] = k - 1;
            }
        }
    }
    sv_free(&next);

    for (size_t i = 0; i < n; i++) {
        if (ps[i].name && !reach[i])
            sv_push(out, names.v[i]);
        pkg_free(&ps[i]);
    }
    free(ps);
    free(reach);
    free(stack);
    hm_free(&idx, NULL);
    hm_free(&prov, free_strvec);
    sv_free(&names);
}
