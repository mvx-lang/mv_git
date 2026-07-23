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
