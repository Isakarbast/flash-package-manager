#include "fpm.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- strings ---- */

void sv_push_own(strvec *sv, char *s)
{
    if (sv->n + 2 > sv->cap) {
        sv->cap = sv->cap ? sv->cap * 2 : 8;
        sv->v = xrealloc(sv->v, sv->cap * sizeof *sv->v);
    }
    sv->v[sv->n++] = s;
    sv->v[sv->n] = NULL;
}

void sv_push(strvec *sv, const char *s) { sv_push_own(sv, xstrdup(s)); }

int sv_contains(const strvec *sv, const char *s)
{
    for (size_t i = 0; i < sv->n; i++)
        if (strcmp(sv->v[i], s) == 0)
            return 1;
    return 0;
}

void sv_split(strvec *sv, const char *s)
{
    while (*s) {
        while (isspace((unsigned char)*s))
            s++;
        const char *start = s;
        while (*s && !isspace((unsigned char)*s))
            s++;
        if (s > start)
            sv_push_own(sv, strndup(start, (size_t)(s - start)));
    }
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void sv_sort(strvec *sv)
{
    if (sv->n)
        qsort(sv->v, sv->n, sizeof *sv->v, cmp_str);
}

char *sv_join(const strvec *sv, const char *sep)
{
    size_t len = 1, sl = strlen(sep);
    for (size_t i = 0; i < sv->n; i++)
        len += strlen(sv->v[i]) + sl;
    char *out = xmalloc(len), *p = out;
    *p = 0;
    for (size_t i = 0; i < sv->n; i++) {
        if (i) {
            memcpy(p, sep, sl);
            p += sl;
        }
        size_t l = strlen(sv->v[i]);
        memcpy(p, sv->v[i], l);
        p += l;
        *p = 0;
    }
    return out;
}

void sv_free(strvec *sv)
{
    for (size_t i = 0; i < sv->n; i++)
        free(sv->v[i]);
    free(sv->v);
    sv->v = NULL;
    sv->n = sv->cap = 0;
}

char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = 0;
    return s;
}

int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

/* ---- messages & memory ---- */

static void vmsg(FILE *f, const char *prefix, const char *fmt, va_list ap)
{
    fflush(stdout);
    fputs(prefix, f);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
}

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmsg(stderr, "fpm: error: ", fmt, ap);
    va_end(ap);
    exit(1);
}

void warnf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmsg(stderr, "fpm: ", fmt, ap);
    va_end(ap);
}

void step(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmsg(stdout, "==> ", fmt, ap);
    va_end(ap);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    char *d = strdup(s ? s : "");
    if (!d)
        die("out of memory");
    return d;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap;
    char *s;
    va_start(ap, fmt);
    if (vasprintf(&s, fmt, ap) < 0)
        die("out of memory");
    va_end(ap);
    return s;
}

/* ---- filesystem ---- */

int mkdir_p(const char *path, mode_t mode)
{
    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/')
            continue;
        *s = 0;
        if (mkdir(p, mode) && errno != EEXIST) {
            free(p);
            return -1;
        }
        *s = '/';
    }
    int rc = mkdir(p, mode);
    free(p);
    return rc && errno != EEXIST ? -1 : 0;
}

static int rm_cb(const char *p, const struct stat *st, int type, struct FTW *f)
{
    (void)st; (void)type; (void)f;
    return remove(p) && errno != ENOENT ? -1 : 0;
}

int rm_rf(const char *path)
{
    if (!path_exists(path))
        return 0;
    return nftw(path, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
}

char *read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    size_t cap = 4096, n = 0;
    char *buf = xmalloc(cap);
    ssize_t r;
    while ((r = read(fd, buf + n, cap - n - 1)) > 0) {
        n += (size_t)r;
        if (cap - n < 2)
            buf = xrealloc(buf, cap *= 2);
    }
    close(fd);
    if (r < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    if (len)
        *len = n;
    return buf;
}

int write_file_atomic(const char *path, const char *data, size_t len)
{
    char *tmp = xasprintf("%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        free(tmp);
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmp);
            free(tmp);
            return -1;
        }
        off += (size_t)w;
    }
    int rc = fsync(fd);
    rc |= close(fd);
    if (rc == 0)
        rc = rename(tmp, path);
    if (rc)
        unlink(tmp);
    free(tmp);
    return rc;
}

int path_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int copy_file(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[65536];
    ssize_t r;
    int rc = 0;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        for (ssize_t off = 0; off < r;) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = -1;
                break;
            }
            off += w;
        }
        if (rc)
            break;
    }
    if (r < 0)
        rc = -1;
    if (rc == 0)
        rc = fchmod(out, mode);
    close(in);
    if (close(out))
        rc = -1;
    return rc;
}

/* ---- processes ---- */

int run(char *const argv[], const char *cwd, const strvec *env)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (cwd && chdir(cwd) != 0) {
            fprintf(stderr, "fpm: cannot enter %s: %s\n", cwd, strerror(errno));
            _exit(127);
        }
        signal(SIGPIPE, SIG_DFL);   /* fpm ignores it; its children shouldn't */
        if (env)
            for (size_t i = 0; i < env->n; i++)
                putenv(env->v[i]);
        execvp(argv[0], argv);
        fprintf(stderr, "fpm: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return 128 + WTERMSIG(st);
}

int have_program(const char *name)
{
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/bin:/bin";
    char *dirs = xstrdup(path), *save = NULL;
    int found = 0;
    for (char *d = strtok_r(dirs, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
        char *p = xasprintf("%s/%s", d, name);
        found = access(p, X_OK) == 0;
        free(p);
    }
    free(dirs);
    return found;
}

/* ---- key = value files ---- */

int kv_parse(const char *text, kv_cb cb, void *ud)
{
    char *buf = xstrdup(text), *p = buf;
    int lineno = 0, err = 0;
    while (p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = 0;
        lineno++;
        char *line = trim(p);
        if (*line && *line != '#') {
            char *eq = strchr(line, '=');
            if (!eq) {
                err = lineno;
                break;
            }
            *eq = 0;
            cb(trim(line), trim(eq + 1), ud);
        }
        p = nl ? nl + 1 : NULL;
    }
    free(buf);
    return err;
}

int run_capture(char *const argv[], char **out)
{
    int fds[2];
    *out = NULL;
    if (pipe(fds))
        return -1;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        signal(SIGPIPE, SIG_DFL);
        execvp(argv[0], argv);
        fprintf(stderr, "fpm: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    size_t cap = 4096, n = 0;
    char *buf = xmalloc(cap);
    ssize_t r;
    while ((r = read(fds[0], buf + n, cap - n - 1)) != 0) {
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        n += (size_t)r;
        if (cap - n < 2)
            buf = xrealloc(buf, cap *= 2);
    }
    close(fds[0]);
    buf[n] = 0;
    *out = buf;
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return 128 + WTERMSIG(st);
}
