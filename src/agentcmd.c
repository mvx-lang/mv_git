/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* agentcmd.c — `<driver>-git agent`, the diagnostic for the session layer.
 *
 * One opcode and its arguments, or a sequence on stdin run on ONE session.  The
 * sequence form is the one that matters: a file handle from OPEN means nothing
 * to a later call on a different session, so a probe that cannot hold a session
 * cannot exercise the part most likely to be wrong.
 *
 * Shared by uv-git and udt-git because it is about the SESSION, not about either
 * platform — the same reason mvsession.c and agent_rt.c are shared.  It exists
 * because when a record operation misbehaves, this says whether the session, the
 * protocol or the caller is at fault, with no repository in the way.  Bringing a
 * platform up without it means debugging blind, which is expensive: on UniVerse
 * it cost an afternoon on a handler that had silently compiled to nothing.
 */

#define _POSIX_C_SOURCE 200809L

#include "gitproto.h"
#include "mvsession.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

/* One call, rendered for a person: the reply as lines, or the status. */
static int agent_one(mv_session *ses, const char *op, int na,
                     const char *const *a) {
    /* Opcodes are uppercase on the wire; a person typing `agent ping` means
       PING.  Fold here rather than at the agent, so the protocol stays exact
       and only this diagnostic is forgiving. */
    char upop[MVG_OPCODE_LEN + 1];
    size_t oi = 0;
    for (; op[oi] && oi < sizeof upop - 1; oi++)
        upop[oi] = (char)toupper((unsigned char)op[oi]);
    upop[oi] = '\0';

    char *body = NULL;
    long blen = 0;
    int st = mvs_calls(ses, upop, na, a, &body, &blen);
    if (st != 0) {
        fprintf(stderr, "status %d%s%.*s\n", st, blen ? ": " : "",
                (int)blen, body ? body : "");
        free(body);
        return 1;
    }
    if (body) {
        /* record and list text is @AM/@VM separated; show the structure */
        for (long k = 0; k < blen; k++) {
            unsigned char c = (unsigned char)body[k];
            if (c == 0xFE) body[k] = '\n';
            else if (c == 0xFD) body[k] = '|';
        }
        fwrite(body, 1, (size_t)blen, stdout);
        free(body);
    }
    fputc('\n', stdout);
    return 0;
}

/* `agent [OPCODE [arg...]]` — with an opcode, run it; without, read a sequence
   from stdin, one "OP arg arg…" per line, all on one session.  `i` indexes the
   `agent` word itself.  Returns a process exit status. */
int mv_agent_cmd(int argc, char **argv, int i) {
    char err[512] = "";
    mv_session *ses = mvs_open(".", err, sizeof err);
    if (!ses) {
        fprintf(stderr, "agent: %s\n", err);
        return 1;
    }
    int rc = 0;
    if (i + 1 < argc) {
        const char *a[8];
        int na = 0;
        for (int k = i + 2; k < argc && na < 8; k++) a[na++] = argv[k];
        rc = agent_one(ses, argv[i + 1], na, a);
    } else {
        char line[8192];
        while (fgets(line, sizeof line, stdin)) {
            size_t n = strlen(line);
            while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
            if (!line[0] || line[0] == '#') continue;
            const char *a[8];
            int na = 0;
            char *save = NULL;
            char *op = strtok_r(line, " \t", &save);
            if (!op) continue;
            for (char *t; na < 8 && (t = strtok_r(NULL, " \t", &save)); )
                a[na++] = t;
            printf("%s: ", op);
            fflush(stdout);
            if (agent_one(ses, op, na, a) != 0) rc = 1;
        }
    }
    mvs_close(ses);
    return rc;
}
