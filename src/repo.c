/* Binary repositories.
 *
 * A repository is a directory served over https, http or file:// that holds
 * package archives and index.fpm.zst, plus index.fpm.zst.sig: an ed25519
 * signature made by `fpm index --sign KEY`. The index is plain text: a header
 * block (format, timestamp) followed by one block of package metadata per
 * package, separated by blank lines.
 *
 * `fpm sync` verifies the signature with the repository's public key, refuses
 * an index older than the copy it already has (so a mirror can't replay an old,
 * vulnerable snapshot), and keeps it under <db>/sync/<repo>/. Archives are
 * downloaded in parallel and checked against the sha256 in the signed index. */
#include "fpm.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static pkg **all;        /* every entry, in repository order */
static size_t nall;
static pkg **latest;     /* newest version of each package */
static size_t nlatest;
static hmap by_name, by_ver, by_prov, by_shlib;   /* by_ver key: "name version-release" */
static int loaded;

/* ---- index text ---- */

typedef void (*block_cb)(const char *block, void *ud);

/* Call cb for each blank-line separated block of text. */
static void each_block(const char *text, block_cb cb, void *ud)
{
    char *buf = xstrdup(text), *start = NULL;
    for (char *p = buf, *nl; p; p = nl ? nl + 1 : NULL) {
        nl = strchr(p, '\n');
        char *end = nl ? nl : p + strlen(p);
        int blank = 1;
        for (char *q = p; q < end; q++)
            if (*q != ' ' && *q != '\t' && *q != '\r')
                blank = 0;
        if (blank) {
            if (start) {
                *p = 0;
                cb(start, ud);
                start = NULL;
            }
        } else if (!start) {
            start = p;
        }
    }
    if (start)
        cb(start, ud);
    free(buf);
}

static long long timestamp_of(const char *text)
{
    const char *t = text ? strstr(text, "timestamp =") : NULL;
    return t ? atoll(t + 11) : 0;
}

/* ---- loading synced indexes ---- */

typedef struct { const config *c; const char *repo; } load_ctx;

static void load_block(const char *block, void *ud)
{
    load_ctx *x = ud;
    if (strstr(block, "format ="))
        return;   /* header */
    pkg *p = xmalloc(sizeof *p);
    if (pkg_parse(block, p)) {
        free(p);
        return;
    }
    const pkg *cur = hm_get(&by_name, p->name);
    char *key = xasprintf("%s %s-%d", p->name, p->version, p->release);
    if ((p->arch && strcmp(p->arch, "noarch") && strcmp(p->arch, x->c->arch)) ||
        (cur && strcmp(cur->repo, x->repo)) || hm_get(&by_ver, key)) {
        /* other architecture, a package an earlier repository owns, or a duplicate */
        pkg_free(p);
        free(p);
        free(key);
        return;
    }
    p->repo = xstrdup(x->repo);
    all = xrealloc(all, (nall + 1) * sizeof *all);
    all[nall++] = p;
    hm_put(&by_ver, key, p);
    free(key);
    if (!cur || vrcmp(p->version, p->release, cur->version, cur->release) > 0)
        hm_put(&by_name, p->name, p);
}

void repos_load(const config *c)
{
    if (loaded)
        return;
    loaded = 1;
    for (size_t i = 0; i < c->nbrepos; i++) {
        char *path = xasprintf("%s/sync/%s/index.fpm", c->db, c->brepos[i].name);
        char *text = read_file(path, NULL);
        free(path);
        if (!text)
            continue;
        load_ctx x = { c, c->brepos[i].name };
        each_block(text, load_block, &x);
        free(text);
    }
    /* Provides and libraries come from the newest versions; earlier repositories win. */
    for (size_t i = 0; i < nall; i++) {
        pkg *p = all[i];
        if (hm_get(&by_name, p->name) != p)
            continue;
        latest = xrealloc(latest, (nlatest + 1) * sizeof *latest);
        latest[nlatest++] = p;
        for (size_t j = 0; j < p->provides.n; j++) {
            char *pn = dep_name(p->provides.v[j]);
            if (!hm_get(&by_prov, pn))
                hm_put(&by_prov, pn, p);
            free(pn);
        }
        for (size_t j = 0; j < p->shlib_provides.n; j++)
            if (!hm_get(&by_shlib, p->shlib_provides.v[j]))
                hm_put(&by_shlib, p->shlib_provides.v[j], p);
    }
}

const pkg *repo_find_version(const config *c, const char *name, const char *version, int release)
{
    repos_load(c);
    char *key = xasprintf("%s %s-%d", name, version, release);
    const pkg *p = hm_get(&by_ver, key);
    free(key);
    return p;
}

const pkg *repo_find_match(const config *c, const char *name, const char *op, const char *want)
{
    repos_load(c);
    const pkg *best = NULL;
    for (size_t i = 0; i < nall; i++) {
        const pkg *p = all[i];
        if (!strcmp(p->name, name) && dep_satisfied(op, want, p->version) &&
            (!best || vrcmp(p->version, p->release, best->version, best->release) > 0))
            best = p;
    }
    return best;
}

const pkg *repo_find(const config *c, const char *name)
{
    repos_load(c);
    return hm_get(&by_name, name);
}

const pkg *repo_provider(const config *c, const char *virt)
{
    repos_load(c);
    return hm_get(&by_prov, virt);
}

const pkg *repo_shlib_provider(const config *c, const char *soname)
{
    repos_load(c);
    return hm_get(&by_shlib, soname);
}

void repo_all(const config *c, const pkg ***out, size_t *n)
{
    repos_load(c);
    *out = (const pkg **)latest;
    *n = nlatest;
}

/* ---- downloading ---- */

static const binrepo *repo_named(const config *c, const char *name)
{
    for (size_t i = 0; i < c->nbrepos; i++)
        if (!strcmp(c->brepos[i].name, name))
            return &c->brepos[i];
    return NULL;
}

static char *repo_url(const binrepo *r, const char *file)
{
    size_t l = strlen(r->url);
    while (l && r->url[l - 1] == '/')
        l--;
    return xasprintf("%.*s/%s", (int)l, r->url, file);
}

static void curl_base(const config *c, strvec *argv)
{
    sv_split(argv, "curl -fsSL --retry 3 --connect-timeout 30");
    if (c->proxy && *c->proxy) {
        sv_push(argv, "-x");
        sv_push(argv, c->proxy);
    }
}

static int curl_get(const config *c, const char *url, const char *out)
{
    strvec argv = {0};
    curl_base(c, &argv);
    sv_push(&argv, "-o");
    sv_push(&argv, out);
    sv_push(&argv, url);
    int rc = run(argv.v, NULL, NULL);
    sv_free(&argv);
    return rc;
}

static int verify_sig(const char *pubkey, const char *file, const char *sig)
{
    char *out = NULL;
    char *argv[] = {
        "openssl", "pkeyutl", "-verify", "-pubin", "-inkey", (char *)pubkey,
        "-rawin", "-in", (char *)file, "-sigfile", (char *)sig, NULL,
    };
    int rc = run_capture(argv, &out);
    free(out);
    return rc;
}

int repo_sync(const config *c, const binrepo *r)
{
    int ret = -1;
    char *dir = xasprintf("%s/sync/%s", c->db, r->name);
    char *tmp = xasprintf("%s.new", dir);
    char *idx = xasprintf("%s/" INDEX_FILE, tmp), *sig = xasprintf("%s.sig", idx);
    char *plain = xasprintf("%s/index.fpm", tmp), *oldplain = xasprintf("%s/index.fpm", dir);
    char *url = repo_url(r, INDEX_FILE), *sigurl = xasprintf("%s.sig", url);
    char *text = NULL, *old = NULL;

    step("Syncing repository %s", r->name);
    rm_rf(tmp);
    if (mkdir_p(tmp, 0755)) {
        warnf("cannot create %s: %s", tmp, strerror(errno));
        goto out;
    }
    if (curl_get(c, url, idx)) {
        warnf("%s: cannot download %s", r->name, url);
        goto out;
    }
    if (r->key) {
        if (curl_get(c, sigurl, sig)) {
            warnf("%s: the index has no signature (%s)", r->name, sigurl);
            goto out;
        }
        if (verify_sig(r->key, idx, sig)) {
            warnf("%s: SIGNATURE CHECK FAILED; the index was not signed with %s", r->name, r->key);
            goto out;
        }
    } else {
        warnf("%s: index is not signature-checked (allow_unsigned = yes)", r->name);
    }
    char *unz[] = { "zstd", "-d", "-q", "-f", idx, "-o", plain, NULL };
    if (run(unz, NULL, NULL) || !(text = read_file(plain, NULL))) {
        warnf("%s: cannot unpack the index", r->name);
        goto out;
    }
    old = read_file(oldplain, NULL);
    if (old && timestamp_of(text) < timestamp_of(old)) {
        warnf("%s: the new index is older than the one you have; refusing it (stale or replayed mirror)", r->name);
        goto out;
    }
    rm_rf(dir);
    if (rename(tmp, dir)) {
        warnf("cannot replace %s: %s", dir, strerror(errno));
        goto out;
    }
    ret = 0;
out:
    if (ret)
        rm_rf(tmp);
    free(dir);
    free(tmp);
    free(idx);
    free(sig);
    free(plain);
    free(oldplain);
    free(url);
    free(sigurl);
    free(text);
    free(old);
    return ret;
}

int repo_fetch(const config *c, const pkg *const *pkgs, size_t n)
{
    char *pdir = xasprintf("%s/pkgs", c->cache);
    mkdir_p(pdir, 0755);
    free(pdir);
    strvec argv = {0}, parts = {0}, dests = {0};
    const pkg **want = xmalloc((n ? n : 1) * sizeof *want);
    size_t nw = 0;
    curl_base(c, &argv);
    sv_split(&argv, "--parallel --parallel-max 8");

    for (size_t i = 0; i < n; i++) {
        const pkg *p = pkgs[i];
        char *dest = archive_path(c, p->name, p->version, p->release);
        struct stat st;
        if (stat(dest, &st) == 0 && (!p->csize || st.st_size == p->csize)) {
            free(dest);
            continue;   /* already cached */
        }
        const binrepo *r = repo_named(c, p->repo);
        char *file = xasprintf("%s-%s-%d" PKG_EXT, p->name, p->version, p->release);
        char *url = repo_url(r, file);
        char *part = xasprintf("%s.part", dest);
        step("Downloading %s %s-%d from %s", p->name, p->version, p->release, p->repo);
        sv_push(&argv, "-o");
        sv_push(&argv, part);
        sv_push(&argv, url);
        sv_push_own(&parts, part);
        sv_push_own(&dests, dest);
        want[nw++] = p;
        free(file);
        free(url);
    }

    int failed = 0;
    if (nw) {
        run(argv.v, NULL, NULL);
        for (size_t i = 0; i < nw; i++) {
            char got[65];
            if (sha256_file(parts.v[i], got)) {
                warnf("download of %s failed", want[i]->name);
                failed = 1;
            } else if (want[i]->sha256 && strcmp(got, want[i]->sha256)) {
                warnf("checksum mismatch for %s; the download was corrupted or tampered with", want[i]->name);
                unlink(parts.v[i]);
                failed = 1;
            } else if (rename(parts.v[i], dests.v[i])) {
                warnf("cannot store %s: %s", dests.v[i], strerror(errno));
                failed = 1;
            }
        }
    }
    sv_free(&argv);
    sv_free(&parts);
    sv_free(&dests);
    free(want);
    return failed ? -1 : 0;
}

/* ---- creating repositories ---- */

static void old_block(const char *block, void *ud)
{
    hmap *old = ud;
    pkg *p = xmalloc(sizeof *p);
    if (strstr(block, "format =") || pkg_parse(block, p)) {
        free(p);
        return;
    }
    char *key = xasprintf("%s-%s-%d" PKG_EXT, p->name, p->version, p->release);
    hm_put(old, key, p);
    free(key);
}

static void free_pkg(void *v)
{
    if (v) {
        pkg_free(v);
        free(v);
    }
}

/* By name, newest version first. */
static int cmp_pkgname(const void *a, const void *b)
{
    const pkg *x = *(pkg *const *)a, *y = *(pkg *const *)b;
    int r = strcmp(x->name, y->name);
    return r ? r : vrcmp(y->version, y->release, x->version, x->release);
}

/* Old index entries (as a filename -> pkg map), so unchanged archives aren't re-read. */
static void load_old_index(const char *dir, hmap *old)
{
    char *idx = xasprintf("%s/" INDEX_FILE, dir);
    char *out = NULL;
    char *argv[] = { "zstd", "-d", "-q", "-c", idx, NULL };
    if (path_exists(idx) && run_capture(argv, &out) == 0)
        each_block(out, old_block, old);
    free(out);
    free(idx);
}

int repo_index(const char *dir, const char *key)
{
    hmap old = {0}, best = {0};   /* best: archive file name -> package */
    load_old_index(dir, &old);
    DIR *d = opendir(dir);
    if (!d) {
        warnf("cannot open %s: %s", dir, strerror(errno));
        return -1;
    }
    size_t reused = 0, read_new = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!ends_with(e->d_name, PKG_EXT))
            continue;
        char *path = xasprintf("%s/%s", dir, e->d_name);
        struct stat st;
        pkg *p = NULL;
        if (stat(path, &st) == 0) {
            pkg *o = hm_get(&old, e->d_name);
            if (o && o->csize == st.st_size) {
                p = hm_del(&old, e->d_name);
                reused++;
            } else {
                char *info = NULL;
                char *argv[] = { "tar", "--zstd", "-xOf", path, "--occurrence=1", "./.fpminfo", NULL };
                p = xmalloc(sizeof *p);
                char sha[65];
                if (run_capture(argv, &info) || pkg_parse(info, p) || sha256_file(path, sha)) {
                    warnf("%s: not a valid fpm package, skipped", e->d_name);
                    free(p);
                    p = NULL;
                } else {
                    p->csize = st.st_size;
                    p->sha256 = xstrdup(sha);
                    read_new++;
                }
                free(info);
            }
        }
        if (p) {
            char *expect = xasprintf("%s-%s-%d" PKG_EXT, p->name, p->version, p->release);
            if (strcmp(expect, e->d_name)) {
                warnf("%s: file name doesn't match its contents (%s), skipped", e->d_name, expect);
                free_pkg(p);
            } else {
                hm_put(&best, e->d_name, p);   /* every version stays available */
            }
            free(expect);
        }
        free(path);
    }
    closedir(d);

    pkg **list = xmalloc((best.live ? best.live : 1) * sizeof *list);
    size_t n = 0;
    for (size_t i = 0; i < best.cap; i++)
        if (best.t[i].key && best.t[i].val)
            list[n++] = best.t[i].val;
    qsort(list, n, sizeof *list, cmp_pkgname);

    strvec blocks = {0};
    sv_push_own(&blocks, xasprintf("format = 1\ntimestamp = %lld\n", (long long)time(NULL)));
    for (size_t i = 0; i < n; i++)
        sv_push_own(&blocks, pkg_format(list[i], 0));
    char *text = sv_join(&blocks, "\n");
    char *plain = xasprintf("%s/index.fpm", dir), *idx = xasprintf("%s/" INDEX_FILE, dir);
    char *sig = xasprintf("%s.sig", idx), *tmpidx = xasprintf("%s.tmp", idx);
    int rc = write_file_atomic(plain, text, strlen(text));
    char *zst[] = { "zstd", "-q", "-19", "-f", plain, "-o", tmpidx, NULL };
    if (rc == 0)
        rc = run(zst, NULL, NULL) ? -1 : 0;
    if (rc == 0 && key) {
        char *tmpsig = xasprintf("%s.tmp", sig);
        char *argv[] = { "openssl", "pkeyutl", "-sign", "-inkey", (char *)key, "-rawin",
                         "-in", tmpidx, "-out", tmpsig, NULL };
        rc = run(argv, NULL, NULL) || rename(tmpsig, sig) ? -1 : 0;
        if (rc)
            warnf("cannot sign the index with %s", key);
        free(tmpsig);
    } else if (rc == 0) {
        unlink(sig);   /* a stale signature would no longer match */
    }
    if (rc == 0)
        rc = rename(tmpidx, idx);
    if (rc == 0)
        step("Indexed %zu package version(s) in %s (%zu new, %zu unchanged)%s", n, dir, read_new, reused,
             key ? ", signed" : ", UNSIGNED");
    unlink(tmpidx);
    free(text);
    free(plain);
    free(idx);
    free(sig);
    free(tmpidx);
    free(list);
    sv_free(&blocks);
    hm_free(&best, free_pkg);
    hm_free(&old, free_pkg);
    return rc;
}

int repo_keygen(const char *base)
{
    char *key = xasprintf("%s.key", base), *pub = xasprintf("%s.pub", base);
    if (path_exists(key)) {
        warnf("%s already exists; not overwriting it", key);
        free(key);
        free(pub);
        return -1;
    }
    mode_t old = umask(077);
    char *gen[] = { "openssl", "genpkey", "-algorithm", "ed25519", "-out", key, NULL };
    char *exp[] = { "openssl", "pkey", "-in", key, "-pubout", "-out", pub, NULL };
    int rc = run(gen, NULL, NULL) || run(exp, NULL, NULL) ? -1 : 0;
    umask(old);
    if (rc == 0) {
        chmod(pub, 0644);
        step("Private key: %s (keep it secret; sign with fpm index --sign %s)", key, key);
        step("Public key:  %s (give this to users, as 'key =' in their [repo] section)", pub);
    }
    free(key);
    free(pub);
    return rc;
}
