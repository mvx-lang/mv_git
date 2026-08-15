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
    char sentence[8192];
    snprintf(sentence, sizeof sentence, "GIT");
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

    char cmd[4096];
    snprintf(cmd, sizeof cmd, "%s < %s", UV_BIN, script);
    FILE *uv = popen(cmd, "r");
    if (!uv) {
        fprintf(stderr, "uv-git: cannot run %s: %s\n", UV_BIN, strerror(errno));
        unlink(script);
        return 1;
    }

    char line[4096];
    while (fgets(line, sizeof line, uv)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (!is_noise(line)) printf("%s\n", line);
    }

    int rc = pclose(uv);
    unlink(script);
    return rc == 0 ? 0 : 1;
}
