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
#include <strings.h>          /* strcasecmp, strncasecmp */
#include <sys/wait.h>
#include <unistd.h>

/* Is this repository kept in the open (portable) account form?
 *
 * The engine asks mv_openaccount(), which reads $MVX_OPENACCOUNT -- and a CLI
 * is the only thing that can set it, because the flag lives in .git/config
 * where the engine does not look.  udt-git and uv-git both do this; jb-git did
 * not, so the engine decided an open account was not one and staged the NATIVE
 * descriptor: every commit deleted .mv-account and added .mvx, on a clone that
 * had just been reported clean.  (The same missing seed was half of #108.) */
/* "Make this an open account?" -- the same question, defaults and env override
   the other CLIs use (mv_git#88).  With no terminal the answer is yes, said out
   loud, because erroring would break every scripted clone. */
static int g_open_flag;

static int ask_open_account(void) {
    if (g_open_flag) return g_open_flag > 0;
    const char *env = getenv("MVXGIT_OPEN_ACCOUNT");
    if (env && env[0])
        return !(env[0] == '0' || !strcasecmp(env, "no") ||
                 !strcasecmp(env, "false") || !strcasecmp(env, "off"));
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "jb-git: keeping the open account format "
                        "(--no-open-account, or MVXGIT_OPEN_ACCOUNT=0, "
                        "declines)\n");
        return 1;
    }
    char line[16];
    fprintf(stderr, "Make it an open account? [Y/n] ");
    fflush(stderr);
    if (!fgets(line, sizeof line, stdin)) return 1;
    return !(line[0] == 'n' || line[0] == 'N');
}          /* 1 = --open-account, -1 = --no-open-account */

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
        "remote", "fetch", "push", "pull", "clone", "adopt", NULL};
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
    /* Ours, not git's: strip them so a forwarded command never sees them. */
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--open-account"))         g_open_flag = 1;
        else if (!strcmp(argv[a], "--no-open-account")) g_open_flag = -1;
        else continue;
        for (int b = a; b + 1 < argc; b++) argv[b] = argv[b + 1];
        argc--; a--;
    }
    if (i < argc && !strcmp(argv[i], "-a") && i + 1 < argc) {
        account = argv[i + 1];
        i += 2;
    }
    if (i >= argc || !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
        fputs("usage: jb-git [-a <account>] <command> [args]\n"
              "  init | add | rm | commit | status | log | diff | show\n"
              "  branch | checkout | restore | tag | config | version\n"
              "  remote | fetch | push | pull | clone | adopt\n"
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

    if (open_account_on()) setenv("MVX_OPENACCOUNT", "1", 1);

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
    else if (!strcmp(sub, "adopt")) {
        /* Take a checkout somebody made with plain git and build the account
           from it.  git gives you files; this gives you an account.
           This is `clone` minus the fetching, and it shares every piece with
           udt-git and uv-git (mv_git#124, #125) -- a jBASE account is just a
           directory, so unlike UniData there is nothing to create first. */
        const char *dir = p0 ? p0 : ".";
        if (chdir(dir) != 0) {
            fprintf(stderr, "jb-git adopt: cannot enter %s: %s\n",
                    dir, strerror(errno));
            mv_ctx_destroy(ctx);
            return 1;
        }
        /* A descriptor is the checkout's own statement that it IS an account;
           without one this is an ordinary repository and adopting it would
           build an account around files that never were one. */
        static const char *const dnames[] = { ".mv-account", ".mvx", ".udt",
                                              ".uv", ".jbase", NULL };
        const char *found = NULL;
        for (int k = 0; dnames[k] && !found; k++)
            if (access(dnames[k], F_OK) == 0) found = dnames[k];
        if (!found) {
            fprintf(stderr,
                "jb-git adopt: %s carries no MV account descriptor "
                "(.mv-account, .mvx, .udt, .uv or .jbase),\n"
                "        so there is no account here to adopt.\n", dir);
            mv_ctx_destroy(ctx);
            return 1;
        }

        /* The same question the other CLIs ask, from the same engine decision. */
        switch (mv_git_adopt_question(found, open_account_on())) {
        case MV_ADOPT_ASK_ENABLE:
            fprintf(stderr,
                "jb-git adopt: %s is already in the open account format, but "
                "this repository\n        does not have the flag set -- without "
                "it the next commit writes the native form.\n", dir);
            if (ask_open_account())
                (void)system("git config mvx.openaccount true");
            break;
        case MV_ADOPT_ASK_CONVERT:
            fprintf(stderr, "jb-git adopt: %s is a native %s account and will "
                            "be converted to a jBASE one.\n", dir, found);
            if (ask_open_account())
                (void)system("git config mvx.openaccount true");
            break;
        default:
            break;              /* native to jBASE: nothing to convert or ask */
        }

        /* Stash the tree so an EDIT in the checkout is what gets adopted rather
           than the committed version of it, clear what git left behind -- the
           open form sits on the names the native files want -- and build from
           what was stashed.  Never popped: that would put the open form back
           over a native account. */
        char captured[192] = "";
        int stashed = 0;
        if (mv_git_worktree_stash(captured, sizeof captured, &stashed) != 0) {
            fprintf(stderr, "jb-git adopt: could not read the working tree\n");
            mv_ctx_destroy(ctx);
            return 1;
        }
        if (stashed)
            fprintf(stderr, "jb-git adopt: your uncommitted changes are in the "
                            "stash and will be built into the account\n");
        {
            char why[512];
            (void)mv_git_worktree_clear(why, sizeof why);
        }
        /* A descriptor plain git checked out must not survive: off MVX it is
           virtual (#122). */
        mv_git_drop_native_desc();

        char acctpath2[4096];
        if (getcwd(acctpath2, sizeof acctpath2))
            setenv("MVXACCOUNT", acctpath2, 1);
        if (open_account_on()) setenv("MVX_OPENACCOUNT", "1", 1);

        /* Not the literal ".git": an account can be a SUBDIRECTORY of a
           repository (#44, #49), and there is no .git in it -- the repository's
           is above.  udt-git had the same bug: materialise failed with "failed
           to resolve path '.git'", the account was built empty, and adopt
           reported success anyway. */
        char gdir[4096] = ".git";
        {
            FILE *g = popen("git rev-parse --absolute-git-dir 2>/dev/null", "r");
            if (g) {
                if (fgets(gdir, sizeof gdir, g)) {
                    char *gn = strpbrk(gdir, "\r\n");
                    if (gn) *gn = '\0';
                }
                pclose(g);
            }
            if (!gdir[0]) snprintf(gdir, sizeof gdir, ".git");
        }
        mv_ctx_destroy(ctx);
        ctx = mv_ctx_create();
        out = mv_git_materialize_rev(ctx, gdir, captured);
        print_out(out);
        out = NULL;
        if (stashed) (void)system("git stash drop -q >/dev/null 2>&1");
        printf("adopted %s as a jBASE account\n", dir);
    }
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
        /* --no-checkout, exactly as udt-git does: the account is BUILT from
           HEAD's tree by mv_git_materialize, and a git checkout would only put
           the open form on disk -- %FILE% controls and all -- for the build to
           then contradict.  jb-git used to call mv_git_clone, which is
           git_clone WITH a checkout and nothing after it, so a "clone" left a
           directory tree that jBASE opens as UD files because it opens any
           directory as one.  It read clean; then `add -A` rewrote every file's
           %FILE% from `hash 2 DYNAMIC` to `DIR`, because by then that was the
           truth on disk. */
        {
            pid_t pid = fork();
            if (pid == 0) {
                execlp("git", "git", "clone", "--no-checkout", p0, p1,
                       (char *)NULL);
                _exit(127);
            }
            int st = 0;
            if (pid < 0 || waitpid(pid, &st, 0) < 0 ||
                !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                fprintf(stderr, "jb-git clone: git clone failed\n");
                mv_ctx_destroy(ctx);
                return 1;
            }
        }
        printf("cloned %s -> %s\n", p0, p1);
        /* mv_git_clone is git_clone and nothing more: it leaves the OPEN FORM
           checked out on disk -- %FILE% controls and all -- and the account is
           built from it afterwards.  udt-git and uv-git both do this step and
           say so in their closing line; jb-git did not, so a "clone" produced a
           directory tree that jBASE would open as UD files because it opens any
           directory as one.  It read clean, and then `add -A` rewrote every
           file's %FILE% from `hash 2 DYNAMIC` to `DIR` -- because by then that
           WAS the truth on disk. */
        if (chdir(p1) != 0) {
            fprintf(stderr, "jb-git clone: cannot enter %s\n", p1);
            mv_ctx_destroy(ctx);
            return 1;
        }
        char acctpath[4096];
        if (getcwd(acctpath, sizeof acctpath))
            setenv("MVXACCOUNT", acctpath, 1);
        /* A repository committed in the OPEN form has to be checked out as an
           open account, or every later commit here writes the native shape and
           the account stops travelling.  The flag lives in the cloner's own
           config, so cloning is the only moment it can be set -- and it is
           asked rather than assumed (#88).  Without this, a jBASE clone of an
           open account committed .mvx over .mv-account on its very first
           commit, having just reported itself clean. */
        if (system("git cat-file -e HEAD:.mv-account >/dev/null 2>&1") == 0) {
            if (ask_open_account()) {
                if (system("git config mvx.openaccount true") != 0)
                    fprintf(stderr, "jb-git clone: could not set "
                                    "mvx.openaccount\n");
                else
                    fprintf(stderr, "jb-git: '%s' was committed in the open "
                                    "account format - checking it out as an "
                                    "open account.\n"
                                    "        (--no-open-account, or "
                                    "MVXGIT_OPEN_ACCOUNT=0, for a native "
                                    "checkout instead)\n", p1);
            }
        }
        if (open_account_on()) setenv("MVX_OPENACCOUNT", "1", 1);
        mv_ctx_destroy(ctx);
        ctx = mv_ctx_create();
        out = mv_git_materialize(ctx, ".git");
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
