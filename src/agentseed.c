/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* agentseed.c — put an account I/O agent into an account (mv_git#45).
 *
 * Shared by uv-git and udt-git for the same reason mvsession.c is: what an
 * account needs before it can answer is a property of the AGENT, not of the
 * platform.  The two genuine platform differences are named below and nothing
 * else here branches.
 *
 * This runs where NOTHING is installed yet — a bare account made by `clone`, or
 * a stock account stood up to learn a baseline from.  So it cannot read the
 * agent off disk (the package may be nowhere near) and it cannot assume BP
 * exists: it creates the file, writes the source from the copy compiled into
 * this binary, writes a PLATFORM.H for it to $INCLUDE, and compiles it.
 */

#define _POSIX_C_SOURCE 200809L

#include "agent_src.h"
#include "mvsession.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The platform's own name, uppercased — "UV" or "UDT" — taken from the shell
   this driver runs, so neither driver has to know about the other. */
static void platform_name(char *out, size_t cap) {
    const char *sh = mvs_shell();
    size_t k = 0;
    for (; sh && sh[k] && k + 1 < cap; k++)
        out[k] = (char)toupper((unsigned char)sh[k]);
    out[k] = '\0';
}

static int is_udt(void) {
    const char *sh = mvs_shell();
    return sh && strcmp(sh, "udt") == 0;
}

/* Create a directory-type file, whatever this platform's CREATE.FILE wants.
 *
 * THE ONE REAL DIFFERENCE, and it is in the verb's own syntax: UniVerse prompts
 * for type and modulo interactively, so the answers are piped in (with an EMPTY
 * description — a described file reads "F <text>" and the account scan cannot
 * see it).  UniData takes the type as a word on the sentence and asks nothing.
 */
static void create_dir_file(const char *name) {
    char cmd[512];
    if (is_udt())
        snprintf(cmd, sizeof cmd,
                 "printf 'CREATE.FILE DIR %s\nQUIT\n' | %s >/dev/null 2>&1",
                 name, mvs_shell());
    else
        snprintf(cmd, sizeof cmd,
                 "printf 'CREATE.FILE %s\n1\n2\n3\n1\n2\n19\n\nQUIT\n' | %s "
                 ">/dev/null 2>&1", name, mvs_shell());
    if (system(cmd) != 0) { /* prompts are noisy; the compile below is the test */ }
}

/* Did the compile produce an object?
 *
 * THE OTHER REAL DIFFERENCE, and it is in where each platform PUTS one:
 *   UniVerse   a separate FILE named for the source — BP.O/GIT.AGENT
 *   UniData    a record in the SAME file, underscore-prefixed — BP/_GIT.AGENT
 * The same distinction the object-file exclusion follows, so the two agree by
 * construction rather than by coincidence.
 */
static int agent_compiled(void) {
    return access(is_udt() ? "BP/_GIT.AGENT" : "BP.O/GIT.AGENT", F_OK) == 0;
}

int mv_agent_seed(void) {
    char cmd[512];

    create_dir_file("BP");
    /* The agent $INCLUDEs PLATFORM.H now that its transport differs per
       platform, so a bare account needs one before it will compile.  Written
       here rather than assumed present: the whole point of seeding is that this
       account has nothing installed in it. */
    create_dir_file("BP.INC");
    mkdir("BP.INC", 0755);
    {
        FILE *ph = fopen("BP.INC/PLATFORM.H", "wb");
        if (!ph) return -1;
        char up[16];
        platform_name(up, sizeof up);
        fprintf(ph, "* PLATFORM.H - written by mv_agent_seed for a bare account.\n"
                    "$DEFINE MV\n$DEFINE %s\n", up);
        fclose(ph);
    }

    /* From the copy compiled into this binary, not from disk: the account being
       seeded may be anywhere, and the package may be nowhere near it. */
    FILE *o = fopen("BP/GIT.AGENT", "wb");
    if (!o) return -1;
    fputs(GIT_AGENT_SRC, o);
    fclose(o);

    snprintf(cmd, sizeof cmd,
             "printf 'BASIC BP GIT.AGENT\nQUIT\n' | %s >/dev/null 2>&1",
             mvs_shell());
    if (system(cmd) != 0) { /* likewise: the check below is the test */ }
    return agent_compiled() ? 0 : -1;
}
