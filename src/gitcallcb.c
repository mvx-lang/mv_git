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

/* GITFLUSH(repo) — write the accumulated index to disk.
 *
 * NOT a no-op, though it was one here until UniData was actually measured.
 * Staging batches into an IN-MEMORY index and only mv_git_batch_end() writes it.
 * Each `GIT` sentence at TCL may be its own process, so a batch left open when
 * ADD returns is simply lost: `GIT ADD -A` reported 1543 records staged, `GIT
 * COMMIT` in the next session committed nothing, and every later command saw an
 * empty repository — status clean, show empty, restore with nothing to restore.
 * That is mv_git#41, answered for UniVerse and deferred here; the answer is the
 * same.  Cheap when there is no open batch, so ADD can always call it.
 */
char *GITFLUSH(char *repo) {
    mv_git_batch_end();
    /* Write the channel even with nothing to say (mv_git#58): an op that
       leaves it alone is read back as this op's result, so `add` echoed the
       ids a previous INDEXIDS had left there.  Every op owns the channel for
       the length of its call. */
    return emit(repo, NULL);
}

/* GITPRUNE(repo) — unstage every record of a file the account no longer has.
   The wholesale add is BASIC here, so it calls this when its walk is done; the
   C add calls the same function directly. */
char *GITPRUNE(char *repo, char *live) {
    mv_git_batch_end();                 /* the index must be on disk first */
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_prune_gone(ctx, rp(repo), live ? live : "");
    mv_ctx_destroy(ctx);
    /* Write the channel even with nothing to say (mv_git#58): an op that
       leaves it alone is read back as this op's result, so `add` echoed the
       ids a previous INDEXIDS had left there.  Every op owns the channel for
       the length of its call. */
    return emit(repo, r);          /* r is "" when nothing was unstaged */
}

/* GITSTOCKIDS(repo, ids) — the stock account's VOC ids, so a checkout does not
   delete the records the commit deliberately never carried (mv_git#46). */
char *GITSTOCKIDS(char *repo, char *ids) {
    mv_git_stock_ids(ids ? ids : "");
    return emit(repo, NULL);
}

/* GITINDEXIDS(repo, file) — the ids staged under <file>/, @AM-separated. */
char *GITINDEXIDS(char *repo, char *file) {
    mv_git_batch_end();                 /* the index must be on disk first */
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_index_ids(ctx, rp(repo), file ? file : "");
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}

/* GITPULLREF(repo, remote, branch) — fetch + move the ref, records left to the
   caller.  Same reason as the daemon's: the records belong to the session. */
char *GITPULLREF(char *repo, char *remote, char *branch) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_pullref(ctx, rp(repo), remote ? remote : "",
                             branch ? branch : "");
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
    /* Write the channel even with nothing to say (mv_git#58): an op that
       leaves it alone is read back as this op's result, so `add` echoed the
       ids a previous INDEXIDS had left there.  Every op owns the channel for
       the length of its call. */
    return emit(repo, NULL);
}

/* GITSTAGEBLOB(repo, path, content) — stage a raw blob (the open-account
   controls: <file>.DICT/%FILE%, .mv-account).  %FILE% goes through
   mv_git_sticky_control, so a resize is not a commit. */
char *GITSTAGEBLOB(char *repo, char *path, char *content) {
    const char *p = path ? path : "";
    const char *use = content ? content : "";
    char keep[MV_GIT_CTL_MAX];
    int64_t ulen = (int64_t)strlen(use);
    use = mv_git_sticky_control(rp(repo), p, use, &ulen, keep, sizeof keep);
    mv_git_batch_begin(rp(repo));
    mv_git_batch_add(p, use, ulen, 0);
    /* Write the channel even with nothing to say (mv_git#58): an op that
       leaves it alone is read back as this op's result, so `add` echoed the
       ids a previous INDEXIDS had left there.  Every op owns the channel for
       the length of its call. */
    return emit(repo, NULL);
}

/* GITSTAGECTL(repo, path, content) — stage a control blob VERBATIM.
   GITSTAGEBLOB puts the recorded geometry back through mv_git_sticky_control,
   which is what stops a resize becoming a commit; the attribute editor is the
   one place that is meant to yield, so it has its own way in (mv_git#15).  The
   batch is ended first, so a pending add cannot be written over the edit. */
char *GITSTAGECTL(char *repo, char *path, char *content) {
    mv_git_batch_end();
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_stagectl(ctx, rp(repo), path ? path : "",
                              content ? content : "");
    mv_ctx_destroy(ctx);
    free(r);
    return emit(repo, NULL);
}

/* GITIXCAT(repo, path) — the STAGED blob at `path` (the index, not HEAD),
   written to <repo>/gitcat like GITCAT: an editor has to build on the edit
   before it rather than on the last commit. */
char *GITIXCAT(char *repo, char *path) {
    mv_git_batch_end();               /* read what this session has staged */
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_ixcat(ctx, rp(repo), path ? path : "");
    mv_ctx_destroy(ctx);
    char p[1300];
    snprintf(p, sizeof p, "%s/gitcat", rp(repo));
    FILE *f = fopen(p, "wb");
    if (f) { if (r) fwrite(r, 1, strlen(r), f); fclose(f); }
    free(r);
    return "";
}

/* GITSTAGED(repo) — what the index holds that HEAD does not, so the in-session
   status can report a staged edit the way the C status already does. */
char *GITSTAGED(char *repo) {
    mv_git_batch_end();
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_staged(ctx, rp(repo));
    mv_ctx_destroy(ctx);
    return emit(repo, r);
}

/* GITUDIFF(old, new, path) — a unified diff of two contents.  Answers through
   the gitcat side channel like CAT: what comes back is content-shaped and may
   be long, not a status message. */
char *GITUDIFF(char *repo, char *oldtext, char *newtext, char *path) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_udiff(ctx, oldtext ? oldtext : "", newtext ? newtext : "",
                           path ? path : "");
    mv_ctx_destroy(ctx);
    char p[1300];
    snprintf(p, sizeof p, "%s/gitcat", rp(repo));
    FILE *f = fopen(p, "wb");
    if (f) { if (r) fwrite(r, 1, strlen(r), f); fclose(f); }
    free(r);
    return "";
}

/* GITPUTDESC(repo, path, content) — put an edited descriptor back on disk.
   UniData keeps none there, so this is the engine's no-op; it exists so the
   BASIC side can call it unconditionally rather than knowing which platforms
   have an on-disk descriptor. */
char *GITPUTDESC(char *repo, char *path, char *content) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_putdesc(ctx, rp(repo), path ? path : "",
                             content ? content : "");
    mv_ctx_destroy(ctx);
    free(r);
    return emit(repo, NULL);
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

/* GITSTAGEDESC(repo, prefix, open) — stage the account descriptor.  Shared with
   the CLI so a commit does not depend on which of the two made it (mv_git#81). */
char *GITSTAGEDESC(char *repo, char *prefix, char *open) {
    char path[700], desc[2048];
    if (mv_git_desc_for(path, sizeof path, desc, sizeof desc,
                        prefix ? prefix : "", open && open[0] == '1')) {
        /* Through the BATCH, like every other in-session staging op: a plain
           mv_git_stageblob() here writes an index the batch flush at commit
           then overwrites, so the descriptor reported as staged never reached
           the tree. */
        mv_git_batch_begin(rp(repo));
        mv_git_batch_add(path, desc, (int64_t)strlen(desc), 0);
    }
    return emit(repo, NULL);
}

/* GITVERSION(repo) — what this account's engine actually is. */
char *GITVERSION(char *repo) {
    return emit(repo, mv_git_versions("mv_git (in-session, UniData CallC)"));
}

/* GITFURNITURE(repo, list) — the account-furniture rules, answered for the file
   list the BASIC walk produced.  The verb has to walk VOC itself (mvgitd cannot
   open the account), but it must not carry its own idea of what furniture is:
   that lives in mv_account_furniture() and nowhere else (mv_git#81). */
char *GITFURNITURE(char *repo, char *list) {
    return emit(repo, mv_git_filter_furniture(list ? list : ""));
}

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
/* GITSWITCH(repo, name) — move HEAD to branch `name` (git ref + index only, no
   record materialise); the verb then re-materialises the new HEAD natively. */
char *GITSWITCH(char *repo, char *name) {
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_switch(ctx, rp(repo), name ? name : "");
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
