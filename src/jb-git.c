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
/* Before EVERY include, mvxgit.h included: -std=c11 asks for strict ANSI, so
   strdup is only declared when _POSIX_C_SOURCE is set -- and a feature-test
   macro set after the first header has already missed features.h.  It sat
   below <stdio.h>, so strdup was implicitly declared as returning int, which
   truncates the pointer on 64-bit. */
#define _POSIX_C_SOURCE 200809L

#include "mvxgit.h"

#include <ctype.h>
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
        "branch", "checkout", "restore", "tag", "version", "--version",
        "config",
        /* A record account's remote work is the ENGINE's, not git's: a push
           sends the projected records and a pull has to re-materialise them
           into live files.  Forwarded to git these looked like they worked and
           left the account untouched. */
        "remote", "fetch", "push", "pull", "clone", NULL};
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

/* `from` is the index of the subcommand: everything before it is ours (the
   -a <account> we have already acted on by chdir'ing) and must NOT reach git,
   which rejects it with "unknown option: -a" and a usage block.  Getting this
   wrong makes every forwarded command fail -- clone, push, pull, remote -- so
   the driver stops being a drop-in for git at exactly the point where being one
   matters most. */
static int run_git(int argc, char **argv, int from) {
    char **g = malloc((size_t)(argc + 2) * sizeof *g);
    if (!g) { perror("jb-git"); return 1; }
    g[0] = (char *)"git";
    int n = 1;
    for (int i = from; i < argc; i++) g[n++] = argv[i];
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
              "  branch | checkout | restore | tag | config | version\n"
              "  remote | fetch | push | pull | clone\n"
              "anything else is passed to git.\n", stdout);
        return i >= argc ? 1 : 0;
    }

    /* An MV user types the sentence in UPPER CASE -- `GIT STATUS`, `GIT PUSH`
       -- because that is how TCL has always been written, and the verb accepts
       it.  The CLI has to as well, or the two disagree about the same command;
       git itself only knows lower case, so the token is folded for matching AND
       for forwarding (`git PUSH` is "not a git command"). */
    static char subbuf[64];
    {
        const char *p = argv[i];
        size_t n = 0;
        while (p[n] && n < sizeof subbuf - 1) {
            subbuf[n] = (char)tolower((unsigned char)p[n]);
            n++;
        }
        subbuf[n] = '\0';
        argv[i] = subbuf;             /* forwarded to git in this form too */
    }
    const char *sub = argv[i];
    int subidx = i;

    if (strcmp(account, ".") && chdir(account) != 0) {
        fprintf(stderr, "jb-git: cannot enter %s: %s\n",
                account, strerror(errno));
        return 1;
    }

    /* ATTR IS AN IN-SESSION VERB and deliberately has no twin here -- it is
       BASIC (registry, validation, staging, a full-screen editor), and verbs
       are BASIC rather than C on purpose.  Forwarding it to git answered
       "'attr' is not a git command", which reads as "this does not exist"
       rather than "this lives somewhere else".  uv-git says the same thing. */
    if (!strcmp(sub, "attr")) {
        fprintf(stderr,
            "jb-git: 'attr' is an in-session verb, not a shell command.\n"
            "        Run it inside a jBASE session in this account:\n"
            "            GIT ATTR                       account attributes\n"
            "            GIT ATTR <file>                a file's parameters\n"
            "            GIT ATTR <file> --set k=v      change one\n");
        return 2;
    }
    if (!engine_sub(sub)) return run_git(argc, argv, subidx);

    const char *p0 = subidx + 1 < argc ? argv[subidx + 1] : NULL;
    const char *p1 = subidx + 2 < argc ? argv[subidx + 2] : NULL;
    const char *p2 = subidx + 3 < argc ? argv[subidx + 3] : NULL;
    if (p0 && p0[0] == '-') p0 = NULL;
    if (p1 && p1[0] == '-') p1 = NULL;
    if (p2 && p2[0] == '-') p2 = NULL;

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
    else if (!strcmp(sub, "config"))  out = mv_git_config(ctx, repo,
                                                         p0 ? p0 : "",
                                                         p1 ? p1 : "");
    else if (!strcmp(sub, "remote"))  out = mv_git_remote(ctx, repo,
                                                        p0 ? p0 : "",
                                                        p1 ? p1 : "",
                                                        p2 ? p2 : "");
    else if (!strcmp(sub, "clone")) {
        /* Forwarded, this ran a PLAIN git clone: it produced a directory of
           files and no account at all, and still read as a pass because git's
           own "you appear to have cloned an empty repository" contains the
           word the assertion looks for. */
        if (!p0 || !p1) {
            fprintf(stderr, "usage: jb-git clone <url> <dir> [ref]\n");
            mv_ctx_destroy(ctx);
            return 2;
        }
        out = mv_git_clone(ctx, p0, p1, p2 ? p2 : "");
    }
    else if (!strcmp(sub, "fetch"))   out = mv_git_fetch(ctx, repo, p0 ? p0 : "");
    else if (!strcmp(sub, "push"))    out = mv_git_push(ctx, repo, p0 ? p0 : "",
                                                       p1 ? p1 : "");
    else if (!strcmp(sub, "pull"))    out = mv_git_pull(ctx, repo, p0 ? p0 : "",
                                                       p1 ? p1 : "");
    else if (!strcmp(sub, "version") || !strcmp(sub, "--version")) {
        /* Forwarded, this answered `git version 2.43.7` -- jb-git reporting
           stock git's version as its own, which is worse than not answering. */
        char self[64];
        snprintf(self, sizeof self, "jb-git %s", JBGIT_VERSION);
        char *v = mv_git_versions(self);
        fputs(v ? v : "", stdout);
        free(v);
        out = NULL;
    }
    else if (!strcmp(sub, "tag")) {
        /* git's own spelling, decoded to the engine's op -- the same shapes
           uv-git accepts, so the CLIs stay drop-in replacements for each other
           as much as for git:
             tag                      list
             tag name {commit}        lightweight, HEAD if no commit
             tag -a name -m message   annotated
             tag -d name              delete                                  */
        const char *op = "list", *name = "", *target = "", *msg = "";
        for (int k = subidx + 1; k < argc; k++) {
            if (!strcmp(argv[k], "-d") && k + 1 < argc) {
                op = "delete"; name = argv[++k];
            } else if (!strcmp(argv[k], "-a") && k + 1 < argc) {
                op = "add";    name = argv[++k];
            } else if (!strcmp(argv[k], "-m") && k + 1 < argc) {
                msg = argv[++k];
            } else if (argv[k][0] != '-') {
                if (!*name)        { op = "add"; name = argv[k]; }
                else if (!*target) target = argv[k];
            }
        }
        if (strcmp(op, "list") != 0 && !*name) {
            fprintf(stderr, "usage: jb-git tag {name {commit} | -a name "
                            "-m msg | -d name}\n");
            mv_ctx_destroy(ctx);
            return 2;
        }
        out = mv_git_tag(ctx, repo, op, name, target, msg);
    }

    print_out(out);
    mv_ctx_destroy(ctx);
    return 0;
}
