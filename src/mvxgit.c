/* Native git operations for the git package, via libgit2.
 *
 * These are cataloged subroutines: each matches the MVX subroutine ABI
 * (ctx, argc, argv), so the BASIC GIT verb CALLs them like any other
 * subroutine, and the runtime CALL resolver loads this library from the
 * package's LIB/ on first use.  libgit2 is a dependency of this library
 * alone, not of the core runtime or of compiled programs.
 *
 * Structured arguments, no shell: the commit message cannot inject, and
 * git needs no external binary.  These are library calls, not exec, so
 * they run at any privilege tier.
 *
 * Local plumbing only (init, add, commit, status, log, diff).  Network
 * operations (clone/push/pull) need transport and credential handling
 * and are left to a later pass.
 */
#include "mvx_runtime.h"

#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- arg helpers ------------------------------------------------------- */

static void arg_str(const mv_value *v, char *out, size_t cap) {
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(v, nb, sizeof nb, &p);
    if ((size_t)n >= cap) n = (int64_t)cap - 1;
    memcpy(out, p, (size_t)n);
    out[n] = '\0';
}

static void ensure_init(void) {
    static int done;
    if (!done) {
        git_libgit2_init();
        done = 1;
    }
}

/* Report an error into the out value (last arg), prefixed "git: ". */
static void fail(mv_value *out, const char *what) {
    const git_error *e = git_error_last();
    char buf[512];
    snprintf(buf, sizeof buf, "git: %s: %s", what,
             e && e->message ? e->message : "failed");
    mv_set_str(out, buf, (int64_t)strlen(buf));
}

/* --- dynamic-string output builder ------------------------------------ */

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
    if (s->len) {
        char am = (char)0xFE;           /* attribute mark between lines */
        sb_put(s, &am, 1);
    }
    sb_put(s, line, strlen(line));
}

/* --- GITINIT(path, out) ------------------------------------------------ */
void mvx_sub_GITINIT(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 2) return;
    ensure_init();
    char path[4096];
    arg_str(argv[0], path, sizeof path);
    git_repository *repo = NULL;
    if (git_repository_init(&repo, path, 0) != 0) {
        fail(argv[1], "init");
        return;
    }
    git_repository_free(repo);
    mv_set_str(argv[1], "", 0);
}

/* --- GITADD(repo, pathspec, out) --------------------------------------- */
void mvx_sub_GITADD(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char path[4096], spec[1024];
    arg_str(argv[0], path, sizeof path);
    arg_str(argv[1], spec, sizeof spec);

    git_repository *repo = NULL;
    if (git_repository_open(&repo, path) != 0) {
        fail(argv[2], "open");
        return;
    }
    git_index *idx = NULL;
    int rc = git_repository_index(&idx, repo);
    if (rc == 0) {
        char *paths[] = {spec};
        git_strarray ps = {paths, 1};
        rc = git_index_add_all(idx, &ps, GIT_INDEX_ADD_DEFAULT, NULL, NULL);
        if (rc == 0) rc = git_index_write(idx);
    }
    if (idx) git_index_free(idx);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[2], "add"); return; }
    mv_set_str(argv[2], "", 0);
}

/* --- GITCOMMIT(repo, message, out) ------------------------------------- */
void mvx_sub_GITCOMMIT(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char path[4096];
    char msg[4096];
    arg_str(argv[0], path, sizeof path);
    arg_str(argv[1], msg, sizeof msg);

    git_repository *repo = NULL;
    if (git_repository_open(&repo, path) != 0) {
        fail(argv[2], "open");
        return;
    }

    git_index *idx = NULL;
    git_oid tree_oid, commit_oid;
    git_tree *tree = NULL;
    git_signature *sig = NULL;
    git_object *parent_obj = NULL;
    git_commit *parent = NULL;
    int rc = git_repository_index(&idx, repo);
    if (rc == 0) rc = git_index_write_tree(&tree_oid, idx);
    if (rc == 0) rc = git_tree_lookup(&tree, repo, &tree_oid);
    if (rc == 0 && git_signature_default(&sig, repo) != 0)
        rc = git_signature_now(&sig, "MVX", "mvx@localhost");

    const git_commit *parents[1];
    int nparents = 0;
    if (rc == 0 &&
        git_revparse_single(&parent_obj, repo, "HEAD") == 0) {
        if (git_commit_lookup(&parent, repo,
                              git_object_id(parent_obj)) == 0) {
            parents[0] = parent;
            nparents = 1;
        }
    }
    if (rc == 0)
        rc = git_commit_create(&commit_oid, repo, "HEAD", sig, sig,
                               NULL, msg, tree, nparents, parents);

    if (parent) git_commit_free(parent);
    if (parent_obj) git_object_free(parent_obj);
    if (sig) git_signature_free(sig);
    if (tree) git_tree_free(tree);
    if (idx) git_index_free(idx);
    git_repository_free(repo);

    if (rc != 0) { fail(argv[2], "commit"); return; }
    char sha[8];
    git_oid_tostr(sha, sizeof sha, &commit_oid);
    char out[64];
    snprintf(out, sizeof out, "committed %s", sha);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* --- GITSTATUS(repo, out) ---------------------------------------------- */
void mvx_sub_GITSTATUS(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 2) return;
    ensure_init();
    char path[4096];
    arg_str(argv[0], path, sizeof path);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, path) != 0) {
        fail(argv[1], "open");
        return;
    }
    git_status_list *list = NULL;
    git_status_options opt = GIT_STATUS_OPTIONS_INIT;
    opt.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED;
    sbuf s = {0, 0, 0};
    if (git_status_list_new(&list, repo, &opt) == 0) {
        size_t n = git_status_list_entrycount(list);
        for (size_t i = 0; i < n; i++) {
            const git_status_entry *e = git_status_byindex(list, i);
            const char *p = e->index_to_workdir
                ? e->index_to_workdir->new_file.path
                : (e->head_to_index
                       ? e->head_to_index->new_file.path : "?");
            const char *tag = "  ";
            unsigned st = e->status;
            if (st & GIT_STATUS_WT_NEW)             tag = "??";
            else if (st & GIT_STATUS_INDEX_NEW)     tag = "A ";
            else if (st & (GIT_STATUS_INDEX_MODIFIED |
                           GIT_STATUS_WT_MODIFIED))  tag = "M ";
            else if (st & (GIT_STATUS_INDEX_DELETED |
                           GIT_STATUS_WT_DELETED))   tag = "D ";
            char line[4200];
            snprintf(line, sizeof line, "%s%s", tag, p);
            sb_line(&s, line);
        }
        if (n == 0) sb_line(&s, "clean");
        git_status_list_free(list);
    }
    git_repository_free(repo);
    mv_set_str(argv[1], s.d ? s.d : "clean", s.d ? (int64_t)s.len : 5);
    free(s.d);
}

/* --- GITLOG(repo, count, out) ------------------------------------------ */
void mvx_sub_GITLOG(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char path[4096];
    arg_str(argv[0], path, sizeof path);
    int64_t want = 0;
    {
        char nb[40];
        const char *p;
        int64_t n = mv_val_chars(argv[1], nb, sizeof nb, &p);
        char tmp[32];
        if (n > 0 && (size_t)n < sizeof tmp) {
            memcpy(tmp, p, (size_t)n);
            tmp[n] = '\0';
            want = atoll(tmp);
        }
    }
    if (want <= 0) want = 10;

    git_repository *repo = NULL;
    if (git_repository_open(&repo, path) != 0) {
        fail(argv[2], "open");
        return;
    }
    git_revwalk *walk = NULL;
    sbuf s = {0, 0, 0};
    if (git_revwalk_new(&walk, repo) == 0 &&
        git_revwalk_push_head(walk) == 0) {
        git_oid oid;
        int64_t seen = 0;
        while (seen < want && git_revwalk_next(&oid, walk) == 0) {
            git_commit *c = NULL;
            if (git_commit_lookup(&c, repo, &oid) != 0) break;
            char sha[8];
            git_oid_tostr(sha, sizeof sha, &oid);
            const char *summary = git_commit_summary(c);
            char line[4200];
            snprintf(line, sizeof line, "%s %s", sha,
                     summary ? summary : "");
            sb_line(&s, line);
            git_commit_free(c);
            seen++;
        }
    }
    if (walk) git_revwalk_free(walk);
    git_repository_free(repo);
    if (!s.d) sb_line(&s, "no history");
    mv_set_str(argv[2], s.d, (int64_t)s.len);
    free(s.d);
}

/* --- direct record versioning ------------------------------------------
   GITSAVE / GITRESTORE version hash-file records IN PLACE: a record's
   bytes become a git blob directly (no export to a file), and a blob's
   bytes are written straight back to the record.  The hash file is
   git's source of truth; git objects live in a small bare repo.  Marks
   translate to newlines in the blob so git diffs are line-oriented, and
   back on restore. */

static char *xlate(const char *p, int64_t n, char from, char to,
                   int64_t *outn) {
    char *b = malloc(n ? (size_t)n : 1);
    if (!b) mvx_fatal("out of memory in git record translate");
    for (int64_t i = 0; i < n; i++) b[i] = p[i] == from ? to : p[i];
    *outn = n;
    return b;
}

/* Open the record-history repo, initialising a bare one if absent. */
static int rec_repo(git_repository **repo, const char *path) {
    if (git_repository_open(repo, path) == 0) return 0;
    return git_repository_init(repo, path, 1);      /* bare */
}

/* GITSAVE(repopath, file, message, out) — commit a file's records. */
void mvx_sub_GITSAVE(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 4) return;
    ensure_init();
    char rp[4096], fn[256], msg[4096];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], fn, sizeof fn);
    arg_str(argv[2], msg, sizeof msg);

    git_repository *repo = NULL;
    if (rec_repo(&repo, rp) != 0) {
        fail(argv[3], "open repo");
        return;
    }

    git_index *index = NULL;
    git_object *headobj = NULL;
    git_commit *parent = NULL;
    git_tree *ptree = NULL, *tree = NULL;
    git_signature *sig = NULL;
    git_oid tree_oid, commit_oid;
    int rc = git_index_new(&index);

    if (rc == 0 && git_revparse_single(&headobj, repo, "HEAD") == 0 &&
        git_commit_lookup(&parent, repo, git_object_id(headobj)) == 0 &&
        git_commit_tree(&ptree, parent) == 0)
        git_index_read_tree(index, ptree);

    if (rc == 0) git_index_remove_directory(index, fn, 0);

    /* stream records into blobs + index entries, directly */
    mv_value spec, fvar, id, rec;
    mv_init(&spec); mv_init(&fvar); mv_init(&id); mv_init(&rec);
    mv_set_str(&spec, fn, (int64_t)strlen(fn));
    int64_t n = 0;
    if (rc == 0 && mvx_open(ctx, NULL, &spec, &fvar)) {
        mvx_select(ctx, &fvar);
        while (mvx_readnext(ctx, &id)) {
            if (!mvx_read(ctx, &rec, &fvar, &id, 0)) continue;
            char idb[256], nb[40];
            arg_str(&id, idb, sizeof idb);
            const char *cp;
            int64_t clen = mv_val_chars(&rec, nb, sizeof nb, &cp);
            int64_t bl;
            char *blob = xlate(cp, clen, (char)0xFE, '\n', &bl);
            git_oid boid;
            if (git_blob_create_from_buffer(&boid, repo, blob,
                                            (size_t)bl) == 0) {
                git_index_entry e;
                memset(&e, 0, sizeof e);
                char pathbuf[600];
                snprintf(pathbuf, sizeof pathbuf, "%s/%s", fn, idb);
                e.path = pathbuf;
                e.mode = GIT_FILEMODE_BLOB;
                e.id = boid;
                git_index_add(index, &e);
                n++;
            }
            free(blob);
        }
    }
    mv_clear(&spec); mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);

    if (rc == 0) rc = git_index_write_tree_to(&tree_oid, index, repo);
    if (rc == 0) rc = git_tree_lookup(&tree, repo, &tree_oid);
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
    if (tree) git_tree_free(tree);
    if (ptree) git_tree_free(ptree);
    if (parent) git_commit_free(parent);
    if (headobj) git_object_free(headobj);
    if (index) git_index_free(index);
    git_repository_free(repo);

    if (rc != 0) { fail(argv[3], "save"); return; }
    char sha[8], out[80];
    git_oid_tostr(sha, sizeof sha, &commit_oid);
    snprintf(out, sizeof out, "saved %lld record(s), %s",
             (long long)n, sha);
    mv_set_str(argv[3], out, (int64_t)strlen(out));
}

/* GITRESTORE(repopath, file, out) — write a file's records back from
   the latest commit, deleting records absent from it. */
void mvx_sub_GITRESTORE(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 3) return;
    ensure_init();
    char rp[4096], fn[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], fn, sizeof fn);

    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) {
        fail(argv[2], "open repo");
        return;
    }
    git_object *headobj = NULL;
    git_commit *commit = NULL;
    git_tree *tree = NULL, *subtree = NULL;
    git_tree_entry *sub = NULL;
    int64_t nw = 0, nd = 0;

    int rc = git_revparse_single(&headobj, repo, "HEAD");
    if (rc == 0)
        rc = git_commit_lookup(&commit, repo, git_object_id(headobj));
    if (rc == 0) rc = git_commit_tree(&tree, commit);
    if (rc == 0) rc = git_tree_entry_bypath(&sub, tree, fn);
    if (rc == 0)
        rc = git_tree_lookup(&subtree, repo, git_tree_entry_id(sub));
    if (rc != 0) {
        if (headobj) git_object_free(headobj);
        if (commit) git_commit_free(commit);
        if (tree) git_tree_free(tree);
        git_repository_free(repo);
        fail(argv[2], "no saved history for file");
        return;
    }

    mv_value spec, fvar, id, rec;
    mv_init(&spec); mv_init(&fvar); mv_init(&id); mv_init(&rec);
    mv_set_str(&spec, fn, (int64_t)strlen(fn));
    mvx_createfile(ctx, &spec, NULL);
    if (!mvx_open(ctx, NULL, &spec, &fvar)) {
        mv_clear(&spec); mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
        git_tree_entry_free(sub); git_tree_free(subtree);
        git_tree_free(tree); git_commit_free(commit);
        git_object_free(headobj); git_repository_free(repo);
        fail(argv[2], "cannot open file");
        return;
    }

    /* collect ids present in git, writing each record back */
    char (*seen)[256] = NULL;
    size_t nseen = 0, seencap = 0;
    size_t cnt = git_tree_entrycount(subtree);
    for (size_t i = 0; i < cnt; i++) {
        const git_tree_entry *te = git_tree_entry_byindex(subtree, i);
        const char *name = git_tree_entry_name(te);
        git_blob *blob = NULL;
        if (git_blob_lookup(&blob, repo, git_tree_entry_id(te)) != 0)
            continue;
        const char *cp = git_blob_rawcontent(blob);
        int64_t clen = (int64_t)git_blob_rawsize(blob);
        int64_t rl;
        char *recbuf = xlate(cp, clen, '\n', (char)0xFE, &rl);
        mv_set_str(&rec, recbuf, rl);
        free(recbuf);
        mv_set_str(&id, name, (int64_t)strlen(name));
        mvx_write(ctx, &rec, &fvar, &id, 0);
        nw++;
        if (nseen == seencap) {
            seencap = seencap ? seencap * 2 : 64;
            seen = realloc(seen, seencap * sizeof *seen);
            if (!seen) mvx_fatal("out of memory in GITRESTORE");
        }
        snprintf(seen[nseen++], 256, "%s", name);
        git_blob_free(blob);
    }

    /* delete records no longer in git */
    mvx_select(ctx, &fvar);
    mv_value delid;
    mv_init(&delid);
    while (mvx_readnext(ctx, &delid)) {
        char idb[256];
        arg_str(&delid, idb, sizeof idb);
        int found = 0;
        for (size_t i = 0; i < nseen; i++)
            if (strcmp(seen[i], idb) == 0) { found = 1; break; }
        if (!found) { mvx_delete_rec(ctx, &fvar, &delid); nd++; }
    }
    mv_clear(&delid);
    free(seen);

    mv_clear(&spec); mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
    git_tree_entry_free(sub);
    git_tree_free(subtree);
    git_tree_free(tree);
    git_commit_free(commit);
    git_object_free(headobj);
    git_repository_free(repo);

    char out[80];
    snprintf(out, sizeof out, "restored %lld record(s), %lld removed",
             (long long)nw, (long long)nd);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* --- GITDIFF(repo, out) ------------------------------------------------ */
void mvx_sub_GITDIFF(mvx_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 2) return;
    ensure_init();
    char path[4096];
    arg_str(argv[0], path, sizeof path);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, path) != 0) {
        fail(argv[1], "open");
        return;
    }
    git_diff *diff = NULL;
    sbuf s = {0, 0, 0};
    if (git_diff_index_to_workdir(&diff, repo, NULL, NULL) == 0) {
        size_t n = git_diff_num_deltas(diff);
        for (size_t i = 0; i < n; i++) {
            const git_diff_delta *d = git_diff_get_delta(diff, i);
            char line[4200];
            snprintf(line, sizeof line, "%c %s",
                     git_diff_status_char(d->status), d->new_file.path);
            sb_line(&s, line);
        }
        if (n == 0) sb_line(&s, "no changes");
        git_diff_free(diff);
    }
    git_repository_free(repo);
    mv_set_str(argv[1], s.d ? s.d : "no changes", s.d ? (int64_t)s.len : 10);
    free(s.d);
}
