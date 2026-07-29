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

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      /* strncasecmp */
#include <sys/stat.h>
#include <sys/wait.h>
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
    /* A blanket add -A never commits compiled BASIC objects (binary records,
       rebuilt on the target) — the engine honours this flag; an explicit
       `udt-git add <file>` leaves it unset and stages everything (#9). */
    setenv("MVX_GIT_SKIP_OBJECTS", "1", 1);
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
                /* open-form control: probe the live file for its class and, for
                   a hash file, its real modulo (so a clone recreates it at true
                   size, keeping a static file static) — "DIR" or
                   "hash <modulo> DYNAMIC|STATIC".  But a hash modulo is only a
                   suggested default: keep an already-committed one STICKY so this
                   account's resize does not overwrite the shipped default and
                   ripple out to other clones — only seed it for a new file. */
                char type[128];
                if (!mv_fileclass(ctx, name, type, sizeof type))
                    snprintf(type, sizeof type, "hash");
                if (strncmp(type, "hash", 4) == 0) {
                    char committed[128];
                    if (mv_git_committed_control(repo, name, committed,
                                                 sizeof committed) >= 0 &&
                        strncmp(committed, "hash", 4) == 0)
                        snprintf(type, sizeof type, "%s", committed);
                }
                snprintf(ctrl, sizeof ctrl, "%s.DICT/%%FILE%%", name);
                free(mv_git_stageblob(ctx, repo, ctrl, type));
                /* %INDEXES%: the file's alternate-key index names, so secondary
                   indexes travel (#10).  Fold @AM to newlines like every record
                   blob; stage only when the file actually has indexes. */
                char ix[8192];
                if (mv_indices(ctx, name, ix, sizeof ix) && ix[0]) {
                    for (char *q = ix; *q; q++)
                        if ((unsigned char)*q == 0xFE) *q = '\n';
                    char ixc[320];
                    snprintf(ixc, sizeof ixc, "%s.DICT/%%INDEXES%%", name);
                    free(mv_git_stageblob(ctx, repo, ixc, ix));
                }
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

/* Fork/exec argv[0] with argv, wait, and return its exit status (127 on
   spawn failure).  Used to shell out to git and newacct during clone. */
static int runcmd(const char *const cmd[]) {
    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        execvp(cmd[0], (char *const *)cmd);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) { /* retry on EINTR */ }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* Provision a fresh, registered UniData account in the current directory with
   UniData's own newacct.  This restores every standard system file (VOC and its
   dictionary, _HOLD_, SAVEDLISTS, BP/CTLG, MENUFILE, ...) that the open-account
   commit deliberately drops — so clone only has to materialise the user's files
   and records on top.  newacct is interactive (continue? / owner login / group
   name), so feed those three answers on its stdin.  Its own exit status is not
   a reliable success signal (it returns 0 even when a login is rejected), so the
   caller confirms VOC exists afterwards. */
static int run_newacct(const char *owner, const char *group) {
    const char *udthome = getenv("UDTHOME");
    if (!udthome || !udthome[0]) udthome = "/usr/ud83";
    char prog[4200];
    snprintf(prog, sizeof prog, "%s/bin/newacct", udthome);

    int fd[2];
    if (pipe(fd) != 0) return 1;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return 1; }
    if (pid == 0) {
        dup2(fd[0], 0);
        close(fd[0]);
        close(fd[1]);
        int nul = open("/dev/null", O_WRONLY);   /* mute the user/group lists */
        if (nul >= 0) { dup2(nul, 1); close(nul); }
        execl(prog, "newacct", (char *)NULL);
        _exit(127);
    }
    close(fd[0]);
    dprintf(fd[1], "y\n%s\n%s\n", owner, group);
    close(fd[1]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) { /* retry on EINTR */ }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* Build a `udt` input script that recreates every file's alternate-key indexes
   from its committed <file>.DICT/%INDEXES% control (#10), or return NULL when no
   file has any.  Per field: `CREATE.INDEX <file> <field>` followed by a BLANK
   line — UniData always prompts for the alternate-key length and a blank line
   takes the default (20); there is no inline length syntax.  Then
   `BUILD.INDEX <file> ALL` populates the file's indexes.  The script is fed to a
   fresh `udt` (not InterCall — ic_execute cannot answer the length prompt), so
   it runs only after the InterCall session is closed: no second concurrent
   session.  Reads HEAD like the checkout does (mv_git_headfiles/mv_git_catpath).
   *count receives the number of CREATE.INDEX lines. */
static char *collect_index_script(mv_ctx *ctx, const char *repo, int *count) {
    *count = 0;
    char *paths = mv_git_headfiles(ctx, repo);
    if (!paths) return NULL;
    const char *suf = ".DICT/%INDEXES%";
    size_t sl = strlen(suf);
    char *scr = NULL;
    size_t cap = 0, len = 0;
    for (char *p = paths; *p;) {
        char *e = p;
        while (*e && (unsigned char)*e != 0xFE) e++;
        size_t pl = (size_t)(e - p);
        if (pl > sl && memcmp(p + pl - sl, suf, sl) == 0) {
            char base[256], path[300];
            snprintf(base, sizeof base, "%.*s", (int)(pl - sl), p);
            snprintf(path, sizeof path, "%.*s", (int)pl, p);
            char *ix = mv_git_catpath(ctx, repo, path);   /* @AM field names */
            if (ix) {
                int hasf = 0;
                for (char *f = ix; *f;) {
                    char *fe = f;
                    while (*fe && (unsigned char)*fe != 0xFE) fe++;
                    if (fe > f) {
                        char line[600];
                        int n = snprintf(line, sizeof line,
                                         "CREATE.INDEX %s %.*s\n\n",
                                         base, (int)(fe - f), f);
                        if (len + (size_t)n + 1 > cap) {
                            cap = (len + (size_t)n + 1) * 2;
                            scr = realloc(scr, cap);
                            if (!scr) mv_fatal("out of memory");
                        }
                        memcpy(scr + len, line, (size_t)n);
                        len += (size_t)n;
                        (*count)++;
                        hasf = 1;
                    }
                    f = *fe ? fe + 1 : fe;
                }
                if (hasf) {
                    char line[300];
                    int n = snprintf(line, sizeof line,
                                     "BUILD.INDEX %s ALL\n", base);
                    if (len + (size_t)n + 1 > cap) {
                        cap = (len + (size_t)n + 1) * 2;
                        scr = realloc(scr, cap);
                        if (!scr) mv_fatal("out of memory");
                    }
                    memcpy(scr + len, line, (size_t)n);
                    len += (size_t)n;
                }
                free(ix);
            }
        }
        p = *e ? e + 1 : e;
    }
    free(paths);
    if (*count == 0) { free(scr); return NULL; }
    const char *q = "QUIT\n";
    size_t qn = strlen(q);
    scr = realloc(scr, len + qn + 1);
    if (!scr) mv_fatal("out of memory");
    memcpy(scr + len, q, qn);
    scr[len + qn] = '\0';
    return scr;
}

/* udt-git clone <repo> [<dir>] — provision a UniData account from a record-git
   repository.  Clone the repo without a working tree, create a fresh registered
   account in place with newacct, then materialise HEAD's files and records into
   it over InterCall.  The account owner/group default to the current login and
   can be overridden with UDT_ACCT_OWNER / UDT_ACCT_GROUP. */
static int do_clone(const char *repo, const char *dir) {
    if (!repo || !repo[0]) {
        fprintf(stderr, "usage: udt-git clone <repo> [<dir>]\n");
        return 2;
    }
    char dbuf[4096];
    if (!dir || !dir[0]) {
        const char *b = strrchr(repo, '/');
        b = b ? b + 1 : repo;
        snprintf(dbuf, sizeof dbuf, "%s", b);
        char *dot = strstr(dbuf, ".git");    if (dot) *dot = 0;
        dot = strstr(dbuf, ".bundle");       if (dot) *dot = 0;
        if (!dbuf[0]) {
            fprintf(stderr, "udt-git: cannot derive a target name from '%s'\n", repo);
            return 1;
        }
        dir = dbuf;
    }

    /* 1. clone without a working tree — materialise reads HEAD's tree directly
          through libgit2, so a git checkout would only be wasted disk writes. */
    const char *ga[] = { "git", "clone", "--no-checkout", repo, dir, NULL };
    if (runcmd(ga) != 0) {
        fprintf(stderr, "udt-git: git clone failed\n");
        return 1;
    }

    /* 2. make the target a real UniData account (VOC + all the system files). */
    if (chdir(dir) != 0) {
        fprintf(stderr, "udt-git: cannot enter %s\n", dir);
        return 1;
    }
    const char *owner = getenv("UDT_ACCT_OWNER");
    const char *group = getenv("UDT_ACCT_GROUP");
    if (!owner || !owner[0]) {
        struct passwd *pw = getpwuid(getuid());
        owner = pw ? pw->pw_name : "root";
    }
    if (!group || !group[0]) {
        struct group *gr = getgrgid(getgid());
        group = gr ? gr->gr_name : owner;
    }
    run_newacct(owner, group);
    if (access("VOC", F_OK) != 0) {
        fprintf(stderr,
                "udt-git: newacct did not provision the account "
                "(owner=%s group=%s); check the login/group are valid\n",
                owner, group);
        return 1;
    }

    /* 3. materialise the user's files and records on top, over InterCall. */
    char acctpath[4096];
    if (getcwd(acctpath, sizeof acctpath))
        setenv("MVXACCOUNT", acctpath, 1);
    if (open_account_on())
        setenv("MVX_OPENACCOUNT", "1", 1);

    mv_ctx *ctx = mv_ctx_create();
    emit(mv_git_materialize(ctx, ".git"));
    /* Collect the index-rebuild script while the repo is readable, then close the
       InterCall session before feeding it to a fresh `udt` — so only one session
       is ever open at a time. */
    int nix = 0;
    char *ixscript = collect_index_script(ctx, ".git", &nix);
    mv_ctx_destroy(ctx);
    if (ixscript) {
        FILE *u = popen("udt >/dev/null 2>&1", "w");   /* cwd = the account */
        if (u) {
            fwrite(ixscript, 1, strlen(ixscript), u);
            pclose(u);
            printf("%d index(es) rebuilt\n", nix);
        }
        free(ixscript);
    }
    return 0;
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

    /* clone provisions a NEW account, so it runs before the chdir-into-an-
       existing-account path below (it creates the directory itself). */
    if (!strcmp(sub, "clone"))
        return do_clone(arg(argc, argv, i), arg(argc, argv, i + 1));

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
