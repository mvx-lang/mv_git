/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* mvconn — see mvconn.h. */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <git2.h>

#include "mvconn.h"

/* Declared here rather than by including mvxgit.h.  That header IS the record
   seam -- it pulls in whichever platform runtime this binary was compiled for --
   and this file must compile the same way for all four drivers.  Keeping the
   seam out is the point of the object, so the one symbol it needs is named
   directly.  (mvxgit.c, which every driver links, defines it.) */
void mv_git_libgit2_boot(void);

#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

struct mv_conn {
    char account[PATH_MAX];
    int  open_override;      /* -1 none, 0 --no-open-account, 1 --open-account */
    int  open_cached;        /* -1 not read yet, else what the config says */
};

/* WHERE THE CONFIG ACTUALLY IS.  Not always <account>/.git/config: in a
   worktree or a submodule `.git` is a FILE naming the real directory, and three
   of the four drivers used to fopen(".git/config") straight and read an account
   as native when it was not.  libgit2 locates it; the value is read below as
   text, which is the other half of the same lesson. */
static void config_path(const char *acct, char *buf, size_t n) {
    mv_git_libgit2_boot();
    git_repository *gr = NULL;
    if (git_repository_open(&gr, acct) == 0) {
        snprintf(buf, n, "%sconfig", git_repository_path(gr));
        git_repository_free(gr);
    } else {
        snprintf(buf, n, "%s/.git/config", acct);
    }
}

/* PLAIN TEXT, NOT git_config_get_bool.  get_bool answered differently on
   different libgit2 builds, which made an account's own type depend on which
   libgit2 the driver happened to link (the reason mvx-git stopped using it).
   Reading the file is the one answer that is the same everywhere. */
static int config_says_open(const char *acct) {
    char path[PATH_MAX + 64];
    config_path(acct, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[512];
    int in_mvx = 0, on = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { in_mvx = strncasecmp(s, "[mvx]", 5) == 0; continue; }
        if (!in_mvx || strncasecmp(s, "openaccount", 11) != 0) continue;
        char *v = strchr(s, '=');
        if (!v) continue;
        v++;
        while (*v == ' ' || *v == '\t') v++;
        /* git's own truthy set, "on" included -- three of the four readers
           were missing it, so `openaccount = on` meant a native account. */
        on = strncasecmp(v, "true", 4) == 0 || *v == '1' ||
             strncasecmp(v, "yes", 3) == 0 || strncasecmp(v, "on", 2) == 0;
        break;               /* first setting wins, as git itself does not */
    }
    fclose(f);
    return on;
}

void mvconn_config_path(mv_conn *c, char *buf, size_t n) {
    config_path(mvconn_account(c), buf, n);
}

int mvconn_persist_open_account(mv_conn *c) {
    if (!c) return -1;
    char path[PATH_MAX + 64];
    config_path(c->account, path, sizeof path);

    /* libgit2 WRITES it and plain text READS it, which is not inconsistent:
       writing "true" is the same on every build, and it is get_bool that was
       not.  Writing through libgit2 also means no dependency on a `git` binary
       being on PATH -- jBASE shelled out with system() and did not check the
       result in two of its three call sites. */
    mv_git_libgit2_boot();
    git_config *cfg = NULL;
    if (git_config_open_ondisk(&cfg, path) != 0) return -1;
    int rc = git_config_set_bool(cfg, "mvx.openaccount", 1);
    git_config_free(cfg);
    if (rc == 0) c->open_cached = 1;
    return rc;
}

mv_conn *mvconn_open(const char *account) {
    mv_conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->account, sizeof c->account, "%s",
             account && account[0] ? account : ".");
    c->open_override = -1;
    c->open_cached   = -1;
    return c;
}

void mvconn_close(mv_conn *c) { free(c); }

const char *mvconn_account(const mv_conn *c) { return c ? c->account : "."; }

int mvconn_config_open_account(mv_conn *c) {
    if (!c) return 0;
    if (c->open_cached < 0) c->open_cached = config_says_open(c->account);
    return c->open_cached;
}

void mvconn_set_open_account(mv_conn *c, int on) {
    if (c) c->open_override = on ? 1 : 0;
}

int mvconn_have_open_account(const mv_conn *c) {
    return c && c->open_override >= 0;
}

int mvconn_open_account(mv_conn *c) {
    if (!c) return 0;
    if (c->open_override >= 0) return c->open_override;
    return mvconn_config_open_account(c);
}

int mvconn_ask_open_account(mv_conn *c, const char *prog, const char *subject) {
    if (!c) return 0;
    if (c->open_override >= 0) return c->open_override;

    const char *env = getenv("MVXGIT_OPEN_ACCOUNT");
    if (env && env[0]) {
        c->open_override = mvconn_env_true(env);
        return c->open_override;
    }

    if (!isatty(STDIN_FILENO)) {
        /* NOBODY TO ASK MEANS YES.  The account was committed in the portable
           form; checking it out natively is what stops it travelling back, and
           a script that wanted that says so with --no-open-account. */
        fprintf(stderr,
                "%s: '%s' was committed in the open account format — "
                "checking it out as an open account.\n"
                "         (--no-open-account, or MVXGIT_OPEN_ACCOUNT=0, for a "
                "native checkout instead)\n",
                prog, subject ? subject : mvconn_account(c));
        c->open_override = 1;
        return 1;
    }

    fprintf(stderr,
            "\n'%s' was committed in the open account format: its dictionaries "
            "and file\ncontrols are in the portable shape that moves between MV "
            "platforms.  Keeping\nit open is what lets this account travel back "
            "the same way.\n\n"
            "Make it an open account? [Y/n] ",
            subject ? subject : mvconn_account(c));
    fflush(stderr);

    char buf[16];
    if (!fgets(buf, sizeof buf, stdin)) c->open_override = 1;
    else c->open_override = !(buf[0] == 'n' || buf[0] == 'N');
    return c->open_override;
}

void mvconn_export_open_account(mv_conn *c) {
    if (mvconn_open_account(c)) setenv("MVX_OPENACCOUNT", "1", 1);
}
