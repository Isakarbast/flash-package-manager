/* fpm - Freedom Package Manager */
#ifndef FPM_H
#define FPM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define FPM_VERSION "0.2.0"
#define PKG_EXT ".fpm.tar.zst"
#define INDEX_FILE "index.fpm.zst"

/* ---- util.c ---- */

typedef struct {
    char **v;            /* always NULL-terminated, usable as argv */
    size_t n, cap;
} strvec;

void  sv_push(strvec *sv, const char *s);
void  sv_push_own(strvec *sv, char *s);
int   sv_contains(const strvec *sv, const char *s);
void  sv_split(strvec *sv, const char *s);
void  sv_sort(strvec *sv);
char *sv_join(const strvec *sv, const char *sep);
void  sv_free(strvec *sv);

_Noreturn void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void  warnf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void  step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
char *trim(char *s);
int   starts_with(const char *s, const char *prefix);
int   ends_with(const char *s, const char *suffix);

int   mkdir_p(const char *path, mode_t mode);
int   rm_rf(const char *path);
char *read_file(const char *path, size_t *len);
int   write_file_atomic(const char *path, const char *data, size_t len);
int   path_exists(const char *path);   /* lstat: does not follow symlinks */
int   is_dir(const char *path);        /* stat: follows symlinks */
int   copy_file(const char *src, const char *dst, mode_t mode);

/* Run argv[0] (PATH lookup) in cwd with extra "K=V" env entries.
 * Returns the exit status, 128+signal, or -1 if it could not start. */
int   run(char *const argv[], const char *cwd, const strvec *env);
/* Same, but collects stdout into *out (NUL-terminated). */
int   run_capture(char *const argv[], char **out);
int   have_program(const char *name);

typedef void (*kv_cb)(const char *key, const char *val, void *ud);
/* Parse "key = value" lines; '#' comments. Returns 0 or the first bad line number. */
int   kv_parse(const char *text, kv_cb cb, void *ud);

/* ---- sha256.c ---- */

/* Used for two things only: verifying downloads (sources and binary
 * packages), and detecting whether an installed /etc file was edited. */
int sha256_file(const char *path, char hex[65]);

/* ---- hmap.c ---- */

typedef struct { char *key; void *val; } hent;
typedef struct { hent *t; size_t cap, used, live; } hmap;

void *hm_get(const hmap *m, const char *key);
void  hm_put(hmap *m, const char *key, void *val);
void *hm_del(hmap *m, const char *key);
void  hm_free(hmap *m, void (*free_val)(void *));

/* ---- vercmp.c ---- */

int vercmp(const char *a, const char *b);
int vrcmp(const char *va, int ra, const char *vb, int rb);

/* ---- config.c ---- */

typedef struct {
    char *name, *url, *key;   /* key: ed25519 public key (PEM) the index must be signed with */
    int allow_unsigned;
} binrepo;

typedef struct {
    char *name, *path, *git;
    int require_signed;       /* sync refuses an unsigned git HEAD */
    int review;               /* show recipe + build script before building (AUR style) */
} reciperepo;

typedef struct {
    char *root, *conf, *arch;
    char *db, *cache, *tmp;          /* derived from root */
    binrepo *brepos;
    size_t nbrepos;
    reciperepo *rrepos;
    size_t nrrepos;
    strvec repos;                    /* recipe repository paths, same order as rrepos */
    strvec shlib_ignore;             /* sonames treated as always present */
    int jobs, parallel, sandbox, prefer_source;
    char *cflags, *cxxflags, *ldflags, *proxy;
    /* command-line flags */
    int force, dry_run, reinstall, rebuild, keep_build, explicit_only, keep_gens, sources;
    int yes, source_only;
} config;

void config_load(config *c, int conf_given);
char *rpath(const config *c, const char *rel);
const reciperepo *recipe_repo_of(const config *c, const char *recipe_dir);

/* ---- recipe.c ---- */

/* Extra package split out of a recipe's build ("<sub>.files = ..." keys). */
typedef struct {
    char *name, *description;
    strvec depends, files, provides, conflicts, replaces;
} subpkg;

typedef struct {
    char *name, *version, *description, *license, *url, *dir, *arch;
    int release;
    strvec depends, makedepends, sources, sha256, options, provides, conflicts, replaces;
    subpkg *subs;
    size_t nsubs;
    char *target;   /* package that was asked for: name or one of subs */
} recipe;

/* One package as produced by a recipe (the main package or a subpackage). */
typedef struct {
    const char *name, *description;
    const strvec *depends, *provides, *conflicts, *replaces;
} pkgview;

int  recipe_load(const char *dir, recipe *r);
int  recipe_find(const config *c, const char *name, recipe *r);
void recipe_free(recipe *r);
int  recipe_has_option(const recipe *r, const char *opt);
int  valid_name(const char *s);
int  is_git_source(const char *src);
pkgview recipe_view(const recipe *r, const char *pkgname);
/* Name of a recipe package that provides virt, or NULL. */
const char *recipe_provider(const config *c, const char *virt);
/* Every package name the recipe repositories can build. */
void recipe_names(const config *c, strvec *out);

/* "name", "name>=1.2", "name=2", "name<3" */
void dep_parse(const char *spec, char **name, char **op, char **ver);
int  dep_satisfied(const char *op, const char *want, const char *have);
char *dep_name(const char *spec);

/* ---- db.c ---- */

typedef struct {
    char *name, *version, *description, *arch;
    char *repo, *sha256;            /* repository index entries only */
    int release, explicit_;
    long long size, csize;           /* installed size, archive size */
    strvec depends, provides, conflicts, replaces, shlib_provides, shlib_requires;
} pkg;

typedef struct { char type; char sha[65]; char *path; } mentry;  /* 'd', 'f', 'l' */
typedef struct { mentry *e; size_t n, cap; } manifest;

int   pkg_parse(const char *text, pkg *p);
/* with_reason: include "reason" (database entries) */
char *pkg_format(const pkg *p, int with_reason);
void  pkg_free(pkg *p);

int  db_get(const config *c, const char *name, pkg *p);
void db_list(const config *c, strvec *names);
int  db_put(const config *c, const pkg *p, const manifest *m);
int  db_drop(const config *c, const char *name);
void db_lock(const config *c);
int  db_held(const config *c, const char *name);
int  db_set_held(const config *c, const char *name, int held);
/* Installed package that is or provides virt / provides soname (NULL if none).
 * The result stays valid until the database changes. */
const char *db_provider(const config *c, const char *virt);
const char *db_shlib_provider(const config *c, const char *soname);

int  manifest_read(const config *c, const char *name, manifest *m);
void manifest_push(manifest *m, char type, const char *sha, const char *path);
void manifest_sort(manifest *m);
const mentry *manifest_find(const manifest *m, const char *path);
void manifest_free(manifest *m);

void gen_record(const config *c, const char *cmd);
int  gen_current(const config *c);
int  gen_read(const config *c, int n, strvec *lines, char **header);
void gen_list(const config *c, int **nums, size_t *n);
char *gen_path(const config *c, int n);

/* ---- repo.c: binary repositories ---- */

void repos_load(const config *c);
const pkg *repo_find(const config *c, const char *name);
const pkg *repo_find_version(const config *c, const char *name, const char *version, int release);
/* Newest version of name that satisfies "op want" (e.g. "<", "2.0"). */
const pkg *repo_find_match(const config *c, const char *name, const char *op, const char *want);
const pkg *repo_provider(const config *c, const char *virt);
const pkg *repo_shlib_provider(const config *c, const char *soname);
void repo_all(const config *c, const pkg ***out, size_t *n);
int  repo_sync(const config *c, const binrepo *r);
/* Download (in parallel) and verify the archives of these index entries. */
int  repo_fetch(const config *c, const pkg *const *pkgs, size_t n);
/* Write <dir>/index.fpm.zst listing every package version in dir; sign it when key is given. */
int  repo_index(const char *dir, const char *key);
int  repo_keygen(const char *base);

/* ---- elf.c ---- */

/* DT_NEEDED entries and DT_SONAME of a dynamic ELF file. -1 if not one. */
int elf_dynamic(const char *path, strvec *needed, char **soname);

/* ---- build.c ---- */

char *archive_path(const config *c, const char *name, const char *ver, int rel);
/* Are the archives for the recipe and all its subpackages in the cache? */
int   recipe_built(const config *c, const recipe *r);
int   build_recipe(const config *c, const recipe *r);

/* ---- txn.c ---- */

typedef struct { hmap files; hmap dirs; } owners;  /* path -> owner name / dir refcount */

/* Paths (relative to root) added or removed by transactions in this run; used by hooks. */
extern strvec txn_changed;
/* Every package installed or removed by the current command (set by the caller). */
extern strvec txn_plan;

void owners_load(const config *c, owners *o);
void owners_free(owners *o);
int  txn_install(const config *c, owners *o, const char *archive, int mark_explicit);
int  txn_remove(const config *c, owners *o, const char *name);
/* Finish a transaction that was interrupted (crash, power loss). 1 = repaired, 0 = nothing to do, -1 = failed. */
int  txn_repair(const config *c);
int  txn_pending(const config *c);

/* ---- resolve.c ---- */

/* Where a package comes from: a binary repository or a recipe to build. */
typedef struct {
    char *name, *version;
    int release, binary;
    const pkg *rp;      /* binary: repository index entry */
    recipe r;           /* source: the recipe that builds it */
} cand;

/* Fills *x in place; never copy a cand (cand_view points into it). */
int  cand_find(const config *c, const char *name, cand *x);
pkgview cand_view(const cand *x);
void cand_free(cand *x);
int  shlib_satisfied(const config *c, const char *soname);
/* order: packages to install, dependencies first. removals: installed
 * packages that something in order replaces. */
int  plan_install(const config *c, const strvec *targets, strvec *order, strvec *removals);
void required_by(const config *c, const char *name, const strvec *ignore, strvec *out);
void find_orphans(const config *c, strvec *out);

/* ---- sched.c ---- */

enum { J_WAIT, J_BUILDING, J_READY, J_DONE, J_FAILED };

typedef struct {
    char *name;
    cand x;
    strvec wait;    /* packages in this plan that must be installed first */
    int state, mark;
} job;

/* One job per name in order, with what each must wait for. */
job *jobs_plan(const config *c, const strvec *order, const strvec *explicit_names);
/* Download binaries (in parallel), build sources (up to c->parallel at once)
 * and, when o is given, install everything in dependency order. */
int  jobs_run(const config *c, job *jobs, size_t n, owners *o);
void jobs_free(job *jobs, size_t n);

/* ---- hooks.c ---- */

void hooks_run(const config *c, const strvec *changed);

#endif
