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

/* GITSTAGEBLOB(repo, path, content) — stage a raw blob (the open-account
   controls: <file>.DICT/%FILE%, .mv-account).  For a hash file's %FILE% control,
   a modulo is only a suggested default: keep an already-committed one STICKY so
   this account's resize does not overwrite the shipped default and ripple out to
   other clones — so preserve HEAD's control and only take the fresh (live)
   modulo for a file with none committed yet. */
char *GITSTAGEBLOB(char *repo, char *path, char *content) {
    const char *p = path ? path : "";
    const char *use = content ? content : "";
    char committed[128];
    size_t plen = strlen(p);
    const char *suf = ".DICT/%FILE%";
    size_t sl = strlen(suf);
    if (plen > sl && strcmp(p + plen - sl, suf) == 0 &&
        strncmp(use, "hash", 4) == 0) {
        char base[512];
        snprintf(base, sizeof base, "%.*s", (int)(plen - sl), p);
        if (mv_git_committed_control(rp(repo), base, committed,
                                     sizeof committed) >= 0 &&
            strncmp(committed, "hash", 4) == 0)
            use = committed;
    }
    mv_git_batch_begin(rp(repo));
    mv_git_batch_add(p, use, (int64_t)strlen(use), 0);
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

/* GITBRANCH(repo, name) — list branches (name empty) or create `name`.  A pure
   git-ref operation: it never touches records, so the InterCall session (which
   only opens on a record op) stays closed — no second session. */
char *GITBRANCH(char *repo, char *name) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_branch(ctx, rp(repo), name ? name : "");
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

/* --- remotes, clone, config, tag (libgit2 engine, no OS git) ----------------
   Thin CallC bridges over the mv_git_* wrappers; output goes through gitmsg like
   the rest.  Public transport only for now (no credential callback) — the udt
   libgit2 must be built with USE_HTTPS=ON for https remotes.  GITCLONE writes its
   confirmation to <repo>/gitmsg, so run it from a git account (or after GIT INIT)
   to see it; the clone itself lands regardless. */
char *GITCLONE(char *repo, char *url, char *dir, char *ref) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_clone(ctx, url ? url : "", dir ? dir : "", ref ? ref : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}
char *GITFETCH(char *repo, char *remote) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_fetch(ctx, rp(repo), remote ? remote : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}
char *GITPULL(char *repo, char *remote, char *branch) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_pull(ctx, rp(repo), remote ? remote : "", branch ? branch : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}
char *GITPUSH(char *repo, char *remote, char *refspec) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_push(ctx, rp(repo), remote ? remote : "", refspec ? refspec : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}
char *GITREMOTE(char *repo, char *action, char *name, char *url) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_remote(ctx, rp(repo), action ? action : "",
                            name ? name : "", url ? url : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}
char *GITCONFIG(char *repo, char *key, char *value) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_config(ctx, rp(repo), key ? key : "", value ? value : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}
char *GITTAG(char *repo, char *op, char *name, char *target, char *message) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_tag(ctx, rp(repo), op ? op : "", name ? name : "",
                         target ? target : "", message ? message : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}

/* --- record ops and history rewriting ---------------------------------------
   RM and SHOW are pure git-object ops (index remove, blob read): a thin bridge.
   MERGE and CHERRY-PICK do the git ref work here (moving HEAD); the verb then
   re-materialises the new HEAD *natively* via GITUDT.CHECKOUT — the mv_write
   materialise these engine subs also run cannot reach a UniData hash file, only
   the native WRITE loop can, so we let the ref op stand and re-materialise in
   BASIC.  RESTORE is likewise done natively in BASIC (GITUDT.RESTORE), so it
   needs no bridge at all. */
char *GITRM(char *repo, char *file, char *id) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_rm(ctx, rp(repo), file ? file : "", id ? id : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}
char *GITSHOW(char *repo, char *file, char *id) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_show(ctx, rp(repo), file ? file : "", id ? id : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}
char *GITMERGE(char *repo, char *name) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_merge(ctx, rp(repo), name ? name : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}
char *GITCHERRYPICK(char *repo, char *commit) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_cherrypick(ctx, rp(repo), commit ? commit : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}

/* GITPING(path) — write the marker "callc-ok" to <path>/gitmsg (no git work).  A
   disabled CALLC silently no-ops — no error, no side effect — so the verb probes
   with this and, if the marker does not come back, fails with instructions to
   build libu2callc.so with the GIT* functions (UniData 8.3.2 needs no UDT.OPTIONS
   to use CallC) rather than silently doing nothing. */
char *GITPING(char *path) {
    char *m = (char *)malloc(9);
    if (m) memcpy(m, "callc-ok", 9);
    return emit(path, m);
}
