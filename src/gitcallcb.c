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

static const char *rp(const char *repo) {
    return repo && repo[0] ? repo : ".git";
}

/* Surface the engine's @AM-separated output `r` to the UniBasic verb.  CallC's
   string return / OUT-argument copy-back does not marshal reliably (genfunc
   emits `U_retbuf = fn(...)`, and a plain memcpy into the OUT buffer does not
   update the UniData string length), so the reliable channel is a small file
   `<repo>/gitmsg` that the verb OSREADs — input args (repo, records) marshal
   fine, only the C->BASIC text does not.  Frees `r`. */
static void emit(const char *repo, char *out, char *r) {
    size_t n = r ? strlen(r) : 0;
    if (out) {                          /* also fill OUT, harmless if unused */
        size_t m = n > GITCB_OUTCAP - 1 ? GITCB_OUTCAP - 1 : n;
        if (r) memcpy(out, r, m);
        out[m] = '\0';
    }
    char path[1300];
    snprintf(path, sizeof path, "%s/gitmsg", rp(repo));
    FILE *f = fopen(path, "wb");
    if (f) { if (r) fwrite(r, 1, n, f); fclose(f); }
    free(r);
}

/* GITINIT(repo, out) — create the repository. */
char *GITINIT(char *repo, char *out) {
    mv_ctx *ctx = mv_ctx_create();
    emit(repo, out, mv_git_init(ctx, rp(repo)));
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
    free(mv_git_stageblob(ctx, rp(repo), path, blob));  /* per-record: no msg */
    mv_ctx_destroy(ctx);
    free(blob);
    (void)out;
    return out;
}

/* GITSTAGEBLOB(repo, path, content, out) — stage a raw blob verbatim at `path`
   (the synthesised open-account controls: <file>.DICT/%FILE%, .mv-account). */
char *GITSTAGEBLOB(char *repo, char *path, char *content, char *out) {
    mv_ctx *ctx = mv_ctx_create();
    free(mv_git_stageblob(ctx, rp(repo), path ? path : "",
                          content ? content : ""));
    mv_ctx_destroy(ctx);
    (void)out;
    return out;
}

/* GITCOMMIT(repo, msg, out) — commit the staged index. */
char *GITCOMMIT(char *repo, char *msg, char *out) {
    mv_ctx *ctx = mv_ctx_create();
    emit(repo, out, mv_git_commit(ctx, rp(repo), msg ? msg : ""));
    mv_ctx_destroy(ctx);
    return out;
}

/* GITLOG(repo, count, out) — commit history. */
char *GITLOG(char *repo, char *count, char *out) {
    mv_ctx *ctx = mv_ctx_create();
    emit(repo, out, mv_git_log(ctx, rp(repo), count && count[0] ? count : "20"));
    mv_ctx_destroy(ctx);
    return out;
}
