/* Build a recipe into <cache>/pkgs/<name>-<version>-<release>.fpm.tar.zst:
 * fetch and verify sources (tarballs, local files, git), unpack, run the build
 * script (in bubblewrap when available), strip ELF files, split subpackages
 * off, and pack each result with its metadata. */
#include "fpm.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *archive_path(const config *c, const char *name, const char *ver, int rel)
{
    return xasprintf("%s/pkgs/%s-%s-%d" PKG_EXT, c->cache, name, ver, rel);
}

static int archive_exists(const config *c, const recipe *r, const char *name)
{
    char *a = archive_path(c, name, r->version, r->release);
    int ok = path_exists(a);
    free(a);
    return ok;
}

int recipe_built(const config *c, const recipe *r)
{
    if (!archive_exists(c, r, r->name))
        return 0;
    for (size_t i = 0; i < r->nsubs; i++)
        if (!archive_exists(c, r, r->subs[i].name))
            return 0;
    return 1;
}

static int is_tarball(const char *f)
{
    static const char *ext[] = {
        ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz", ".txz",
        ".tar.zst", ".tar.lz", ".tar.lz4", NULL,
    };
    for (int i = 0; ext[i]; i++)
        if (ends_with(f, ext[i]))
            return 1;
    return 0;
}

/* "file.tar.gz::https://..." renames the download; otherwise the URL basename is used. */
static void split_source(const char *src, char **file, char **url)
{
    const char *sep = strstr(src, "::");
    if (sep) {
        *file = strndup(src, (size_t)(sep - src));
        *url = xstrdup(sep + 2);
        return;
    }
    *url = strstr(src, "://") ? xstrdup(src) : NULL;
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    *file = strndup(base, strcspn(base, "?#"));
}

static int verify(const char *path, const char *want)
{
    if (!strcmp(want, "SKIP"))
        return 0;
    char got[65];
    if (sha256_file(path, got)) {
        warnf("cannot read %s", path);
        return -1;
    }
    if (strcmp(got, want)) {
        warnf("checksum mismatch for %s\n  expected %s\n  got      %s", path, want, got);
        return -1;
    }
    return 0;
}

static void proxy_env(const config *c, strvec *env)
{
    if (!c->proxy || !*c->proxy)
        return;
    sv_push_own(env, xasprintf("http_proxy=%s", c->proxy));
    sv_push_own(env, xasprintf("https_proxy=%s", c->proxy));
    sv_push_own(env, xasprintf("ALL_PROXY=%s", c->proxy));
}

/* Returns the local path of source i (downloaded into the source cache if remote). */
static char *fetch(const config *c, const recipe *r, size_t i)
{
    char *file, *url;
    split_source(r->sources.v[i], &file, &url);
    const char *want = r->sha256.v[i];
    char *local;

    if (!url) {
        local = xasprintf("%s/%s", r->dir, r->sources.v[i]);
        if (!path_exists(local) || verify(local, want)) {
            if (!path_exists(local))
                warnf("missing local source %s", local);
            free(local);
            local = NULL;
        }
        goto out;
    }

    local = xasprintf("%s/sources/%s", c->cache, file);
    if (path_exists(local)) {
        if (verify(local, want) == 0)
            goto out;
        warnf("re-downloading %s", file);
        unlink(local);
    }
    step("Fetching %s", url);
    char *part = xasprintf("%s.part", local);
    strvec argv = {0};
    sv_split(&argv, "curl -fL --retry 3 --connect-timeout 30 --progress-bar -o");
    sv_push(&argv, part);
    if (c->proxy && *c->proxy) {
        sv_push(&argv, "-x");
        sv_push(&argv, c->proxy);
    }
    sv_push(&argv, url);
    int rc = run(argv.v, NULL, NULL);
    sv_free(&argv);
    if (rc || verify(part, want) || rename(part, local)) {
        if (rc)
            warnf("download failed: %s", url);
        unlink(part);
        free(local);
        local = NULL;
    }
    free(part);
out:
    free(file);
    free(url);
    return local;
}

/* [dir::]git+URL#commit=SHA | #tag=NAME | #branch=NAME
 * Keeps a bare mirror in the source cache and checks the ref out into srcdir/<dir>. */
static int fetch_git(const config *c, const char *src, const char *srcdir)
{
    char *dirname = NULL;
    const char *sep = strstr(src, "::");
    if (sep) {
        dirname = strndup(src, (size_t)(sep - src));
        src = sep + 2;
    }
    src += 4;   /* "git+" */
    const char *frag = strrchr(src, '#');
    char *url = strndup(src, (size_t)(frag - src));
    const char *kind = frag + 1, *ref = strchr(kind, '=') + 1;
    if (!dirname) {
        const char *b = strrchr(url, '/');
        dirname = xstrdup(b ? b + 1 : url);
        if (ends_with(dirname, ".git"))
            dirname[strlen(dirname) - 4] = 0;
    }

    /* FNV-1a of the URL keeps mirrors of same-named repositories apart. */
    uint64_t h = 1469598103934665603ULL;
    for (const char *u = url; *u; u++)
        h = (h ^ (unsigned char)*u) * 1099511628211ULL;
    char *mirror = xasprintf("%s/sources/git/%s-%016llx.git", c->cache, dirname, (unsigned long long)h);
    char *dest = xasprintf("%s/%s", srcdir, dirname);
    char *rev = starts_with(kind, "commit=") ? xstrdup(ref)
              : starts_with(kind, "tag=")    ? xasprintf("refs/tags/%s", ref)
                                             : xasprintf("refs/remotes/origin/%s", ref);
    strvec env = {0};
    proxy_env(c, &env);
    sv_push(&env, "GIT_TERMINAL_PROMPT=0");
    int rc = -1;

    if (!is_dir(mirror)) {
        step("Cloning %s", url);
        char *parent = xasprintf("%s/sources/git", c->cache);
        mkdir_p(parent, 0755);
        free(parent);
        char *argv[] = { "git", "clone", "--quiet", "--mirror", url, mirror, NULL };
        if (run(argv, NULL, &env)) {
            warnf("cannot clone %s", url);
            rm_rf(mirror);
            goto out;
        }
    } else {
        /* A pinned commit we already have needs no network at all. */
        char *obj = xasprintf("%s^{commit}", rev);
        char *have[] = { "git", "--git-dir", mirror, "cat-file", "-e", obj, NULL };
        int present = starts_with(kind, "commit=") && run(have, NULL, NULL) == 0;
        free(obj);
        if (!present) {
            step("Updating %s", url);
            char *argv[] = { "git", "--git-dir", mirror, "remote", "update", "--prune", NULL };
            if (run(argv, NULL, &env))
                warnf("cannot update %s; using the cached copy", url);
        }
    }

    char *clone[] = { "git", "clone", "--quiet", "--no-checkout", mirror, dest, NULL };
    char *checkout[] = { "git", "-C", dest, "checkout", "--quiet", "--detach", rev, NULL };
    if (run(clone, NULL, NULL) || run(checkout, NULL, NULL)) {
        warnf("cannot check out %s at %s", url, ref);
        goto out;
    }

    /* Submodules, at the commits the checkout pins. They are fetched here, before
     * the sandboxed build, which has no network. Local (file://) submodules are
     * only allowed when the repository itself is local. */
    char *gm = xasprintf("%s/.gitmodules", dest);
    int has_subs = path_exists(gm);
    free(gm);
    if (has_subs) {
        step("Fetching submodules of %s", url);
        char *sub[] = {
            "git", "-c", starts_with(url, "file://") ? "protocol.file.allow=always" : "protocol.file.allow=user",
            "-C", dest, "submodule", "--quiet", "update", "--init", "--recursive", NULL,
        };
        if (run(sub, NULL, &env)) {
            warnf("cannot fetch the submodules of %s", url);
            goto out;
        }
    }
    rc = 0;
out:
    sv_free(&env);
    free(dirname);
    free(url);
    free(mirror);
    free(dest);
    free(rev);
    return rc;
}

static int strip_cb(const char *p, const struct stat *st, int type, struct FTW *f)
{
    (void)f;
    if (type != FTW_F || !S_ISREG(st->st_mode) || ends_with(p, ".ko"))
        return 0;
    const char *how = NULL;
    if (ends_with(p, ".a")) {
        how = "--strip-debug";
    } else {
        unsigned char h[18];
        FILE *fp = fopen(p, "rb");
        if (!fp)
            return 0;
        size_t n = fread(h, 1, sizeof h, fp);
        fclose(fp);
        if (n < sizeof h || memcmp(h, "\177ELF", 4))
            return 0;
        int etype = h[5] == 1 ? h[16] | h[17] << 8 : h[16] << 8 | h[17];
        if (etype == 1)
            how = "--strip-debug";          /* relocatable object */
        else if (etype == 2 || etype == 3)
            how = "--strip-unneeded";       /* executable or shared library */
    }
    if (how) {
        char *argv[] = { "strip", (char *)how, (char *)p, NULL };
        run(argv, NULL, NULL);
    }
    return 0;
}

/* usrmerge: /bin, /sbin, /lib, /lib64 and /usr/sbin are symlinks into /usr on
 * the target, so files a build put there move to usr/bin and usr/lib, and the
 * package never ships those directories (or symlinks in their place). */
static int merge_into(const char *src, const char *dst)
{
    if (mkdir_p(dst, 0755))
        return -1;
    DIR *d = opendir(src);
    if (!d)
        return -1;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char *s = xasprintf("%s/%s", src, e->d_name), *t = xasprintf("%s/%s", dst, e->d_name);
        struct stat ss, ts;
        if (lstat(t, &ts) != 0) {
            if (rename(s, t))
                rc = -1;
        } else if (lstat(s, &ss) == 0 && S_ISDIR(ss.st_mode) && S_ISDIR(ts.st_mode)) {
            rc |= merge_into(s, t);
        } else {
            warnf("usrmerge: %s and %s both exist", s, t);
            rc = -1;
        }
        free(s);
        free(t);
    }
    closedir(d);
    if (rc == 0 && rmdir(src))
        rc = -1;
    return rc;
}

static int usr_merge(const char *pkgdir)
{
    static const char *pairs[][2] = {
        { "usr/sbin", "usr/bin" }, { "sbin", "usr/bin" }, { "bin", "usr/bin" },
        { "usr/lib64", "usr/lib" }, { "lib64", "usr/lib" }, { "lib", "usr/lib" },
    };
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        char *src = xasprintf("%s/%s", pkgdir, pairs[i][0]);
        char *dst = xasprintf("%s/%s", pkgdir, pairs[i][1]);
        struct stat st;
        int rc = 0;
        if (lstat(src, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                rc = merge_into(src, dst);
            else
                rc = unlink(src);
        }
        free(src);
        free(dst);
        if (rc) {
            warnf("usrmerge failed for %s", pairs[i][0]);
            return -1;
        }
    }
    return 0;
}

static const strvec *purge_globs;
static size_t purge_prefix;

/* A glob with a slash matches the path from the package root; one without
 * matches the file name anywhere (like makepkg's PURGE_TARGETS). */
static int purge_cb(const char *p, const struct stat *st, int type, struct FTW *f)
{
    (void)st;
    if (type == FTW_D || type == FTW_DP)
        return 0;
    const char *rel = p + purge_prefix;
    for (size_t i = 0; i < purge_globs->n; i++) {
        const char *g = purge_globs->v[i];
        if (strchr(g, '/') ? !fnmatch(g, rel, FNM_PATHNAME) : !fnmatch(g, p + f->base, 0)) {
            unlink(p);
            break;
        }
    }
    return 0;
}

static void purge(const config *c, const char *pkgdir)
{
    if (!c->purge.n)
        return;
    purge_globs = &c->purge;
    purge_prefix = strlen(pkgdir) + 1;
    nftw(pkgdir, purge_cb, 32, FTW_PHYS | FTW_DEPTH);
}

static int run_build_script(const config *c, const recipe *r, const char *bdir,
                            const char *srcdir, const char *pkgdir)
{
    char *script = xasprintf("%s/build", r->dir);
    if (!path_exists(script)) {
        free(script);
        return 0;   /* metapackage: dependencies only */
    }
    char *home = xasprintf("%s/home", bdir);
    mkdir_p(home, 0755);

    strvec env = {0};
    sv_push_own(&env, xasprintf("DESTDIR=%s", pkgdir));
    sv_push_own(&env, xasprintf("pkgdir=%s", pkgdir));
    sv_push_own(&env, xasprintf("srcdir=%s", srcdir));
    sv_push_own(&env, xasprintf("name=%s", r->name));
    sv_push_own(&env, xasprintf("version=%s", r->version));
    sv_push_own(&env, xasprintf("release=%d", r->release));
    sv_push_own(&env, xasprintf("JOBS=%d", c->jobs));
    sv_push_own(&env, xasprintf("MAKEFLAGS=-j%d", c->jobs));
    sv_push_own(&env, xasprintf("CFLAGS=%s", c->cflags));
    sv_push_own(&env, xasprintf("CXXFLAGS=%s", c->cxxflags));
    sv_push_own(&env, xasprintf("LDFLAGS=%s", c->ldflags));
    sv_push_own(&env, xasprintf("HOME=%s", home));

    int sandbox = c->sandbox < 0 ? have_program("bwrap") : c->sandbox;
    strvec argv = {0};
    if (sandbox) {
        /* Read-only host, writable build tree only, no network (sources are already fetched). */
        sv_split(&argv, "bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp");
        sv_push(&argv, "--ro-bind");
        sv_push(&argv, r->dir);
        sv_push(&argv, r->dir);
        sv_push(&argv, "--bind");
        sv_push(&argv, bdir);
        sv_push(&argv, bdir);
        sv_split(&argv, "--unshare-net --unshare-ipc --unshare-pid --unshare-uts --die-with-parent --chdir");
        sv_push(&argv, srcdir);
        sv_push(&argv, "--");
    }
    sv_push(&argv, "sh");
    sv_push(&argv, "-e");
    sv_push(&argv, script);

    step("Running build script for %s%s", r->name, sandbox ? " (sandboxed)" : "");
    int rc = run(argv.v, srcdir, &env);
    if (rc)
        warnf("build script for %s failed (exit %d); build tree kept in %s", r->name, rc, bdir);
    sv_free(&argv);
    sv_free(&env);
    free(home);
    free(script);
    return rc ? -1 : 0;
}

/* ---- subpackage splitting ---- */

static strvec walk_paths;
static size_t walk_prefix;

static int collect_cb(const char *p, const struct stat *st, int type, struct FTW *f)
{
    (void)st; (void)f;
    if (type != FTW_D && type != FTW_DP && p[walk_prefix])
        sv_push(&walk_paths, p + walk_prefix + 1);
    return 0;
}

/* Does rel, or any directory above it, match one of the patterns? */
static int matches(const strvec *pats, const char *rel)
{
    char *p = xstrdup(rel);
    int hit = 0;
    while (!hit) {
        for (size_t i = 0; i < pats->n && !hit; i++)
            hit = fnmatch(pats->v[i], p, FNM_PATHNAME) == 0;
        char *slash = strrchr(p, '/');
        if (!slash)
            break;
        *slash = 0;
    }
    free(p);
    return hit;
}

/* Move each subpackage's files out of pkgdir into pkgdir-<sub>. */
static int split(const recipe *r, const char *bdir, const char *pkgdir)
{
    if (!r->nsubs)
        return 0;
    sv_free(&walk_paths);
    walk_prefix = strlen(pkgdir);
    nftw(pkgdir, collect_cb, 32, FTW_PHYS);
    char *taken = calloc(walk_paths.n ? walk_paths.n : 1, 1);
    if (!taken)
        die("out of memory");
    int rc = 0;

    for (size_t s = 0; s < r->nsubs && !rc; s++) {
        const subpkg *sub = &r->subs[s];
        char *subdir = xasprintf("%s/pkg-%s", bdir, sub->name);
        mkdir_p(subdir, 0755);
        size_t moved = 0;
        for (size_t i = 0; i < walk_paths.n; i++) {
            if (taken[i] || !matches(&sub->files, walk_paths.v[i]))
                continue;
            char *src = xasprintf("%s/%s", pkgdir, walk_paths.v[i]);
            char *dst = xasprintf("%s/%s", subdir, walk_paths.v[i]);
            char *parent = xstrdup(dst);
            *strrchr(parent, '/') = 0;
            if (mkdir_p(parent, 0755) || rename(src, dst)) {
                warnf("cannot move %s into %s: %s", walk_paths.v[i], sub->name, strerror(errno));
                rc = -1;
            }
            /* Drop directories the move left empty. */
            for (char *d = strrchr(src, '/'); d && (size_t)(d - src) > strlen(pkgdir); d = strrchr(src, '/')) {
                *d = 0;
                if (rmdir(src))
                    break;
            }
            taken[i] = 1;
            moved++;
            free(src);
            free(dst);
            free(parent);
        }
        if (!moved)
            warnf("%s: no files matched %s.files", r->name, sub->name);
        free(subdir);
    }
    free(taken);
    sv_free(&walk_paths);
    return rc;
}

/* ---- shared libraries and size (nftw has no user pointer) ---- */

static long long scan_size;
static strvec scan_needed, scan_sonames;

static int scan_cb(const char *p, const struct stat *st, int type, struct FTW *f)
{
    if (type != FTW_F || !S_ISREG(st->st_mode))
        return 0;
    const char *base = p + f->base;
    if (!strcmp(base, ".fpminfo") || !strcmp(base, ".fpminstall") || !strcmp(base, ".fpmremove"))
        return 0;
    scan_size += st->st_size;
    char *so = NULL;
    if (elf_dynamic(p, &scan_needed, &so) == 0) {
        /* A library linked without -soname (perl's libperl.so) is found by
         * its file name, so that is what it provides. */
        if (!so && !strncmp(base, "lib", 3) && strstr(base, ".so"))
            so = xstrdup(base);
        if (so && !sv_contains(&scan_sonames, so))
            sv_push(&scan_sonames, so);
    }
    free(so);
    return 0;
}

/* Write .fpminfo into dir and pack it into the cache. Shared libraries the
 * package ships become shlib-provides; libraries its programs load that it
 * doesn't ship itself become shlib-requires. */
static int pack(const config *c, const recipe *r, const char *name, const char *dir)
{
    scan_size = 0;
    sv_free(&scan_needed);
    sv_free(&scan_sonames);
    nftw(dir, scan_cb, 32, FTW_PHYS);
    strvec requires = {0};
    /* !shlibs: the libraries come from the program's own runtime (a container
     * or bundled loader path), not from the system. */
    if (!recipe_has_option(r, "!shlibs"))
        for (size_t i = 0; i < scan_needed.n; i++)
            if (!sv_contains(&scan_sonames, scan_needed.v[i]))
                sv_push(&requires, scan_needed.v[i]);
    sv_sort(&requires);
    sv_sort(&scan_sonames);

    pkgview v = recipe_view(r, name);
    pkg p = {
        .name = (char *)name, .version = r->version, .release = r->release,
        .description = (char *)v.description, .arch = r->arch ? r->arch : c->arch,
        .depends = *v.depends, .provides = *v.provides, .conflicts = *v.conflicts,
        .replaces = *v.replaces, .shlib_provides = scan_sonames, .shlib_requires = requires,
        .size = scan_size,
    };
    char *info = pkg_format(&p, 0), *infop = xasprintf("%s/.fpminfo", dir);
    int rc = write_file_atomic(infop, info, strlen(info));
    free(info);
    free(infop);
    sv_free(&requires);
    if (rc)
        return -1;

    char *out = archive_path(c, name, r->version, r->release);
    char *part = xasprintf("%s.part", out);
    char *argv[] = {
        "tar", "--zstd", "--sort=name", "--numeric-owner", "--owner=0", "--group=0",
        "-C", (char *)dir, "-cf", part, ".", NULL,
    };
    rc = run(argv, NULL, NULL) || rename(part, out) ? -1 : 0;
    if (rc) {
        warnf("cannot create %s", out);
        unlink(part);
    } else {
        step("Built %s", out);
    }
    free(out);
    free(part);
    return rc;
}

int build_recipe(const config *c, const recipe *r)
{
    if (recipe_built(c, r) && !c->rebuild)
        return 0;
    step("Building %s %s-%d", r->name, r->version, r->release);

    int ret = -1;
    char *pkgs = xasprintf("%s/pkgs", c->cache), *srcs = xasprintf("%s/sources", c->cache);
    char *bdir = xasprintf("%s/build/%s", c->tmp, r->name);
    char *srcdir = xasprintf("%s/src", bdir), *pkgdir = xasprintf("%s/pkg", bdir);
    rm_rf(bdir);
    if (mkdir_p(pkgs, 0755) || mkdir_p(srcs, 0755) || mkdir_p(srcdir, 0755) || mkdir_p(pkgdir, 0755)) {
        warnf("cannot create build directories under %s: %s", c->tmp, strerror(errno));
        goto out;
    }

    for (size_t i = 0; i < r->sources.n; i++) {
        if (is_git_source(r->sources.v[i])) {
            if (fetch_git(c, r->sources.v[i], srcdir))
                goto out;
            continue;
        }
        char *local = fetch(c, r, i);
        if (!local)
            goto out;
        const char *base = strrchr(local, '/') + 1;
        int rc;
        if (is_tarball(base)) {
            char *argv[] = { "tar", "--no-same-owner", "-xf", local, "-C", srcdir, NULL };
            rc = run(argv, NULL, NULL);
        } else {
            char *dst = xasprintf("%s/%s", srcdir, base);
            rc = copy_file(local, dst, 0644);
            free(dst);
        }
        if (rc) {
            warnf("cannot unpack %s", local);
            free(local);
            goto out;
        }
        free(local);
    }

    if (run_build_script(c, r, bdir, srcdir, pkgdir))
        goto out;
    if (c->usrmerge && usr_merge(pkgdir))
        goto out;
    purge(c, pkgdir);

    if (!recipe_has_option(r, "!strip") && have_program("strip"))
        nftw(pkgdir, strip_cb, 32, FTW_PHYS);

    if (split(r, bdir, pkgdir))
        goto out;

    /* Package scripts travel inside the archive as dot files. */
    static const char *scripts[][2] = { { "post-install", ".fpminstall" }, { "pre-remove", ".fpmremove" } };
    for (int i = 0; i < 2; i++) {
        char *src = xasprintf("%s/%s", r->dir, scripts[i][0]);
        if (path_exists(src)) {
            char *dst = xasprintf("%s/%s", pkgdir, scripts[i][1]);
            copy_file(src, dst, 0755);
            free(dst);
        }
        free(src);
    }

    if (pack(c, r, r->name, pkgdir))
        goto out;
    for (size_t i = 0; i < r->nsubs; i++) {
        char *subdir = xasprintf("%s/pkg-%s", bdir, r->subs[i].name);
        int rc = pack(c, r, r->subs[i].name, subdir);
        free(subdir);
        if (rc)
            goto out;
    }
    if (!c->keep_build)
        rm_rf(bdir);
    ret = 0;
out:
    free(pkgs);
    free(srcs);
    free(bdir);
    free(srcdir);
    free(pkgdir);
    return ret;
}
