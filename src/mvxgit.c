/*
 * MVX — a native compiler and runtime for Pick/MultiValue BASIC.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.  There is NO WARRANTY, to
 * the extent permitted by law; see the LICENSE file for details.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Native git for hash-file records, via libgit2 — modelled on real git.
 *
 * The working tree is the live records in an MVX file; the index (at
 * <repo>/index) is git's staging area; and commits are commits, in the
 * account's own git repository (its .git).  A record maps to the git path
 * "<file>/<id>", so ordinary git tooling and hosts read the history.
 * Attribute marks translate to newlines in the blob (and back on restore) so
 * diffs are line-oriented.
 *
 * These are cataloged subroutines (the MVX subroutine ABI), CALLed by
 * the BASIC GIT verb.  libgit2 links into this library alone; records
 * flow through the runtime storage API called from here — nothing
 * touches a filesystem working tree.  Library calls, not exec: any
 * privilege tier.
 */
#include "mvx_runtime.h"

#include <fnmatch.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- helpers ----------------------------------------------------------- */

static void ensure_init(void) {
    static int done;
    if (!done) { git_libgit2_init(); done = 1; }
}

static void arg_str(const mv_value *v, char *out, size_t cap) {
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(v, nb, sizeof nb, &p);
    if ((size_t)n >= cap) n = (int64_t)cap - 1;
    memcpy(out, p, (size_t)n);
    out[n] = '\0';
}

static void fail(mv_value *out, const char *what) {
    const git_error *e = git_error_last();
    char buf[512];
    snprintf(buf, sizeof buf, "git: %s: %s", what,
             e && e->message ? e->message : "failed");
    mv_set_str(out, buf, (int64_t)strlen(buf));
}

static char *xlate(const char *p, int64_t n, char from, char to,
                   int64_t *outn) {
    char *b = malloc(n ? (size_t)n : 1);
    if (!b) mvx_fatal("out of memory in git translate");
    for (int64_t i = 0; i < n; i++) b[i] = p[i] == from ? to : p[i];
    *outn = n;
    return b;
}

typedef struct { char *d; size_t len, cap; } sbuf;

static void sb_put(sbuf *s, const char *p, size_t n) {
    if (s->len + n + 1 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        while (s->cap < s->len + n + 1) s->cap *= 2;
        s->d = realloc(s->d, s->cap);
        if (!s->d) mvx_fatal("out of memory in git output");
    }
    memcpy(s->d + s->len, p, n);
    s->len += n;
}

static void sb_line(sbuf *s, const char *line) {
    if (s->len) { char am = (char)0xFE; sb_put(s, &am, 1); }
    sb_put(s, line, strlen(line));
}

static void sb_out(sbuf *s, mv_value *dst, const char *empty) {
    if (s->d) mv_set_str(dst, s->d, (int64_t)s->len);
    else mv_set_str(dst, empty, (int64_t)strlen(empty));
    free(s->d);
}

/* --- ignore rules ------------------------------------------------------
   The account's GITIGNORE record (a file in the account root, one glob
   per line) keeps bulk data out of history: ignore "ORDERS" to skip a
   million order records while "ORDERS.DICT" (the dictionary) stays
   trackable.  A pattern matches either the file name or the full
   "file/record" path. */
static char g_ign[128][256];
static int g_nign, g_ign_loaded;

static void load_ignores(void) {
    if (g_ign_loaded) return;
    g_ign_loaded = 1;
    g_nign = 0;
    const char *acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";
    char path[4096];
    snprintf(path, sizeof path, "%s/GITIGNORE", acct);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char ln[512];
    while (fgets(ln, sizeof ln, fp) && g_nign < 128) {
        size_t n = strlen(ln);
        while (n && (ln[n - 1] == '\n' || ln[n - 1] == '\r' ||
                     ln[n - 1] == ' '))
            ln[--n] = '\0';
        char *p = ln;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        snprintf(g_ign[g_nign++], 256, "%s", p);
    }
    fclose(fp);
}

static int ignored(const char *file, const char *path) {
    load_ignores();
    for (int i = 0; i < g_nign; i++) {
        if (fnmatch(g_ign[i], file, 0) == 0) return 1;
        if (fnmatch(g_ign[i], path, FNM_PATHNAME) == 0) return 1;
    }
    return 0;
}

/* Open an MVX file by its git name: a trailing ".DICT" opens the
   dictionary of the base file, so dictionaries are trackable as
   "<file>.DICT". */
static int open_named(mvx_ctx *ctx, const char *name, mv_value *fvar) {
    size_t n = strlen(name);
    mv_value spec;
    mv_init(&spec);
    if (n > 5 && strcmp(name + n - 5, ".DICT") == 0) {
        mv_value dictv;
        mv_init(&dictv);
        mv_set_str(&dictv, "DICT", 4);
        mv_set_str(&spec, name, (int64_t)(n - 5));
        int r = mvx_open(ctx, &dictv, &spec, fvar);
        mv_clear(&dictv);
        mv_clear(&spec);
        return r;
    }
    mv_set_str(&spec, name, (int64_t)n);
    int r = mvx_open(ctx, NULL, &spec, fvar);
    mv_clear(&spec);
    return r;
}

/* Init the account's git repository (initial branch main).  The path is the
   account's ".git" directory, so this is an ordinary (non-bare) repo that git
   and hosts like GitHub recognise — the account directory is its working tree.
   The working tree "checks out incorrectly" under plain git (records are the
   real working tree, not files), which is fine: the .mvx descriptor marks the
   directory as an MVX account, and the account tooling rebuilds the records. */
static int init_bare_main(git_repository **repo, const char *path) {
    /* A non-bare repo is initialised at its working directory (the account
       root), where git puts the gitdir at ".git".  Callers pass the gitdir
       (".git"); its working tree is the parent, i.e. the current directory the
       caller has already entered. */
    (void)path;
    git_repository_init_options o = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    o.flags = GIT_REPOSITORY_INIT_MKPATH;
    o.initial_head = "main";
    return git_repository_init_ext(repo, ".", &o);
}

/* Open (init if needed) the bare repo, and its persistent index. */
static int repo_open(const char *path, git_repository **repo,
                     git_index **index) {
    if (git_repository_open(repo, path) != 0 &&
        init_bare_main(repo, path) != 0)
        return -1;
    if (index) {
        char ipath[4200];
        snprintf(ipath, sizeof ipath, "%s/index", path);
        if (git_index_open(index, ipath) != 0) {
            git_repository_free(*repo);
            return -1;
        }
        git_index_read(*index, 0);      /* load if present */
    }
    return 0;
}

static git_tree *head_tree(git_repository *repo) {
    git_object *h = NULL;
    git_commit *c = NULL;
    git_tree *t = NULL;
    if (git_revparse_single(&h, repo, "HEAD") == 0 &&
        git_commit_lookup(&c, repo, git_object_id(h)) == 0)
        git_commit_tree(&t, c);
    if (c) git_commit_free(c);
    if (h) git_object_free(h);
    return t;
}

/* Blob oid of a record's current content (translated, not stored). */
static int record_oid(const char *content, int64_t len, git_oid *oid) {
    int64_t bl;
    char *b = xlate(content, len, (char)0xFE, '\n', &bl);
    int rc = git_odb_hash(oid, b, (size_t)bl, GIT_OBJECT_BLOB);
    free(b);
    return rc;
}

/* --- GITINIT(repo, out) ------------------------------------------------ */
void mvx_sub_GITINIT(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) == 0) {
        git_repository_free(repo);
        mv_set_str(argv[1], "already a repository", 20);
        return;
    }
    if (init_bare_main(&repo, rp) != 0) {
        fail(argv[1], "init");
        return;
    }
    git_repository_free(repo);
    mv_set_str(argv[1], "initialised empty git repository", 32);
}

/* Stage the current content of one MVX file's records (or one record)
   into the index.  GITADD(repo, file, record-or-empty, out) */
void mvx_sub_GITADD(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 4) return;
    ensure_init();
    char rp[4096], fn[256], only[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], fn, sizeof fn);
    arg_str(argv[2], only, sizeof only);

    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[3], "open"); return; }

    if (ignored(fn, fn)) {
        char out[300];
        snprintf(out, sizeof out, "%s is in GITIGNORE, nothing staged", fn);
        mv_set_str(argv[3], out, (int64_t)strlen(out));
        git_index_free(index);
        git_repository_free(repo);
        return;
    }

    mv_value fvar, id, rec;
    mv_init(&fvar); mv_init(&id); mv_init(&rec);
    int64_t n = 0, skipped = 0;
    if (open_named(ctx, fn, &fvar)) {
        int one = only[0] != '\0';
        int have = 1;
        if (one) {
            mv_set_str(&id, only, (int64_t)strlen(only));
            have = mvx_read(ctx, &rec, &fvar, &id, 0);
        } else {
            mvx_select(ctx, &fvar);
        }
        while (have) {
            if (!one) {
                if (!mvx_readnext(ctx, &id)) break;
                if (!mvx_read(ctx, &rec, &fvar, &id, 0)) continue;
            }
            char idb[256], nb[40];
            arg_str(&id, idb, sizeof idb);
            {
                char pcheck[600];
                snprintf(pcheck, sizeof pcheck, "%s/%s", fn, idb);
                if (ignored(fn, pcheck)) { skipped++; if (one) break;
                    else continue; }
            }
            const char *cp;
            int64_t clen = mv_val_chars(&rec, nb, sizeof nb, &cp);
            int64_t bl;
            char *blob = xlate(cp, clen, (char)0xFE, '\n', &bl);
            git_oid boid;
            if (git_blob_create_from_buffer(&boid, repo, blob,
                                            (size_t)bl) == 0) {
                git_index_entry e;
                memset(&e, 0, sizeof e);
                char path[600];
                snprintf(path, sizeof path, "%s/%s", fn, idb);
                e.path = path;
                e.mode = GIT_FILEMODE_BLOB;
                e.id = boid;
                git_index_add(index, &e);
                n++;
            }
            free(blob);
            if (one) break;
        }
    }
    mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);

    int rc = git_index_write(index);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[3], "write index"); return; }
    char out[96];
    if (skipped)
        snprintf(out, sizeof out, "staged %lld record(s), %lld ignored",
                 (long long)n, (long long)skipped);
    else
        snprintf(out, sizeof out, "staged %lld record(s)", (long long)n);
    mv_set_str(argv[3], out, (int64_t)strlen(out));
}

/* `git add` with git's own semantics: stage every on-disk file in the working
   tree — honouring .gitignore, executable bits, and top-level as well as nested
   paths — and stage deletions, exactly as stock git would.  This is step one of
   a drop-in `mvx-git add -A`; the caller layers step two on top (staging the
   records of VOC files that are not plain on-disk files, e.g. LMDB hash files).
   Uses the repository-owned index so libgit2 can stat the working tree and
   apply the ignore rules.  GITADDDISK(repo, out) */
/* The record-git model tracks records as git blobs, never the binary LMDB
   store — so the account's mvxdata.lmdb must never be staged, even when no
   .gitignore lists it.  Returns >0 to skip the path. */
static int addall_skip(const char *path, const char *matched, void *payload) {
    (void)matched;
    (void)payload;
    return strncmp(path, "mvxdata.lmdb", 12) == 0 ? 1 : 0;
}

void mvx_sub_GITADDDISK(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0 &&
        init_bare_main(&repo, rp) != 0) { fail(argv[1], "open"); return; }
    git_index *index = NULL;
    if (git_repository_index(&index, repo) != 0) {
        git_repository_free(repo);
        fail(argv[1], "index");
        return;
    }
    git_strarray all = {NULL, 0};              /* empty pathspec ⇒ everything */
    int rc = git_index_add_all(index, &all, GIT_INDEX_ADD_DEFAULT,
                               addall_skip, NULL);
    if (rc == 0) rc = git_index_update_all(index, &all, NULL, NULL);
    if (rc == 0) rc = git_index_write(index);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[1], "add"); return; }
    mv_set_str(argv[1], "staged working tree", 19);
}

/* Normalise the staged index to the open account format — the record-git
   cross-platform interchange (mvx#73).  The working tree stays a native account
   on disk; only the git objects carry the open form, so this rewrites staged
   blobs after `add`: a dictionary's `%FILE%` control (`FILE <VM> type <VM>
   conn`) becomes the portable class `DIR` or `hash`, and the account descriptor
   `.mvx` is stored at path `.mv-account`.  Runs only when the account opts in
   (`mvx.openaccount`).  GITOPENFORM(repo, out) */
void mvx_sub_GITOPENFORM(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[1], "open"); return; }

    /* Collect changes first — adding to the index mid-scan invalidates the
       entry pointers git_index_get_byindex returns. */
    struct fix { char path[600]; git_oid id; } *fix = NULL;
    size_t nfix = 0, capfix = 0;
    int have_mvx = 0;
    git_oid mvx_blob;
    const char *suffix = ".DICT/%FILE%";
    size_t sl = strlen(suffix);

    size_t ic = git_index_entrycount(index);
    for (size_t i = 0; i < ic; i++) {
        const git_index_entry *e = git_index_get_byindex(index, i);
        if (e->mode == GIT_FILEMODE_COMMIT) continue;   /* submodule gitlink */
        size_t pl = strlen(e->path);

        if (strcmp(e->path, ".mvx") == 0) {             /* .mvx -> .mv-account */
            git_blob *b = NULL;
            if (git_blob_lookup(&b, repo, &e->id) == 0) {
                if (git_blob_create_from_buffer(&mvx_blob, repo,
                        git_blob_rawcontent(b),
                        (size_t)git_blob_rawsize(b)) == 0)
                    have_mvx = 1;
                git_blob_free(b);
            }
            continue;
        }
        if (pl < sl || strcmp(e->path + pl - sl, suffix) != 0)
            continue;                                   /* not a %FILE% control */
        git_blob *b = NULL;
        if (git_blob_lookup(&b, repo, &e->id) != 0) continue;
        const char *c = git_blob_rawcontent(b);
        int64_t cl = (int64_t)git_blob_rawsize(b);
        const char *cls = NULL;                         /* native -> DIR/hash */
        if (cl >= 5 && memcmp(c, "FILE", 4) == 0 && (unsigned char)c[4] == 0xFD) {
            const char *t = c + 5, *end = c + cl;
            const char *m2 = memchr(t, 0xFD, (size_t)(end - t));
            const char *te = m2 ? m2 : end;
            while (te > t && (te[-1] == '\n' || te[-1] == '\r' ||
                              te[-1] == ' ' || te[-1] == '\t'))
                te--;                                   /* trim trailing ws */
            cls = (te - t == 3 && strncasecmp(t, "dir", 3) == 0) ? "DIR" : "hash";
        }
        git_blob_free(b);
        if (!cls) continue;
        git_oid nb;
        if (git_blob_create_from_buffer(&nb, repo, cls, strlen(cls)) != 0)
            continue;
        if (nfix == capfix) {
            capfix = capfix ? capfix * 2 : 16;
            fix = realloc(fix, capfix * sizeof *fix);
            if (!fix) mvx_fatal("out of memory in openform");
        }
        snprintf(fix[nfix].path, sizeof fix[nfix].path, "%s", e->path);
        fix[nfix].id = nb;
        nfix++;
    }

    for (size_t i = 0; i < nfix; i++) {
        git_index_entry e;
        memset(&e, 0, sizeof e);
        e.path = fix[i].path;
        e.mode = GIT_FILEMODE_BLOB;
        e.id = fix[i].id;
        git_index_add(index, &e);
    }
    if (have_mvx) {
        git_index_entry e;
        memset(&e, 0, sizeof e);
        e.path = ".mv-account";
        e.mode = GIT_FILEMODE_BLOB;
        e.id = mvx_blob;
        git_index_add(index, &e);
        git_index_remove_bypath(index, ".mvx");
    }
    free(fix);
    int rc = git_index_write(index);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[1], "openform"); return; }
    char out[80];
    snprintf(out, sizeof out, "open form: %zu file(s) normalised",
             nfix + (size_t)have_mvx);
    mv_set_str(argv[1], out, (int64_t)strlen(out));
}

/* Stage a git submodule as a gitlink (mode 0160000, id = the submodule's
   current HEAD) rather than recursing into it as a directory file — so an
   account can carry submodules (e.g. docs -> the repo wiki).  The submodule is
   added with plain `git submodule add` (which writes .gitmodules and clones
   it); this keeps the gitlink current on add/commit.  GITADDSUB(repo, name,
   out) */
void mvx_sub_GITADDSUB(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], name[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], name, sizeof name);

    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[2], "open"); return; }

    /* the submodule's HEAD is the commit the gitlink records */
    git_repository *sub = NULL;
    git_oid head;
    int rc = git_repository_open(&sub, name);
    if (rc == 0) rc = git_reference_name_to_id(&head, sub, "HEAD");
    if (sub) git_repository_free(sub);
    if (rc != 0) {
        git_index_free(index);
        git_repository_free(repo);
        fail(argv[2], "submodule HEAD");
        return;
    }
    git_index_entry e;
    memset(&e, 0, sizeof e);
    e.path = name;
    e.mode = GIT_FILEMODE_COMMIT;           /* 0160000 gitlink */
    e.id = head;
    rc = git_index_add(index, &e);
    if (rc == 0) rc = git_index_write(index);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[2], "stage submodule"); return; }
    char out[300];
    snprintf(out, sizeof out, "staged submodule %s", name);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* Unstage/remove tracking of a record.  GITRM(repo, file, record, out) */
void mvx_sub_GITRM(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 4) return;
    ensure_init();
    char rp[4096], fn[256], recid[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], fn, sizeof fn);
    arg_str(argv[2], recid, sizeof recid);

    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[3], "open"); return; }
    char path[600];
    snprintf(path, sizeof path, "%s/%s", fn, recid);
    int rc;
    if (recid[0]) rc = git_index_remove_bypath(index, path);
    else rc = git_index_remove_directory(index, fn, 0);
    if (rc == 0) rc = git_index_write(index);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[3], "rm"); return; }
    mv_set_str(argv[3], "removed from tracking", 21);
}

/* GITCOMMIT(repo, message, out) — commit the staged index. */
void mvx_sub_GITCOMMIT(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], msg[4096];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], msg, sizeof msg);

    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[2], "open"); return; }

    git_oid tree_oid, commit_oid;
    git_tree *tree = NULL, *ptree = NULL;
    git_signature *sig = NULL;
    git_object *headobj = NULL;
    git_commit *parent = NULL;
    int rc = git_index_write_tree_to(&tree_oid, index, repo);
    if (rc == 0) rc = git_tree_lookup(&tree, repo, &tree_oid);

    if (git_revparse_single(&headobj, repo, "HEAD") == 0 &&
        git_commit_lookup(&parent, repo, git_object_id(headobj)) == 0 &&
        git_commit_tree(&ptree, parent) == 0) {
        /* nothing to commit if the tree is unchanged */
        if (rc == 0 && git_oid_equal(git_tree_id(ptree), &tree_oid)) {
            git_tree_free(ptree); git_commit_free(parent);
            git_object_free(headobj);
            if (tree) git_tree_free(tree);
            git_index_free(index); git_repository_free(repo);
            mv_set_str(argv[2], "nothing to commit", 17);
            return;
        }
    }
    if (rc == 0 && git_signature_default(&sig, repo) != 0)
        rc = git_signature_now(&sig, "MVX", "mvx@localhost");
    if (rc == 0) {
        const git_commit *parents[1];
        int np = 0;
        if (parent) { parents[0] = parent; np = 1; }
        rc = git_commit_create(&commit_oid, repo, "HEAD", sig, sig,
                               NULL, msg, tree, np, parents);
    }
    if (sig) git_signature_free(sig);
    if (ptree) git_tree_free(ptree);
    if (parent) git_commit_free(parent);
    if (headobj) git_object_free(headobj);
    if (tree) git_tree_free(tree);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[2], "commit"); return; }
    char sha[8], out[80];
    git_oid_tostr(sha, sizeof sha, &commit_oid);
    snprintf(out, sizeof out, "[%s] %s", sha, msg);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* Distinct top-level file names across index + HEAD tree. */
typedef struct { char (*n)[256]; size_t c, cap; } nameset;

static void ns_add(nameset *s, const char *name) {
    for (size_t i = 0; i < s->c; i++)
        if (strcmp(s->n[i], name) == 0) return;
    if (s->c == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->n = realloc(s->n, s->cap * sizeof *s->n);
        if (!s->n) mvx_fatal("out of memory in git status");
    }
    snprintf(s->n[s->c++], 256, "%s", name);
}

static void split_top(const char *path, char *top, size_t cap) {
    const char *slash = strchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : strlen(path);
    if (n >= cap) n = cap - 1;
    memcpy(top, path, n);
    top[n] = '\0';
}

/* Whether a top-level name is an MV file (its records are stored canonically,
   so status/commit read them through the driver) rather than a plain file or
   directory git tracks verbatim.  An on-disk directory is an MV file only if it
   carries a dictionary control (<name>.DICT/%FILE%) — a proper DIR file always
   has one; a plain directory (server/, test/, a .DICT itself, a submodule) does
   not.  A name with no on-disk directory but tracked in the index is an
   LMDB-backed hash file, so it is an MV file.  Pure stat(), run in the account
   (the engine has chdir'd there): it never opens the store, so status on a
   directory-only account cannot conjure an mvxdata.lmdb. */
static int is_mv_file(const char *name) {
    struct stat sb;
    if (stat(name, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        char ctl[600];
        snprintf(ctl, sizeof ctl, "%s.DICT/%%FILE%%", name);
        return stat(ctl, &sb) == 0;
    }
    return 1;   /* no on-disk directory ⇒ LMDB-backed */
}

/* GITSTATUS(repo, out) — real-git short status across tracked files. */
void mvx_sub_GITSTATUS(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[1], "open"); return; }
    git_tree *ht = head_tree(repo);
    sbuf s = {0, 0, 0};

    /* staged: HEAD tree vs index */
    git_diff *sd = NULL;
    if (git_diff_tree_to_index(&sd, repo, ht, index, NULL) == 0) {
        size_t n = git_diff_num_deltas(sd);
        for (size_t i = 0; i < n; i++) {
            const git_diff_delta *d = git_diff_get_delta(sd, i);
            char line[700];
            snprintf(line, sizeof line, "%c  %s",
                     git_diff_status_char(d->status), d->new_file.path);
            sb_line(&s, line);
        }
        git_diff_free(sd);
    }

    /* working (records) vs index: modified / untracked / deleted */
    nameset files = {0, 0, 0};
    size_t ic = git_index_entrycount(index);
    for (size_t i = 0; i < ic; i++) {
        const git_index_entry *e = git_index_get_byindex(index, i);
        if (e->mode == GIT_FILEMODE_COMMIT) continue;   /* submodule gitlink */
        char top[256];
        split_top(e->path, top, sizeof top);
        ns_add(&files, top);
    }
    for (size_t f = 0; f < files.c; f++) {
        const char *fn = files.n[f];
        if (!is_mv_file(fn)) continue;    /* plain path — git diffs it, below */
        mv_value fvar, id, rec;
        mv_init(&fvar); mv_init(&id); mv_init(&rec);
        if (open_named(ctx, fn, &fvar)) {
            mvx_select(ctx, &fvar);
            while (mvx_readnext(ctx, &id)) {
                if (!mvx_read(ctx, &rec, &fvar, &id, 0)) continue;
                char idb[256], nb[40], path[600];
                arg_str(&id, idb, sizeof idb);
                snprintf(path, sizeof path, "%s/%s", fn, idb);
                const git_index_entry *entry =
                    git_index_get_bypath(index, path, 0);
                const char *cp;
                int64_t clen = mv_val_chars(&rec, nb, sizeof nb, &cp);
                git_oid woid;
                record_oid(cp, clen, &woid);
                if (!entry) {
                    if (ignored(fn, path)) continue;   /* not untracked */
                    char line[700];
                    snprintf(line, sizeof line, "?? %s", path);
                    sb_line(&s, line);
                } else if (!git_oid_equal(&entry->id, &woid)) {
                    char line[700];
                    snprintf(line, sizeof line, " M %s", path);
                    sb_line(&s, line);
                }
            }
        }
        mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
    }
    /* deleted: index entry whose record no longer exists */
    for (size_t i = 0; i < ic; i++) {
        const git_index_entry *e = git_index_get_byindex(index, i);
        if (e->mode == GIT_FILEMODE_COMMIT) continue;   /* submodule gitlink */
        char top[256];
        split_top(e->path, top, sizeof top);
        if (!is_mv_file(top)) continue;   /* plain path — git tracks deletions */
        const char *recid = strchr(e->path, '/');
        recid = recid ? recid + 1 : e->path;
        mv_value fvar, id, rec;
        mv_init(&fvar); mv_init(&id); mv_init(&rec);
        int isrec = 0, gone = 0;
        if (open_named(ctx, top, &fvar)) {
            isrec = 1;                      /* a real MV file; the entry is a record */
            mv_set_str(&id, recid, (int64_t)strlen(recid));
            gone = !mvx_read(ctx, &rec, &fvar, &id, 0);
        }
        mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
        /* only a record of an MV file can be reported deleted; a plain file
           (README, .gitmodules, …) that isn't an MV record is left to git */
        if (isrec && gone) {
            char line[700];
            snprintf(line, sizeof line, " D %s", e->path);
            sb_line(&s, line);
        }
    }
    free(files.n);

    /* plain (non-MV) paths: diff the working tree against the index for tracked
       files exactly as git would — modified or deleted — skipping MV files,
       whose records are diffed above (their git-native working bytes differ
       from the canonical staged blob, so git's own diff would mis-report them).
       Untracked files are left out: the record-git model reports through its
       own GITIGNORE, not the working tree at large. */
    git_diff *wd = NULL;
    git_diff_options wo = GIT_DIFF_OPTIONS_INIT;
    if (git_diff_index_to_workdir(&wd, repo, index, &wo) == 0) {
        size_t n = git_diff_num_deltas(wd);
        for (size_t i = 0; i < n; i++) {
            const git_diff_delta *d = git_diff_get_delta(wd, i);
            char top[256];
            split_top(d->new_file.path, top, sizeof top);
            if (is_mv_file(top)) continue;
            char line[700];
            snprintf(line, sizeof line, " %c %s",
                     git_diff_status_char(d->status), d->new_file.path);
            sb_line(&s, line);
        }
        git_diff_free(wd);
    }

    if (ht) git_tree_free(ht);
    git_index_free(index);
    git_repository_free(repo);
    sb_out(&s, argv[1], "nothing to commit, working tree clean");
}

/* GITLOG(repo, count, out) */
void mvx_sub_GITLOG(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], cnt[32];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], cnt, sizeof cnt);
    int64_t want = atoll(cnt);
    if (want <= 0) want = 20;
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) {
        mv_set_str(argv[2], "no history", 10);
        return;
    }
    git_revwalk *w = NULL;
    sbuf s = {0, 0, 0};
    if (git_revwalk_new(&w, repo) == 0 && git_revwalk_push_head(w) == 0) {
        git_oid oid;
        int64_t seen = 0;
        while (seen < want && git_revwalk_next(&oid, w) == 0) {
            git_commit *c = NULL;
            if (git_commit_lookup(&c, repo, &oid) != 0) break;
            char sha[8];
            git_oid_tostr(sha, sizeof sha, &oid);
            const char *sum = git_commit_summary(c);
            char line[4200];
            snprintf(line, sizeof line, "%s %s", sha, sum ? sum : "");
            sb_line(&s, line);
            git_commit_free(c);
            seen++;
        }
    }
    if (w) git_revwalk_free(w);
    git_repository_free(repo);
    sb_out(&s, argv[2], "no history");
}

/* diff line callback: accumulate +/-/space lines into an sbuf. */
static int diff_line_cb(const git_diff_delta *d, const git_diff_hunk *h,
                        const git_diff_line *l, void *payload) {
    (void)d; (void)h;
    sbuf *s = payload;
    char pfx = l->origin;               /* '+', '-', ' ', 'F','H' */
    /* records never end in a newline; drop the "no newline" markers */
    if (pfx == GIT_DIFF_LINE_CONTEXT_EOFNL ||
        pfx == GIT_DIFF_LINE_ADD_EOFNL ||
        pfx == GIT_DIFF_LINE_DEL_EOFNL)
        return 0;
    char line[4200];
    int n;
    if (pfx == '+' || pfx == '-' || pfx == ' ')
        n = snprintf(line, sizeof line, "%c%.*s", pfx,
                     (int)l->content_len, l->content);
    else
        n = snprintf(line, sizeof line, "%.*s",
                     (int)l->content_len, l->content);
    /* content includes its own newline; strip for line-per-attribute */
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    sb_line(s, line);
    return 0;
}

/* GITDIFF(repo, file, out) — unstaged record changes (working vs index)
   as a unified diff.  file "" diffs all tracked files. */
void mvx_sub_GITDIFF(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 3) return;
    ensure_init();
    char rp[4096], only[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], only, sizeof only);
    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[2], "open"); return; }
    sbuf s = {0, 0, 0};

    size_t ic = git_index_entrycount(index);
    for (size_t i = 0; i < ic; i++) {
        const git_index_entry *e = git_index_get_byindex(index, i);
        char top[256];
        split_top(e->path, top, sizeof top);
        if (only[0] && strcmp(only, top) != 0) continue;
        const char *recid = strchr(e->path, '/');
        recid = recid ? recid + 1 : e->path;

        mv_value fvar, id, rec;
        mv_init(&fvar); mv_init(&id); mv_init(&rec);
        int have = 0;
        if (open_named(ctx, top, &fvar)) {
            mv_set_str(&id, recid, (int64_t)strlen(recid));
            have = mvx_read(ctx, &rec, &fvar, &id, 0);
        }
        git_blob *old = NULL;
        if (git_blob_lookup(&old, repo, &e->id) == 0) {
            char nb[40];
            const char *cp = "";
            int64_t clen = 0, bl = 0;
            char *buf = NULL;
            if (have) {
                clen = mv_val_chars(&rec, nb, sizeof nb, &cp);
                buf = xlate(cp, clen, (char)0xFE, '\n', &bl);
            }
            git_oid woid;
            int changed = 1;
            if (git_odb_hash(&woid, buf ? buf : "", (size_t)bl,
                             GIT_OBJECT_BLOB) == 0)
                changed = !git_oid_equal(&woid, &e->id);
            if (changed) {
                char hdr[700];
                snprintf(hdr, sizeof hdr, "diff %s", e->path);
                sb_line(&s, hdr);
                git_diff_blob_to_buffer(old, e->path, buf, (size_t)bl,
                                        e->path, NULL, NULL, NULL, NULL,
                                        diff_line_cb, &s);
            }
            free(buf);
            git_blob_free(old);
        }
        mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
    }
    git_index_free(index);
    git_repository_free(repo);
    sb_out(&s, argv[2], "no changes");
}

/* GITSHOW(repo, file, record, out) — committed content of a record. */
void mvx_sub_GITSHOW(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 4) return;
    ensure_init();
    char rp[4096], fn[256], recid[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], fn, sizeof fn);
    arg_str(argv[2], recid, sizeof recid);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[3], "open"); return; }
    git_tree *t = head_tree(repo);
    char path[600];
    snprintf(path, sizeof path, "%s/%s", fn, recid);
    git_tree_entry *te = NULL;
    git_blob *blob = NULL;
    if (t && git_tree_entry_bypath(&te, t, path) == 0 &&
        git_blob_lookup(&blob, repo, git_tree_entry_id(te)) == 0) {
        const char *cp = git_blob_rawcontent(blob);
        int64_t clen = (int64_t)git_blob_rawsize(blob), rl;
        char *r = xlate(cp, clen, '\n', (char)0xFE, &rl);
        mv_set_str(argv[3], r, rl);
        free(r);
        git_blob_free(blob);
    } else {
        mv_set_str(argv[3], "", 0);
    }
    if (te) git_tree_entry_free(te);
    if (t) git_tree_free(t);
    git_repository_free(repo);
}

/* Write one tracked subtree's records back into its MVX file, deleting
   records absent from the tree. */
/* Map a %FILE% control's content — the open form DIR/hash, or a native
   FILE<VM>type — to a CREATE-FILE type: "DIR" for a directory file, "" for the
   account's default hash backend. */
static void control_type(const char *c, int64_t cl, char *out, size_t cap) {
    out[0] = '\0';
    if (!c) return;
    while (cl > 0 && (c[cl - 1] == '\n' || c[cl - 1] == '\r' ||
                      c[cl - 1] == ' ' || c[cl - 1] == '\t'))
        cl--;
    if (cl == 3 && strncasecmp(c, "DIR", 3) == 0) { snprintf(out, cap, "DIR"); return; }
    if (cl == 4 && strncasecmp(c, "hash", 4) == 0) return;   /* default hash */
    if (cl >= 5 && memcmp(c, "FILE", 4) == 0 && (unsigned char)c[4] == 0xFD) {
        const char *t = c + 5, *end = c + cl;
        const char *m2 = memchr(t, 0xFD, (size_t)(end - t));
        const char *te = m2 ? m2 : end;
        if (te - t == 3 && strncasecmp(t, "dir", 3) == 0) snprintf(out, cap, "DIR");
    }
}

/* The CREATE-FILE type of the file whose dictionary control lives at
   `<base>.DICT/%FILE%` in `head` (DIR, or "" for the default hash backend). */
static void file_type_of(git_repository *repo, git_tree *head, const char *base,
                         char *out, size_t cap) {
    out[0] = '\0';
    char path[600];
    snprintf(path, sizeof path, "%s.DICT/%%FILE%%", base);
    git_tree_entry *te = NULL;
    if (git_tree_entry_bypath(&te, head, path) != 0) return;
    git_blob *b = NULL;
    if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) == 0) {
        control_type(git_blob_rawcontent(b), (int64_t)git_blob_rawsize(b),
                     out, cap);
        git_blob_free(b);
    }
    git_tree_entry_free(te);
}

static void materialize_file(mvx_ctx *ctx, git_repository *repo, git_tree *head,
                             git_tree *subtree, const char *fn,
                             int64_t *nw, int64_t *nd) {
    mv_value fvar, id, rec;
    mv_init(&fvar); mv_init(&id); mv_init(&rec);
    size_t ln = strlen(fn);
    int is_dict = ln > 5 && strcmp(fn + ln - 5, ".DICT") == 0;
    if (fn[0]) {
        /* Create the file with the backend its %FILE% names (a fresh clone);
           a no-op when it already exists (a branch switch).  The dictionary
           tree names the same base file, so an empty file that has only a
           dictionary in git still gets created. */
        char base[300], type[64];
        if (is_dict) snprintf(base, sizeof base, "%.*s", (int)(ln - 5), fn);
        else         snprintf(base, sizeof base, "%s", fn);
        file_type_of(repo, head, base, type, sizeof type);
        mv_value spec;
        mv_init(&spec);
        mv_set_str(&spec, base, (int64_t)strlen(base));
        if (type[0]) {
            mv_value tv;
            mv_init(&tv);
            mv_set_str(&tv, type, (int64_t)strlen(type));
            mvx_createfile(ctx, &spec, &tv);
            mv_clear(&tv);
        } else {
            mvx_createfile(ctx, &spec, NULL);
        }
        mv_clear(&spec);
    }
    if (!open_named(ctx, fn, &fvar)) {
        mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
        return;
    }
    char (*seen)[256] = NULL;
    size_t ns = 0, cap = 0;
    size_t cnt = git_tree_entrycount(subtree);
    for (size_t i = 0; i < cnt; i++) {
        const git_tree_entry *te = git_tree_entry_byindex(subtree, i);
        const char *name = git_tree_entry_name(te);
        /* The %FILE% control was written natively by createfile; keep it (and
           mark it seen so the reconcile pass below does not delete it) rather
           than overwrite with the open DIR/hash form. */
        if (is_dict && strcmp(name, "%FILE%") == 0) {
            if (ns == cap) { cap = cap ? cap * 2 : 64;
                seen = realloc(seen, cap * sizeof *seen);
                if (!seen) mvx_fatal("out of memory in checkout"); }
            snprintf(seen[ns++], 256, "%s", name);
            continue;
        }
        git_blob *blob = NULL;
        if (git_blob_lookup(&blob, repo, git_tree_entry_id(te)) != 0)
            continue;
        const char *cp = git_blob_rawcontent(blob);
        int64_t clen = (int64_t)git_blob_rawsize(blob), rl;
        char *r = xlate(cp, clen, '\n', (char)0xFE, &rl);
        mv_set_str(&rec, r, rl);
        free(r);
        mv_set_str(&id, name, (int64_t)strlen(name));
        mvx_write(ctx, &rec, &fvar, &id, 0, 0);
        (*nw)++;
        if (ns == cap) { cap = cap ? cap * 2 : 64;
            seen = realloc(seen, cap * sizeof *seen);
            if (!seen) mvx_fatal("out of memory in checkout"); }
        snprintf(seen[ns++], 256, "%s", name);
        git_blob_free(blob);
    }
    mvx_select(ctx, &fvar);
    mv_value dl;
    mv_init(&dl);
    while (mvx_readnext(ctx, &dl)) {
        char idb[256];
        arg_str(&dl, idb, sizeof idb);
        int found = 0;
        for (size_t i = 0; i < ns; i++)
            if (strcmp(seen[i], idb) == 0) { found = 1; break; }
        if (!found) { mvx_delete_rec(ctx, &fvar, &dl); (*nd)++; }
    }
    mv_clear(&dl);
    free(seen);
    mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
}

/* Whether a top-level tree name is an MV file (its records go to the backend)
   rather than a plain directory git tracks verbatim: a data file `<name>` has a
   `<name>.DICT/%FILE%` control; a dictionary `<name>.DICT` has its own
   `%FILE%`. */
static int tree_is_mv_file(git_tree *head, const char *name) {
    char path[600];
    size_t ln = strlen(name);
    if (ln > 5 && strcmp(name + ln - 5, ".DICT") == 0)
        snprintf(path, sizeof path, "%s/%%FILE%%", name);
    else
        snprintf(path, sizeof path, "%s.DICT/%%FILE%%", name);
    git_tree_entry *te = NULL;
    if (git_tree_entry_bypath(&te, head, path) == 0) {
        git_tree_entry_free(te);
        return 1;
    }
    return 0;
}

/* Materialize the tracked MV files in a commit tree into their backends.  With
   `strict` (a fresh clone, where the files cannot be opened to tell an MV file
   from a plain directory) only trees that carry a dictionary control are
   materialised, so a plain directory (server/, test/, a submodule) is left to
   git.  Without it (a branch switch on an existing account) every top-level tree
   is materialised, as before — a file added without its dictionary still
   round-trips. */
static void materialize_tree_x(mvx_ctx *ctx, git_repository *repo,
                               git_tree *tree, int strict,
                               int64_t *nw, int64_t *nd) {
    size_t n = git_tree_entrycount(tree);
    for (size_t i = 0; i < n; i++) {
        const git_tree_entry *te = git_tree_entry_byindex(tree, i);
        if (git_tree_entry_type(te) != GIT_OBJECT_TREE) continue;
        const char *name = git_tree_entry_name(te);
        if (strict && !tree_is_mv_file(tree, name)) continue;
        git_tree *sub = NULL;
        if (git_tree_lookup(&sub, repo, git_tree_entry_id(te)) != 0)
            continue;
        materialize_file(ctx, repo, tree, sub, name, nw, nd);
        git_tree_free(sub);
    }
}

static void materialize_tree(mvx_ctx *ctx, git_repository *repo,
                             git_tree *tree, int64_t *nw, int64_t *nd) {
    materialize_tree_x(ctx, repo, tree, 0, nw, nd);   /* branch ops: all trees */
}

/* Reset the persistent index to a commit tree (so status is clean). */
static void sync_index(git_repository *repo, const char *rp,
                       git_tree *tree) {
    char ipath[4200];
    snprintf(ipath, sizeof ipath, "%s/index", rp);
    git_index *idx = NULL;
    if (git_index_open(&idx, ipath) == 0) {
        git_index_read_tree(idx, tree);
        git_index_write(idx);
        git_index_free(idx);
    }
}

/* Write a tree's blobs to disk under `prefix` (recursively): the plain files a
   checkout leaves on disk — README, scripts, a plain subdirectory, a submodule
   is skipped.  MV files never come here (they go to the backend). */
static void checkout_plain_tree(git_repository *repo, git_tree *tree,
                                const char *prefix) {
    size_t n = git_tree_entrycount(tree);
    for (size_t i = 0; i < n; i++) {
        const git_tree_entry *te = git_tree_entry_byindex(tree, i);
        const char *name = git_tree_entry_name(te);
        char path[4096];
        snprintf(path, sizeof path, "%s%s%s", prefix, prefix[0] ? "/" : "", name);
        git_object_t ty = git_tree_entry_type(te);
        if (ty == GIT_OBJECT_TREE) {
            git_tree *sub = NULL;
            if (git_tree_lookup(&sub, repo, git_tree_entry_id(te)) == 0) {
                mkdir(path, 0755);
                checkout_plain_tree(repo, sub, path);
                git_tree_free(sub);
            }
        } else if (ty == GIT_OBJECT_BLOB) {
            git_blob *b = NULL;
            if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) == 0) {
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(git_blob_rawcontent(b), 1,
                           (size_t)git_blob_rawsize(b), f);
                    fclose(f);
                    if (git_tree_entry_filemode(te) ==
                        GIT_FILEMODE_BLOB_EXECUTABLE)
                        chmod(path, 0755);
                }
                git_blob_free(b);
            }
        }
    }
}

/* Materialise a native account directly from the repo's HEAD tree — the clone
   path.  MV files (records + dictionaries) go straight to the backend in the
   native form; the account descriptor `.mv-account`/`.mvx` becomes `.mvx`; plain
   files land on disk.  The open form never touches disk, and no external adopt
   is run.  GITMATERIALIZE(repo, out) */
void mvx_sub_GITMATERIALIZE(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[1], "open"); return; }
    git_tree *tree = head_tree(repo);
    if (!tree) { git_repository_free(repo); mv_set_str(argv[1], "empty", 5); return; }

    int64_t nw = 0, nd = 0;
    materialize_tree_x(ctx, repo, tree, 1, &nw, &nd);   /* strict: MV files only */

    /* Everything that is not an MV file: the descriptor, and plain files. */
    size_t n = git_tree_entrycount(tree);
    for (size_t i = 0; i < n; i++) {
        const git_tree_entry *te = git_tree_entry_byindex(tree, i);
        const char *name = git_tree_entry_name(te);
        git_object_t ty = git_tree_entry_type(te);
        if (ty == GIT_OBJECT_TREE && tree_is_mv_file(tree, name)) continue;
        if (ty == GIT_OBJECT_BLOB &&
            (strcmp(name, ".mv-account") == 0 || strcmp(name, ".mvx") == 0)) {
            git_blob *b = NULL;                          /* descriptor -> .mvx */
            if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) == 0) {
                FILE *f = fopen(".mvx", "wb");
                if (f) { fwrite(git_blob_rawcontent(b), 1,
                                (size_t)git_blob_rawsize(b), f); fclose(f); }
                git_blob_free(b);
            }
            continue;
        }
        if (ty == GIT_OBJECT_TREE) {
            git_tree *sub = NULL;
            if (git_tree_lookup(&sub, repo, git_tree_entry_id(te)) == 0) {
                mkdir(name, 0755);
                checkout_plain_tree(repo, sub, name);
                git_tree_free(sub);
            }
        } else if (ty == GIT_OBJECT_BLOB) {
            git_blob *b = NULL;
            if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) == 0) {
                FILE *f = fopen(name, "wb");
                if (f) {
                    fwrite(git_blob_rawcontent(b), 1,
                           (size_t)git_blob_rawsize(b), f);
                    fclose(f);
                    if (git_tree_entry_filemode(te) ==
                        GIT_FILEMODE_BLOB_EXECUTABLE)
                        chmod(name, 0755);
                }
                git_blob_free(b);
            }
        }
    }

    sync_index(repo, rp, tree);
    git_tree_free(tree);
    git_repository_free(repo);
    char out[80];
    snprintf(out, sizeof out, "materialised %lld record(s)", (long long)nw);
    mv_set_str(argv[1], out, (int64_t)strlen(out));
}

/* GITBRANCH(repo, name, out) — list branches, or create one at HEAD. */
void mvx_sub_GITBRANCH(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], name[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], name, sizeof name);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }

    if (!name[0]) {                     /* list */
        sbuf s = {0, 0, 0};
        git_branch_iterator *it = NULL;
        git_reference *cur = NULL;
        const char *curname = NULL;
        if (git_repository_head(&cur, repo) == 0)
            git_branch_name(&curname, cur);
        if (git_branch_iterator_new(&it, repo, GIT_BRANCH_LOCAL) == 0) {
            git_reference *ref = NULL;
            git_branch_t bt;
            while (git_branch_next(&ref, &bt, it) == 0) {
                const char *bn = NULL;
                git_branch_name(&bn, ref);
                char line[300];
                snprintf(line, sizeof line, "%s %s",
                         (curname && bn && strcmp(curname, bn) == 0)
                             ? "*" : " ",
                         bn ? bn : "?");
                sb_line(&s, line);
                git_reference_free(ref);
            }
            git_branch_iterator_free(it);
        }
        if (cur) git_reference_free(cur);
        git_repository_free(repo);
        sb_out(&s, argv[2], "no branches");
        return;
    }

    git_object *head = NULL;
    git_commit *c = NULL;
    git_reference *br = NULL;
    int rc = git_revparse_single(&head, repo, "HEAD");
    if (rc == 0) rc = git_commit_lookup(&c, repo, git_object_id(head));
    if (rc == 0) rc = git_branch_create(&br, repo, name, c, 0);
    if (br) git_reference_free(br);
    if (c) git_commit_free(c);
    if (head) git_object_free(head);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[2], "branch"); return; }
    char out[300];
    snprintf(out, sizeof out, "created branch %s", name);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* GITCHECKOUT(repo, name, out) — switch to a branch and write its
   records into the hash files (our working tree). */
void mvx_sub_GITCHECKOUT(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 3) return;
    ensure_init();
    char rp[4096], name[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], name, sizeof name);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }
    git_reference *br = NULL;
    if (git_branch_lookup(&br, repo, name, GIT_BRANCH_LOCAL) != 0) {
        git_repository_free(repo);
        fail(argv[2], "no such branch");
        return;
    }
    git_repository_set_head(repo, git_reference_name(br));
    git_reference_free(br);
    git_tree *t = head_tree(repo);
    int64_t nw = 0, nd = 0;
    if (t) {
        materialize_tree(ctx, repo, t, &nw, &nd);
        sync_index(repo, rp, t);
        git_tree_free(t);
    }
    git_repository_free(repo);
    char out[300];
    snprintf(out, sizeof out,
             "switched to %s (%lld record(s) updated, %lld removed)",
             name, (long long)nw, (long long)nd);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* Commit a merged/cherry-picked index and materialize it. */
static void finish_merge(mvx_ctx *ctx, git_repository *repo,
                         const char *rp, git_index *mindex,
                         git_commit *ours, git_commit *theirs,
                         const char *msg, mv_value *out) {
    if (git_index_has_conflicts(mindex)) {
        sbuf s = {0, 0, 0};
        sb_line(&s, "CONFLICT — resolve these records, then GIT ADD "
                    "and GIT COMMIT:");
        git_index_conflict_iterator *ci = NULL;
        if (git_index_conflict_iterator_new(&ci, mindex) == 0) {
            const git_index_entry *a, *o, *th;
            while (git_index_conflict_next(&a, &o, &th, ci) == 0) {
                const char *p = th ? th->path : (o ? o->path :
                                (a ? a->path : "?"));
                char line[700];
                snprintf(line, sizeof line, "  %s", p);
                sb_line(&s, line);
            }
            git_index_conflict_iterator_free(ci);
        }
        sb_out(&s, out, "conflict");
        return;
    }
    git_oid tree_oid, commit_oid;
    git_tree *tree = NULL;
    git_signature *sig = NULL;
    int rc = git_index_write_tree_to(&tree_oid, mindex, repo);
    if (rc == 0) rc = git_tree_lookup(&tree, repo, &tree_oid);
    if (rc == 0 && git_signature_default(&sig, repo) != 0)
        rc = git_signature_now(&sig, "MVX", "mvx@localhost");
    if (rc == 0) {
        const git_commit *parents[2];
        int np = 1;
        parents[0] = ours;
        if (theirs) { parents[1] = theirs; np = 2; }
        rc = git_commit_create(&commit_oid, repo, "HEAD", sig, sig,
                               NULL, msg, tree, np, parents);
    }
    int64_t nw = 0, nd = 0;
    if (rc == 0 && tree) {
        materialize_tree(ctx, repo, tree, &nw, &nd);
        sync_index(repo, rp, tree);
    }
    if (sig) git_signature_free(sig);
    if (tree) git_tree_free(tree);
    if (rc != 0) { fail(out, "merge commit"); return; }
    char o[300];
    snprintf(o, sizeof o, "%s (%lld record(s) updated, %lld removed)",
             msg, (long long)nw, (long long)nd);
    mv_set_str(out, o, (int64_t)strlen(o));
}

/* GITMERGE(repo, branch, out) — 3-way merge a branch into HEAD. */
void mvx_sub_GITMERGE(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 3) return;
    ensure_init();
    char rp[4096], name[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], name, sizeof name);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }
    git_object *ho = NULL;
    git_commit *ours = NULL, *theirs = NULL;
    git_reference *br = NULL;
    git_index *mindex = NULL;
    int rc = git_revparse_single(&ho, repo, "HEAD");
    if (rc == 0) rc = git_commit_lookup(&ours, repo, git_object_id(ho));
    if (rc == 0) rc = git_branch_lookup(&br, repo, name, GIT_BRANCH_LOCAL);
    if (rc == 0)
        rc = git_commit_lookup(&theirs, repo,
                               git_reference_target(br));
    if (rc == 0)
        rc = git_merge_commits(&mindex, repo, ours, theirs, NULL);
    if (rc == 0) {
        char msg[300];
        snprintf(msg, sizeof msg, "Merge branch '%s'", name);
        finish_merge(ctx, repo, rp, mindex, ours, theirs, msg, argv[2]);
    } else {
        fail(argv[2], "merge");
    }
    if (mindex) git_index_free(mindex);
    if (br) git_reference_free(br);
    if (theirs) git_commit_free(theirs);
    if (ours) git_commit_free(ours);
    if (ho) git_object_free(ho);
    git_repository_free(repo);
}

/* GITCHERRYPICK(repo, commitish, out) — apply one commit onto HEAD. */
void mvx_sub_GITCHERRYPICK(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 3) return;
    ensure_init();
    char rp[4096], rev[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], rev, sizeof rev);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }
    git_object *ho = NULL, *co = NULL;
    git_commit *ours = NULL, *pick = NULL;
    git_index *mindex = NULL;
    int rc = git_revparse_single(&ho, repo, "HEAD");
    if (rc == 0) rc = git_commit_lookup(&ours, repo, git_object_id(ho));
    if (rc == 0) rc = git_revparse_single(&co, repo, rev);
    if (rc == 0) rc = git_commit_lookup(&pick, repo, git_object_id(co));
    if (rc == 0)
        rc = git_cherrypick_commit(&mindex, repo, pick, ours, 0, NULL);
    if (rc == 0) {
        const char *sum = git_commit_summary(pick);
        char msg[400];
        snprintf(msg, sizeof msg, "%s", sum ? sum : "cherry-pick");
        finish_merge(ctx, repo, rp, mindex, ours, NULL, msg, argv[2]);
    } else {
        fail(argv[2], "cherry-pick");
    }
    if (mindex) git_index_free(mindex);
    if (pick) git_commit_free(pick);
    if (ours) git_commit_free(ours);
    if (co) git_object_free(co);
    if (ho) git_object_free(ho);
    git_repository_free(repo);
}

/* GITRESTORE(repo, file, out) — write records back from HEAD, deleting
   records absent from the commit (git restore / checkout of the file). */
void mvx_sub_GITRESTORE(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 3) return;
    ensure_init();
    char rp[4096], fn[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], fn, sizeof fn);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }
    git_tree *t = head_tree(repo);
    git_tree_entry *sub = NULL;
    git_tree *subtree = NULL;
    if (!t || git_tree_entry_bypath(&sub, t, fn) != 0 ||
        git_tree_lookup(&subtree, repo, git_tree_entry_id(sub)) != 0) {
        if (sub) git_tree_entry_free(sub);
        if (t) git_tree_free(t);
        git_repository_free(repo);
        fail(argv[2], "no committed history for file");
        return;
    }
    mv_value fvar, id, rec;
    mv_init(&fvar); mv_init(&id); mv_init(&rec);
    int64_t nw = 0, nd = 0;
    char (*seen)[256] = NULL;
    size_t ns = 0, cap = 0;
    if (open_named(ctx, fn, &fvar)) {
        size_t cnt = git_tree_entrycount(subtree);
        for (size_t i = 0; i < cnt; i++) {
            const git_tree_entry *te = git_tree_entry_byindex(subtree, i);
            const char *name = git_tree_entry_name(te);
            git_blob *blob = NULL;
            if (git_blob_lookup(&blob, repo, git_tree_entry_id(te)) != 0)
                continue;
            const char *cp = git_blob_rawcontent(blob);
            int64_t clen = (int64_t)git_blob_rawsize(blob), rl;
            char *r = xlate(cp, clen, '\n', (char)0xFE, &rl);
            mv_set_str(&rec, r, rl);
            free(r);
            mv_set_str(&id, name, (int64_t)strlen(name));
            mvx_write(ctx, &rec, &fvar, &id, 0, 0);
            nw++;
            if (ns == cap) { cap = cap ? cap * 2 : 64;
                seen = realloc(seen, cap * sizeof *seen);
                if (!seen) mvx_fatal("out of memory in restore"); }
            snprintf(seen[ns++], 256, "%s", name);
            git_blob_free(blob);
        }
        mvx_select(ctx, &fvar);
        mv_value dl;
        mv_init(&dl);
        while (mvx_readnext(ctx, &dl)) {
            char idb[256];
            arg_str(&dl, idb, sizeof idb);
            int found = 0;
            for (size_t i = 0; i < ns; i++)
                if (strcmp(seen[i], idb) == 0) { found = 1; break; }
            if (!found) { mvx_delete_rec(ctx, &fvar, &dl); nd++; }
        }
        mv_clear(&dl);
    }
    free(seen);
    mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
    git_tree_entry_free(sub);
    git_tree_free(subtree);
    git_tree_free(t);
    git_repository_free(repo);
    char out[80];
    snprintf(out, sizeof out, "restored %lld record(s), %lld removed",
             (long long)nw, (long long)nd);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* --- plain-C API (#58) -------------------------------------------------
   Thin wrappers the mvx-git executable calls: build the mv_value argv the
   subroutine ABI expects, invoke the very same engine the GIT verb uses, and
   return the last (output) arg as a malloc'd @AM-separated string.  So the
   executable and the verb drive one mechanism — records straight to/from git
   objects, no export copy. */
#include "mvxgit.h"

typedef void (*sub_fn)(mvx_ctx *, int32_t, mv_value **);

static char *run_sub(sub_fn fn, mvx_ctx *ctx, const char **args, int n) {
    mv_value vals[8];
    mv_value *argv[8];
    for (int i = 0; i < n; i++) {
        mv_init(&vals[i]);
        mv_set_str(&vals[i], args[i], (int64_t)strlen(args[i]));
        argv[i] = &vals[i];
    }
    mv_init(&vals[n]);                   /* output slot */
    argv[n] = &vals[n];
    fn(ctx, n + 1, argv);
    char nb[40];
    const char *p;
    int64_t len = mv_val_chars(&vals[n], nb, sizeof nb, &p);
    char *r = malloc((size_t)len + 1);
    if (!r) mvx_fatal("out of memory in git");
    memcpy(r, p, (size_t)len);
    r[len] = '\0';
    for (int i = 0; i <= n; i++) mv_clear(&vals[i]);
    return r;
}

char *mvx_git_init(mvx_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITINIT, ctx, a, 1);
}
char *mvx_git_add(mvx_ctx *ctx, const char *repo, const char *file,
                  const char *id) {
    const char *a[] = {repo, file, id};
    return run_sub(mvx_sub_GITADD, ctx, a, 3);
}
char *mvx_git_addsub(mvx_ctx *ctx, const char *repo, const char *name) {
    const char *a[] = {repo, name};
    return run_sub(mvx_sub_GITADDSUB, ctx, a, 2);
}
char *mvx_git_adddisk(mvx_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITADDDISK, ctx, a, 1);
}
char *mvx_git_openform(mvx_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITOPENFORM, ctx, a, 1);
}
char *mvx_git_materialize(mvx_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITMATERIALIZE, ctx, a, 1);
}
char *mvx_git_rm(mvx_ctx *ctx, const char *repo, const char *file,
                 const char *id) {
    const char *a[] = {repo, file, id};
    return run_sub(mvx_sub_GITRM, ctx, a, 3);
}
char *mvx_git_commit(mvx_ctx *ctx, const char *repo, const char *msg) {
    const char *a[] = {repo, msg};
    return run_sub(mvx_sub_GITCOMMIT, ctx, a, 2);
}
char *mvx_git_status(mvx_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITSTATUS, ctx, a, 1);
}
char *mvx_git_log(mvx_ctx *ctx, const char *repo, const char *count) {
    const char *a[] = {repo, count};
    return run_sub(mvx_sub_GITLOG, ctx, a, 2);
}
char *mvx_git_diff(mvx_ctx *ctx, const char *repo, const char *file) {
    const char *a[] = {repo, file};
    return run_sub(mvx_sub_GITDIFF, ctx, a, 2);
}
char *mvx_git_show(mvx_ctx *ctx, const char *repo, const char *file,
                   const char *id) {
    const char *a[] = {repo, file, id};
    return run_sub(mvx_sub_GITSHOW, ctx, a, 3);
}
char *mvx_git_branch(mvx_ctx *ctx, const char *repo, const char *name) {
    const char *a[] = {repo, name};
    return run_sub(mvx_sub_GITBRANCH, ctx, a, 2);
}
char *mvx_git_checkout(mvx_ctx *ctx, const char *repo, const char *name) {
    const char *a[] = {repo, name};
    return run_sub(mvx_sub_GITCHECKOUT, ctx, a, 2);
}
char *mvx_git_merge(mvx_ctx *ctx, const char *repo, const char *branch) {
    const char *a[] = {repo, branch};
    return run_sub(mvx_sub_GITMERGE, ctx, a, 2);
}
char *mvx_git_cherrypick(mvx_ctx *ctx, const char *repo, const char *commit) {
    const char *a[] = {repo, commit};
    return run_sub(mvx_sub_GITCHERRYPICK, ctx, a, 2);
}
char *mvx_git_restore(mvx_ctx *ctx, const char *repo, const char *file) {
    const char *a[] = {repo, file};
    return run_sub(mvx_sub_GITRESTORE, ctx, a, 2);
}
