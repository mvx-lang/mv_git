/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* uv-git — shell-side git for a UniVerse account.
 *
 * The counterpart of mvx-git and udt-git, and the odd one of the three: those
 * two reach records from C directly — mvx-git through the MVX runtime, udt-git
 * through UniData's InterCall — and drive the engine themselves.  uv-git cannot,
 * and the reason is not a design preference:
 *
 *   - GCI, the in-process C bridge, is licensed and non-functional in the
 *     UniVerse Trial Edition (mv_git#37) — it refuses even Rocket's own shipped
 *     example.
 *   - InterCall, the documented record API with ic_universe_session(), is a
 *     CLIENT SDK shipped in the Rocket U2 Clients bundle, not with the UniVerse
 *     server, and is not available for Linux to us (mv_git#42).  The library
 *     that IS installed, libvm_ici.a, is the superseded ICI: undocumented in
 *     both shipped headers, and it segfaults rather than erroring when called.
 *
 * So on UniVerse the only supported way into the records is a session — which
 * is exactly what the in-session GIT verb already is, and it works (mv_git#40).
 * uv-git therefore runs the verb: it enters the account, hands the sentence to
 * `uv`, and passes the output back.  The record loop runs in BASIC where
 * UniVerse allows it, and the git-object work goes out to the mvgitd background
 * process over its pipe.
 *
 * This is a thinner program than its siblings by necessity, not by neglect.  If
 * an InterCall SDK for Linux ever becomes available, the honest upgrade is a
 * uvgit_rt.c implementing the mv_* primitives over ic_universe_session() — a
 * near-mechanical port of udtgit_rt.c — and this driver retires.
 *
 * TWO PATHS, chosen by what the directory actually is:
 *
 *   an ACCOUNT (it has a VOC)   drive the in-session verb, which owns the
 *                               records; git objects go out to mvgitd
 *   an ORDINARY DIRECTORY       serve it here, directly against libgit2 —
 *                               no session, because there is nothing a session
 *                               could contribute to a directory of plain files
 *
 * The second is not a courtesy: a repository may hold ordinary files beside its
 * accounts, and refusing to work on them would make uv-git useless for exactly
 * the repositories that need it most.  libgit2 is already linked (textconv needs
 * it), so the plain path costs nothing.
 *
 *   uv-git [-a account] <command> [args...]
 */

#define _POSIX_C_SOURCE 200809L

#include "mvxgit.h"
#include <git2.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UV_BIN "uv"

/* The fence the verb prints around its own output in machine mode (GIT -M).
   Must match BP/GIT. */
#define GIT_BEGIN "<<<GIT-BEGIN>>>"
#define GIT_END   "<<<GIT-END>>>"

/* UniVerse greets every session and prompts between commands.  None of that is
   the verb's output, so it is filtered — a caller piping `uv-git status` into
   something else should see what GIT printed and nothing more. */
static int is_noise(const char *line) {
    static const char *prefixes[] = {
        "UniVerse Command Language", "Copyright ", "Rocket Software",
        ">", ":", NULL
    };
    if (!*line) return 1;
    for (int i = 0; prefixes[i]; i++)
        if (strncmp(line, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    /* the per-session logon line: "<account> logged on: <date>" */
    if (strstr(line, " logged on:")) return 1;
    return 0;
}

/* Quote one argument for a UniVerse sentence.  Anything with a space has to be
   quoted or the verb sees several arguments — a commit message being the case
   that matters, since it is normally a sentence of its own. */
static void append_arg(char *buf, size_t cap, const char *arg) {
    size_t used = strlen(buf);
    int needs_quotes = (*arg == '\0') || strpbrk(arg, " \t") != NULL;
    const char *q = needs_quotes ? "\"" : "";
    snprintf(buf + used, cap - used, " %s%s%s", q, arg, q);
}

/* Is this directory inside a repository that lives ABOVE it?
 *
 * Everything here — and in the verb, and in mvgitd — assumes the repository is
 * `.git` directly inside the account.  An account can perfectly well sit BELOW a
 * repo root instead (a repo holding several accounts, or an account inside a
 * larger project), and real git would find the enclosing repo by searching
 * upward.  We do not, so `init` would create a SECOND repository nested inside
 * the first and quietly split the history in two.
 *
 * Detecting it is cheap — git_repository_discover is the same upward walk git
 * itself does — so the damage is prevented even though nesting is not yet
 * SUPPORTED.  Supporting it needs the account-aware staging and checkout
 * recursion in mv_git#44; refusing clearly is the honest interim answer.
 *
 * Returns 1 and fills `out` when an enclosing repo exists above us; 0 when
 * there is none, or when the repo found is our own `.git`. */
static int enclosing_repo(char *out, size_t cap) {
    if (access(".git", F_OK) == 0) return 0;      /* our own — not enclosing */
    git_libgit2_init();
    git_buf found = {0};
    /* across_fs = 0: do not wander over a mount boundary looking for a repo */
    if (git_repository_discover(&found, ".", 0, NULL) != 0) return 0;
    snprintf(out, cap, "%s", found.ptr ? found.ptr : "");
    git_buf_dispose(&found);
    return out[0] != '\0';
}

/* status for a plain directory, straight from libgit2.
 *
 * The engine's mv_git_status cannot serve this: it compares LIVE RECORDS against
 * HEAD, so it calls mv_open and — in this binary, built against the recordless
 * backend — aborts.  That abort is the backend working as intended (it announces
 * a wrong assumption rather than misbehaving quietly), and it caught this very
 * mistake.  A directory of files has no records to compare, so the right answer
 * is git's own status, which is what a user typing `uv-git status` there means.
 *
 * mv_git_diff is record-based for the same reason and has no plain equivalent
 * here, so it is simply not offered — `git diff` is the tool for that. */
static int plain_status(void) {
    git_libgit2_init();
    git_repository *repo = NULL;
    if (git_repository_open(&repo, ".") != 0) {
        fprintf(stderr, "uv-git: not a git repository\n");
        return 1;
    }
    git_status_list *sl = NULL;
    git_status_options o = GIT_STATUS_OPTIONS_INIT;
    o.show  = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    o.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED;
    int rc = git_status_list_new(&sl, repo, &o);
    if (rc != 0) {
        const git_error *e = git_error_last();
        fprintf(stderr, "uv-git: status: %s\n",
                (e && e->message) ? e->message : "failed");
        git_repository_free(repo);
        return 1;
    }
    size_t n = git_status_list_entrycount(sl), shown = 0;
    for (size_t k = 0; k < n; k++) {
        const git_status_entry *se = git_status_byindex(sl, k);
        const char *path = se->index_to_workdir ? se->index_to_workdir->old_file.path
                                                : se->head_to_index->old_file.path;
        const char *how = "?";
        unsigned st = se->status;
        if      (st & (GIT_STATUS_INDEX_NEW))                       how = "A";
        else if (st & (GIT_STATUS_INDEX_MODIFIED))                  how = "M";
        else if (st & (GIT_STATUS_INDEX_DELETED))                   how = "D";
        else if (st & (GIT_STATUS_WT_MODIFIED))                     how = " M";
        else if (st & (GIT_STATUS_WT_DELETED))                      how = " D";
        else if (st & (GIT_STATUS_WT_NEW))                          how = "??";
        printf(" %s %s\n", how, path);
        shown++;
    }
    if (!shown) printf("nothing to commit, working tree clean\n");
    git_status_list_free(sl);
    git_repository_free(repo);
    return 0;
}

/* Serve an ordinary directory: no session, no records, just git.
 *
 * The engine ops used here are disk-side ones — init, adddisk, commit, log —
 * which touch no record primitives.  That set was established by RUNNING them
 * against the recordless backend, not by reading: `status` was in the list
 * until it aborted with "mv_open was called", because it compares live records
 * against HEAD.  It is served by plain_status() instead, and `diff` is left out
 * for the same reason.  Anything else is refused by name rather than attempted. */
static int run_plain(int argc, char **argv, int i) {
    const char *sub = argv[i++];
    const char *a0 = (i     < argc) ? argv[i]     : "";
    const char *a1 = (i + 1 < argc) ? argv[i + 1] : "";
    mv_ctx *ctx = mv_ctx_create();
    char *out = NULL;

    /* Find the repository the way git does: search upward, so running in a
       subdirectory works on the enclosing repo rather than failing to find one.
       Falls back to ".git" here, which is what `init` needs — there is nothing
       to discover before it exists. */
    char found[4096], repobuf[4096];
    const char *repo = ".git";
    if (enclosing_repo(found, sizeof found)) {
        snprintf(repobuf, sizeof repobuf, "%s", found);
        repo = repobuf;
    }

    if      (!strcmp(sub, "init"))    out = mv_git_init(ctx, repo);
    else if (!strcmp(sub, "status"))  { mv_ctx_destroy(ctx); return plain_status(); }
    else if (!strcmp(sub, "log"))     out = mv_git_log(ctx, repo, *a0 ? a0 : "20");
    else if (!strcmp(sub, "branch"))  out = mv_git_branch(ctx, repo, a0);
    else if (!strcmp(sub, "config"))  out = mv_git_config(ctx, repo, a0, a1);
    else if (!strcmp(sub, "add"))     out = mv_git_adddisk(ctx, repo);
    else if (!strcmp(sub, "commit")) {
        /* -m <msg>, as git spells it */
        const char *msg = a0;
        if (!strcmp(a0, "-m")) msg = a1;
        out = mv_git_commit(ctx, repo, msg);
    } else {
        fprintf(stderr,
            "uv-git: '%s' needs a UniVerse account, and this is an ordinary "
            "directory.\n"
            "        Available here: init, add, commit, status, log, branch, "
            "config.\n", sub);
        mv_ctx_destroy(ctx);
        return 2;
    }

    if (out) {
        /* engine output is @AM-separated; render it as lines */
        for (char *p = out; *p; p++) if ((unsigned char)*p == 0xFE) *p = '\n';
        printf("%s\n", out);
        free(out);
    }
    mv_ctx_destroy(ctx);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    const char *account = ".";

    if (i < argc && strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
        account = argv[i + 1];
        i += 2;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: uv-git [-a account] <command> [args]\n");
        return 2;
    }

    /* textconv is a git diff filter — it renders a record blob legibly for the
       diff view and touches no records at all, so it runs here rather than
       paying for a session. */
    if (strcmp(argv[i], "textconv") == 0)
        return mv_git_textconv(i + 1 < argc ? argv[i + 1] : "-");

    if (chdir(account) != 0) {
        fprintf(stderr, "uv-git: cannot enter account %s: %s\n",
                account, strerror(errno));
        return 1;
    }

    /* A UniVerse account is identified by its VOC.  Without one this is an
       ordinary directory, and there is nothing a session could do for it — but
       there is plenty git can, and we are already linked against libgit2 for
       textconv.  So serve it directly rather than refusing: one binary, two
       paths, chosen by what the directory actually is.  That mirrors how
       mvx-git treats record files and plain files.
       Ops that would need records are simply not offered here; the recordless
       backend aborts loudly if one is ever reached, so a wrong assumption
       announces itself instead of quietly doing the wrong thing. */
    if (access("VOC", F_OK) != 0)
        return run_plain(argc, argv, i);

    /* An ACCOUNT below a repository root is the unsupported case, and it is
       specific to accounts: records would have to be staged under the account's
       own prefix, and checkout would have to recurse back into it — neither of
       which exists yet (mv_git#44).  A plain subdirectory inside a repo needs
       none of that; git works there every day, and so does the path above.
       init is called out separately because it does not merely misbehave — it
       creates a SECOND repository nested in the first and splits the history. */
    {
        char up[4096];
        if (enclosing_repo(up, sizeof up)) {
            fprintf(stderr,
                "uv-git: this account is inside the repository at\n"
                "          %s\n"
                "        and an account below a repository root is not supported "
                "yet (mv_git#44).\n", up);
            if (!strcmp(argv[i], "init"))
                fprintf(stderr,
                "        Running init here would create a SECOND repository "
                "nested in that one,\n"
                "        splitting the history.  Run it at the repository root "
                "instead.\n");
            return 1;
        }
    }

    /* Build the sentence the verb will see.  GIT is the verb; the rest is its
       command and arguments, in the order given. */
    /* -M asks the verb for machine output: it prints a fence around its own
       output so we can find it exactly, rather than inferring where it starts.
       See BP/GIT.  A verb too old to know the flag treats it as an unknown
       subcommand, so the echo fence below stays as the fallback. */
    char sentence[8192];
    snprintf(sentence, sizeof sentence, "GIT -M");
    for (; i < argc; i++)
        append_arg(sentence, sizeof sentence, argv[i]);

    /* Drive a UniVerse session: the sentence, then QUIT so it exits rather than
       waiting at the prompt.  The input goes through a temp file rather than a
       bidirectional popen, because popen is only defined for "r" or "w" — a
       "w+" pipe is a glibc-specific extension and not portable. */
    char script[] = "/tmp/uvgitXXXXXX";
    int sfd = mkstemp(script);
    if (sfd < 0) {
        fprintf(stderr, "uv-git: cannot create a temp script: %s\n", strerror(errno));
        return 1;
    }
    FILE *sf = fdopen(sfd, "w");
    if (!sf) { close(sfd); unlink(script); return 1; }
    fprintf(sf, "%s\nQUIT\n", sentence);
    fclose(sf);

    /* A driver never wants curses.  UniVerse already treats a session with no
       terminal as a phantom — @TTY reports "phantom" and @USERNO is negative
       even with stdin merely redirected — so a LOGIN paragraph carrying the
       usual non-interactive guard exits by itself, which is the behaviour we
       want rather than something to suppress.  Forcing a dumb terminal is the
       belt to that braces: it stops anything that runs anyway from trying to
       paint a screen, which would otherwise land in our output. */
    setenv("TERM", "dumb", 1);

    char cmd[4096];
    snprintf(cmd, sizeof cmd, "%s < %s", UV_BIN, script);
    FILE *uv = popen(cmd, "r");
    if (!uv) {
        fprintf(stderr, "uv-git: cannot run %s: %s\n", UV_BIN, strerror(errno));
        unlink(script);
        return 1;
    }

    /* Print only what the verb itself produced.  This matters because a LOGIN
       paragraph runs BEFORE our sentence and cannot be assumed well behaved:
       the usual one guards on a non-interactive session and exits silently
       (UniVerse reports @TTY as "phantom" here even with stdin merely
       redirected, so that guard does fire), but an unguarded one prints its
       banner or menu straight into what a caller is trying to parse.

       Two fences, preferred first:

         1. The verb's own, from `GIT -M` — it PRINTs <<<GIT-BEGIN>>> before
            dispatching and <<<GIT-END>>> after.  An explicit contract rather
            than an inference, and it comes from BASIC, where a program can
            always CRT a literal; there is no TCL verb that reliably echoes one
            (DISPLAY is a paragraph statement, not a verb, in this flavour).
         2. Failing that, UniVerse's command echo: a session reading a pipe
            echoes each command after its prompt (">GIT -M ADD TESTF"), so the
            output lies between that echo and the next prompt.  Kept for a verb
            too old to know -M, which treats the flag as an unknown subcommand.

       Either way, whatever LOGIN prints arrives before both fences and is
       outside them. */
    char line[4096];
    int inside = 0, saw_end = 0, fenced = 0, no_verb = 0;
    while (fgets(line, sizeof line, uv)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        /* The account exists but has no GIT verb.  UniVerse reports this on the
           way past, and it is worth catching by name: it is not a git failure
           but a missing installation, and the user needs to be told which. */
        if (strstr(line, "is not in your VOC")) { no_verb = 1; continue; }

        /* Preferred: the verb's own fence.  Once seen, it governs alone — the
           echo heuristic below is abandoned, since the verb's word about where
           its output begins beats anything inferred from the display. */
        if (strcmp(line, GIT_BEGIN) == 0) { fenced = 1; inside = 1; saw_end = 0; continue; }
        if (fenced && strcmp(line, GIT_END) == 0) { inside = 0; saw_end = 1; continue; }

        if (!fenced && (line[0] == '>' || line[0] == ':')) {  /* prompt + its echo */
            if (!inside && strstr(line, sentence)) inside = 1;
            else if (inside) { inside = 0; saw_end = 1; }
            continue;
        }
        if (inside && !is_noise(line)) printf("%s\n", line);
    }

    if (no_verb) {
        fprintf(stderr,
            "uv-git: git is not set up in this account — the GIT verb is not "
            "in its VOC.\n"
            "        Install it there (the package's install.sh catalogs the "
            "verb and its\n"
            "        handlers), then try again.\n");
        pclose(uv);
        unlink(script);
        return 1;
    }

    /* No closing marker means the session did not reach the end of our script —
       a LOGIN paragraph that prompted and swallowed it, or a session that died.
       Say so, rather than let a silent empty result read as success. */
    if (!saw_end)
        fprintf(stderr, "uv-git: the UniVerse session did not run the command"
                        " (a LOGIN paragraph that prompts will consume it from"
                        " stdin before the verb is reached)\n");

    int rc = pclose(uv);
    unlink(script);
    if (!saw_end) return 1;
    return rc == 0 ? 0 : 1;
}
