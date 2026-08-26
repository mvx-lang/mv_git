/* jbasecallc.c — the GIT* entry points the in-session verb calls, for jBASE.
 * Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
 *
 * jBASE reaches C with DEFC, which declares a FUNCTION rather than a
 * subroutine, and hands it the session:
 *
 *     DEFC VAR JBGITSTATUS(VAR, VAR)
 *     X = JBGITSTATUS(".git", OUT.TXT)
 *
 * An argument VAR written by the C side IS visible to the caller (verified), so
 * output travels back through an argument exactly as it does for the engine's
 * own mvx_sub_* entry points, and the return value is only a status.
 *
 * The session is the whole point.  jBASE gives the C function its own
 * DPSTRUCT, and mv_jbase_use_session() hands it to the record layer so the verb
 * reads the records of the session that called it rather than opening a second
 * one -- which is what makes an in-session `GIT ADD` see uncommitted work.
 *
 * The entry points are named JBGIT* rather than GIT*, so that BP/GIT<op> can be
 * a BASIC SUBROUTINE of the same name as MVX's -- one line of glue each -- and
 * every handler's engine arm then works here unchanged.  DEFC declares a
 * function, which cannot satisfy a CALL.
 *
 * This layer is DELIBERATELY WIDER than UniData's (src/gitcallcb.c), which has
 * no GITSTATUS or GITADD at all: there, C cannot read records, so status and
 * add are inlined BASIC.  Here the engine does them, so both are exposed and
 * the in-session verb runs the same code the CLI does (mv_git#114).
 */
#include "mvxgit.h"
#include "jbasegit_rt.h"

#include <jsystem.h>

#include <stdlib.h>
#include <string.h>

#ifdef DPSTRUCT_DEF
#define JBASEDP DPSTRUCT *dp,
#else
#define JBASEDP
#endif

/* A VAR's bytes as a C string, and the reverse.
 *
 * Both take `dp` by that NAME on purpose: jBASE's CONV_SFB / STORE_VBC are
 * macros that expand to calls passing `dp` implicitly, so a helper without a
 * parameter of exactly that name will not compile -- and the error points at
 * jsystem.h rather than at the helper. */
static const char *sfb(DPSTRUCT *dp, VAR *v) {
    char *p = v ? (char *)CONV_SFB(v) : NULL;
    return p ? p : "";
}

static void give(DPSTRUCT *dp, VAR *out, char *answer) {
    STORE_VBC(out, answer ? answer : "");
    free(answer);
}

VAR *JBGITADD(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_add(ctx, sfb(dp, A0), sfb(dp, A1), sfb(dp, A2));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITADDALL(VAR *Result, JBASEDP VAR *A0, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_addall(ctx, sfb(dp, A0));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITBRANCH(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_branch(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITCAT(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_catpath(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITCHECKOUT(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_checkout(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITCHERRYPICK(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_cherrypick(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITCLONE(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *A3, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    /* mv_git_clone takes no repo: a clone has no repository yet. */
    char *r = mv_git_clone(ctx, sfb(dp, A1), sfb(dp, A2), sfb(dp, A3));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITCOMMIT(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_commit(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITCONFIG(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_config(ctx, sfb(dp, A0), sfb(dp, A1), sfb(dp, A2));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITDIFF(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_diff(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITDIFFU(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_diff_u(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITFETCH(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_fetch(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITFILES(VAR *Result, JBASEDP VAR *A0, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_headfiles(ctx, sfb(dp, A0));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITINIT(VAR *Result, JBASEDP VAR *A0, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_init(ctx, sfb(dp, A0));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITIXCAT(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_ixcat(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITLOG(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_log(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITMERGE(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_merge(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITPULL(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_pull(ctx, sfb(dp, A0), sfb(dp, A1), sfb(dp, A2));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITPUSH(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_push(ctx, sfb(dp, A0), sfb(dp, A1), sfb(dp, A2));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITPUTDESC(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_putdesc(ctx, sfb(dp, A0), sfb(dp, A1), sfb(dp, A2));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITREMOTE(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *A3, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_remote(ctx, sfb(dp, A0), sfb(dp, A1), sfb(dp, A2), sfb(dp, A3));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITRESTORE(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_restore(ctx, sfb(dp, A0), sfb(dp, A1));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITRM(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_rm(ctx, sfb(dp, A0), sfb(dp, A1), sfb(dp, A2));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITSHOW(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_show(ctx, sfb(dp, A0), sfb(dp, A1), sfb(dp, A2));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITSTAGECTL(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_stagectl(ctx, sfb(dp, A0), sfb(dp, A1), sfb(dp, A2));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITSTAGED(VAR *Result, JBASEDP VAR *A0, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_staged(ctx, sfb(dp, A0));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITSTATUS(VAR *Result, JBASEDP VAR *A0, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_status(ctx, sfb(dp, A0));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITTAG(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *A3, VAR *A4, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_tag(ctx, sfb(dp, A0), sfb(dp, A1), sfb(dp, A2), sfb(dp, A3), sfb(dp, A4));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

/* udiff works on two texts, not on a repository -- so unlike every other op
   here it takes no repo, and the BASIC shim's signature matches MVX's. */
VAR *JBGITUDIFF(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_udiff(ctx, sfb(dp, A0), sfb(dp, A1), sfb(dp, A2));
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

/* The three that do not fit the pattern.
   filter_furniture takes a list and no session; versions answers about the
   build; stagedesc synthesises the account descriptor and stages it. */

VAR *JBGITFURNITURE(VAR *Result, JBASEDP VAR *A0, VAR *Out) {
    mv_jbase_use_session(dp);
    char *r = mv_git_filter_furniture(sfb(dp, A0));
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITVERSION(VAR *Result, JBASEDP VAR *A0, VAR *Out) {
    (void)A0;
    mv_jbase_use_session(dp);
    char *r = mv_git_versions("jb-git " MVXGIT_VERSION);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}

VAR *JBGITSTAGEDESC(VAR *Result, JBASEDP VAR *A0, VAR *A1, VAR *A2, VAR *Out) {
    mv_jbase_use_session(dp);
    mv_ctx *ctx = mv_ctx_create();
    char dpath[700], ddesc[2048];
    char *r = NULL;
    const char *op = sfb(dp, A2);
    if (mv_git_desc_for(dpath, sizeof dpath, ddesc, sizeof ddesc,
                        sfb(dp, A1), op[0] == '1'))
        r = mv_git_stageblob(ctx, sfb(dp, A0), dpath, ddesc);
    mv_ctx_destroy(ctx);
    give(dp, Out, r);
    STORE_VBI(Result, 0);
    return Result;
}
