/* Install and remove packages.
 *
 * Install unpacks the archive into a stage directory, builds a manifest
 * (with a sha256 for each /etc file), checks for conflicts, then moves each entry into
 * place with an atomic rename. Files from the old version that the new one
 * no longer ships are removed. Edited files under /etc are never overwritten:
 * the new version is saved as .fpmnew, and on removal an edited file is kept
 * as .fpmsave. */
#include "fpm.h"

#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

strvec txn_changed;
strvec txn_plan;

/* ---- ownership index ---- */

static void owners_add(owners *o, const char *name, const manifest *m)
{
    for (size_t i = 0; i < m->n; i++) {
        const mentry *e = &m->e[i];
        if (e->type == 'd') {
            intptr_t n = (intptr_t)hm_get(&o->dirs, e->path);
            hm_put(&o->dirs, e->path, (void *)(n + 1));
        } else {
            free(hm_del(&o->files, e->path));
            hm_put(&o->files, e->path, xstrdup(name));
        }
    }
}

static void owners_drop(owners *o, const char *name, const manifest *m)
{
    for (size_t i = 0; i < m->n; i++) {
        const mentry *e = &m->e[i];
        if (e->type == 'd') {
            intptr_t n = (intptr_t)hm_get(&o->dirs, e->path) - 1;
            if (n > 0)
                hm_put(&o->dirs, e->path, (void *)n);
            else
                hm_del(&o->dirs, e->path);
        } else {
            const char *own = hm_get(&o->files, e->path);
            if (own && !strcmp(own, name))
                free(hm_del(&o->files, e->path));
        }
    }
}

void owners_load(const config *c, owners *o)
{
    memset(o, 0, sizeof *o);
    strvec names = {0};
    db_list(c, &names);
    for (size_t i = 0; i < names.n; i++) {
        manifest m;
        if (manifest_read(c, names.v[i], &m) == 0) {
            owners_add(o, names.v[i], &m);
            manifest_free(&m);
        }
    }
    sv_free(&names);
}

void owners_free(owners *o)
{
    hm_free(&o->files, free);
    hm_free(&o->dirs, NULL);
}

/* ---- helpers ---- */

static int is_config(const char *rel)
{
    return starts_with(rel, "etc/");
}

/* Move src (file or symlink) to dst atomically; copies across filesystems. */
static int place(const char *src, const char *dst)
{
    char *tmp = xasprintf("%s.fpm-tmp", dst);
    unlink(tmp);
    int rc = rename(src, tmp);
    if (rc && errno == EXDEV) {
        struct stat st;
        rc = lstat(src, &st);
        if (rc == 0 && S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            ssize_t n = readlink(src, target, sizeof target - 1);
            rc = n < 0 ? -1 : (target[n] = 0, symlink(target, tmp));
        } else if (rc == 0) {
            rc = copy_file(src, tmp, st.st_mode & 07777);
        }
    }
    if (rc == 0)
        rc = rename(tmp, dst);
    if (rc) {
        warnf("cannot install %s: %s", dst, strerror(errno));
        unlink(tmp);
    }
    free(tmp);
    return rc;
}

/* Remove one manifest entry from disk, respecting other owners and edited configs. */
static void remove_entry(const config *c, const owners *o, const char *name, const mentry *e)
{
    char *dst = rpath(c, e->path);
    if (e->type == 'd') {
        if ((intptr_t)hm_get(&o->dirs, e->path) <= 1)
            rmdir(dst);   /* only succeeds when empty */
        goto out;
    }
    const char *own = hm_get(&o->files, e->path);
    if (own && strcmp(own, name))
        goto out;     /* another package took this path over */
    if (e->type == 'f' && is_config(e->path)) {
        char cur[65];
        if (sha256_file(dst, cur) == 0 && strcmp(cur, e->sha)) {
            char *saved = xasprintf("%s.fpmsave", dst);
            if (rename(dst, saved) == 0)
                warnf("%s was edited; kept as %s", dst, saved);
            free(saved);
            goto out;
        }
    }
    if (unlink(dst) && errno != ENOENT)
        warnf("cannot remove %s: %s", dst, strerror(errno));
    else
        sv_push(&txn_changed, e->path);
out:
    free(dst);
}

static void set_pending(const config *c, const char *what)
{
    char *path = xasprintf("%s/pending", c->db);
    if (what)
        write_file_atomic(path, what, strlen(what));
    else
        unlink(path);
    free(path);
}

/* ---- stage walk (nftw has no user pointer, so these are file-scoped) ---- */

static manifest *walk_m;
static size_t walk_prefix;
static int walk_err;

static int walk_cb(const char *p, const struct stat *st, int type, struct FTW *f)
{
    (void)f;
    const char *rel = p + walk_prefix;
    if (!*rel)
        return 0;
    rel++;
    if (!strcmp(rel, ".fpminfo") || !strcmp(rel, ".fpminstall") || !strcmp(rel, ".fpmremove"))
        return 0;
    if (strchr(rel, '\n')) {
        warnf("package contains a path with a newline: %s", rel);
        walk_err = 1;
        return 1;
    }
    if (type == FTW_D) {
        manifest_push(walk_m, 'd', NULL, rel);
    } else if (type == FTW_SL) {
        manifest_push(walk_m, 'l', NULL, rel);
    } else if (type == FTW_F && S_ISREG(st->st_mode)) {
        /* Only config files are hashed: it's how edits to them are detected. */
        char h[65] = "";
        if (is_config(rel) && sha256_file(p, h)) {
            walk_err = 1;
            return 1;
        }
        manifest_push(walk_m, 'f', h, rel);
    } else {
        warnf("unsupported file type in package: %s", rel);
        walk_err = 1;
        return 1;
    }
    return 0;
}

/* ---- install ---- */

/* Every shared library p needs must be provided, and upgrading p must not take
 * away a library that another installed package still uses. */
static int check_shlibs(const config *c, const pkg *p, const pkg *old)
{
    int bad = 0;
    for (size_t i = 0; i < p->shlib_requires.n; i++) {
        if (!shlib_satisfied(c, p->shlib_requires.v[i])) {
            warnf("%s needs %s, which no installed package provides", p->name, p->shlib_requires.v[i]);
            bad = 1;
        }
    }
    if (!old)
        return bad;
    strvec names = {0};
    db_list(c, &names);
    for (size_t i = 0; i < old->shlib_provides.n; i++) {
        const char *so = old->shlib_provides.v[i];
        if (sv_contains(&p->shlib_provides, so))
            continue;
        const char *other = db_shlib_provider(c, so);
        if (other && strcmp(other, p->name))
            continue;
        for (size_t j = 0; j < names.n; j++) {
            pkg q;
            /* Packages changing in this same transaction are checked on their own. */
            if (!strcmp(names.v[j], p->name) || sv_contains(&txn_plan, names.v[j]) ||
                db_get(c, names.v[j], &q))
                continue;
            if (sv_contains(&q.shlib_requires, so)) {
                warnf("%s %s-%d drops %s, which %s still uses; rebuild or upgrade %s too",
                      p->name, p->version, p->release, so, q.name, q.name);
                bad = 1;
            }
            pkg_free(&q);
        }
    }
    sv_free(&names);
    return bad;
}

/* Files owned by a package this one replaces may be taken over. */
static int check_conflicts(const config *c, const owners *o, const pkg *p, const manifest *m)
{
    const char *name = p->name;
    int n = 0;
    for (size_t i = 0; i < m->n; i++) {
        const mentry *e = &m->e[i];
        char *dst = rpath(c, e->path);
        struct stat st;
        int exists = lstat(dst, &st) == 0;
        if (e->type == 'd') {
            if (exists && !S_ISDIR(st.st_mode) && !is_dir(dst)) {
                warnf("conflict: %s exists and is not a directory", dst);
                n++;
            }
        } else {
            const char *own = hm_get(&o->files, e->path);
            if (exists && S_ISDIR(st.st_mode)) {
                warnf("conflict: %s is a directory", dst);
                n++;
            } else if (own && strcmp(own, name) && !sv_contains(&p->replaces, own)) {
                warnf("conflict: %s is owned by %s", dst, own);
                n++;
            } else if (!own && exists && !is_config(e->path)) {
                warnf("conflict: %s exists in the filesystem", dst);
                n++;
            }
        }
        free(dst);
    }
    return n;
}

int txn_install(const config *c, owners *o, const char *archive, int mark_explicit)
{
    int ret = -1, errors = 0, had = 0;
    pkg p = {0}, old = {0};
    manifest nm = {0}, om = {0};
    char *text = NULL, *infop = NULL;

    mkdir_p(c->tmp, 0755);
    char *stage = xasprintf("%s/stage.XXXXXX", c->tmp);
    if (!mkdtemp(stage))
        die("cannot create stage in %s: %s", c->tmp, strerror(errno));

    char *argv[] = { "tar", "-xf", (char *)archive, "-C", stage, NULL };
    if (run(argv, NULL, NULL)) {
        warnf("cannot unpack %s", archive);
        goto out;
    }
    infop = xasprintf("%s/.fpminfo", stage);
    text = read_file(infop, NULL);
    if (!text || pkg_parse(text, &p)) {
        warnf("%s is not an fpm package", archive);
        goto out;
    }
    had = db_get(c, p.name, &old) == 0;
    p.explicit_ = mark_explicit || (had && old.explicit_);

    walk_m = &nm;
    walk_prefix = strlen(stage);
    walk_err = 0;
    if (nftw(stage, walk_cb, 64, FTW_PHYS) || walk_err)
        goto out;
    manifest_sort(&nm);
    if (had)
        manifest_read(c, p.name, &om);

    if (p.arch && strcmp(p.arch, "noarch") && strcmp(p.arch, c->arch) && !c->force) {
        warnf("%s is built for %s, but this system is %s", p.name, p.arch, c->arch);
        goto out;
    }
    if (check_shlibs(c, &p, had ? &old : NULL) && !c->force) {
        warnf("%s: not installed (--force installs anyway)", p.name);
        goto out;
    }
    int conflicts = check_conflicts(c, o, &p, &nm);
    if (conflicts && !c->force) {
        warnf("%s: %d conflict(s); nothing changed (--force overwrites)", p.name, conflicts);
        goto out;
    }

    if (had)
        step("Upgrading %s %s-%d -> %s-%d", p.name, old.version, old.release, p.version, p.release);
    else
        step("Installing %s %s-%d", p.name, p.version, p.release);
    /* Journal: enough to redo this install if we are interrupted. */
    char *pending = xasprintf("install\t%s\t%d\n", archive, p.explicit_);
    set_pending(c, pending);
    free(pending);

    for (size_t i = 0; i < nm.n; i++) {
        const mentry *e = &nm.e[i];
        char *src = xasprintf("%s/%s", stage, e->path);
        char *dst = rpath(c, e->path);
        struct stat st;
        lstat(src, &st);
        if (e->type == 'd') {
            if (!is_dir(dst)) {
                if (path_exists(dst))
                    unlink(dst);   /* only reachable with --force */
                if (mkdir(dst, st.st_mode & 07777) && errno != EEXIST) {
                    warnf("cannot create %s: %s", dst, strerror(errno));
                    errors++;
                }
            }
        } else if (e->type == 'f' && is_config(e->path) && path_exists(dst)) {
            char cur[65];
            const mentry *oe = had ? manifest_find(&om, e->path) : NULL;
            int readable = sha256_file(dst, cur) == 0;
            if (readable && !strcmp(cur, e->sha)) {
                /* already identical */
            } else if (readable && oe && !strcmp(cur, oe->sha)) {
                errors += place(src, dst) != 0;   /* unedited: update it */
            } else {
                char *nw = xasprintf("%s.fpmnew", dst);
                if (place(src, nw) == 0)
                    warnf("kept your %s; new version saved as %s", dst, nw);
                free(nw);
            }
        } else {
            errors += place(src, dst) != 0;
        }
        if (e->type != 'd')
            sv_push(&txn_changed, e->path);
        free(src);
        free(dst);
    }

    /* Test hook: simulate a crash after files are placed but before the database is updated. */
    if (getenv("FPM_TEST_INTERRUPT"))
        _exit(99);

    if (had)
        for (size_t i = om.n; i-- > 0;)
            if (!manifest_find(&nm, om.e[i].path))
                remove_entry(c, o, p.name, &om.e[i]);
    if (had)
        owners_drop(o, p.name, &om);
    owners_add(o, p.name, &nm);

    if (db_put(c, &p, &nm)) {
        warnf("cannot write database entry for %s", p.name);
        errors++;
    }
    /* Keep the pre-remove script (if any) for when the package is removed. */
    char *rm_src = xasprintf("%s/.fpmremove", stage);
    char *rm_dst = xasprintf("%s/installed/%s/pre-remove", c->db, p.name);
    if (path_exists(rm_src))
        copy_file(rm_src, rm_dst, 0755);
    else
        unlink(rm_dst);
    free(rm_src);
    free(rm_dst);
    set_pending(c, NULL);

    char *hook = xasprintf("%s/.fpminstall", stage);
    if (path_exists(hook)) {
        strvec env = {0};
        sv_push_own(&env, xasprintf("ROOT=%s", c->root));
        char *hargv[] = { "sh", "-e", hook, NULL };
        if (run(hargv, c->root, &env))
            warnf("%s: post-install script failed", p.name);
        sv_free(&env);
    }
    free(hook);
    ret = errors ? -1 : 0;

out:
    rm_rf(stage);
    free(stage);
    free(infop);
    free(text);
    pkg_free(&p);
    pkg_free(&old);
    manifest_free(&nm);
    manifest_free(&om);
    return ret;
}

/* ---- remove ---- */

int txn_remove(const config *c, owners *o, const char *name)
{
    pkg p;
    if (db_get(c, name, &p)) {
        warnf("%s is not installed", name);
        return -1;
    }
    /* The pre-remove script runs while every file is still in place; if it
     * fails, nothing is removed (unless --force). */
    char *script = xasprintf("%s/installed/%s/pre-remove", c->db, name);
    if (path_exists(script)) {
        strvec env = {0};
        sv_push_own(&env, xasprintf("ROOT=%s", c->root));
        char *argv[] = { "sh", "-e", script, NULL };
        int rc = run(argv, c->root, &env);
        sv_free(&env);
        if (rc && !c->force) {
            warnf("%s: pre-remove script failed (exit %d); nothing removed (--force removes anyway)", name, rc);
            free(script);
            pkg_free(&p);
            return -1;
        }
        if (rc)
            warnf("%s: pre-remove script failed (exit %d); removing anyway", name, rc);
    }
    free(script);

    manifest m;
    if (manifest_read(c, name, &m))
        memset(&m, 0, sizeof m);
    step("Removing %s %s-%d", p.name, p.version, p.release);
    char *pending = xasprintf("remove\t%s\n", name);
    set_pending(c, pending);
    free(pending);

    for (size_t i = m.n; i-- > 0;)
        remove_entry(c, o, name, &m.e[i]);
    owners_drop(o, name, &m);
    int rc = db_drop(c, name);
    set_pending(c, NULL);

    manifest_free(&m);
    pkg_free(&p);
    return rc;
}

/* ---- crash recovery ---- */

int txn_pending(const config *c)
{
    char *path = xasprintf("%s/pending", c->db);
    int p = path_exists(path);
    free(path);
    return p;
}

/* Redo the journaled step. Installs are idempotent: rerunning one from the
 * cached archive puts every file in place, removes what the old version left,
 * and writes the database. Interrupted removals are simply finished. */
int txn_repair(const config *c)
{
    char *path = xasprintf("%s/pending", c->db);
    char *text = read_file(path, NULL);
    free(path);
    if (!text)
        return 0;

    int rc = -1;
    char *f1 = strtok(text, "\t\n"), *f2 = strtok(NULL, "\t\n"), *f3 = strtok(NULL, "\t\n");
    config fc = *c;
    fc.force = 1;   /* files from the half-done step are ours, not conflicts */
    owners o;
    owners_load(c, &o);

    if (f1 && f2 && !strcmp(f1, "install")) {
        warnf("repairing an interrupted install of %s", f2);
        if (!path_exists(f2))
            warnf("cannot repair: %s is no longer cached; reinstall that package", f2);
        else
            rc = txn_install(&fc, &o, f2, f3 && !strcmp(f3, "1"));
    } else if (f1 && f2 && !strcmp(f1, "remove")) {
        warnf("repairing an interrupted removal of %s", f2);
        pkg p;
        if (db_get(c, f2, &p) == 0) {
            pkg_free(&p);
            rc = txn_remove(&fc, &o, f2);
        } else {
            rc = 0;
        }
    } else {
        warnf("unreadable transaction journal; discarding it");
    }
    if (rc == 0)
        set_pending(c, NULL);
    owners_free(&o);
    free(text);

    /* Stage directories left by the crash. */
    DIR *d = opendir(c->tmp);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (starts_with(e->d_name, "stage.")) {
            char *sp = xasprintf("%s/%s", c->tmp, e->d_name);
            rm_rf(sp);
            free(sp);
        }
    }
    if (d)
        closedir(d);
    if (rc == 0)
        step("Repair complete");
    return rc == 0 ? 1 : -1;
}
