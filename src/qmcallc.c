/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* qmcallc.c — the engine entry point the in-session verb calls, on OpenQM /
 * ScarletDME (mv_git#243).
 *
 * ONE ENTRY POINT, NOT THIRTY-SEVEN.  Everywhere else each engine op is its
 * own C function, because the platform can declare one: jBASE has
 * `DEFC VAR JBGITSTATUS(VAR, VAR)' and UniData has CallC.  QM has CCALL, which
 * is not a declaration at all -- the BASIC side hand-builds a little pcode
 * program naming the library, the symbol and each argument, and every distinct
 * arity means another pcode shape to assemble in BASIC.  Thirty-seven of those
 * would be thirty-seven chances to mis-count a push.
 *
 * So the bridge is one symbol with one shape, and the OPERATION IS DATA:
 *
 *     u_int64 QMGIT(char *work, u_int64 cap)
 *
 * `work' arrives holding   OP <AM> ARG1 <AM> ARG2 ...
 * and leaves holding       the engine's answer
 * with the return value the answer's length, or an error code above CAP.
 *
 * That also suits CCALL's own shape: the data buffer is both the argument and
 * the result (op_ccall returns string 1), so there is exactly one buffer to
 * place and one to read back.
 *
 * THE SESSION IS THE POINT.  The record layer under this is qmgit_dh.c, which
 * calls QM's own dh_ file layer in the caller's process -- so `GIT ADD' from
 * the TCL prompt sees the records of the session that typed it, which is the
 * whole reason for an in-session verb rather than shelling out to qm-git.
 */
#include "mvxgit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

/* Errors are returned ABOVE the buffer capacity, so a caller that only looks
   at the length cannot mistake one for a short answer. */
#define QMGIT_ERR_BASE  0xF0000000ULL
#define QMGIT_E_ARGS    (QMGIT_ERR_BASE + 1)
#define QMGIT_E_OP      (QMGIT_ERR_BASE + 2)
#define QMGIT_E_SPACE   (QMGIT_ERR_BASE + 3)

/* One context for the life of the session.  The record layer keeps its open
   files in it, so re-creating it per call would reopen every file every time —
   and the files are the session's own. */
static mv_ctx *g_ctx;

typedef char *(*f1)(mv_ctx *, const char *);
typedef char *(*f2)(mv_ctx *, const char *, const char *);
typedef char *(*f3)(mv_ctx *, const char *, const char *, const char *);
typedef char *(*f4)(mv_ctx *, const char *, const char *, const char *,
                    const char *);
typedef char *(*f5)(mv_ctx *, const char *, const char *, const char *,
                    const char *, const char *);

static const struct {
    const char *op;
    int         arity;      /* engine arguments, not counting ctx */
    void       *fn;
} OPS[] = {
    { "INIT",          1, (void *)(f1)mv_git_init },
    { "STATUS",        1, (void *)(f1)mv_git_status },
    { "ADDALL",        1, (void *)(f1)mv_git_addall },
    { "ADDDISK",       1, (void *)(f1)mv_git_adddisk },
    { "STAGED",        1, (void *)(f1)mv_git_staged },
    { "OPENFORM",      1, (void *)(f1)mv_git_openform },
    { "MATERIALISE",   1, (void *)(f1)mv_git_materialize },
    { "MATERIALISEACCT", 1, (void *)(f1)mv_git_materialize_account },
    { "HEADFILES",     1, (void *)(f1)mv_git_headfiles },

    { "COMMIT",        2, (void *)(f2)mv_git_commit },
    { "LOG",           2, (void *)(f2)mv_git_log },
    { "DIFF",          2, (void *)(f2)mv_git_diff },
    { "DIFFU",         2, (void *)(f2)mv_git_diff_u },
    { "BRANCH",        2, (void *)(f2)mv_git_branch },
    { "CHECKOUT",      2, (void *)(f2)mv_git_checkout },
    { "SWITCH",        2, (void *)(f2)mv_git_switch },
    { "MERGE",         2, (void *)(f2)mv_git_merge },
    { "CHERRYPICK",    2, (void *)(f2)mv_git_cherrypick },
    { "RESTORE",       2, (void *)(f2)mv_git_restore },
    { "FETCH",         2, (void *)(f2)mv_git_fetch },
    { "ADDSUB",        2, (void *)(f2)mv_git_addsub },
    { "IXCAT",         2, (void *)(f2)mv_git_ixcat },
    { "CATPATH",       2, (void *)(f2)mv_git_catpath },
    { "ADDDISKFOR",    2, (void *)(f2)mv_git_adddisk_for },
    { "PRUNEGONE",     2, (void *)(f2)mv_git_prune_gone },
    { "INDEXIDS",      2, (void *)(f2)mv_git_index_ids },
    { "MATERIALISEREV",2, (void *)(f2)mv_git_materialize_rev },

    { "ADD",           3, (void *)(f3)mv_git_add },
    { "RM",            3, (void *)(f3)mv_git_rm },
    { "SHOW",          3, (void *)(f3)mv_git_show },
    { "PUSH",          3, (void *)(f3)mv_git_push },
    { "PULL",          3, (void *)(f3)mv_git_pull },
    { "CLONE",         3, (void *)(f3)mv_git_clone },
    { "CONFIG",        3, (void *)(f3)mv_git_config },
    { "STAGEBLOB",     3, (void *)(f3)mv_git_stageblob },
    { "STAGECTL",      3, (void *)(f3)mv_git_stagectl },
    { "PUTDESC",       3, (void *)(f3)mv_git_putdesc },
    { "UDIFF",         3, (void *)(f3)mv_git_udiff },

    { "REMOTE",        4, (void *)(f4)mv_git_remote },
    { "TAG",           5, (void *)(f5)mv_git_tag },
    { NULL, 0, NULL }
};

/* --- the ops the table above cannot hold --------------------------------- *
 *
 * Five of the engine's ops are not `char *f(mv_ctx *, args...)'.  None of them
 * touches a record -- they answer from the engine's own tables, or stage into
 * the commit batch -- so none of them takes a context, and two answer nothing
 * at all.  Bending the engine's signatures so a table could dispatch them would
 * be changing the engine for this port's convenience; naming them here is not.
 *
 * ARGUMENT ONE IS THE REPOSITORY, for all five, exactly as it is for every op
 * in the table -- even where the engine half ignores it (VERSION) or has no
 * parameter for it (FURNITURE).  The wrapper supplies it; see qm/BP/GITFURNITURE
 * for why that is the MVX shape too.
 *
 * Returns 1 when `op' is one of these, with the answer -- malloc'd, and NULL
 * for the ops that produce none -- in *out. */
static int run_noctx(const char *op, const char *a[5], char **out) {
    if (strcmp(op, "VERSION") == 0) {
        /* THE ONLY OP THAT ANSWERS IN NEWLINES.  mv_git_versions() builds a
           block of text for a CLI to print; the verb prints it through
           GIT.ECHO, which splits on attribute marks.  The MVX half does the
           same translation for the same reason. */
        char *v = mv_git_versions("mv_git " MVXGIT_VERSION
                                  " (in-session, OpenQM CCALL)");
        if (v) {
            for (char *p = v; *p; p++) if (*p == '\n') *p = (char)0xFE;
            size_t n = strlen(v);
            while (n && (unsigned char)v[n - 1] == 0xFE) v[--n] = '\0';
        }
        *out = v;
        return 1;
    }
    if (strcmp(op, "FURNITURE") == 0) { *out = mv_git_filter_furniture(a[1]); return 1; }
    if (strcmp(op, "VOCDROP") == 0)   { *out = mv_git_filter_vocdrop(a[0], a[1]); return 1; }
    if (strcmp(op, "ISOPEN") == 0)    { *out = mv_git_is_open(a[0]); return 1; }
    if (strcmp(op, "STAGEDESC") == 0) {
        char path[700], desc[2048];
        if (mv_git_desc_for(path, sizeof path, desc, sizeof desc,
                            a[1], a[2][0] == '1')) {
            /* THROUGH THE BATCH, like every other in-session staging op: a
               plain stageblob here writes an index that the batch flush at
               commit then overwrites, so the descriptor reported as staged
               never reaches the tree (mv_git#81). */
            mv_git_batch_begin(a[0]);
            mv_git_batch_add(path, desc, (int64_t)strlen(desc), 0);
        }
        *out = NULL;
        return 1;
    }
    return 0;
}

/* Split the work buffer on attribute marks, in place: the engine wants NUL
   terminated arguments and the caller wants its buffer back, and it gets
   overwritten by the answer anyway. */
static int split_args(char *work, char *argv[], int max) {
    int n = 0;
    char *p = work;
    argv[n++] = p;
    while (*p && n < max) {
        if ((unsigned char)*p == 0xFE) { *p = '\0'; argv[n++] = p + 1; }
        p++;
    }
    return n;
}

/* Hand the engine's answer back in the work buffer, freeing it: the buffer is
   both argument and result, so there is one place this happens. */
static u64 reply(char *work, u64 cap, char *ans) {
    u64 len = ans ? (u64)strlen(ans) : 0;
    if (len >= cap) { free(ans); return QMGIT_E_SPACE; }
    memcpy(work, ans ? ans : "", (size_t)len);
    work[len] = '\0';
    free(ans);
    return len;
}

u64 QMGIT(char *work, u64 cap) {
    if (!work || cap < 2) return QMGIT_E_ARGS;
    char *argv[8];
    int n = split_args(work, argv, 8);
    const char *op = argv[0];

    /* A missing trailing argument is an EMPTY one, not a short call: the BASIC
       side leaves optional arguments off rather than sending empty marks, and
       the engine's ops all take "" to mean "not given". */
    const char *a[5] = { "", "", "", "", "" };
    for (int i = 0; i + 1 < n && i < 5; i++) a[i] = argv[i + 1];

    char *ans = NULL;
    if (run_noctx(op, a, &ans)) return reply(work, cap, ans);

    int idx = -1;
    for (int i = 0; OPS[i].op; i++)
        if (strcmp(OPS[i].op, op) == 0) { idx = i; break; }
    if (idx < 0) return QMGIT_E_OP;

    if (!g_ctx) g_ctx = mv_ctx_create();
    switch (OPS[idx].arity) {
        case 1: ans = ((f1)OPS[idx].fn)(g_ctx, a[0]); break;
        case 2: ans = ((f2)OPS[idx].fn)(g_ctx, a[0], a[1]); break;
        case 3: ans = ((f3)OPS[idx].fn)(g_ctx, a[0], a[1], a[2]); break;
        case 4: ans = ((f4)OPS[idx].fn)(g_ctx, a[0], a[1], a[2], a[3]); break;
        case 5: ans = ((f5)OPS[idx].fn)(g_ctx, a[0], a[1], a[2], a[3], a[4]);
                break;
        default: return QMGIT_E_OP;
    }

    return reply(work, cap, ans);
}

/* Let the session drop the context — a verb that has finished with the
   repository should not hold its files open for the rest of the login. */
u64 QMGITDONE(void) {
    if (g_ctx) { mv_ctx_destroy(g_ctx); g_ctx = NULL; }
    return 1;
}
