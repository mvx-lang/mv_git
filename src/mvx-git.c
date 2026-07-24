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

/* mvx-git — a drop-in wrapper around git for MVX accounts.
 *
 * Every command is forwarded verbatim to the real git, so mvx-git is a
 * complete git replacement (you can `alias git=mvx-git`).  The one
 * addition: after a command that changes the working tree — clone,
 * checkout, switch, pull, merge, rebase, reset, restore, cherry-pick,
 * revert, stash — succeeds in a directory that is an MVX account (it
 * carries a .mvx descriptor), mvx-git rebuilds the account so its hash
 * files match the git-tracked legible form.  The rebuild is exactly
 * `mvx -a <account> -c CONVERT-ACCOUNT` (developer privilege); no MVX
 * internals are changed.  In an ordinary repository it does nothing
 * extra and behaves precisely like git.
 *
 * The real git is found on PATH as "git"; the MVX shell as $MVX, else
 * "mvx" on PATH.
 */

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* git subcommands that change the working tree, so the account's hash
 * files may need rebuilding from the updated legible records. */
static int tree_changing(const char *sub) {
    static const char *ops[] = {
        "clone", "checkout", "switch", "pull", "merge", "rebase",
        "reset", "restore", "cherry-pick", "revert", "stash", "am", NULL};
    for (int i = 0; ops[i]; i++)
        if (strcmp(sub, ops[i]) == 0) return 1;
    return 0;
}

/* git clone options that consume the following argument (so it is not
 * mistaken for the URL or target directory). */
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

/* Derive the directory `git clone` creates from a repository URL:
 * the last path component with any trailing "/" and ".git" removed. */
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

/* Account directory a clone created: the explicit target argument if
 * given, else the URL basename.  Returns 0 if it cannot be determined. */
static int clone_target(int argc, char **argv, int subidx,
                        char *out, size_t cap) {
    const char *pos[4];
    int np = 0;
    for (int i = subidx + 1; i < argc && np < 4; i++) {
        if (argv[i][0] == '-') {
            if (clone_opt_takes_value(argv[i])) i++;   /* skip its value */
            continue;
        }
        pos[np++] = argv[i];
    }
    if (np >= 2) { snprintf(out, cap, "%s", pos[1]); return 1; }
    if (np == 1) { dir_from_url(pos[0], out, cap); return 1; }
    return 0;
}

/* True if `dir` is an MVX account (carries a .mvx descriptor). */
static int is_account(const char *dir) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/.mvx", dir);
    struct stat sb;
    return stat(p, &sb) == 0;
}

/* Walk up from the current directory to the nearest MVX account root.
 * Returns 1 and fills `out` on success. */
static int account_from_cwd(char *out, size_t cap) {
    char cur[PATH_MAX];
    if (!getcwd(cur, sizeof cur)) return 0;
    for (;;) {
        if (is_account(cur)) { snprintf(out, cap, "%s", cur); return 1; }
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) return 0;   /* reached root */
        *slash = '\0';
    }
}

int main(int argc, char **argv) {
    /* Forward everything to the real git, verbatim. */
    char **gargv = malloc((size_t)(argc + 1) * sizeof *gargv);
    if (!gargv) { perror("mvx-git"); return 1; }
    gargv[0] = "git";
    for (int i = 1; i < argc; i++) gargv[i] = argv[i];
    gargv[argc] = NULL;

    /* The subcommand is the first non-option argument. */
    const char *sub = NULL;
    int subidx = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') { sub = argv[i]; subidx = i; break; }
        /* git's own value-taking globals (-C, -c) precede the subcommand */
        if (strcmp(argv[i], "-C") == 0 || strcmp(argv[i], "-c") == 0) i++;
    }

    int code = run(gargv);
    free(gargv);

    /* Only rebuild after a successful tree-changing command. */
    if (code != 0 || !sub || !tree_changing(sub)) return code;

    char acct[PATH_MAX];
    int have = 0;
    if (strcmp(sub, "clone") == 0)
        have = clone_target(argc, argv, subidx, acct, sizeof acct);
    else
        have = account_from_cwd(acct, sizeof acct);

    if (!have || !is_account(acct)) return code;   /* not an MVX account */

    fprintf(stderr, "mvx-git: rebuilding MVX account %s\n", acct);
    const char *mvx = getenv("MVX");
    if (!mvx || !mvx[0]) mvx = "mvx";
    setenv("MVXPRIV", "developer", 1);             /* CONVERT catalogs */
    char *rargv[] = {(char *)mvx, "-a", acct, "-c", "CONVERT-ACCOUNT", NULL};
    int rc = run(rargv);
    if (rc != 0)
        fprintf(stderr, "mvx-git: account rebuild failed (exit %d)\n", rc);

    return code;   /* git's exit code is what callers expect */
}
