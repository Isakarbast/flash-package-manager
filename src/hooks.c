/* Hooks run once after a transaction when any changed file matches them.
 *
 * <root>/usr/share/fpm/hooks/NAME.hook ship with packages; a file of the same
 * name in <root>/etc/fpm/hooks/ overrides it (an empty one disables it).
 *
 *   paths  = usr/lib/lib*.so* usr/lib64/lib*.so*   shell globs matched against changed paths
 *   exec   = ldconfig                        run with sh -c
 *   chroot = yes                             run inside the target root (default);
 *                                            "no" runs on the host with $ROOT set
 *
 * With --root and chroot = yes, hooks need root to chroot and are skipped otherwise. */
#include "fpm.h"

#include <dirent.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    strvec paths;
    char *exec;
    int chroot;
} hook;

static void hook_kv(const char *k, const char *v, void *ud)
{
    hook *h = ud;
    if (!strcmp(k, "paths")) {
        sv_split(&h->paths, v);
    } else if (!strcmp(k, "exec")) {
        free(h->exec);
        h->exec = xstrdup(v);
    } else if (!strcmp(k, "chroot")) {
        h->chroot = strcmp(v, "no") != 0;
    }
}

static void collect(const config *c, const char *rel, hmap *files)
{
    char *dir = rpath(c, rel);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!ends_with(e->d_name, ".hook"))
            continue;
        free(hm_del(files, e->d_name));   /* later directories override earlier ones */
        hm_put(files, e->d_name, xasprintf("%s/%s", dir, e->d_name));
    }
    if (d)
        closedir(d);
    free(dir);
}

static int matches(const hook *h, const strvec *changed)
{
    for (size_t i = 0; i < changed->n; i++)
        for (size_t j = 0; j < h->paths.n; j++)
            if (fnmatch(h->paths.v[j], changed->v[i], 0) == 0)
                return 1;
    return 0;
}

void hooks_run(const config *c, const strvec *changed)
{
    if (!changed->n || c->dry_run)
        return;
    hmap files = {0};
    collect(c, "usr/share/fpm/hooks", &files);
    collect(c, "etc/fpm/hooks", &files);
    strvec names = {0};
    for (size_t i = 0; i < files.cap; i++)
        if (files.t[i].key && files.t[i].val)
            sv_push(&names, files.t[i].key);
    sv_sort(&names);

    for (size_t i = 0; i < names.n; i++) {
        const char *path = hm_get(&files, names.v[i]);
        char *text = read_file(path, NULL);
        hook h = { .chroot = 1 };
        if (text && kv_parse(text, hook_kv, &h) == 0 && h.exec && matches(&h, changed)) {
            int in_root = strcmp(c->root, "/") != 0 && h.chroot;
            if (in_root && geteuid() != 0) {
                warnf("hook %s skipped: running it inside %s needs root", names.v[i], c->root);
            } else {
                step("Running hook %s", names.v[i]);
                strvec env = {0};
                sv_push_own(&env, xasprintf("ROOT=%s", c->root));
                char *plain[] = { "sh", "-c", h.exec, NULL };
                char *chrooted[] = { "chroot", c->root, "sh", "-c", h.exec, NULL };
                if (run(in_root ? chrooted : plain, c->root, &env))
                    warnf("hook %s failed", names.v[i]);
                sv_free(&env);
            }
        }
        free(text);
        free(h.exec);
        sv_free(&h.paths);
    }
    sv_free(&names);
    hm_free(&files, free);
}
