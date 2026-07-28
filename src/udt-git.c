/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* udt-git — the UniData build's command-line driver.
 *
 * The record-git engine (mvxgit.c) is shared with mvx-git; this driver is the
 * UniData-side entry point.  It dispatches a subcommand to the engine's
 * mv_git_* functions and renders the engine's @AM-separated output as lines.
 * Records flow through UniData InterCall (udtgit_rt.c), selected at compile
 * time with -DMVXGIT_UDT — the git-object logic is identical to mvx-git.
 *
 * The account is the current directory (its .git is the repository); set
 * MVXACCOUNT so the InterCall session binds to it.  Session credentials come
 * from the environment (UDT_HOST/UDT_USER/UDT_PASSWORD/UDT_SERVICE).
 *
 * This first cut covers the in-account verbs (init/add/rm/status/commit/log/
 * diff/show/branch/checkout/merge/cherry-pick/restore).  clone + materialise
 * (provisioning a UniData account from git) come next. */

#define _POSIX_C_SOURCE 200809L   /* setenv, chdir under -std=c11 */

#include "mvxgit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Render the engine's output (lines separated by @AM = 0xFE) to stdout. */
static void emit(char *out) {
    if (!out) return;
    for (char *p = out; *p; p++)
        putchar((unsigned char)*p == 0xFE ? '\n' : *p);
    putchar('\n');
    free(out);
}

static const char *arg(int argc, char **argv, int n) {
    return n < argc ? argv[n] : "";
}

int main(int argc, char **argv) {
    /* optional "-a <account>" before the subcommand */
    int i = 1;
    const char *account = ".";
    if (i < argc && strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
        account = argv[i + 1];
        i += 2;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: udt-git [-a account] <command> [args]\n");
        return 2;
    }
    const char *sub = argv[i++];

    if (chdir(account) != 0) {
        fprintf(stderr, "udt-git: cannot enter account %s\n", account);
        return 1;
    }
    setenv("MVXACCOUNT", account, 1);

    mv_ctx *ctx = mv_ctx_create();
    const char *repo = ".git";
    const char *p0 = arg(argc, argv, i);
    const char *p1 = arg(argc, argv, i + 1);
    int rc = 0;

    if (!strcmp(sub, "init")) {
        emit(mv_git_init(ctx, repo));
    } else if (!strcmp(sub, "add")) {
        emit(mv_git_add(ctx, repo, p0, p1));
    } else if (!strcmp(sub, "rm")) {
        emit(mv_git_rm(ctx, repo, p0, p1));
    } else if (!strcmp(sub, "status")) {
        emit(mv_git_status(ctx, repo));
    } else if (!strcmp(sub, "commit")) {
        /* commit -m <msg> */
        const char *msg = "";
        for (int k = i; k < argc; k++)
            if (!strcmp(argv[k], "-m") && k + 1 < argc) { msg = argv[k + 1]; break; }
        emit(mv_git_commit(ctx, repo, msg));
    } else if (!strcmp(sub, "log")) {
        emit(mv_git_log(ctx, repo, p0[0] ? p0 : "20"));
    } else if (!strcmp(sub, "diff")) {
        emit(mv_git_diff(ctx, repo, p0));
    } else if (!strcmp(sub, "show")) {
        emit(mv_git_show(ctx, repo, p0, p1));
    } else if (!strcmp(sub, "branch")) {
        emit(mv_git_branch(ctx, repo, p0));
    } else if (!strcmp(sub, "checkout")) {
        emit(mv_git_checkout(ctx, repo, p0));
    } else if (!strcmp(sub, "merge")) {
        emit(mv_git_merge(ctx, repo, p0));
    } else if (!strcmp(sub, "cherry-pick")) {
        emit(mv_git_cherrypick(ctx, repo, p0));
    } else if (!strcmp(sub, "restore")) {
        emit(mv_git_restore(ctx, repo, p0));
    } else {
        fprintf(stderr, "udt-git: unknown command '%s'\n", sub);
        rc = 2;
    }

    mv_ctx_destroy(ctx);
    return rc;
}
