/*
 * MVX — a native compiler and runtime for Pick/MultiValue BASIC.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.  There is NO WARRANTY, to
 * the extent permitted by law; see the LICENSE file for details.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* mvx-git — git for MVX accounts.
 *
 * Inside an MVX account (a directory carrying a .mvx descriptor) mvx-git drives
 * the record-git engine directly: it reads and writes the account's hash-file
 * records straight to/from git objects in the account's own .git via libgit2 —
 * exactly the engine the BASIC GIT verb uses (#58).  The working tree is the
 * live records, so there is no export copy; commit/add/checkout touch the
 * records.  The .git is an ordinary repository (a host like GitHub sees a
 * normal repo); a plain git clone/checkout materialises the records as files
 * "incorrectly", and the committed .mvx marks the result as an account so the
 * account tooling rebuilds it.
 *
 * Everything else is forwarded verbatim to the real git: commands the engine
 * does not implement, and any command run outside a record-git account (so
 * `alias git=mvx-git` still works, and a legible account tracked by ordinary
 * git behaves normally).  mvx-git-adopt is used only to *adopt* a checkout
 * made by plain git — turning a cloned legible directory into a live account.
 */

#include "mvxgit.h"

#include <dirent.h>
#include <errno.h>
#include <git2.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>          /* _NSGetExecutablePath */
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- forwarding to real git ------------------------------------------- */

/* git subcommands that change the working tree, so a legible account may need
 * rebuilding into hash files afterwards (the plain-git adoption path). */
static int tree_changing(const char *sub) {
    static const char *ops[] = {
        "clone", "checkout", "switch", "pull", "merge", "rebase",
        "reset", "restore", "cherry-pick", "revert", "stash", "am", NULL};
    for (int i = 0; ops[i]; i++)
        if (strcmp(sub, ops[i]) == 0) return 1;
    return 0;
}

/* git clone options that consume the following argument. */
static int clone_opt_takes_value(const char *o) {
    static const char *v[] = {
        "-b", "--branch", "-o", "--origin", "-u", "--upload-pack",
        "--depth", "--reference", "--reference-if-able", "-j", "--jobs",
        "-c", "--config", "--template", "--separate-git-dir", "--filter",
        "--shallow-since", "--shallow-exclude", "--server-option", NULL};
    for (int i = 0; v[i]; i++)
        if (strcmp(o, v[i]) == 0) return 1;
    return 0;
}

/* Fork/exec argv, inheriting stdio, and return its exit code. */
static int run(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) { perror("mvx-git: fork"); return 1; }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "mvx-git: cannot run %s: %s\n",
                argv[0], strerror(errno));
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static void dir_from_url(const char *url, char *out, size_t cap) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", url);
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = '\0';
    const char *base = tmp;
    for (size_t i = 0; tmp[i]; i++)
        if (tmp[i] == '/' || tmp[i] == ':') base = tmp + i + 1;
    snprintf(out, cap, "%s", base);
    size_t bn = strlen(out);
    if (bn > 4 && strcmp(out + bn - 4, ".git") == 0) out[bn - 4] = '\0';
}

static int clone_target(int argc, char **argv, int subidx,
                        char *out, size_t cap) {
    const char *pos[4];
    int np = 0;
    for (int i = subidx + 1; i < argc && np < 4; i++) {
        if (argv[i][0] == '-') {
            if (clone_opt_takes_value(argv[i])) i++;
            continue;
        }
        pos[np++] = argv[i];
    }
    if (np >= 2) { snprintf(out, cap, "%s", pos[1]); return 1; }
    if (np == 1) { dir_from_url(pos[0], out, cap); return 1; }
    return 0;
}

/* --- account detection ------------------------------------------------- */

static int is_account(const char *dir) {
    char p[PATH_MAX];
    struct stat sb;
    /* `.mvx` natively, `.mv-account` in the open account format. */
    snprintf(p, sizeof p, "%s/.mvx", dir);
    if (stat(p, &sb) == 0) return 1;
    snprintf(p, sizeof p, "%s/.mv-account", dir);
    return stat(p, &sb) == 0;
}

/* True if the account carries its own git repository (a .git directly in the
 * account root).  A standalone account does; an account that is a subdirectory
 * of a larger repo does not — it is tracked by that repo, so mvx-git forwards
 * to it and the account is rebuilt with mvx-git-adopt (or an mvx-git clone). */
static int has_own_git(const char *dir) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/.git", dir);
    struct stat sb;
    return stat(p, &sb) == 0;
}

static int account_from_cwd(char *out, size_t cap) {
    char cur[PATH_MAX];
    if (!getcwd(cur, sizeof cur)) return 0;
    for (;;) {
        if (is_account(cur)) { snprintf(out, cap, "%s", cur); return 1; }
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) return 0;
        *slash = '\0';
    }
}

/* Adopt a plain-git checkout: mvx-git-adopt (import) builds the live hash
 * files from a cloned/checked-out legible directory.  Found via $MVXCONVERT or
 * PATH.  This is the only place mvx-git needs convert — never on its own
 * record-git add/commit/checkout. */
static void convert_import(const char *acct) {
    fprintf(stderr, "mvx-git: rebuilding account %s\n", acct);
    const char *tool = getenv("MVXCONVERT");
    if (!tool || !tool[0]) tool = "mvx-git-adopt";
    char *rargv[3] = {(char *)tool, (char *)acct, NULL};
    if (run(rargv) != 0)
        fprintf(stderr, "mvx-git: account rebuild failed\n");
}

/* True if the cloned HEAD tree carries the descriptor `name` — checked in the
   git objects, since a --no-checkout clone has an empty working tree. */
static int head_has(const char *acct, const char *name) {
    char rp[PATH_MAX];
    snprintf(rp, sizeof rp, "%s/.git", acct);
    git_libgit2_init();
    git_repository *repo = NULL;
    int yes = 0;
    if (git_repository_open(&repo, rp) == 0) {
        git_object *obj = NULL;
        if (git_revparse_single(&obj, repo, "HEAD^{tree}") == 0) {
            git_tree_entry *te = NULL;
            if (git_tree_entry_bypath(&te, (git_tree *)obj, name) == 0) {
                yes = 1;
                git_tree_entry_free(te);
            }
            git_object_free(obj);
        }
        git_repository_free(repo);
    }
    return yes;
}

/* An MV account descriptor is present: `.mv-account` (open form), `.mvx` (native
   MVX) or `.udt` (native UniData).  A native foreign form still materialises —
   best effort — but the account may not be fully functional here. */
static int head_is_account(const char *acct) {
    return head_has(acct, ".mv-account") || head_has(acct, ".mvx") ||
           head_has(acct, ".udt");
}

/* True if the cloned HEAD carries the *open* descriptor `.mv-account` — the repo
   was committed in the open account format, so the materialised native account
   is an open account and status/diff must translate to open-space.  Cloning such
   a repo turns the opt-in on automatically, without needing --open-account. */
static int head_is_open(const char *acct) {
    return head_has(acct, ".mv-account");
}

/* Append n bytes of s to a malloc'd growing buffer (NUL-terminated). */
static void sb_app(char **buf, size_t *cap, size_t *len, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        *cap = (*len + n + 1) * 2;
        *buf = realloc(*buf, *cap);
        if (!*buf) { perror("mvx-git"); exit(1); }
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

/* Collect the alternate-key indexes declared in HEAD's <file>.DICT/%INDEXES%
   controls into a "<file> <field>\n"-per-line list to (re)build (#11), plus a
   human report "  file (f1 f2)\n".  Returns NULL when none.  Reads HEAD like the
   checkout does (mv_git_headfiles/mv_git_catpath). */
static char *collect_index_list(mv_ctx *ctx, const char *repo, int *ncreate,
                                int *nfiles, char **report) {
    *ncreate = 0; *nfiles = 0; *report = NULL;
    char *paths = mv_git_headfiles(ctx, repo);
    if (!paths) return NULL;
    const char *suf = ".DICT/%INDEXES%";
    size_t sl = strlen(suf);
    char *list = NULL, *rep = NULL;
    size_t lcap = 0, llen = 0, rcap = 0, rlen = 0;
    for (char *p = paths; *p;) {
        char *e = p;
        while (*e && (unsigned char)*e != 0xFE) e++;
        size_t pl = (size_t)(e - p);
        if (pl > sl && memcmp(p + pl - sl, suf, sl) == 0) {
            char base[256], path[300];
            snprintf(base, sizeof base, "%.*s", (int)(pl - sl), p);
            snprintf(path, sizeof path, "%.*s", (int)pl, p);
            char *ix = mv_git_catpath(ctx, repo, path);   /* @AM field names */
            if (ix && *ix) {
                (*nfiles)++;
                char rl[400];
                sb_app(&rep, &rcap, &rlen, rl,
                       (size_t)snprintf(rl, sizeof rl, "  %s (", base));
                int first = 1;
                for (char *f = ix; *f;) {
                    char *fe = f;
                    while (*fe && (unsigned char)*fe != 0xFE) fe++;
                    if (fe > f) {
                        char ll[600];
                        sb_app(&list, &lcap, &llen, ll,
                               (size_t)snprintf(ll, sizeof ll, "%s %.*s\n",
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
            }
            free(ix);
        }
        p = *e ? e + 1 : e;
    }
    free(paths);
    if (*ncreate == 0) { free(list); free(rep); return NULL; }
    *report = rep;
    return list;
}

/* Materialise a cloned account directly from its git objects into the backend —
   the open form never lands on disk, no adopt tool is run — then provision it
   (cataloged BP, linked packages) with BUILD.  Alternate-key indexes declared in
   %INDEXES% are (re)built only after asking (#11): CREATE-INDEX builds the real
   LMDB index; the store already serves the declared index, so it is optional. */
static void materialize_clone(const char *acct) {
    fprintf(stderr, "mvx-git: materialising account %s\n", acct);
    char cwd0[PATH_MAX];
    if (!getcwd(cwd0, sizeof cwd0)) cwd0[0] = '\0';
    if (chdir(acct) != 0) {
        fprintf(stderr, "mvx-git: cannot enter %s\n", acct);
        return;
    }
    setenv("MVXACCOUNT", ".", 1);
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_materialize(ctx, ".git");
    free(r);
    int ncreate = 0, nfiles = 0;
    char *report = NULL;
    char *ixlist = collect_index_list(ctx, ".git", &ncreate, &nfiles, &report);
    mv_ctx_destroy(ctx);
    const char *mvx = getenv("MVX");
    if (!mvx || !mvx[0]) mvx = "mvx";
    setenv("MVXPRIV", "developer", 1);          /* BUILD catalogs BP */
    char *bargv[] = {(char *)mvx, "-a", ".", "-c", "BUILD", NULL};
    run(bargv);
    if (ixlist) {
        fflush(stdout);
        fprintf(stderr, "Indexes have changed for %d file(s):\n%s", nfiles,
                report ? report : "");
        fprintf(stderr, "Rebuild them now? [y/N] ");
        fflush(stderr);
        char ans[16];
        int yes = fgets(ans, sizeof ans, stdin) &&
                  (ans[0] == 'y' || ans[0] == 'Y');
        if (yes) {
            for (char *l = ixlist; *l;) {
                char *le = strchr(l, '\n');
                size_t n = le ? (size_t)(le - l) : strlen(l);
                char cmd[700];
                snprintf(cmd, sizeof cmd, "CREATE-INDEX %.*s", (int)n, l);
                char *cargv[] = {(char *)mvx, "-a", ".", "-c", cmd, NULL};
                run(cargv);
                l = le ? le + 1 : l + n;
            }
            printf("%d index(es) rebuilt\n", ncreate);
        } else {
            printf("Indexes not rebuilt; re-run the clone to rebuild them.\n");
        }
        free(ixlist);
        free(report);
    }
    if (cwd0[0] && chdir(cwd0) != 0) { /* best effort */ }
}

/* The open account format is opt-in per account via the git config flag
   `mvx.openaccount` (the core.autocrlf analogue).  These read/set it in the
   account's .git/config and surface it to the runtime as $MVX_OPENACCOUNT, so
   both the in-process engine and the mvx-git-adopt subprocess honour it. */
static int open_config_on(const char *acct) {
    /* Read `mvx.openaccount` straight from the account's .git/config.  A plain
       text scan (not libgit2) so the result is identical across libgit2
       versions — get_bool behaved differently on the CI toolchain. */
    char cfgpath[PATH_MAX + 16];
    snprintf(cfgpath, sizeof cfgpath, "%s/.git/config", acct);
    FILE *f = fopen(cfgpath, "r");
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
        on = strncasecmp(v, "true", 4) == 0 || *v == '1' ||
             strncasecmp(v, "yes", 3) == 0 || strncasecmp(v, "on", 2) == 0;
        break;
    }
    fclose(f);
    return on;
}

static void open_config_set(const char *acct) {
    char cfgpath[PATH_MAX + 16];
    snprintf(cfgpath, sizeof cfgpath, "%s/.git/config", acct);
    git_libgit2_init();
    git_config *cfg = NULL;
    if (git_config_open_ondisk(&cfg, cfgpath) == 0) {
        git_config_set_bool(cfg, "mvx.openaccount", 1);
        git_config_free(cfg);
    }
}

/* Surface the account's open-account flag to the runtime through the env. */
static void apply_open_env(const char *acct) {
    if (open_config_on(acct)) setenv("MVX_OPENACCOUNT", "1", 1);
}

static int ask_create_account(const char *acct) {
    const char *env = getenv("MVXGIT_CREATE");
    if (env && env[0] && env[0] != '0' && strcasecmp(env, "no") != 0)
        return 1;
    if (!isatty(STDIN_FILENO)) return 0;
    fprintf(stderr, "Directory %s is not an MVX account. "
                    "Create one here? (y/N) ", acct);
    fflush(stderr);
    char buf[16];
    if (!fgets(buf, sizeof buf, stdin)) return 0;
    return buf[0] == 'y' || buf[0] == 'Y';
}

/* --- record-git engine path ------------------------------------------- */

/* Subcommands the record-git engine implements directly. */
static int engine_sub(const char *sub) {
    static const char *ops[] = {
        "init", "add", "rm", "commit", "status", "log", "diff", "show",
        "branch", "checkout", "merge", "cherry-pick", "restore",
        "diff-setup", NULL};
    for (int i = 0; ops[i]; i++)
        if (strcmp(sub, ops[i]) == 0) return 1;
    return 0;
}

/* Print engine output (attribute-mark separated) as newline-separated lines. */
static void print_out(char *s) {
    if (!s) return;
    for (char *p = s; *p; p++)
        if (*p == (char)0xFE) *p = '\n';
    if (*s) { fputs(s, stdout); fputc('\n', stdout); }
    free(s);
}

/* Append b (and a leading @AM if a already has content) to *a. */
static void join(char **a, const char *b) {
    if (!b || !*b) return;
    size_t la = *a ? strlen(*a) : 0, lb = strlen(b);
    char *r = malloc(la + lb + 2);
    if (!r) return;
    if (la) { memcpy(r, *a, la); r[la++] = (char)0xFE; }
    memcpy(r + la, b, lb + 1);
    free(*a);
    *a = r;
}

/* `add -A` is a two-fold process, which is what makes mvx-git a drop-in for
 * git rather than an MV-only tool:
 *
 *   1. Exactly what git does — stage every on-disk file in the working tree,
 *      honouring .gitignore, executable bits, top-level and nested paths, and
 *      deletions (git also treats a nested repo as a submodule gitlink here).
 *   2. Then, only when the directory is an MVX account, stage the records of
 *      every MV file canonically (the record-git form, which drops the trailing
 *      newline the dir-driver file carries — so `status`/`commit`, which read
 *      records through the driver, agree with what was staged; a git-native
 *      blob of the same file would show a permanent phantom modification).
 *      Because step 2 runs last, MV files end up in their canonical form while
 *      genuinely plain files (README, scripts, submodules) keep the git-native
 *      blob and executable bit step 1 gave them.
 *
 * An MV file is a directory that either has a dictionary (`<name>.DICT`) or is
 * itself the dictionary of an existing file (`<base>` for `<base>.DICT`); a
 * plain directory (server/, test/, docs submodule) has neither and is left to
 * step 1.  LMDB-backed files have no on-disk directory at all and are found via
 * the file list — but only when an LMDB store already exists, so a directory-
 * only account is never given a spurious mvxdata.lmdb.
 */
static int is_mv_file(const char *acct, const char *n) {
    struct stat sb;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s.DICT/%%FILE%%", acct, n);   /* dictionary control */
    return stat(p, &sb) == 0;
}

static char *add_all(mv_ctx *ctx, const char *repo, const char *acct) {
    char *out = NULL;

    char *r = mv_git_adddisk(ctx, repo);          /* 1. git's own add */
    join(&out, r);
    free(r);

    if (is_account(acct)) {                          /* 2. MV files, canonical */
        DIR *d = opendir(acct);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            const char *n = e->d_name;
            if (n[0] == '.') continue;
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", acct, n);
            struct stat sb;
            if (stat(p, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
            if (!is_mv_file(acct, n)) continue;
            r = mv_git_add(ctx, repo, n, "");
            join(&out, r);
            free(r);
        }
        if (d) closedir(d);

        /* LMDB-backed files (records not on disk), only if the store exists. */
        char lmdbp[PATH_MAX];
        snprintf(lmdbp, sizeof lmdbp, "%s/mvxdata.lmdb", acct);
        struct stat lsb;
        if (stat(lmdbp, &lsb) == 0) {
            mv_value fl;
            mv_init(&fl);
            mv_filelist(ctx, &fl);                  /* name<VM>type, @AM-sep */
            char nb[40];
            const char *p;
            int64_t len = mv_val_chars(&fl, nb, sizeof nb, &p);
            int64_t i = 0;
            while (i < len) {
                int64_t s = i;
                while (i < len && (unsigned char)p[i] != 0xFE &&
                       (unsigned char)p[i] != 0xFD)
                    i++;
                int64_t nl = i - s;
                if (nl > 0 && nl < 256) {
                    char name[256];
                    memcpy(name, p + s, (size_t)nl);
                    name[nl] = '\0';
                    char fp[PATH_MAX];
                    snprintf(fp, sizeof fp, "%s/%s", acct, name);
                    struct stat ns;
                    /* on-disk directories were handled above; stage only the
                       files that have no directory, i.e. the LMDB hash files. */
                    if (stat(fp, &ns) != 0 || !S_ISDIR(ns.st_mode)) {
                        r = mv_git_add(ctx, repo, name, "");
                        join(&out, r);
                        free(r);
                        /* Its dictionary lives in LMDB too (no on-disk .DICT
                           directory for git's own add to catch), so stage its
                           records at <name>.DICT/<id> — open_named opens the
                           dictionary for a name ending in .DICT. */
                        char dictname[300];
                        snprintf(dictname, sizeof dictname, "%s.DICT", name);
                        r = mv_git_add(ctx, repo, dictname, "");
                        join(&out, r);
                        free(r);
                    }
                }
                while (i < len && (unsigned char)p[i] != 0xFE) i++;
                if (i < len) i++;
            }
            mv_clear(&fl);
        }

        /* 3. Open account format (mvx.openaccount): the working tree stays a
           native account, but the git objects carry the portable open form —
           normalise the staged %FILE% controls to DIR/hash and store .mvx at
           .mv-account. */
        if (mv_openaccount()) {
            r = mv_git_openform(ctx, repo);
            join(&out, r);
            free(r);
        }
    }
    return out ? out : strdup("nothing to stage");
}

static const char *opt_value(int argc, char **argv, int from,
                             const char *flag) {
    size_t fl = strlen(flag);
    for (int i = from; i < argc; i++) {
        if (!strcmp(argv[i], flag) && i + 1 < argc) return argv[i + 1];
        if (!strncmp(argv[i], flag, fl) && argv[i][fl]) return argv[i] + fl;
    }
    return NULL;
}

/* Nth (0-based) positional (non-option) argument after the subcommand. */
static const char *positional(int argc, char **argv, int subidx, int nth) {
    int seen = 0;
    for (int i = subidx + 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (seen++ == nth) return argv[i];
    }
    return NULL;
}

/* Configure the tidy-diff textconv driver for the repo at `.git` (idempotent):
   a `diff.mvxrec.textconv` pointing at this binary, and a `.gitattributes`
   mapping every path to it.  Display only — the stored blobs are unchanged.
   Runs at `init` and via `mvx-git diff-setup` on an existing account. */
static void setup_textconv(void) {
    char self[PATH_MAX] = "mvx-git";
#ifdef __APPLE__
    uint32_t sz = sizeof self;
    if (_NSGetExecutablePath(self, &sz) != 0) snprintf(self, sizeof self, "mvx-git");
#else
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) self[n] = '\0'; else snprintf(self, sizeof self, "mvx-git");
#endif
    git_libgit2_init();
    git_config *cfg = NULL;
    if (git_config_open_ondisk(&cfg, ".git/config") == 0) {
        char val[PATH_MAX + 16];
        snprintf(val, sizeof val, "%s textconv", self);
        git_config_set_string(cfg, "diff.mvxrec.textconv", val);
        git_config_set_bool(cfg, "diff.mvxrec.cachetextconv", 0);
        git_config_free(cfg);
    }
    int have = 0;
    FILE *r = fopen(".gitattributes", "r");
    if (r) { char l[256]; while (fgets(l, sizeof l, r)) if (strstr(l, "diff=mvxrec")) { have = 1; break; } fclose(r); }
    if (!have) { FILE *w = fopen(".gitattributes", "a"); if (w) { fputs("* diff=mvxrec\n", w); fclose(w); } }
}

static int engine_run(const char *acct, const char *sub,
                      int argc, char **argv, int subidx) {
    setenv("MVXACCOUNT", acct, 1);
    if (chdir(acct) != 0) {
        fprintf(stderr, "mvx-git: cannot enter %s\n", acct);
        return 1;
    }
    mv_ctx *ctx = mv_ctx_create();
    const char *repo = ".git";
    const char *p0 = positional(argc, argv, subidx, 0);
    const char *p1 = positional(argc, argv, subidx, 1);
    char *out = NULL;

    if (!strcmp(sub, "init")) {
        out = mv_git_init(ctx, repo);
        setup_textconv();                 /* tidy-diff textconv driver */
    } else if (!strcmp(sub, "diff-setup")) {
        setup_textconv();
        out = strdup("tidy-diff configured (diff.mvxrec textconv + .gitattributes)");
    } else if (!strcmp(sub, "status")) {
        out = mv_git_status(ctx, repo);
    } else if (!strcmp(sub, "add")) {
        int all = 0;
        for (int i = subidx + 1; i < argc; i++)
            if (!strcmp(argv[i], "-A") || !strcmp(argv[i], "--all") ||
                !strcmp(argv[i], "."))
                all = 1;
        if (all) out = add_all(ctx, repo, acct);
        else if (p0) out = mv_git_add(ctx, repo, p0, p1 ? p1 : "");
        else out = strdup("usage: mvx-git add <file> [id] | -A");
    } else if (!strcmp(sub, "rm")) {
        if (p0) out = mv_git_rm(ctx, repo, p0, p1 ? p1 : "");
        else out = strdup("usage: mvx-git rm <file> [id]");
    } else if (!strcmp(sub, "commit")) {
        const char *msg = opt_value(argc, argv, subidx + 1, "-m");
        out = mv_git_commit(ctx, repo, msg ? msg : "");
    } else if (!strcmp(sub, "log")) {
        const char *n = opt_value(argc, argv, subidx + 1, "-n");
        if (!n && p0) n = p0;                       /* `log 5` */
        if (!n)                                     /* `log -5` */
            for (int i = subidx + 1; i < argc; i++)
                if (argv[i][0] == '-' && argv[i][1] >= '1' && argv[i][1] <= '9')
                    n = argv[i] + 1;
        out = mv_git_log(ctx, repo, n ? n : "20");
    } else if (!strcmp(sub, "diff")) {
        out = mv_git_diff(ctx, repo, p0 ? p0 : "");
    } else if (!strcmp(sub, "show")) {
        if (p0 && p1) out = mv_git_show(ctx, repo, p0, p1);
        else out = strdup("usage: mvx-git show <file> <id>");
    } else if (!strcmp(sub, "branch")) {
        out = mv_git_branch(ctx, repo, p0 ? p0 : "");
    } else if (!strcmp(sub, "checkout")) {
        if (p0) out = mv_git_checkout(ctx, repo, p0);
        else out = strdup("usage: mvx-git checkout <branch>");
    } else if (!strcmp(sub, "merge")) {
        if (p0) out = mv_git_merge(ctx, repo, p0);
        else out = strdup("usage: mvx-git merge <branch>");
    } else if (!strcmp(sub, "cherry-pick")) {
        if (p0) out = mv_git_cherrypick(ctx, repo, p0);
        else out = strdup("usage: mvx-git cherry-pick <commit>");
    } else if (!strcmp(sub, "restore")) {
        if (p0) out = mv_git_restore(ctx, repo, p0);
        else out = strdup("usage: mvx-git restore <file>");
    }

    print_out(out);
    mv_ctx_destroy(ctx);
    return 0;
}

int main(int argc, char **argv) {
    /* --open-account is an mvx-git-only clone flag (git never sees it): check
       the checkout out with the open account format turned on.  Strip it from
       the args before anything parses or forwards them. */
    int want_open = 0;
    {
        int w = 1;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--open-account") == 0) { want_open = 1; continue; }
            argv[w++] = argv[i];
        }
        argc = w;
        argv[argc] = NULL;
    }

    /* The subcommand is the first non-option argument. */
    const char *sub = NULL;
    int subidx = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') { sub = argv[i]; subidx = i; break; }
        if (strcmp(argv[i], "-C") == 0 || strcmp(argv[i], "-c") == 0) i++;
    }

    /* `textconv` is a git diff filter, not an account operation: git runs
       `mvx-git textconv <tempfile>` to render a record blob legibly for the
       diff view (the blob is untouched).  Handle it before any account
       detection so it works from anywhere. */
    if (sub && !strcmp(sub, "textconv"))
        return mv_git_textconv(subidx + 1 < argc ? argv[subidx + 1] : "-");

    /* Record-git path: an engine command in an MVX account (a .mvx marks it)
     * that has its own .git — or `init`, which creates one — drives the engine
     * on that .git: records straight to/from git objects, no export copy, no
     * convert.  A subdirectory account (no .git of its own) is tracked by the
     * enclosing repo, so it falls through to plain git below. */
    char acct[PATH_MAX];
    if (sub && engine_sub(sub) && account_from_cwd(acct, sizeof acct) &&
        (has_own_git(acct) || !strcmp(sub, "init"))) {
        apply_open_env(acct);          /* mvx.openaccount -> $MVX_OPENACCOUNT */
        return engine_run(acct, sub, argc, argv, subidx);
    }

    /* Otherwise forward to real git.  A clone is cloned `--no-checkout`: the
     * account is materialised directly from the git objects into its backend,
     * so the open form never lands on disk and no adopt tool is run. */
    int is_clone = sub && strcmp(sub, "clone") == 0;
    char **gargv = malloc((size_t)(argc + 2) * sizeof *gargv);
    if (!gargv) { perror("mvx-git"); return 1; }
    gargv[0] = "git";
    int gi = 1;
    for (int i = 1; i < argc; i++) {
        gargv[gi++] = argv[i];
        if (i == subidx && is_clone) gargv[gi++] = "--no-checkout";
    }
    gargv[gi] = NULL;
    int code = run(gargv);
    free(gargv);

    if (code != 0 || !sub || !tree_changing(sub)) return code;

    if (is_clone) {
        if (!clone_target(argc, argv, subidx, acct, sizeof acct))
            return code;
        if (head_is_account(acct)) {
            /* A native foreign account (.udt = native UniData) is not the open
               interchange form: materialise it best-effort so the files and
               records are restored, but warn that the account may not be fully
               functional here (its %FILE% controls and system verbs are in the
               source platform's form). */
            if (head_has(acct, ".udt") && !head_is_open(acct))
                fprintf(stderr,
                        "mvx-git: warning: '%s' is a native UniData account "
                        "(.udt); restoring files and records, but the account "
                        "may not be fully functional on MVX — commit it with "
                        "mvx.openaccount set for a portable checkout\n", acct);
            /* persist the opt-in when asked, or when HEAD is itself open-form */
            if (want_open || head_is_open(acct)) open_config_set(acct);
            materialize_clone(acct);              /* direct: git objects -> backend */
        } else {
            /* not an MVX account: a plain --no-checkout clone needs its working
               tree populated the normal way. */
            char *co[] = {"git", "-C", acct, "checkout", NULL};
            run(co);
            if (ask_create_account(acct)) convert_import(acct);
        }
        return code;
    }

    /* Any other tree-changing command forwarded to plain git (pull, rebase,
     * reset, …) leaves the account's working tree as checked-out record files;
     * rebuild the live hash files from that directory form. */
    if (account_from_cwd(acct, sizeof acct) && is_account(acct)) {
        apply_open_env(acct);
        convert_import(acct);
    }

    return code;
}
