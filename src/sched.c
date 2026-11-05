/* Job scheduler. Binary packages are downloaded up front, all in parallel.
 * Source packages build in parallel (up to c->parallel at once) as soon as
 * everything they depend on in this plan is installed. Installs stay
 * sequential, in dependency order, in this process. With more than one build
 * slot, build output goes to <cache>/logs/<recipe>.log. */
#include "fpm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    pid_t pid;
    char *recipe, *log;
} worker;

static int job_index(const job *jobs, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(jobs[i].name, name))
            return (int)i;
    return -1;
}

static int deps_done(const job *jobs, size_t n, const job *j)
{
    for (size_t i = 0; i < j->wait.n; i++) {
        int k = job_index(jobs, n, j->wait.v[i]);
        if (k >= 0 && jobs[k].state != J_DONE)
            return 0;
    }
    return 1;
}

static void add_wait(job *j, const char *name, const strvec *order)
{
    if (strcmp(name, j->name) && sv_contains(order, name) && !sv_contains(&j->wait, name))
        sv_push(&j->wait, name);
}

static void add_waits(job *j, const strvec *deps, const strvec *order)
{
    for (size_t i = 0; i < deps->n; i++) {
        char *dn = dep_name(deps->v[i]);
        add_wait(j, dn, order);
        free(dn);
    }
}

job *jobs_plan(const config *c, const strvec *order, const strvec *explicit_names)
{
    job *jobs = calloc(order->n ? order->n : 1, sizeof *jobs);
    if (!jobs)
        die("out of memory");
    for (size_t i = 0; i < order->n; i++) {
        job *j = &jobs[i];
        j->name = xstrdup(order->v[i]);
        if (cand_find(c, j->name, &j->x))
            die("%s: disappeared from the repositories", j->name);
        j->mark = explicit_names && sv_contains(explicit_names, j->name);
        pkgview v = cand_view(&j->x);
        add_waits(j, v.depends, order);
        if (j->x.binary) {
            /* Wait only for library providers planned earlier: a later one is
             * a cycle (binutils needs gcc's libstdc++, gcc depends on binutils),
             * and the install accepts a library this transaction provides. */
            for (size_t k = 0; k < j->x.rp->shlib_requires.n; k++) {
                const pkg *prov = repo_shlib_provider(c, j->x.rp->shlib_requires.v[k]);
                if (prov && sv_index(order, prov->name) < (long)i)
                    add_wait(j, prov->name, order);
            }
        } else if (c->rebuild || !recipe_built(c, &j->x.r)) {
            add_waits(j, &j->x.r.depends, order);
            add_waits(j, &j->x.r.makedepends, order);
        }
    }
    return jobs;
}

void jobs_free(job *jobs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(jobs[i].name);
        cand_free(&jobs[i].x);
        sv_free(&jobs[i].wait);
    }
    free(jobs);
}

static int start_build(const config *c, const recipe *r, int slots, worker *w)
{
    w->recipe = xstrdup(r->name);
    w->log = NULL;
    if (slots > 1) {
        char *dir = xasprintf("%s/logs", c->cache);
        mkdir_p(dir, 0755);
        w->log = xasprintf("%s/%s.log", dir, r->name);
        free(dir);
        step("Building %s %s-%d (log: %s)", r->name, r->version, r->release, w->log);
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        warnf("cannot start build of %s: %s", r->name, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (w->log) {
            int fd = open(w->log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            int nul = open("/dev/null", O_RDONLY);
            if (fd >= 0) {
                dup2(fd, 1);
                dup2(fd, 2);
            }
            if (nul >= 0)
                dup2(nul, 0);
        }
        _exit(build_recipe(c, r) ? 1 : 0);
    }
    w->pid = pid;
    return 0;
}

static void show_log_tail(const char *log)
{
    if (!log)
        return;
    warnf("last lines of %s:", log);
    char *argv[] = { "tail", "-n", "25", (char *)log, NULL };
    run(argv, NULL, NULL);
}

static int fetch_binaries(const config *c, job *jobs, size_t n)
{
    const pkg **want = xmalloc((n ? n : 1) * sizeof *want);
    size_t nw = 0;
    for (size_t i = 0; i < n; i++)
        if (jobs[i].x.binary)
            want[nw++] = jobs[i].x.rp;
    int rc = nw ? repo_fetch(c, want, nw) : 0;
    free(want);
    return rc;
}

/* Are other jobs of this transaction still to be installed? */
static int others_pending(const job *jobs, size_t n, const job *self)
{
    for (size_t i = 0; i < n; i++)
        if (&jobs[i] != self && jobs[i].state != J_DONE && jobs[i].state != J_FAILED)
            return 1;
    return 0;
}

/* Retry deferred jobs; returns how many there were. */
static int wake_deferred(job *jobs, size_t n)
{
    int k = 0;
    for (size_t i = 0; i < n; i++)
        if (jobs[i].state == J_DEFERRED) {
            jobs[i].state = J_READY;
            k++;
        }
    return k;
}

int jobs_run(const config *c, job *jobs, size_t n, owners *o)
{
    if (o && fetch_binaries(c, jobs, n)) {
        warnf("nothing was changed");
        return 1;
    }
    int slots = c->parallel > 0 ? c->parallel : 1, running = 0, failed = 0, final_pass = 0;
    worker *w = calloc((size_t)slots, sizeof *w);
    strvec built = {0};
    if (!w)
        die("out of memory");

    for (;;) {
        int progress = 0;

        for (size_t i = 0; i < n; i++) {
            job *j = &jobs[i];
            if (j->state != J_READY)
                continue;
            if (!o) {
                j->state = J_DONE;
                progress = 1;
                continue;
            }
            /* Its libraries may come from a package later in this transaction. */
            txn_may_defer = !final_pass && others_pending(jobs, n, j);
            char *a = archive_path(c, j->name, j->x.version, j->x.release);
            int rc = txn_install(c, o, a, j->mark);
            free(a);
            txn_may_defer = 0;
            if (rc == TXN_DEFERRED) {
                j->state = J_DEFERRED;
                continue;
            }
            progress = 1;
            j->state = rc ? J_FAILED : J_DONE;
            failed |= rc != 0;
            if (!rc)
                wake_deferred(jobs, n);
        }

        for (size_t i = 0; i < n && !failed; i++) {
            job *j = &jobs[i];
            if (j->state != J_WAIT || !deps_done(jobs, n, j))
                continue;
            if (j->x.binary) {
                j->state = J_READY;   /* already downloaded */
                progress = 1;
                continue;
            }
            int busy = 0;
            for (int k = 0; k < slots; k++)
                busy |= w[k].pid && !strcmp(w[k].recipe, j->x.r.name);
            if (busy) {
                j->state = J_BUILDING;   /* sibling subpackage is already being built */
                continue;
            }
            if (sv_contains(&built, j->x.r.name) || (!c->rebuild && recipe_built(c, &j->x.r))) {
                j->state = J_READY;
                progress = 1;
                continue;
            }
            if (running == slots)
                continue;
            int k = 0;
            while (w[k].pid)
                k++;
            if (start_build(c, &j->x.r, slots, &w[k])) {
                j->state = J_FAILED;
                failed = 1;
                break;
            }
            j->state = J_BUILDING;
            running++;
            progress = 1;
        }

        if (progress)
            continue;
        if (!running) {
            /* Nothing else can provide what the deferred ones need: install
             * them now, which reports the missing libraries. */
            if (final_pass || !wake_deferred(jobs, n))
                break;
            final_pass = 1;
            continue;
        }

        int st;
        pid_t pid = waitpid(-1, &st, 0);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        int k = 0;
        while (k < slots && w[k].pid != pid)
            k++;
        if (k == slots)
            continue;
        int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        sv_push(&built, w[k].recipe);
        for (size_t i = 0; i < n; i++)
            if (jobs[i].state == J_BUILDING && !jobs[i].x.binary && !strcmp(jobs[i].x.r.name, w[k].recipe))
                jobs[i].state = ok ? J_READY : J_FAILED;
        if (!ok) {
            warnf("build of %s failed", w[k].recipe);
            show_log_tail(w[k].log);
            failed = 1;
        }
        free(w[k].recipe);
        free(w[k].log);
        w[k].pid = 0;
        running--;
    }

    strvec skipped = {0};
    for (size_t i = 0; i < n; i++)
        if (jobs[i].state == J_WAIT)
            sv_push(&skipped, jobs[i].name);
    if (skipped.n) {
        char *s = sv_join(&skipped, " ");
        warnf("not installed because something before them failed: %s", s);
        free(s);
        failed = 1;
    }
    sv_free(&skipped);
    sv_free(&built);
    free(w);
    return failed;
}
