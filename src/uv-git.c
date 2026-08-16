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
 *   uv-git [-a account] <command> [args...]
 */

#define _POSIX_C_SOURCE 200809L

#include "mvxgit.h"

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
    int inside = 0, saw_end = 0, fenced = 0;
    while (fgets(line, sizeof line, uv)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

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
