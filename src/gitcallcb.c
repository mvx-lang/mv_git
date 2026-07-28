/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* gitcallcb.c — the UniData CallC git-object functions for the in-session GIT
 * verb (Model B).
 *
 * One-directional BASIC->C CallC entry points: the UniBasic GIT verb drives the
 * record loop on the CURRENT session (no InterCall, no second licence) and
 * calls these only for the libgit2 work; C never calls back into BASIC.
 *
 * No output-buffer argument.  UniData's CallC marshals both string arguments
 * and the return value with strlen, so any un-NUL-terminated buffer (a
 * SPACE(n)) segfaults it by running off the end into unmapped memory.  Instead
 * these take only their inputs, return a static "" (NUL-terminated), and surface
 * their @AM output through the file <repo>/gitmsg, which the verb OSREADs.
 *
 * Staging is BATCHED (mv_git_batch_*): open the repo+index once, accumulate
 * blobs in memory across GITSTAGE calls, write once at GITCOMMIT — O(n). */

#define _POSIX_C_SOURCE 200809L

#include "mvxgit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *rp(const char *repo) {
    return repo && repo[0] ? repo : ".git";
}

/* Write the engine's @AM-separated `r` to <repo>/gitmsg (the verb OSREADs it),
   free `r`, and return a NUL-terminated static string for CallC to marshal. */
static char *emit(const char *repo, char *r) {
    char path[1300];
    snprintf(path, sizeof path, "%s/gitmsg", rp(repo));
    FILE *f = fopen(path, "wb");
    if (f) { if (r) fwrite(r, 1, strlen(r), f); fclose(f); }
    free(r);
    return "";
}

/* GITINIT(repo) — create the repository. */
char *GITINIT(char *repo) {
    mv_git_batch_end();                 /* discard any stale open batch */
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_init(ctx, rp(repo));
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}

/* GITSTAGE(repo, file, id, record) — stage one record at file/id into the
   batched index, @AM (0xFE) marks -> newlines.  The verb has already READ it. */
char *GITSTAGE(char *repo, char *file, char *id, char *record) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", file ? file : "", id ? id : "");
    mv_git_batch_begin(rp(repo));       /* idempotent */
    mv_git_batch_add(path, record ? record : "",
                     record ? (int64_t)strlen(record) : 0, 1);
    return "";
}

/* GITSTAGEBLOB(repo, path, content) — stage a raw blob verbatim (the open-
   account controls: <file>.DICT/%FILE%, .mv-account). */
char *GITSTAGEBLOB(char *repo, char *path, char *content) {
    mv_git_batch_begin(rp(repo));
    mv_git_batch_add(path ? path : "", content ? content : "",
                     content ? (int64_t)strlen(content) : 0, 0);
    return "";
}

/* GITCOMMIT(repo, msg) — flush the batched index, then commit. */
char *GITCOMMIT(char *repo, char *msg) {
    mv_git_batch_end();                 /* write the accumulated index to disk */
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_commit(ctx, rp(repo), msg ? msg : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}

/* GITLOG(repo, count) — commit history. */
char *GITLOG(char *repo, char *count) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_log(ctx, rp(repo), count && count[0] ? count : "20");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}

/* --- checkout side (git -> UniData account) -------------------------------- */

/* GITFILES(repo) — every blob path in HEAD, @AM-separated, via <repo>/gitmsg
   (the verb OSREADs it and drives CREATE.FILE + WRITE on the current session). */
char *GITFILES(char *repo) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_headfiles(ctx, rp(repo));
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}

/* GITCAT(repo, path) — the committed record at `path` (attribute marks
   restored) written to <repo>/gitcat, which the verb OSREADs and WRITEs.  A
   separate file from gitmsg since the content is a raw record (marks/binary). */
char *GITCAT(char *repo, char *path) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_catpath(ctx, rp(repo), path ? path : "");
    mv_ctx_destroy(ctx);
    char p[1300];
    snprintf(p, sizeof p, "%s/gitcat", rp(repo));
    FILE *f = fopen(p, "wb");
    if (f) { if (r) fwrite(r, 1, strlen(r), f); fclose(f); }
    free(r);
    return "";
}
