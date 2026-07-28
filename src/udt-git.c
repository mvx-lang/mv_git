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
#include <sys/stat.h>
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

/* True if the account (cwd) opts into the open interchange format via git config
   `mvx.openaccount = true` — the same flag mvx-git honours.  Plain text scan of
   .git/config so the result matches across libgit2 versions. */
static int open_account_on(void) {
    FILE *f = fopen(".git/config", "r");
    if (!f) return 0;
    char line[512];
    int in_mvx = 0, on = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { in_mvx = strncasecmp(s, "[mvx]", 5) == 0; continue; }
        if (in_mvx && strncasecmp(s, "openaccount", 11) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                while (*eq == ' ' || *eq == '\t') eq++;
                on = strncasecmp(eq, "true", 4) == 0 || *eq == '1' ||
                     strncasecmp(eq, "yes", 3) == 0;
            }
        }
    }
    fclose(f);
    return on;
}

/* Stage the whole account in the open (portable) form: every local file (from
   mv_filelist) — its data records, its dictionary, and a synthesised
   `<file>.DICT/%FILE%` control (DIR for a UniData directory file, else hash) —
   plus the `.mv-account` descriptor.  UniData has no on-disk %FILE% or
   descriptor, so udt-git writes the open form directly; the git tree is then
   portable and mvx-git can materialise a live MVX account from it. */
static void add_all(mv_ctx *ctx, const char *repo) {
    int open = open_account_on();   /* open interchange vs native udt->udt */
    mv_value fl;
    mv_init(&fl);
    mv_filelist(ctx, &fl);
    char nb[40];
    const char *p;
    int64_t len = mv_val_chars(&fl, nb, sizeof nb, &p);
    int64_t i = 0;
    while (i <= len) {
        int64_t s = i;
        while (i < len && (unsigned char)p[i] != 0xFE) i++;
        int64_t nl = i - s;
        if (nl > 0 && nl < 240) {
            char name[256], dict[300], ctrl[320];
            memcpy(name, p + s, (size_t)nl);
            name[nl] = '\0';
            snprintf(dict, sizeof dict, "%s.DICT", name);
            emit(mv_git_add(ctx, repo, name, ""));   /* data records */
            emit(mv_git_add(ctx, repo, dict, ""));   /* dictionary   */
            if (open) {
                /* open-form control: a UniData directory file is an OS
                   directory, a hashed file is a regular file. */
                struct stat sb;
                const char *type = (stat(name, &sb) == 0 &&
                                    S_ISDIR(sb.st_mode)) ? "DIR" : "hash";
                snprintf(ctrl, sizeof ctrl, "%s.DICT/%%FILE%%", name);
                free(mv_git_stageblob(ctx, repo, ctrl, type));
            }
        }
        i++;
    }
    mv_clear(&fl);

    const char *acctpath = getenv("MVXACCOUNT");
    const char *base = acctpath ? acctpath : "account";
    const char *slash = strrchr(base, '/');
    if (slash && slash[1]) base = slash + 1;
    char desc[512];
    if (open) {
        /* the portable account descriptor (UniData has no .mvx on disk) */
        snprintf(desc, sizeof desc,
                 "# MV account descriptor\nname = %s\nversion = 1\n"
                 "hash = lmdb\n", base);
        free(mv_git_stageblob(ctx, repo, ".mv-account", desc));
        printf(".mv-account + %%FILE%% controls written (open format)\n");
    } else {
        /* native UniData account marker: UniData has no on-disk descriptor, so
           record that this is a UniData account (and carry account-specific
           info) for a checkout back into another UniData instance. */
        snprintf(desc, sizeof desc,
                 "# UniData account descriptor\nname = %s\nversion = 1\n", base);
        free(mv_git_stageblob(ctx, repo, ".udt", desc));
        printf(".udt account marker written (native)\n");
    }
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
    /* The InterCall session binds to the account by absolute path (UniData
       LOGTO does not resolve "." or a relative path), so record on the cwd. */
    char acctpath[4096];
    if (getcwd(acctpath, sizeof acctpath))
        setenv("MVXACCOUNT", acctpath, 1);
    else
        setenv("MVXACCOUNT", account, 1);
    /* Open interchange format is opt-in, exactly as on mvx-git: only when the
       account's git config sets mvx.openaccount does the engine drop the
       platform's system VOC items and synthesise the portable %FILE% /
       .mv-account form.  A plain commit keeps everything native, so it can be
       checked out into another UniData instance unchanged. */
    if (open_account_on())
        setenv("MVX_OPENACCOUNT", "1", 1);

    mv_ctx *ctx = mv_ctx_create();
    const char *repo = ".git";
    const char *p0 = arg(argc, argv, i);
    const char *p1 = arg(argc, argv, i + 1);
    int rc = 0;

    if (!strcmp(sub, "init")) {
        emit(mv_git_init(ctx, repo));
    } else if (!strcmp(sub, "add")) {
        /* bare `add`, `add -A`, or `add .` stages the whole account */
        if (!p0[0] || !strcmp(p0, "-A") || !strcmp(p0, "--all") ||
            !strcmp(p0, "."))
            add_all(ctx, repo);
        else
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
