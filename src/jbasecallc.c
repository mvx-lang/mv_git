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
/* -std=c11 is STRICT: strdup, popen, pclose and the <sys/wait.h> status macros
   are POSIX, not ISO C, so without this they are not declared -- and an
   undeclared function is assumed to return int, which truncates a 64-bit
   pointer.  The failure has no message: free() on the truncated pointer takes
   the process down with exit 201, so the CRT before the CALL prints and the one
   after does not (mv_git#192). */
#define _POSIX_C_SOURCE 200809L

#include "mvxgit.h"
#include "jbasegit_rt.h"

#include <jsystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

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

/* JBGITRUN(cmd, out, status) — run an OS command and answer BOTH streams.
 *
 * jBASE's EXECUTE ... CAPTURING takes stdout ALONE, and its argument handling
 * defeats the usual workaround: `/bin/sh -c "…"` arrives as a filename for sh
 * to run, not as a command line.  So from BASIC there is no way to see what a
 * command said on stderr -- which is exactly where a licence refusal appears.
 * A clone that failed for want of a seat came back as SILENCE, and the verb
 * had nothing to report but success.
 *
 * EXECUTE does not give the exit status either, so even "did it work?" was a
 * guess.  This answers both, and folds stderr in with 2>&1.
 *
 * NOT A NEW CAPABILITY.  The verb can already EXECUTE an OS command and jb-git
 * already calls system(); this is the same reach with the errors attached.  It
 * runs as the logged-on user like everything else the account does, and the
 * privilege gate above it is unchanged (ARCHITECTURE non-negotiable 8).
 */
VAR *JBGITRUN(VAR *Result, JBASEDP VAR *A0, VAR *Out) {
    const char *cmd = sfb(dp, A0);
    size_t nc = strlen(cmd) + 8;
    char *full = (char *)malloc(nc);
    if (!full) { give(dp, Out, NULL); STORE_VBI(Result, -1); return Result; }
    snprintf(full, nc, "%s 2>&1", cmd);
    FILE *f = popen(full, "r");
    free(full);
    if (!f) { give(dp, Out, NULL); STORE_VBI(Result, -1); return Result; }

    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (buf) {
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (len + 2 > cap) {
                size_t ncap = cap * 2;
                char *nb = (char *)realloc(buf, ncap);
                if (!nb) break;
                buf = nb; cap = ncap;
            }
            /* One line per attribute, which is what CAPTURING hands back and
               what the callers already index with <n>. */
            buf[len++] = (c == '\n') ? (char)0xFE : (char)c;
        }
        /* A trailing newline would leave an empty final attribute on every
           call, and DCOUNT would count a line that is not there. */
        if (len && (unsigned char)buf[len - 1] == 0xFE) len--;
        buf[len] = '\0';
    }
    int rc = pclose(f);
    give(dp, Out, buf);                 /* give() frees it */
    /* THE STATUS COMES BACK AS THE RESULT, following every other entry point
       here, which store only into Result. */
    STORE_VBI(Result, WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
    return Result;
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
