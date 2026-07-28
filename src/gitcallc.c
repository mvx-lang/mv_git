/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* gitcallc.c — the UniData CallC entry point for the in-session GIT verb.
 *
 * Built into libu2callc.so (see $UDTHOME/bin/work/cfuncdef + callbas.mk) so a
 * UniBasic GIT verb can `CALLC GITVERB(@SENTENCE, OUT)` and drive the record-git
 * engine for the current account — the same engine and InterCall backend the
 * standalone udt-git uses (it opens its own ic_unidata_session).  The ECL
 * sentence is parsed here; the @AM-separated engine output is written into the
 * caller's OUT string in place (BASIC must pre-size it, e.g. OUT = SPACE(n)). */

#define _POSIX_C_SOURCE 200809L   /* setenv, getcwd, chdir under -std=c11 */

#include "mvxgit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* OUT capacity contract: the UniBasic verb sizes OUT with SPACE(GITVERB_OUTCAP)
   before the call; the engine output is truncated to fit. */
#define GITVERB_OUTCAP (1024 * 1024)

/* Opt into the open interchange when the account's git config sets
   mvx.openaccount (plain text scan, matching udt-git / mvx-git). */
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

/* GITVERB(sentence, out) — run one git subcommand for the current account.
   `sentence` is the whole ECL sentence ("GIT STATUS …"); `out` is filled in
   place with the @AM-separated result and also returned. */
char *GITVERB(char *sentence, char *out) {
    if (out) out[0] = '\0';

    char acct[4096];
    if (getcwd(acct, sizeof acct)) setenv("MVXACCOUNT", acct, 1);
    if (open_account_on()) setenv("MVX_OPENACCOUNT", "1", 1);

    /* tokenise: tok[0] = verb (GIT), tok[1] = subcommand, tok[2…] = args */
    char buf[8192];
    snprintf(buf, sizeof buf, "%s", sentence ? sentence : "");
    char *tok[64];
    int nt = 0;
    for (char *p = strtok(buf, " \t"); p && nt < 64; p = strtok(NULL, " \t"))
        tok[nt++] = p;
    const char *sub = nt > 1 ? tok[1] : "";
    const char *a0 = nt > 2 ? tok[2] : "";
    const char *a1 = nt > 3 ? tok[3] : "";

    mv_ctx *ctx = mv_ctx_create();
    const char *repo = ".git";
    char *r = NULL;
    if (!strcasecmp(sub, "init"))
        r = mv_git_init(ctx, repo);
    else if (!strcasecmp(sub, "add"))
        r = mv_git_add(ctx, repo, a0, a1);
    else if (!strcasecmp(sub, "rm"))
        r = mv_git_rm(ctx, repo, a0, a1);
    else if (!strcasecmp(sub, "status"))
        r = mv_git_status(ctx, repo);
    else if (!strcasecmp(sub, "commit")) {
        const char *msg = "";
        for (int i = 2; i + 1 < nt; i++)
            if (!strcmp(tok[i], "-m")) { msg = tok[i + 1]; break; }
        r = mv_git_commit(ctx, repo, msg);
    } else if (!strcasecmp(sub, "log"))
        r = mv_git_log(ctx, repo, a0[0] ? a0 : "20");
    else if (!strcasecmp(sub, "diff"))
        r = mv_git_diff(ctx, repo, a0);
    else if (!strcasecmp(sub, "branch"))
        r = mv_git_branch(ctx, repo, a0);
    else if (!strcasecmp(sub, "checkout"))
        r = mv_git_checkout(ctx, repo, a0);
    else
        r = strdup("usage: GIT <init|add|rm|status|commit|log|diff|branch|checkout>");

    if (r && out) {
        size_t n = strlen(r);
        if (n > GITVERB_OUTCAP - 1) n = GITVERB_OUTCAP - 1;
        memcpy(out, r, n);
        out[n] = '\0';
    }
    free(r);
    mv_ctx_destroy(ctx);
    return out;
}
