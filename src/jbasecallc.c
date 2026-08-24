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

/* Every op is the same shape: adopt the caller's session, run the engine,
   answer through the output argument. */
#define JB_OP0(NAME, CALL)                                                    \
    VAR *NAME(VAR *Result, JBASEDP VAR *Repo, VAR *Out) {                     \
        mv_jbase_use_session(dp);                                             \
        mv_ctx *ctx = mv_ctx_create();                                        \
        char *r = CALL;                                                       \
        mv_ctx_destroy(ctx);                                                  \
        give(dp, Out, r);                                                     \
        STORE_VBI(Result, 0);                                                 \
        return Result;                                                        \
    }

#define JB_OP1(NAME, CALL)                                                    \
    VAR *NAME(VAR *Result, JBASEDP VAR *Repo, VAR *A1, VAR *Out) {            \
        mv_jbase_use_session(dp);                                             \
        mv_ctx *ctx = mv_ctx_create();                                        \
        char *r = CALL;                                                       \
        mv_ctx_destroy(ctx);                                                  \
        give(dp, Out, r);                                                     \
        STORE_VBI(Result, 0);                                                 \
        return Result;                                                        \
    }

#define JB_OP2(NAME, CALL)                                                    \
    VAR *NAME(VAR *Result, JBASEDP VAR *Repo, VAR *A1, VAR *A2, VAR *Out) {   \
        mv_jbase_use_session(dp);                                             \
        mv_ctx *ctx = mv_ctx_create();                                        \
        char *r = CALL;                                                       \
        mv_ctx_destroy(ctx);                                                  \
        give(dp, Out, r);                                                     \
        STORE_VBI(Result, 0);                                                 \
        return Result;                                                        \
    }

JB_OP0(JBGITINIT,     mv_git_init(ctx, sfb(dp, Repo)))
JB_OP0(JBGITSTATUS,   mv_git_status(ctx, sfb(dp, Repo)))
JB_OP0(JBGITADDALL,   mv_git_addall(ctx, sfb(dp, Repo)))
JB_OP0(JBGITVERSION,  mv_git_versions("jb-git " MVXGIT_VERSION))

JB_OP1(JBGITCOMMIT,   mv_git_commit(ctx, sfb(dp, Repo), sfb(dp, A1)))
JB_OP1(JBGITLOG,      mv_git_log(ctx, sfb(dp, Repo), sfb(dp, A1)))
JB_OP1(JBGITDIFF,     mv_git_diff(ctx, sfb(dp, Repo), sfb(dp, A1)))
JB_OP1(JBGITBRANCH,   mv_git_branch(ctx, sfb(dp, Repo), sfb(dp, A1)))
JB_OP1(JBGITCHECKOUT, mv_git_checkout(ctx, sfb(dp, Repo), sfb(dp, A1)))
JB_OP1(JBGITRESTORE,  mv_git_restore(ctx, sfb(dp, Repo), sfb(dp, A1)))

JB_OP2(JBGITADD,      mv_git_add(ctx, sfb(dp, Repo), sfb(dp, A1), sfb(dp, A2)))
JB_OP2(JBGITSHOW,     mv_git_show(ctx, sfb(dp, Repo), sfb(dp, A1), sfb(dp, A2)))
JB_OP2(JBGITRM,       mv_git_rm(ctx, sfb(dp, Repo), sfb(dp, A1), sfb(dp, A2)))
