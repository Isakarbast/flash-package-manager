/* fpm - Freedom Package Manager: command-line interface. */
#include "fpm.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static config C;

static void usage(FILE *f)
{
    fputs("usage: fpm [options] <command> [args]\n"
          "\n"
          "packages:\n"
          "  install PKG...     install from a binary repository or build from a recipe\n"
          "  remove PKG...      remove packages (refuses if something still needs them)\n"
          "  upgrade            upgrade everything outdated (held packages stay)\n"
          "  autoremove         remove dependencies nothing needs anymore\n"
          "  hold PKG...        keep packages at their current version\n"
          "  unhold PKG...      allow upgrading them again\n"
          "  rollback [GEN]     return to a generation (default: the previous one)\n"
          "  history            list generations\n"
          "  repair             finish a transaction that was interrupted\n"
          "\n"
          "queries:\n"
          "  search [TERM]      search binary repositories and recipes\n"
          "  info PKG           details: versions, sources, dependencies, libraries\n"
          "  list               installed packages (-e: only explicitly installed)\n"
          "  files PKG          files owned by a package\n"
          "  owner PATH         package owning a file\n"
          "  deps PKG           what a package needs\n"
          "  rdeps PKG          installed packages that need it\n"
          "  orphans            dependencies nothing needs anymore\n"
          "  check [PKG...]     verify installed files are present and unmodified\n"
          "  show PKG           print a package's recipe and build script\n"
          "\n"
          "repositories:\n"
          "  sync               update binary repository indexes and recipe repositories\n"
          "  build PKG...       build packages into the cache without installing\n"
          "  index DIR          (re)write a repository index for the packages in DIR\n"
          "  keygen NAME        create NAME.key / NAME.pub for signing repositories\n"
          "  clean              delete cached packages no generation needs\n"
          "\n"
          "options:\n"
          "  -y, --yes          don't ask for confirmation\n"
          "  -n, --dry-run      show what would happen, change nothing\n"
          "  -f, --force        override conflicts and safety checks\n"
          "  --overwrite        replace existing files that no package owns\n"
          "  -S, --source       build from recipes even when a binary exists\n"
          "  -P, --parallel N   build up to N packages at the same time\n"
          "  --reinstall        reinstall targets that are already up to date\n"
          "  --rebuild          rebuild even when a cached package exists\n"
          "  --keep-build       keep the build tree after a successful build\n"
          "  --sign KEY         index: sign with this private key\n"
          "  --keep N           clean: keep only the newest N generations\n"
          "  --sources          clean: also delete downloaded sources\n"
          "  -e, --explicit     list: only explicitly installed packages\n"
          "  --root DIR         operate on another root filesystem\n"
          "  --config FILE      configuration file (default /etc/fpm.conf)\n"
          "  -V, --version      print the version\n", f);
}

static void need_root(void)
{
    if (!strcmp(C.root, "/") && geteuid() != 0)
        die("this command needs root (or use --root DIR)");
}

static int confirm(const char *question)
{
    if (C.yes || !isatty(0))
        return 1;
    printf("%s [Y/n] ", question);
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof line, stdin))
        return 0;
    char *a = trim(line);
    return !*a || *a == 'y' || *a == 'Y';
}

/* Finish an interrupted transaction before changing anything else. */
static void repair(void)
{
    int rc = txn_repair(&C);
    if (rc < 0)
        die("the interrupted transaction could not be repaired (see above)");
    if (rc > 0)
        gen_record(&C, "repair");
}

static void begin_change(void)
{
    need_root();
    db_lock(&C);
    repair();
}

static void warn_pending(void)
{
    if (txn_pending(&C))
        warnf("an interrupted transaction is waiting; 'fpm repair' (or any change) finishes it");
}

static void finish(const char *cmd)
{
    hooks_run(&C, &txn_changed);
    gen_record(&C, cmd);
}

static void human(long long bytes, char *buf, size_t len)
{
    const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    snprintf(buf, len, u ? "%.1f %s" : "%.0f %s", v, unit[u]);
}

/* ---- AUR-style review of recipes from repositories with review = yes ---- */

static char *recipe_digest(const recipe *r)
{
    static const char *files[] = { "recipe", "build", "post-install", "pre-remove", NULL };
    strvec parts = {0};
    for (int i = 0; files[i]; i++) {
        char *p = xasprintf("%s/%s", r->dir, files[i]);
        char h[65] = "-";
        if (path_exists(p))
            sha256_file(p, h);
        sv_push(&parts, h);
        free(p);
    }
    char *s = sv_join(&parts, " ");
    sv_free(&parts);
    return s;
}

static void print_recipe_files(const recipe *r)
{
    static const char *files[] = { "recipe", "build", "post-install", "pre-remove", NULL };
    for (int i = 0; files[i]; i++) {
        char *p = xasprintf("%s/%s", r->dir, files[i]);
        char *text = read_file(p, NULL);
        if (text)
            printf("\n----- %s -----\n%s", p, text);
        free(text);
        free(p);
    }
    printf("\n");
}

/* Recipes from review repositories that are new or changed since you last
 * approved them are shown before anything is built. */
static int review(job *jobs, size_t n)
{
    strvec seen = {0};
    int ok = 1;
    for (size_t i = 0; i < n && ok; i++) {
        const job *j = &jobs[i];
        if (j->x.binary || sv_contains(&seen, j->x.r.name))
            continue;
        sv_push(&seen, j->x.r.name);
        const reciperepo *rr = recipe_repo_of(&C, j->x.r.dir);
        if (!rr || !rr->review)
            continue;
        char *digest = recipe_digest(&j->x.r);
        char *path = xasprintf("%s/reviewed/%s", C.db, j->x.r.name);
        char *prev = read_file(path, NULL);
        if (!prev || strcmp(prev, digest)) {
            step("%s comes from '%s', which asks you to review its recipes", j->x.r.name, rr->name);
            print_recipe_files(&j->x.r);
            if (!C.yes && !isatty(0)) {
                warnf("not reviewed: run interactively or pass --yes");
                ok = 0;
            } else if (!confirm("Build it?")) {
                ok = 0;
            } else {
                char *dir = xasprintf("%s/reviewed", C.db);
                mkdir_p(dir, 0755);
                free(dir);
                write_file_atomic(path, digest, strlen(digest));
            }
        }
        free(prev);
        free(path);
        free(digest);
    }
    sv_free(&seen);
    return ok;
}

/* ---- install / upgrade / build ---- */

static void print_plan(job *jobs, size_t n, const strvec *removals)
{
    long long dl = 0, grow = 0;
    printf("Packages (%zu):\n", n);
    for (size_t i = 0; i < n; i++) {
        const job *j = &jobs[i];
        pkg old;
        int had = db_get(&C, j->name, &old) == 0;
        char ver[128], from[160];
        snprintf(ver, sizeof ver, "%s-%d", j->x.version, j->x.release);
        if (j->x.binary)
            snprintf(from, sizeof from, "repo %s", j->x.rp->repo);
        else
            snprintf(from, sizeof from, "%s", recipe_built(&C, &j->x.r) && !C.rebuild ? "cached build" : "build");
        printf("  %-28s %-18s %-14s%s%s%s\n", j->name, ver, from, had ? "(was " : "",
               had ? old.version : "", had ? ")" : "");
        if (j->x.binary) {
            char *a = archive_path(&C, j->name, j->x.version, j->x.release);
            if (!path_exists(a))
                dl += j->x.rp->csize;
            free(a);
            grow += j->x.rp->size;
        }
        if (had) {
            grow -= old.size;
            pkg_free(&old);
        }
    }
    for (size_t i = 0; i < removals->n; i++) {
        printf("  %-28s %-18s replaced\n", removals->v[i], "");
    }
    char a[32], b[32];
    human(dl, a, sizeof a);
    human(grow < 0 ? -grow : grow, b, sizeof b);
    printf("Download: %s   Installed size: %s%s (binary packages)\n", a, grow < 0 ? "-" : "+", b);
}

static int do_install(const strvec *targets, int mark_targets, const char *cmd)
{
    strvec order = {0}, removals = {0}, tnames = {0};
    if (plan_install(&C, targets, &order, &removals))
        die("cannot resolve the transaction");
    if (!order.n)
        return 0;
    for (size_t i = 0; i < targets->n; i++)
        sv_push_own(&tnames, dep_name(targets->v[i]));
    job *jobs = jobs_plan(&C, &order, mark_targets ? &tnames : NULL);
    print_plan(jobs, order.n, &removals);
    if (C.dry_run || !confirm("Proceed?") || !review(jobs, order.n)) {
        jobs_free(jobs, order.n);
        return C.dry_run ? 0 : 1;
    }

    for (size_t i = 0; i < order.n; i++)
        sv_push(&txn_plan, order.v[i]);
    for (size_t i = 0; i < removals.n; i++)
        sv_push(&txn_plan, removals.v[i]);
    owners o;
    owners_load(&C, &o);
    int failed = jobs_run(&C, jobs, order.n, &o);
    for (size_t i = 0; i < removals.n && !failed; i++)
        failed |= txn_remove(&C, &o, removals.v[i]) != 0;
    owners_free(&o);
    finish(cmd);
    jobs_free(jobs, order.n);
    sv_free(&order);
    sv_free(&removals);
    sv_free(&tnames);
    return failed;
}

static int cmd_upgrade(const char *cmd)
{
    strvec names = {0}, targets = {0};
    db_list(&C, &names);
    for (size_t i = 0; i < names.n; i++) {
        pkg p;
        cand x;
        if (db_get(&C, names.v[i], &p))
            continue;
        if (cand_find(&C, p.name, &x) == 0) {
            if (vrcmp(x.version, x.release, p.version, p.release) > 0) {
                if (db_held(&C, p.name))
                    printf("  %s is held at %s-%d (%s-%d available)\n", p.name, p.version, p.release,
                           x.version, x.release);
                else
                    sv_push(&targets, p.name);
            }
            cand_free(&x);
        }
        pkg_free(&p);
    }
    sv_free(&names);
    if (!targets.n) {
        step("Everything is up to date");
        return 0;
    }
    int rc = do_install(&targets, 0, cmd);
    sv_free(&targets);
    return rc;
}

static int cmd_build(char **args, size_t n)
{
    strvec order = {0};
    int failed = 0;
    for (size_t i = 0; i < n; i++) {
        recipe r;
        if (recipe_find(&C, args[i], &r)) {
            warnf("%s: no recipe found", args[i]);
            failed = 1;
            continue;
        }
        for (size_t k = 0; k < r.makedepends.n; k++) {
            char *dn = dep_name(r.makedepends.v[k]);
            if (!db_provider(&C, dn))
                warnf("%s: build dependency %s is not installed", r.name, dn);
            free(dn);
        }
        recipe_free(&r);
        sv_push(&order, args[i]);
    }
    if (C.dry_run || !order.n) {
        sv_free(&order);
        return failed;
    }
    int src = C.source_only;
    C.source_only = 1;   /* "build" always means from the recipe */
    job *jobs = jobs_plan(&C, &order, NULL);
    C.source_only = src;
    for (size_t i = 0; i < order.n; i++)
        sv_free(&jobs[i].wait);   /* dependencies are the caller's job here */
    failed |= jobs_run(&C, jobs, order.n, NULL);
    jobs_free(jobs, order.n);
    sv_free(&order);
    return failed;
}

/* ---- remove / autoremove ---- */

static int remove_set(const strvec *names, const char *cmd)
{
    printf("Removing (%zu): ", names->n);
    for (size_t i = 0; i < names->n; i++)
        printf("%s%s", i ? " " : "", names->v[i]);
    printf("\n");
    if (C.dry_run)
        return 0;
    if (!confirm("Proceed?"))
        return 1;
    for (size_t i = 0; i < names->n; i++)
        sv_push(&txn_plan, names->v[i]);
    owners o;
    owners_load(&C, &o);
    int failed = 0;
    for (size_t i = 0; i < names->n; i++)
        failed |= txn_remove(&C, &o, names->v[i]) != 0;
    owners_free(&o);
    finish(cmd);
    return failed;
}

static int cmd_remove(char **args, size_t n, const char *cmd)
{
    strvec targets = {0};
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        pkg p;
        if (db_get(&C, args[i], &p)) {
            warnf("%s is not installed", args[i]);
            bad = 1;
            continue;
        }
        pkg_free(&p);
        sv_push(&targets, args[i]);
    }
    for (size_t i = 0; i < targets.n; i++) {
        strvec req = {0};
        required_by(&C, targets.v[i], &targets, &req);
        if (req.n) {
            char *s = sv_join(&req, ", ");
            warnf("%s is required by: %s", targets.v[i], s);
            free(s);
            bad |= !C.force;
        }
        sv_free(&req);
    }
    if (bad)
        die("nothing removed");
    int rc = remove_set(&targets, cmd);
    sv_free(&targets);
    return rc;
}

static int cmd_autoremove(const char *cmd)
{
    strvec drop = {0};
    find_orphans(&C, &drop);
    if (!drop.n) {
        step("No unneeded packages");
        return 0;
    }
    int rc = remove_set(&drop, cmd);
    sv_free(&drop);
    return rc;
}

static int cmd_hold(char **args, size_t n, int on)
{
    int failed = 0;
    for (size_t i = 0; i < n; i++) {
        pkg p;
        if (db_get(&C, args[i], &p)) {
            warnf("%s is not installed", args[i]);
            failed = 1;
            continue;
        }
        if (db_set_held(&C, args[i], on))
            failed = 1;
        else
            step("%s %s", args[i], on ? "is held at its current version" : "can be upgraded again");
        pkg_free(&p);
    }
    return failed;
}

/* ---- generations ---- */

static int cmd_rollback(const char *arg, const char *cmd)
{
    int cur = gen_current(&C), target = 0;
    int *nums;
    size_t n;
    gen_list(&C, &nums, &n);
    if (arg) {
        target = atoi(arg);
    } else {
        for (size_t i = 0; i < n; i++)
            if (nums[i] < cur)
                target = nums[i];
    }
    free(nums);
    if (target <= 0)
        die("no earlier generation to roll back to");

    strvec lines = {0};
    if (gen_read(&C, target, &lines, NULL))
        die("generation %d does not exist", target);

    strvec want = {0}, installs = {0}, shown = {0}, removes = {0}, names = {0};
    int missing = 0;
    const pkg **fetch = NULL;
    size_t nfetch = 0;
    for (size_t i = 0; i < lines.n; i++) {
        char name[256], vr[256], reason[32];
        if (sscanf(lines.v[i], "%255s %255s %31s", name, vr, reason) != 3)
            continue;
        sv_push(&want, name);
        char *dash = strrchr(vr, '-');
        if (!dash)
            continue;
        *dash = 0;
        int rel = atoi(dash + 1);
        pkg p;
        int same = db_get(&C, name, &p) == 0 && !strcmp(p.version, vr) && p.release == rel;
        if (p.name)
            pkg_free(&p);
        if (same)
            continue;
        char *archive = archive_path(&C, name, vr, rel);
        if (!path_exists(archive)) {
            /* Not cached anymore: a repository that still has that version can supply it. */
            const pkg *rp = repo_find_version(&C, name, vr, rel);
            if (rp) {
                fetch = xrealloc(fetch, (nfetch + 1) * sizeof *fetch);
                fetch[nfetch++] = rp;
            } else {
                warnf("%s %s-%d is neither cached nor in any repository", name, vr, rel);
                missing++;
            }
        }
        sv_push_own(&installs, xasprintf("%s %d", archive, !strcmp(reason, "explicit")));
        sv_push_own(&shown, xasprintf("%s %s-%d", name, vr, rel));
        sv_push(&txn_plan, name);
        free(archive);
    }
    db_list(&C, &names);
    for (size_t i = 0; i < names.n; i++)
        if (!sv_contains(&want, names.v[i])) {
            sv_push(&removes, names.v[i]);
            sv_push(&txn_plan, names.v[i]);
        }
    if (missing)
        die("cannot roll back to generation %d", target);
    if (!installs.n && !removes.n) {
        step("Already at the state of generation %d", target);
        return 0;
    }

    step("Rolling back to generation %d", target);
    for (size_t i = 0; i < shown.n; i++)
        printf("  install %s\n", shown.v[i]);
    for (size_t i = 0; i < removes.n; i++)
        printf("  remove  %s\n", removes.v[i]);
    if (C.dry_run)
        return 0;
    if (!confirm("Proceed?"))
        return 1;
    if (nfetch && repo_fetch(&C, fetch, nfetch))
        die("nothing was changed");
    free(fetch);

    owners o;
    owners_load(&C, &o);
    int failed = 0;
    /* Removals first, so files that moved between packages don't conflict. */
    for (size_t i = 0; i < removes.n; i++)
        failed |= txn_remove(&C, &o, removes.v[i]) != 0;
    for (size_t i = 0; i < installs.n; i++) {
        char *sp = strrchr(installs.v[i], ' ');
        *sp = 0;
        failed |= txn_install(&C, &o, installs.v[i], sp[1] == '1') != 0;
    }
    owners_free(&o);
    finish(cmd);
    sv_free(&lines);
    sv_free(&want);
    sv_free(&installs);
    sv_free(&shown);
    sv_free(&removes);
    sv_free(&names);
    return failed;
}

static int cmd_history(void)
{
    int cur = gen_current(&C);
    int *nums;
    size_t n;
    gen_list(&C, &nums, &n);
    if (!n)
        printf("no generations yet\n");
    for (size_t i = 0; i < n; i++) {
        strvec lines = {0};
        char *header = NULL;
        gen_read(&C, nums[i], &lines, &header);
        long long t = 0;
        const char *cmd = "";
        if (header) {
            char *tp = strstr(header, "time="), *cp = strstr(header, "cmd=");
            if (tp)
                t = atoll(tp + 5);
            if (cp)
                cmd = cp + 4;
        }
        char when[32] = "?";
        time_t tt = (time_t)t;
        struct tm tm;
        if (t && localtime_r(&tt, &tm))
            strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
        printf("%c %4d  %s  %4zu pkgs  %s\n", nums[i] == cur ? '*' : ' ', nums[i], when, lines.n, cmd);
        free(header);
        sv_free(&lines);
    }
    free(nums);
    return 0;
}

static int cmd_clean(void)
{
    int *nums;
    size_t n;
    gen_list(&C, &nums, &n);
    int cur = gen_current(&C);
    if (C.keep_gens > 0 && n > (size_t)C.keep_gens) {
        for (size_t i = 0; i + (size_t)C.keep_gens < n; i++) {
            if (nums[i] == cur)
                continue;
            char *p = gen_path(&C, nums[i]);
            unlink(p);
            free(p);
        }
    }
    free(nums);

    /* Everything still referenced by a generation or installed stays. */
    hmap keep = {0};
    gen_list(&C, &nums, &n);
    for (size_t i = 0; i < n; i++) {
        strvec lines = {0};
        gen_read(&C, nums[i], &lines, NULL);
        for (size_t j = 0; j < lines.n; j++) {
            char name[256], vr[256];
            if (sscanf(lines.v[j], "%255s %255s", name, vr) == 2) {
                char *k = xasprintf("%s-%s" PKG_EXT, name, vr);
                hm_put(&keep, k, (void *)1);
                free(k);
            }
        }
        sv_free(&lines);
    }
    free(nums);
    strvec names = {0};
    db_list(&C, &names);
    for (size_t i = 0; i < names.n; i++) {
        pkg p;
        if (db_get(&C, names.v[i], &p) == 0) {
            char *k = xasprintf("%s-%s-%d" PKG_EXT, p.name, p.version, p.release);
            hm_put(&keep, k, (void *)1);
            free(k);
            pkg_free(&p);
        }
    }
    sv_free(&names);

    char *dir = xasprintf("%s/pkgs", C.cache);
    DIR *d = opendir(dir);
    size_t removed = 0;
    long long bytes = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!ends_with(e->d_name, PKG_EXT) || hm_get(&keep, e->d_name))
            continue;
        char *p = xasprintf("%s/%s", dir, e->d_name);
        struct stat st;
        if (!C.dry_run && stat(p, &st) == 0 && unlink(p) == 0) {
            removed++;
            bytes += st.st_size;
        } else if (C.dry_run) {
            printf("  would delete %s\n", e->d_name);
        }
        free(p);
    }
    if (d)
        closedir(d);
    free(dir);
    hm_free(&keep, NULL);

    if (C.sources && !C.dry_run) {
        char *s = xasprintf("%s/sources", C.cache);
        rm_rf(s);
        free(s);
    }
    char *b = xasprintf("%s/build", C.tmp);
    if (!C.dry_run)
        rm_rf(b);
    free(b);
    char h[32];
    human(bytes, h, sizeof h);
    step("Deleted %zu cached package(s), %s", removed, h);
    return 0;
}

/* ---- repositories ---- */

static int cmd_sync(void)
{
    int failed = 0;
    for (size_t i = 0; i < C.nrrepos; i++) {
        const reciperepo *rr = &C.rrepos[i];
        char *git = xasprintf("%s/.git", rr->path);
        int is_git = path_exists(git);
        free(git);
        if (!is_git && rr->git) {
            step("Cloning recipes %s from %s", rr->name, rr->git);
            char *argv[] = { "git", "clone", "--quiet", rr->git, rr->path, NULL };
            if (run(argv, NULL, NULL)) {
                warnf("%s: git clone failed", rr->name);
                failed = 1;
                continue;
            }
        } else if (is_git) {
            step("Syncing recipes %s", rr->name);
            char *pull[] = { "git", "-C", rr->path, "pull", "--ff-only", "--quiet", NULL };
            if (run(pull, NULL, NULL)) {
                warnf("%s: git pull failed", rr->name);
                failed = 1;
                continue;
            }
        } else {
            continue;   /* a plain local directory */
        }
        if (rr->require_signed) {
            char *verify[] = { "git", "-C", rr->path, "verify-commit", "HEAD", NULL };
            if (run(verify, NULL, NULL)) {
                warnf("%s: HEAD is not signed by a trusted key; do not build from it", rr->name);
                failed = 1;
            }
        }
    }
    for (size_t i = 0; i < C.nbrepos; i++)
        failed |= repo_sync(&C, &C.brepos[i]) != 0;
    return failed;
}

/* ---- queries ---- */

static int match(const char *term, const char *name, const char *desc)
{
    return !term || strcasestr(name, term) || (desc && strcasestr(desc, term));
}

static int cmd_search(const char *term)
{
    hmap seen = {0};
    const pkg **all;
    size_t n;
    repo_all(&C, &all, &n);
    strvec lines = {0};
    for (size_t i = 0; i < n; i++) {
        const pkg *p = all[i];
        hm_put(&seen, p->name, (void *)1);
        if (!match(term, p->name, p->description))
            continue;
        pkg inst;
        int ins = db_get(&C, p->name, &inst) == 0;
        sv_push_own(&lines, xasprintf("%s %s-%d [%s]%s\n    %s", p->name, p->version, p->release, p->repo,
                                      ins ? " [installed]" : "", p->description ? p->description : ""));
        if (ins)
            pkg_free(&inst);
    }
    strvec names = {0};
    recipe_names(&C, &names);
    for (size_t i = 0; i < names.n; i++) {
        recipe r;
        if (hm_get(&seen, names.v[i]) || recipe_find(&C, names.v[i], &r))
            continue;
        pkgview v = recipe_view(&r, names.v[i]);
        if (match(term, names.v[i], v.description)) {
            pkg inst;
            int ins = db_get(&C, names.v[i], &inst) == 0;
            sv_push_own(&lines, xasprintf("%s %s-%d [recipe]%s\n    %s", names.v[i], r.version, r.release,
                                          ins ? " [installed]" : "", v.description ? v.description : ""));
            if (ins)
                pkg_free(&inst);
        }
        recipe_free(&r);
    }
    sv_sort(&lines);
    for (size_t i = 0; i < lines.n; i++)
        puts(lines.v[i]);
    sv_free(&lines);
    sv_free(&names);
    hm_free(&seen, NULL);
    return 0;
}

static void field(const char *label, const strvec *v)
{
    if (!v->n)
        return;
    char *s = sv_join(v, " ");
    printf("%-14s: %s\n", label, s);
    free(s);
}

static int cmd_info(const char *name)
{
    cand x;
    pkg p;
    int have_x = cand_find(&C, name, &x) == 0;
    int have_p = db_get(&C, name, &p) == 0;
    if (!have_x && !have_p)
        die("%s: not found", name);
    printf("%-14s: %s\n", "Name", name);
    if (have_x) {
        pkgview v = cand_view(&x);
        printf("%-14s: %s-%d (%s)\n", "Available", x.version, x.release,
               x.binary ? x.rp->repo : "build from recipe");
        if (v.description)
            printf("%-14s: %s\n", "Description", v.description);
        field("Depends", v.depends);
        field("Provides", v.provides);
        field("Conflicts", v.conflicts);
        field("Replaces", v.replaces);
        if (x.binary) {
            char a[32], b[32];
            human(x.rp->csize, a, sizeof a);
            human(x.rp->size, b, sizeof b);
            printf("%-14s: %s download, %s installed\n", "Size", a, b);
            field("Libraries", &x.rp->shlib_provides);
            field("Needs libs", &x.rp->shlib_requires);
        } else {
            printf("%-14s: %s\n", "Recipe", x.r.dir);
            if (x.r.license)
                printf("%-14s: %s\n", "License", x.r.license);
            if (x.r.url)
                printf("%-14s: %s\n", "URL", x.r.url);
            field("Build deps", &x.r.makedepends);
            if (strcmp(x.r.name, name))
                printf("%-14s: %s\n", "Built by", x.r.name);
        }
    }
    if (have_p) {
        printf("%-14s: %s-%d (%s%s)\n", "Installed", p.version, p.release,
               p.explicit_ ? "explicit" : "dependency", db_held(&C, name) ? ", held" : "");
        if (!have_x) {
            field("Depends", &p.depends);
            field("Provides", &p.provides);
        }
        field("Libraries", &p.shlib_provides);
    } else {
        printf("%-14s: no\n", "Installed");
    }
    if (have_x)
        cand_free(&x);
    if (have_p)
        pkg_free(&p);
    return 0;
}

static int cmd_list(void)
{
    strvec names = {0};
    db_list(&C, &names);
    for (size_t i = 0; i < names.n; i++) {
        pkg p;
        if (db_get(&C, names.v[i], &p))
            continue;
        if (!C.explicit_only || p.explicit_)
            printf("%s %s-%d%s\n", p.name, p.version, p.release, db_held(&C, p.name) ? " [held]" : "");
        pkg_free(&p);
    }
    sv_free(&names);
    return 0;
}

static int cmd_files(const char *name)
{
    manifest m;
    if (manifest_read(&C, name, &m))
        die("%s is not installed", name);
    for (size_t i = 0; i < m.n; i++)
        printf("/%s%s\n", m.e[i].path, m.e[i].type == 'd' ? "/" : "");
    manifest_free(&m);
    return 0;
}

/* Prints the package owning rel (relative to the root); 0 if there is one. */
static int owner_of(const char *shown, const char *rel)
{
    strvec names = {0};
    db_list(&C, &names);
    int found = 0;
    for (size_t i = 0; i < names.n && !found; i++) {
        manifest m;
        if (manifest_read(&C, names.v[i], &m))
            continue;
        const mentry *e = manifest_find(&m, rel);
        if (e && e->type != 'd') {
            pkg p;
            db_get(&C, names.v[i], &p);
            printf("/%s is owned by %s %s-%d\n", shown, p.name, p.version, p.release);
            pkg_free(&p);
            found = 1;
        }
        manifest_free(&m);
    }
    sv_free(&names);
    return found ? 0 : -1;
}

static int cmd_owner(const char *path)
{
    while (*path == '/')
        path++;
    if (owner_of(path, path) == 0)
        return 0;
    /* The directory may be a symlink, e.g. /usr/sbin -> bin on a merged-/usr
     * system: look the file up under the directory's real path too. */
    const char *slash = strrchr(path, '/');
    if (slash) {
        char *dir = strndup(path, (size_t)(slash - path));
        char *full = rpath(&C, dir), real[PATH_MAX];
        size_t rl = strcmp(C.root, "/") ? strlen(C.root) : 0;
        int rc = -1;
        if (realpath(full, real) && !strncmp(real, C.root, rl) && (real[rl] == '/' || !real[rl])) {
            const char *rdir = real + rl;
            while (*rdir == '/')
                rdir++;
            char *rel = *rdir ? xasprintf("%s%s", rdir, slash) : xstrdup(slash + 1);
            if (strcmp(rel, path))
                rc = owner_of(path, rel);
            free(rel);
        }
        free(dir);
        free(full);
        if (rc == 0)
            return 0;
    }
    printf("/%s is not owned by any package\n", path);
    return 1;
}

static int cmd_deps(const char *name)
{
    cand x;
    pkg p;
    const strvec *deps, *libs = NULL;
    int have_x = 0, have_p = db_get(&C, name, &p) == 0;
    if (have_p) {
        deps = &p.depends;
        libs = &p.shlib_requires;
    } else if ((have_x = cand_find(&C, name, &x) == 0)) {
        deps = cand_view(&x).depends;
        if (x.binary)
            libs = &x.rp->shlib_requires;
    } else {
        die("%s: not found", name);
    }
    for (size_t i = 0; i < deps->n; i++) {
        char *dn = dep_name(deps->v[i]);
        const char *by = db_provider(&C, dn);
        if (by && strcmp(by, dn))
            printf("%-30s installed (provided by %s)\n", deps->v[i], by);
        else
            printf("%-30s %s\n", deps->v[i], by ? "installed" : "missing");
        free(dn);
    }
    for (size_t i = 0; libs && i < libs->n; i++) {
        const char *by = db_shlib_provider(&C, libs->v[i]);
        printf("%-30s %s%s\n", libs->v[i], by ? "from " : (shlib_satisfied(&C, libs->v[i]) ? "system" : "missing"),
               by ? by : "");
    }
    if (have_p)
        pkg_free(&p);
    if (have_x)
        cand_free(&x);
    return 0;
}

static int cmd_rdeps(const char *name)
{
    pkg p;
    if (db_get(&C, name, &p))
        die("%s is not installed", name);
    pkg_free(&p);
    strvec req = {0};
    required_by(&C, name, NULL, &req);
    for (size_t i = 0; i < req.n; i++)
        puts(req.v[i]);
    sv_free(&req);
    return 0;
}

static int cmd_orphans(void)
{
    strvec drop = {0};
    find_orphans(&C, &drop);
    for (size_t i = 0; i < drop.n; i++)
        puts(drop.v[i]);
    sv_free(&drop);
    return 0;
}

static int cmd_check(char **args, size_t n)
{
    strvec names = {0};
    if (n)
        for (size_t i = 0; i < n; i++)
            sv_push(&names, args[i]);
    else
        db_list(&C, &names);
    size_t missing = 0, edited = 0;
    for (size_t i = 0; i < names.n; i++) {
        manifest m;
        if (manifest_read(&C, names.v[i], &m)) {
            warnf("%s is not installed", names.v[i]);
            missing++;
            continue;
        }
        for (size_t j = 0; j < m.n; j++) {
            const mentry *e = &m.e[j];
            char *p = rpath(&C, e->path);
            struct stat st;
            if (lstat(p, &st)) {
                printf("%s: missing /%s\n", names.v[i], e->path);
                missing++;
            } else if (e->type == 'f' && e->sha[0]) {
                char h[65];
                if (sha256_file(p, h) == 0 && strcmp(h, e->sha)) {
                    printf("%s: edited  /%s\n", names.v[i], e->path);
                    edited++;
                }
            }
            free(p);
        }
        manifest_free(&m);
    }
    sv_free(&names);
    step("%zu missing, %zu edited config file(s)", missing, edited);
    return missing ? 1 : 0;
}

static int cmd_show(const char *name)
{
    recipe r;
    if (recipe_find(&C, name, &r))
        die("%s: no recipe found", name);
    print_recipe_files(&r);
    recipe_free(&r);
    return 0;
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    /* Output piped into `head` must not kill fpm in the middle of a transaction. */
    signal(SIGPIPE, SIG_IGN);
    const char *conf = NULL, *root = NULL, *signkey = NULL;
    strvec pos = {0};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--root") || !strcmp(a, "--config") || !strcmp(a, "--keep") ||
            !strcmp(a, "-P") || !strcmp(a, "--parallel") || !strcmp(a, "--sign")) {
            if (i + 1 >= argc)
                die("%s needs a value", a);
            const char *v = argv[++i];
            if (!strcmp(a, "--root"))
                root = v;
            else if (!strcmp(a, "--config"))
                conf = v;
            else if (!strcmp(a, "--sign"))
                signkey = v;
            else if (!strcmp(a, "--keep"))
                C.keep_gens = atoi(v);
            else if ((C.parallel = atoi(v)) <= 0)
                die("%s needs a number above 0", a);
        } else if (!strcmp(a, "-y") || !strcmp(a, "--yes")) {
            C.yes = 1;
        } else if (!strcmp(a, "-n") || !strcmp(a, "--dry-run")) {
            C.dry_run = 1;
        } else if (!strcmp(a, "-f") || !strcmp(a, "--force")) {
            C.force = 1;
        } else if (!strcmp(a, "--overwrite")) {
            C.overwrite = 1;
        } else if (!strcmp(a, "-S") || !strcmp(a, "--source")) {
            C.source_only = 1;
        } else if (!strcmp(a, "--reinstall")) {
            C.reinstall = 1;
        } else if (!strcmp(a, "--rebuild")) {
            C.rebuild = 1;
        } else if (!strcmp(a, "--keep-build")) {
            C.keep_build = 1;
        } else if (!strcmp(a, "--sources")) {
            C.sources = 1;
        } else if (!strcmp(a, "-e") || !strcmp(a, "--explicit")) {
            C.explicit_only = 1;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("fpm %s\n", FPM_VERSION);
            return 0;
        } else if (a[0] == '-' && a[1]) {
            die("unknown option %s (see fpm --help)", a);
        } else {
            sv_push(&pos, a);
        }
    }
    if (!pos.n) {
        usage(stderr);
        return 2;
    }

    const char *cmd = pos.v[0];
    char **args = pos.v + 1;
    size_t nargs = pos.n - 1;

#define NEED_ARGS(min) do { if (nargs < (min)) die("%s: missing argument (see fpm --help)", cmd); } while (0)

    /* Repository tools work on plain directories; no configuration needed. */
    if (!strcmp(cmd, "index")) {
        NEED_ARGS(1);
        return repo_index(args[0], signkey) ? 1 : 0;
    }
    if (!strcmp(cmd, "keygen")) {
        NEED_ARGS(1);
        return repo_keygen(args[0]) ? 1 : 0;
    }

    C.conf = xstrdup(conf ? conf : "/etc/fpm.conf");
    C.root = root ? xstrdup(root) : NULL;
    config_load(&C, conf != NULL);
    umask(022);

    char *cmdline = sv_join(&pos, " ");
    strvec targets = {0};
    for (size_t i = 0; i < nargs; i++)
        sv_push(&targets, args[i]);

    int rc;
    if (!strcmp(cmd, "install")) {
        NEED_ARGS(1);
        begin_change();
        rc = do_install(&targets, 1, cmdline);
    } else if (!strcmp(cmd, "remove")) {
        NEED_ARGS(1);
        begin_change();
        rc = cmd_remove(args, nargs, cmdline);
    } else if (!strcmp(cmd, "upgrade")) {
        begin_change();
        rc = cmd_upgrade(cmdline);
    } else if (!strcmp(cmd, "autoremove")) {
        begin_change();
        rc = cmd_autoremove(cmdline);
    } else if (!strcmp(cmd, "hold") || !strcmp(cmd, "unhold")) {
        NEED_ARGS(1);
        begin_change();
        rc = cmd_hold(args, nargs, !strcmp(cmd, "hold"));
    } else if (!strcmp(cmd, "rollback")) {
        begin_change();
        rc = cmd_rollback(nargs ? args[0] : NULL, cmdline);
    } else if (!strcmp(cmd, "repair")) {
        need_root();
        db_lock(&C);
        if (!txn_pending(&C))
            step("Nothing to repair");
        repair();
        rc = 0;
    } else if (!strcmp(cmd, "clean")) {
        begin_change();
        rc = cmd_clean();
    } else if (!strcmp(cmd, "build")) {
        NEED_ARGS(1);
        need_root();
        rc = cmd_build(args, nargs);
    } else if (!strcmp(cmd, "sync")) {
        need_root();
        db_lock(&C);
        rc = cmd_sync();
    } else if (!strcmp(cmd, "history")) {
        warn_pending();
        rc = cmd_history();
    } else if (!strcmp(cmd, "search")) {
        rc = cmd_search(nargs ? args[0] : NULL);
    } else if (!strcmp(cmd, "info")) {
        NEED_ARGS(1);
        rc = cmd_info(args[0]);
    } else if (!strcmp(cmd, "list")) {
        warn_pending();
        rc = cmd_list();
    } else if (!strcmp(cmd, "files")) {
        NEED_ARGS(1);
        rc = cmd_files(args[0]);
    } else if (!strcmp(cmd, "owner")) {
        NEED_ARGS(1);
        rc = cmd_owner(args[0]);
    } else if (!strcmp(cmd, "deps")) {
        NEED_ARGS(1);
        rc = cmd_deps(args[0]);
    } else if (!strcmp(cmd, "rdeps")) {
        NEED_ARGS(1);
        rc = cmd_rdeps(args[0]);
    } else if (!strcmp(cmd, "orphans")) {
        rc = cmd_orphans();
    } else if (!strcmp(cmd, "check")) {
        rc = cmd_check(args, nargs);
    } else if (!strcmp(cmd, "show")) {
        NEED_ARGS(1);
        rc = cmd_show(args[0]);
    } else {
        warnf("unknown command '%s'", cmd);
        usage(stderr);
        rc = 2;
    }
    free(cmdline);
    sv_free(&targets);
    sv_free(&pos);
    return rc;
}
