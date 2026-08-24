/* jb-git.c — the shell entry point for mv_git on jBASE.
 * Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
 *
 * FIRST PASS (mv_git#114).  jBASE lets a standalone process make its own
 * session (JBASESessionObjectFactory, in jbasegit_rt.c) and then call the
 * record API itself, so this driver runs the shared engine directly — the
 * arrangement mvx-git uses, not udt-git's.  There is no InterCall to log into
 * and no background process to talk to.
 *
 * Anything the engine does not implement is forwarded to the real git, so a
 * jBASE account behaves like a git working tree for every non-record command.
 */
#include "mvxgit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Subcommands the record-git engine implements. */
static int engine_sub(const char *sub) {
    static const char *ops[] = {
        "init", "add", "rm", "commit", "status", "log", "diff", "show",
        "branch", "checkout", "restore", NULL};
    for (int i = 0; ops[i]; i++)
        if (strcmp(sub, ops[i]) == 0) return 1;
    return 0;
}

/* Engine output is attribute-mark separated; print it a line at a time. */
static void print_out(char *s) {
    if (!s) return;
    for (char *p = s; *p; p++)
        if (*p == (char)0xFE) *p = '\n';
    if (*s) { fputs(s, stdout); fputc('\n', stdout); }
    free(s);
}

static int run_git(int argc, char **argv) {
    char **g = malloc((size_t)(argc + 2) * sizeof *g);
    if (!g) { perror("jb-git"); return 1; }
    g[0] = (char *)"git";
    int n = 1;
    for (int i = 1; i < argc; i++) g[n++] = argv[i];
    g[n] = NULL;
    pid_t pid = fork();
    if (pid < 0) { perror("jb-git: fork"); free(g); return 1; }
    if (pid == 0) {
        execvp(g[0], g);
        fprintf(stderr, "jb-git: cannot run git: %s\n", strerror(errno));
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    free(g);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static const char *opt_value(int argc, char **argv, int from, const char *opt) {
    for (int i = from; i < argc - 1; i++)
        if (!strcmp(argv[i], opt)) return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv) {
    int i = 1;
    const char *account = ".";
    if (i < argc && !strcmp(argv[i], "-a") && i + 1 < argc) {
        account = argv[i + 1];
        i += 2;
    }
    if (i >= argc || !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
        fputs("usage: jb-git [-a <account>] <command> [args]\n"
              "  init | add | rm | commit | status | log | diff | show\n"
              "  branch | checkout | restore\n"
              "anything else is passed to git.\n", stdout);
        return i >= argc ? 1 : 0;
    }

    const char *sub = argv[i];
    int subidx = i;

    if (strcmp(account, ".") && chdir(account) != 0) {
        fprintf(stderr, "jb-git: cannot enter %s: %s\n",
                account, strerror(errno));
        return 1;
    }

    if (!engine_sub(sub)) return run_git(argc, argv);

    const char *p0 = subidx + 1 < argc ? argv[subidx + 1] : NULL;
    const char *p1 = subidx + 2 < argc ? argv[subidx + 2] : NULL;
    if (p0 && p0[0] == '-') p0 = NULL;
    if (p1 && p1[0] == '-') p1 = NULL;

    mv_ctx *ctx = mv_ctx_create();
    char *out = NULL;
    const char *repo = ".git";

    if (!strcmp(sub, "init"))        out = mv_git_init(ctx, repo);
    else if (!strcmp(sub, "add")) {
        /* -A is its own engine op (GITADDALL), not add with an empty name:
           passing "" stages nothing at all, quietly. */
        int all = 0;
        for (int a = subidx + 1; a < argc; a++)
            if (!strcmp(argv[a], "-A") || !strcmp(argv[a], "--all") ||
                !strcmp(argv[a], ".")) all = 1;
        if (all)      out = mv_git_addall(ctx, repo);
        else if (p0)  out = mv_git_add(ctx, repo, p0, p1 ? p1 : "");
        else          out = strdup("usage: jb-git add <file> [id] | -A");
    }
    else if (!strcmp(sub, "rm"))     out = mv_git_rm(ctx, repo, p0 ? p0 : "",
                                                    p1 ? p1 : "");
    else if (!strcmp(sub, "commit")) {
        const char *m = opt_value(argc, argv, subidx + 1, "-m");
        out = mv_git_commit(ctx, repo, m ? m : "");
    }
    else if (!strcmp(sub, "status")) out = mv_git_status(ctx, repo);
    else if (!strcmp(sub, "log")) {
        const char *n = opt_value(argc, argv, subidx + 1, "-n");
        out = mv_git_log(ctx, repo, n ? n : (p0 ? p0 : "20"));
    }
    else if (!strcmp(sub, "diff"))   out = mv_git_diff(ctx, repo, p0 ? p0 : "");
    else if (!strcmp(sub, "show"))   out = mv_git_show(ctx, repo, p0 ? p0 : "",
                                                      p1 ? p1 : "");
    else if (!strcmp(sub, "branch")) out = mv_git_branch(ctx, repo,
                                                        p0 ? p0 : "");
    else if (!strcmp(sub, "checkout")) out = mv_git_checkout(ctx, repo,
                                                            p0 ? p0 : "");
    else if (!strcmp(sub, "restore")) out = mv_git_restore(ctx, repo,
                                                          p0 ? p0 : "");

    print_out(out);
    mv_ctx_destroy(ctx);
    return 0;
}
