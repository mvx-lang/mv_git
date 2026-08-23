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
#include "mvsession.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      /* strncasecmp */
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>

/* This package's registry name, and its version (stamped in at build time from
   the release tag; 0 for an unversioned dev build). */
#define UDTGIT_PKG "mvx-lang/git"
#ifndef UDTGIT_VERSION
#define UDTGIT_VERSION "0"
#endif

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

/* Set up the in-session GIT verb in the current account (defined below); also
   run after clone so a freshly provisioned account can use `GIT` immediately. */
static void deploy_git_verb(void);

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
        /* FILELIST answers name<VM>class, @AM-separated — so stop at the VM too.
           Splitting on @AM alone asked for a file literally called "BP<VM>DIR",
           which opened nothing: every file staged 0 records while every single
           call succeeded.  The engine's own walk has always stopped at both. */
        while (i < len && (unsigned char)p[i] != 0xFE &&
               (unsigned char)p[i] != 0xFD) i++;
        int64_t nl = i - s;
        if (nl > 0 && nl < 240) {
            char name[256], dict[300], ctrl[320];
            memcpy(name, p + s, (size_t)nl);
            name[nl] = '\0';
            snprintf(dict, sizeof dict, "%s.DICT", name);
            emit(mv_git_add(ctx, repo, name, ""));   /* data records */
            emit(mv_git_add(ctx, repo, dict, ""));   /* dictionary   */
            /* The file control: probe the live file for its class and, for
               a hash file, its real modulo (so a clone recreates it at true
               size, keeping a static file static) — "DIR" or
               "hash <modulo> DYNAMIC|STATIC".  But a hash modulo is only a
               suggested default: keep an already-committed one STICKY so this
               account's resize does not overwrite the shipped default and
               ripple out to other clones — only seed it for a new file.

               STAGED ON EVERY ADD, native as well as open (mv_git#15).  This
               is the geometry a checkout reads to drive CREATE.FILE, so
               without it a clone has nothing to build the file from and gets
               the platform default or nothing.  BP/GIT.ADD was fixed for this;
               add_all() — which is what `udt-git add -A` actually calls, never
               the verb — kept the old open-only gate, so every native CLI push
               carried no geometry at all (mv_git#73). */
            char type[MV_GIT_CTL_MAX];
            if (!mv_fileclass(ctx, name, type, sizeof type))
                snprintf(type, sizeof type, "hash");
            if (strncmp(type, "hash", 4) == 0) {
                char committed[MV_GIT_CTL_MAX];
                if (mv_git_committed_control(repo, name, committed,
                                             sizeof committed) >= 0 &&
                    strncmp(committed, "hash", 4) == 0)
                    snprintf(type, sizeof type, "%s", committed);
            }
            /* Under the account's prefix: these paths are built here rather
               than through record_path, so they were the one thing an account
               inside a larger repository still staged at the root (mv_git#44). */
            snprintf(ctrl, sizeof ctrl, "%s%s.DICT/%%FILE%%",
                     mv_git_prefix(), name);
            free(mv_git_stageblob(ctx, repo, ctrl, type));
            if (open) {
                /* %INDEXES%: the file's alternate-key index names, so secondary
                   indexes travel (#10).  Fold @AM to newlines like every record
                   blob; stage only when the file actually has indexes. */
                char ix[8192];
                if (mv_indices(ctx, name, ix, sizeof ix) && ix[0]) {
                    for (char *q = ix; *q; q++)
                        if ((unsigned char)*q == 0xFE) *q = '\n';
                    char ixc[320];
                    snprintf(ixc, sizeof ixc, "%s%s.DICT/%%INDEXES%%",
                             mv_git_prefix(), name);
                    free(mv_git_stageblob(ctx, repo, ixc, ix));
                }
            }
        }
        while (i < len && (unsigned char)p[i] != 0xFE) i++;   /* past the class */
        i++;
    }
    mv_clear(&fl);

    /* The descriptor comes from the engine, not from here: the in-session verb
       stages it through the same call, so a commit does not depend on which of
       the two made it (mv_git#81). */
    {
        char dpath[700], ddesc[2048];
        /* Under the account's own prefix, or an account inside a larger
           repository would write its descriptor and its %FILE% controls at the
           repository ROOT — one account's `.udt` claiming to describe the whole
           repo, and the next one overwriting it (mv_git#44). */
        if (mv_git_desc_for(dpath, sizeof dpath, ddesc, sizeof ddesc,
                            mv_git_prefix(), open))
            free(mv_git_stageblob(ctx, repo, dpath, ddesc));
    }
    printf(open ? ".mv-account + %%FILE%% controls written (open format)\n"
                : ".udt account marker + %%FILE%% controls written (native)\n");
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

/* Like runcmd but with stdout/stderr silenced — for a probe whose only signal
   is its exit status (e.g. `git cat-file -e`). */
static int runcmd_quiet(const char *const cmd[]) {
    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
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

/* Append n bytes of s to a malloc'd growing buffer (NUL-terminated). */
static void sb_app(char **buf, size_t *cap, size_t *len, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        *cap = (*len + n + 1) * 2;
        *buf = realloc(*buf, *cap);
        if (!*buf) mv_fatal("out of memory");
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

/* 1 if the two @AM (0xFE) field-name lists differ as sets (order-independent) —
   i.e. the declared indexes differ from the live ones. */
static int index_set_differs(const char *a, const char *b) {
    for (int dir = 0; dir < 2; dir++) {
        const char *x = dir ? b : a, *y = dir ? a : b;
        for (const char *f = x; *f;) {
            const char *fe = f;
            while (*fe && (unsigned char)*fe != 0xFE) fe++;
            size_t flen = (size_t)(fe - f);
            if (flen) {
                int found = 0;
                for (const char *g = y; *g;) {
                    const char *ge = g;
                    while (*ge && (unsigned char)*ge != 0xFE) ge++;
                    if ((size_t)(ge - g) == flen && memcmp(g, f, flen) == 0) {
                        found = 1; break;
                    }
                    g = *ge ? ge + 1 : ge;
                }
                if (!found) return 1;
            }
            f = *fe ? fe + 1 : fe;
        }
    }
    return 0;
}

/* Detect files whose alternate-key indexes CHANGED — the set declared in
   <file>.DICT/%INDEXES% differs from what is live now — and build a `udt` script
   that recreates them (#10/#11), plus a human report.  Returns the script (or
   NULL when nothing changed), sets *ncreate to the CREATE.INDEX count, *nfiles to
   the changed-file count, and *report to a malloc'd "  file (f1 f2)\n" listing.
   The script is NOT run here: the caller asks the user first, then feeds it to a
   fresh `udt` after the InterCall session closes (ic_execute cannot answer
   CREATE.INDEX's key-length prompt — a blank line in the script does).  Reads
   HEAD like checkout does; mv_indices gives the live set. */
static char *collect_index_changes(mv_ctx *ctx, const char *repo, int *ncreate,
                                   int *nfiles, char **report) {
    *ncreate = 0; *nfiles = 0; *report = NULL;
    char *paths = mv_git_headfiles(ctx, repo);
    if (!paths) return NULL;
    const char *suf = ".DICT/%INDEXES%";
    size_t sl = strlen(suf);
    char *scr = NULL, *rep = NULL;
    size_t scap = 0, slen = 0, rcap = 0, rlen = 0;
    for (char *p = paths; *p;) {
        char *e = p;
        while (*e && (unsigned char)*e != 0xFE) e++;
        size_t pl = (size_t)(e - p);
        if (pl > sl && memcmp(p + pl - sl, suf, sl) == 0) {
            char base[256], path[300];
            snprintf(base, sizeof base, "%.*s", (int)(pl - sl), p);
            snprintf(path, sizeof path, "%.*s", (int)pl, p);
            char *ix = mv_git_catpath(ctx, repo, path);   /* declared, @AM */
            if (ix && *ix) {
                char live[8192];
                if (!mv_indices(ctx, base, live, sizeof live)) live[0] = '\0';
                if (index_set_differs(ix, live)) {
                    (*nfiles)++;
                    char rl[400];
                    sb_app(&rep, &rcap, &rlen, rl,
                           (size_t)snprintf(rl, sizeof rl, "  %s (", base));
                    int first = 1;
                    for (char *f = ix; *f;) {
                        char *fe = f;
                        while (*fe && (unsigned char)*fe != 0xFE) fe++;
                        if (fe > f) {
                            char cl[600];
                            sb_app(&scr, &scap, &slen, cl,
                                   (size_t)snprintf(cl, sizeof cl,
                                       "CREATE.INDEX %s %.*s\n\n",
                                       base, (int)(fe - f), f));
                            (*ncreate)++;
                            char fl[300];
                            sb_app(&rep, &rcap, &rlen, fl,
                                   (size_t)snprintf(fl, sizeof fl, "%s%.*s",
                                       first ? "" : " ", (int)(fe - f), f));
                            first = 0;
                        }
                        f = *fe ? fe + 1 : fe;
                    }
                    sb_app(&rep, &rcap, &rlen, ")\n", 2);
                    char bl[300];
                    sb_app(&scr, &scap, &slen, bl,
                           (size_t)snprintf(bl, sizeof bl,
                               "BUILD.INDEX %s ALL\n", base));
                }
            }
            free(ix);
        }
        p = *e ? e + 1 : e;
    }
    free(paths);
    if (*ncreate == 0) { free(scr); free(rep); return NULL; }
    sb_app(&scr, &scap, &slen, "QUIT\n", 5);
    *report = rep;
    return scr;
}

/* Checking a repo out as an open account writes `mvx.openaccount` into the
   cloner's own config and decides how every later commit here is written, so it
   is asked rather than assumed — the same question, defaults and env override
   mvx-git uses (mv_git#88).  Declined by --no-open-account / the env, or by
   answering no; with no terminal the answer is yes, said out loud, because
   erroring would break every scripted clone. */
static int g_open_flag;                 /* 1 = --open-account, -1 = --no-, 0 = ask */

static int ask_open_account(const char *dir) {
    if (g_open_flag) return g_open_flag > 0;
    const char *env = getenv("MVXGIT_OPEN_ACCOUNT");
    if (env && env[0])
        return !(env[0] == '0' || !strcasecmp(env, "no") ||
                 !strcasecmp(env, "false") || !strcasecmp(env, "off"));
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr,
                "udt-git: '%s' was committed in the open account format — "
                "checking it out as an open account.\n"
                "         (--no-open-account, or MVXGIT_OPEN_ACCOUNT=0, for a "
                "native checkout instead)\n", dir);
        return 1;
    }
    fprintf(stderr,
            "\n'%s' was committed in the open account format: its dictionaries "
            "and file\ncontrols are in the portable shape that moves between MV "
            "platforms.  Keeping\nit open is what lets this account travel back "
            "the same way.\n\n"
            "Make it an open account? [Y/n] ", dir);
    fflush(stderr);
    char buf[16];
    if (!fgets(buf, sizeof buf, stdin)) return 1;
    return !(buf[0] == 'n' || buf[0] == 'N');
}

static int runcmd(const char *const cmd[]);

/* --- a repository of several accounts (mv_git#44) --------------------------
 *
 * Standing at the root of a repository that holds accounts rather than being
 * one: a record operation belongs to each account in turn, because git alone
 * cannot see records and would stage what it can see.
 *
 * ONE PROCESS PER ACCOUNT, unlike uv-git's in-process loop.  A UniData session
 * binds to its account with LOGTO and does not move; a fresh `udt-git -a <acct>`
 * gets a fresh session and hands the licence back when it exits, which is also
 * the honest way to report a failure — the account that failed is the one whose
 * process returned non-zero.  Each child then takes the nested-account path
 * above and prefixes its own records. */
static int subdir_is_account(const char *root, const char *name) {
    char p[4096];
    struct stat sb;
    snprintf(p, sizeof p, "%s/%s", root, name);
    if (stat(p, &sb) != 0 || !S_ISDIR(sb.st_mode)) return 0;
    static const char *marks[] = { ".udt", ".mvx", ".uv", ".mv-account", NULL };
    for (int i = 0; marks[i]; i++) {
        char m[4200];
        snprintf(m, sizeof m, "%s/%s", p, marks[i]);
        if (stat(m, &sb) == 0) return 1;
    }
    /* An account provisioned by newacct before descriptors existed still has a
       VOC; accept that rather than refuse to walk an older repository. */
    snprintf(p, sizeof p, "%s/%s/VOC", root, name);
    return stat(p, &sb) == 0;
}

static int needs_accounts(const char *sub) {
    static const char *rec[] = { "add", "status", "checkout", "restore",
                                 "merge", "cherry-pick", "rm", "diff", NULL };
    for (int i = 0; rec[i]; i++) if (!strcmp(sub, rec[i])) return 1;
    return 0;
}

/* Returns -1 when this is not a repository root holding accounts, so the caller
   carries on with the single-account path. */
static int run_accounts(int argc, char **argv, int subidx) {
    char root[4096];
    struct stat sb;
    if (!getcwd(root, sizeof root)) return -1;
    char gitdir[4200];
    snprintf(gitdir, sizeof gitdir, "%s/.git", root);
    if (stat(gitdir, &sb) != 0) return -1;

    char *names[64];
    int n = 0;
    DIR *d = opendir(root);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 64) {
        if (e->d_name[0] == '.') continue;
        if (subdir_is_account(root, e->d_name)) names[n++] = strdup(e->d_name);
    }
    if (d) closedir(d);
    if (n == 0) return -1;
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0 && strcmp(names[k - 1], names[k]) > 0; k--) {
            char *t = names[k - 1]; names[k - 1] = names[k]; names[k] = t;
        }

    /* The repository's own files — README, docs, whatever sits beside the
       accounts — belong to the repository rather than to any account, so they
       are staged once from here.  Through PLAIN GIT, not the engine: they are
       ordinary files, and standing at the root there is no account for a
       session to bind to — asking the engine here got "the account I/O agent
       did not answer", which is true and beside the point. */
    if (!strcmp(argv[subidx], "add")) {
        const char **ga = malloc((size_t)(n + 4) * sizeof *ga);
        if (ga) {
            int gi = 0;
            ga[gi++] = "git"; ga[gi++] = "add"; ga[gi++] = "-A";
            int any = 0;
            DIR *rd = opendir(root);
            struct dirent *re;
            char **keep = malloc(64 * sizeof *keep);
            int nk = 0;
            while (rd && keep && (re = readdir(rd)) && nk < 60) {
                if (re->d_name[0] == '.') continue;
                int isacct = 0;
                for (int k = 0; k < n; k++)
                    if (!strcmp(re->d_name, names[k])) { isacct = 1; break; }
                if (!isacct) keep[nk++] = strdup(re->d_name);
            }
            if (rd) closedir(rd);
            for (int k = 0; k < nk; k++) { ga[gi++] = keep[k]; any = 1; }
            ga[gi] = NULL;
            if (any) runcmd(ga);
            for (int k = 0; k < nk; k++) free(keep[k]);
            free(keep);
            free(ga);
        }
    }

    int rc = 0;
    for (int k = 0; k < n; k++) {
        if (n > 1) printf("== %s\n", names[k]);
        fflush(stdout);
        char **av = malloc((size_t)(argc + 3) * sizeof *av);
        if (!av) { perror("udt-git"); return 1; }
        int ai = 0;
        av[ai++] = argv[0];
        av[ai++] = "-a";
        av[ai++] = names[k];
        for (int j = subidx; j < argc; j++) av[ai++] = argv[j];
        av[ai] = NULL;
        pid_t pid = fork();
        if (pid == 0) { execv("/proc/self/exe", av); execvp(av[0], av); _exit(127); }
        int st = 0;
        if (pid > 0) waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) rc = 1;
        free(av);
    }
    for (int k = 0; k < n; k++) free(names[k]);
    return rc;
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

    /* A cloned OPEN account carries `.mv-account` in HEAD, but `git clone` does
       not copy the local `mvx.openaccount` flag — it is set here so open-account
       materialisation runs: the committed %FILE% controls and dictionaries are
       in the open form, and the reverse translation (open -> native, incl. the
       dict SM/assoc order) is gated on this flag.  Asked rather than assumed,
       and the same way mvx-git asks (mv_git#88): the flag lands in the user's
       own repository config and decides how every later commit is written. */
    {
        const char *chk[] = { "git", "cat-file", "-e", "HEAD:.mv-account", NULL };
        if (runcmd_quiet(chk) == 0 && ask_open_account(dir)) {
            const char *cfg[] = { "git", "config", "mvx.openaccount", "true", NULL };
            runcmd(cfg);
        }
    }

    /* 3. put an agent in it BEFORE materialising.  Records are reached through
       the account I/O agent, and a just-created account has none — so the
       materialise below failed with "the account I/O agent did not answer" and
       the clone produced an empty account.  uv-git seeds here for the same
       reason; this is that, for UniData. */
    if (!mv_agent_cataloged() && mv_agent_seed() != 0)
        fprintf(stderr, "udt-git clone: could not put an agent into the new "
                        "account; it exists but will hold no records\n");

    /* 4. materialise the user's files and records on top, over InterCall. */
    char acctpath[4096];
    if (getcwd(acctpath, sizeof acctpath))
        setenv("MVXACCOUNT", acctpath, 1);
    if (open_account_on())
        setenv("MVX_OPENACCOUNT", "1", 1);

    mv_ctx *ctx = mv_ctx_create();
    emit(mv_git_materialize(ctx, ".git"));
    /* Detect changed indexes while the repo is readable, then close the InterCall
       session before touching them.  Do NOT rebuild automatically (#11): report
       the changes and ask; only on yes feed the script to a fresh `udt`. */
    int ncreate = 0, nfiles = 0;
    char *report = NULL;
    char *ixscript = collect_index_changes(ctx, ".git", &ncreate, &nfiles,
                                           &report);
    mv_ctx_destroy(ctx);
    if (ixscript) {
        fflush(stdout);   /* the materialise line before the prompt */
        fprintf(stderr, "Indexes have changed for %d file(s):\n%s", nfiles,
                report ? report : "");
        fprintf(stderr, "Rebuild them now? [y/N] ");
        fflush(stderr);
        char ans[16];
        int yes = fgets(ans, sizeof ans, stdin) &&
                  (ans[0] == 'y' || ans[0] == 'Y');
        if (yes) {
            FILE *u = popen("udt >/dev/null 2>&1", "w");   /* cwd = the account */
            if (u) {
                fwrite(ixscript, 1, strlen(ixscript), u);
                pclose(u);
            }
            printf("%d index(es) rebuilt\n", ncreate);
        } else {
            printf("Indexes not rebuilt; re-run the clone to rebuild them.\n");
        }
        free(ixscript);
        free(report);
    }
    deploy_git_verb();   /* the cloned account can run the in-session GIT verb too */
    /* The same closing line uv-git prints.  Without it the caller had no single
       statement that the clone had succeeded — only a record count — and the
       suite, which looks for exactly this, reported every good clone as a
       failure. */
    printf("cloned into %s as a UniData account\n", dir);
    return 0;
}

/* Copy a file byte-for-byte.  Returns 0 on success. */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192];
    size_t r;
    int rc = 0;
    while ((r = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, r, out) != r) { rc = -1; break; }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
}

/* Locate the in-session verb source GIT.udt.b: $MVGIT_VERB, then beside the
   udt-git binary, then $UDTHOME/lib/mvgit.  Returns 1 and fills `out` on hit. */
static int find_verb_src(char *out, size_t n) {
    const char *e = getenv("MVGIT_VERB");
    if (e && *e && access(e, R_OK) == 0) { snprintf(out, n, "%s", e); return 1; }
    char exe[4096];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k > 0) {
        exe[k] = '\0';
        char *d = strrchr(exe, '/');
        if (d) {
            *d = '\0';
            snprintf(out, n, "%s/GIT.udt.b", exe);
            if (access(out, R_OK) == 0) return 1;
        }
    }
    const char *uh = getenv("UDTHOME");
    if (uh && *uh) {
        snprintf(out, n, "%s/lib/mvgit/GIT.udt.b", uh);
        if (access(out, R_OK) == 0) return 1;
    }
    return 0;
}

/* Ensure the manifest at `path` has exactly one line for `name` (its first
   space-delimited field): `fullline`.  Rewrites the file, dropping any prior
   line for `name` (so a re-register updates the version rather than duplicating
   it).  Small files; read all, filter, write back.  Returns 0 on success. */
static int manifest_set(const char *path, const char *name, const char *fullline) {
    size_t nl = strlen(name);
    char *keep = NULL;
    size_t klen = 0;
    FILE *r = fopen(path, "r");
    if (r) {
        char buf[1024];
        while (fgets(buf, sizeof buf, r)) {
            const char *e = buf;
            while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
            if ((size_t)(e - buf) == nl && strncmp(buf, name, nl) == 0)
                continue;                       /* drop the old line for name */
            size_t bl = strlen(buf);
            char *nk = realloc(keep, klen + bl + 1);
            if (!nk) { free(keep); fclose(r); return -1; }
            keep = nk; memcpy(keep + klen, buf, bl); klen += bl; keep[klen] = '\0';
        }
        fclose(r);
    }
    FILE *w = fopen(path, "w");
    if (!w) { free(keep); return -1; }
    if (keep) fputs(keep, w);
    if (klen && keep[klen - 1] != '\n') fputc('\n', w);
    fprintf(w, "%s\n", fullline);
    fclose(w);
    free(keep);
    return 0;
}

/* Register this git install with MVPKG so it manages the package: record
   "mvx-lang/git <version>" in MVPKG's global store manifest and this account's
   manifest (idempotent — a re-register just refreshes the version).  The store
   is $MVPKG_STORE, else $UDTHOME/mvpkg.  `force` (explicit `udt-git register`)
   creates the store if absent; otherwise (a lazy self-register on `init`) it
   registers only when the store already exists — i.e. MVPKG is set up — so a
   plain init before MVPKG is present does not create it prematurely. */
static void self_register(int force) {
    char store[4096];
    const char *s = getenv("MVPKG_STORE");
    if (s && *s) {
        snprintf(store, sizeof store, "%s", s);
    } else {
        const char *uh = getenv("UDTHOME");
        if (!uh || !*uh) return;
        snprintf(store, sizeof store, "%s/mvpkg", uh);
    }
    struct stat st;
    if (stat(store, &st) != 0) {
        if (!force) return;                     /* MVPKG not set up yet */
        if (mkdir(store, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "udt-git: cannot create the MVPKG store %s\n", store);
            return;
        }
    }
    char gman[4200], line[256];
    snprintf(gman, sizeof gman, "%s/installed", store);
    snprintf(line, sizeof line, "%s %s", UDTGIT_PKG, UDTGIT_VERSION);
    manifest_set(gman, UDTGIT_PKG, line);           /* global store: "name version" */
    manifest_set("mvpkg.installed", UDTGIT_PKG, UDTGIT_PKG);   /* account (cwd): "name" */
    if (force)
        printf("udt-git: registered %s %s with MVPKG\n", UDTGIT_PKG, UDTGIT_VERSION);
}

/* After `init`, set up the in-session GIT verb in the current account so the
   user can type `GIT status` etc., not only the udt-git CLI.  Stages GIT.udt.b
   into the account's BP and compiles + catalogs it as the account-local GIT
   verb via a udt session (the same LOCAL FORCE path MVPKG uses).  Non-fatal —
   init already succeeded.  The verb drives the git CallC functions, so it runs
   once the package's CallC library is built (installing git does this); the
   catalog step reports if it is not yet present. */
static void deploy_git_verb(void) {
    char src[4096];
    if (!find_verb_src(src, sizeof src)) {
        fprintf(stderr, "udt-git: GIT.udt.b not found; skipping in-session verb setup"
                        " (set MVGIT_VERB to its path)\n");
        return;
    }
    /* Already in the global catalog?  Then the account needs nothing: GIT
       resolves there for every account on the system.  Copying the source into
       this account's BP and cataloging it LOCAL duplicates what the install
       already did, leaves a private compiled copy that a package upgrade will
       not refresh, and puts two records into BP that a wholesale add then
       commits as if they were the account's own code (mv_git#80). */
    {
        const char *home = getenv("UDTHOME");
        char g[4096];
        if (home && home[0]) {
            snprintf(g, sizeof g, "%s/sys/CTLG/g/GIT", home);
            if (access(g, F_OK) == 0) {
                printf("udt-git: GIT is cataloged globally; nothing to install "
                       "in this account\n");
                return;
            }
        }
    }
    if (access("BP", F_OK) != 0) {
        fprintf(stderr, "udt-git: no BP file here; skipping in-session verb setup\n");
        return;
    }
    if (copy_file(src, "BP/GIT") != 0) {
        fprintf(stderr, "udt-git: could not stage the GIT verb into BP\n");
        return;
    }
    FILE *u = popen("udt >/dev/null 2>&1", "w");   /* cwd = the account */
    if (!u) { fprintf(stderr, "udt-git: could not run udt to catalog the GIT verb\n"); return; }
    fputs("BASIC BP GIT\n", u);
    fputs("CATALOG BP GIT LOCAL FORCE\n", u);
    fputs("QUIT\n", u);            /* leave properly; EOF alone exits non-zero */
    pclose(u);
    /* JUDGE BY THE ARTIFACT, NOT THE EXIT CODE.  A piped `udt` reports whatever
       it likes — install.sh already records that it exits 0 when an internal
       command failed, and this found the reverse: the compile and the catalog
       both SUCCEEDED and the account ran GIT afterwards, while pclose said
       non-zero and clone announced "could not catalog the GIT verb (is the
       package's CallC library built?)".  A false failure on a good clone sends
       the reader hunting a library that was never the problem. */
    if (access("CTLG/GIT", F_OK) == 0)
        printf("udt-git: in-session GIT verb set up in this account (try: GIT STATUS)\n");
    else
        fprintf(stderr, "udt-git: the GIT verb did not catalog into this account"
                        " (no CTLG/GIT); run `BASIC BP GIT` there to see why\n");
}

int main(int argc, char **argv) {
    /* This driver's shell.  Records now come from a SESSION running
       BP/GIT.AGENT (mv_git#45) rather than over InterCall, so there is no stored
       password and history attributes to the person running the command.  The
       session layer is shared with UniVerse and has no default shell, so naming
       it here is what makes these UniData sessions. */
    mvs_set_shell("udt");

    /* --open-account / --no-open-account are clone flags of ours that git never
       sees: answer the open-account question up front instead of being asked.
       Stripped before anything parses or forwards the args. */
    {
        int w = 1;
        for (int a = 1; a < argc; a++) {
            if (!strcmp(argv[a], "--open-account"))    { g_open_flag =  1; continue; }
            if (!strcmp(argv[a], "--no-open-account")) { g_open_flag = -1; continue; }
            argv[w++] = argv[a];
        }
        argc = w;
        argv[argc] = NULL;
    }

    /* optional "-a <account>" before the subcommand */
    int i = 1;
    const char *account = ".";
    if (i < argc && strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
        account = argv[i + 1];
        i += 2;
    }
    if (i >= argc || !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
        /* The bare one-liner said nothing about what this can do, which is how
           `remote` and `push` stayed missing without anyone noticing
           (mv_git#75).  Listing them is the cheapest way to keep the two CLIs
           and the in-session verb visibly in step. */
        fprintf(stderr,
            "usage: udt-git [-a account] <command> [args]\n"
            "commands:\n"
            "  init         create the record repository\n"
            "  clone        clone a repo into a dir (url dir {ref})\n"
            "  add          stage records (-A | file {id})\n"
            "  rm           unstage a record (file id)\n"
            "  status       show staged / changed records\n"
            "  commit       commit staged records (-m msg)\n"
            "  log          commit history\n"
            "  diff         record changes ({file}); --staged for the index\n"
            "  show         committed content of a record (file id)\n"
            "  restore      restore records from HEAD (file)\n"
            "  branch       list or create a branch (name)\n"
            "  checkout     switch branch, update records (name)\n"
            "  merge        merge a branch into this one (name)\n"
            "  cherry-pick  apply one commit (commit)\n"
            "  remote       list or configure remotes"
                            " ({add|set-url|remove} name {url})\n"
            "  fetch        fetch a remote ({remote})\n"
            "  pull         fetch + merge, re-materialising"
                            " ({remote} {branch})\n"
            "  push         push to a remote ({remote} {refspec})\n");
        return 2;
    }
    /* The session-layer diagnostic, before any account handling: it is what
       tells you whether the session, the protocol or the caller is at fault. */
    if (strcmp(argv[i], "agent") == 0) {
        if (chdir(account) != 0) {
            fprintf(stderr, "udt-git: cannot enter %s: %s\n",
                    account, strerror(errno));
            return 1;
        }
        return mv_agent_cmd(argc, argv, i);
    }

    const char *sub = argv[i++];

    /* textconv is a git diff filter (render a record blob legibly for the diff
       view; the blob is untouched) — no account or InterCall session needed. */
    if (!strcmp(sub, "textconv"))
        return mv_git_textconv(i < argc ? argv[i] : "-");

    /* clone provisions a NEW account, so it runs before the chdir-into-an-
       existing-account path below (it creates the directory itself). */
    if (!strcmp(sub, "clone"))
        return do_clone(arg(argc, argv, i), arg(argc, argv, i + 1));

    /* A repository holding accounts, with no -a naming one: visit each. */
    if (!strcmp(account, ".") && needs_accounts(sub)) {
        int rc = run_accounts(argc, argv, i - 1);
        if (rc >= 0) return rc;
    }

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
    /* THE REPOSITORY MAY BE ABOVE THE ACCOUNT (mv_git#44).  An account can sit
       inside a larger repository beside other accounts and ordinary files —
       one repo, one index, one commit — and then its records must commit under
       its own directory rather than at the root, or two accounts would both
       claim `CUST/C1`.  uv-git has worked this way for some time and mvx-git
       now does; this is the same rule here.  An account that owns its .git is
       unaffected: prefix empty, repo ".git", exactly as before. */
    char repobuf[4096] = ".git";
    {
        struct stat gs;
        if (stat(".git", &gs) != 0) {
            char cur[4096];
            if (getcwd(cur, sizeof cur)) {
                char acctdir[4096];
                snprintf(acctdir, sizeof acctdir, "%s", cur);
                for (;;) {
                    char *slash = strrchr(cur, '/');
                    if (!slash || slash == cur) break;
                    *slash = '\0';
                    char probe[4200];
                    snprintf(probe, sizeof probe, "%s/.git", cur);
                    if (stat(probe, &gs) == 0) {
                        snprintf(repobuf, sizeof repobuf, "%s", probe);
                        char pfx[4096];
                        snprintf(pfx, sizeof pfx, "%s/", acctdir + strlen(cur) + 1);
                        mv_git_set_prefix(pfx);
                        break;
                    }
                }
            }
        }
    }
    const char *repo = repobuf;
    const char *p0 = arg(argc, argv, i);
    const char *p1 = arg(argc, argv, i + 1);
    const char *p2 = arg(argc, argv, i + 2);   /* remote add <name> <url> */
    int rc = 0;

    if (!strcmp(sub, "init")) {
        emit(mv_git_init(ctx, repo));
        deploy_git_verb();   /* also set up the in-session GIT verb here */
        /* The CLI reaches records through an account I/O agent, so an account
           with no agent in it cannot be read at all — `init` succeeded and the
           very next `add` failed with "the agent did not answer".  Seed it here,
           where the account is first set up, exactly as uv-git does. */
        if (!mv_agent_cataloged() && mv_agent_seed() != 0)
            fprintf(stderr, "udt-git: could not compile BP/GIT.AGENT in this "
                            "account — the CLI will not be able to reach its "
                            "records (the in-session GIT verb still works)\n");
        self_register(0);    /* adopt into MVPKG if it is already set up */
    } else if (!strcmp(sub, "register")) {
        self_register(1);    /* explicit: record this install with MVPKG */
    } else if (!strcmp(sub, "add")) {
        /* bare `add`, `add -A`, or `add .` stages the whole account */
        if (!p0[0] || !strcmp(p0, "-A") || !strcmp(p0, "--all") ||
            !strcmp(p0, "."))
            add_all(ctx, repo);
        else
            emit(mv_git_add(ctx, repo, p0, p1));
    } else if (!strcmp(sub, "version") || !strcmp(sub, "--version")) {
        char self[128];
        snprintf(self, sizeof self, "udt-git %s", UDTGIT_VERSION);
        char *v = mv_git_versions(self);
        fputs(v ? v : "", stdout);
        free(v);
    } else if (!strcmp(sub, "remote")) {
        /* remote {add|set-url|remove} <name> {url} — bare `remote` lists.
           These four were in uv-git and the in-session verb but never here, so
           a UniData account could not be pointed at a remote or pushed from the
           shell at all: you had to write [remote "origin"] into .git/config by
           hand and push from inside a udt session (mv_git#75). */
        emit(mv_git_remote(ctx, repo, p0, p1, p2));
    } else if (!strcmp(sub, "push")) {
        emit(mv_git_push(ctx, repo, p0, p1));
    } else if (!strcmp(sub, "fetch")) {
        emit(mv_git_fetch(ctx, repo, p0));
    } else if (!strcmp(sub, "pull")) {
        emit(mv_git_pull(ctx, repo, p0, p1));
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
