/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* gitcallcb.c — the UniData CallC git-object functions for the in-session GIT
 * verb (Model B).
 *
 * These are one-directional BASIC->C CallC entry points: the UniBasic GIT verb
 * drives the record loop (OPEN/SELECT/READNEXT/READ/WRITE on the current
 * session — no InterCall, no second license), and calls these only for the
 * libgit2 git-object work.  So C never calls back into UniBasic; there is no
 * BASIC->C->BASIC nesting to corrupt the run-machine level.  Each function is
 * pure libgit2 (repo/index/commit) — it opens no record session — and reuses
 * the shared engine (mvxgit.c: mv_git_init / mv_git_stageblob / mv_git_commit /
 * mv_git_log).  Output is written into the caller's pre-sized string and also
 * returned. */

#define _POSIX_C_SOURCE 200809L

#include "mvxgit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GITCB_OUTCAP (1024 * 1024)

static void put_out(char *out, char *r) {
    if (out) {
        size_t n = r ? strlen(r) : 0;
        if (n > GITCB_OUTCAP - 1) n = GITCB_OUTCAP - 1;
        if (r) memcpy(out, r, n);
        out[n] = '\0';
    }
    free(r);
}

static const char *rp(const char *repo) {
    return repo && repo[0] ? repo : ".git";
}

/* GITINIT(repo, out) — create the repository. */
char *GITINIT(char *repo, char *out) {
    mv_ctx *ctx = mv_ctx_create();
    put_out(out, mv_git_init(ctx, rp(repo)));
    mv_ctx_destroy(ctx);
    return out;
}

/* GITSTAGE(repo, file, id, record, out) — stage one record's blob at file/id.
   The blob is the record with attribute marks (@AM = 0xFE) turned into newlines,
   exactly as the engine stores records, so the git history is line-oriented and
   an mvx-git checkout round-trips.  The UniBasic verb has already READ the
   record on the current session and passes it here. */
char *GITSTAGE(char *repo, char *file, char *id, char *record, char *out) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", file ? file : "", id ? id : "");
    size_t n = record ? strlen(record) : 0;
    char *blob = malloc(n + 1);
    if (!blob) { put_out(out, NULL); return out; }
    for (size_t i = 0; i < n; i++)
        blob[i] = (unsigned char)record[i] == 0xFE ? '\n' : record[i];
    blob[n] = '\0';
    mv_ctx *ctx = mv_ctx_create();
    put_out(out, mv_git_stageblob(ctx, rp(repo), path, blob));
    mv_ctx_destroy(ctx);
    free(blob);
    return out;
}

/* GITSTAGEBLOB(repo, path, content, out) — stage a raw blob verbatim at `path`
   (the synthesised open-account controls: <file>.DICT/%FILE%, .mv-account). */
char *GITSTAGEBLOB(char *repo, char *path, char *content, char *out) {
    mv_ctx *ctx = mv_ctx_create();
    put_out(out, mv_git_stageblob(ctx, rp(repo), path ? path : "",
                                  content ? content : ""));
    mv_ctx_destroy(ctx);
    return out;
}

/* GITCOMMIT(repo, msg, out) — commit the staged index. */
char *GITCOMMIT(char *repo, char *msg, char *out) {
    mv_ctx *ctx = mv_ctx_create();
    put_out(out, mv_git_commit(ctx, rp(repo), msg ? msg : ""));
    mv_ctx_destroy(ctx);
    return out;
}

/* GITLOG(repo, count, out) — commit history. */
char *GITLOG(char *repo, char *count, char *out) {
    mv_ctx *ctx = mv_ctx_create();
    put_out(out, mv_git_log(ctx, rp(repo), count && count[0] ? count : "20"));
    mv_ctx_destroy(ctx);
    return out;
}
