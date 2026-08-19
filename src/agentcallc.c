/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* agentcallc.c — the agent's transport on UniData, over CallC (mv_git#45).
 *
 * BP/GIT.AGENT is portable BASIC except for four operations: open the pipes,
 * read an exact count with a deadline, write, close.  On UniVerse those are
 * OPENSEQ/READBLK/WRITEBLK.  On UniData they cannot be, and not for want of
 * looking — measured on 8.3, in every compile mode:
 *
 *     READBLK B FROM F, n     ->  syntax error   (no exact-count block read)
 *     TIMEOUT F, secs         ->  syntax error   (no timed read)
 *
 * Both are load-bearing.  Exact-count reads are what the length-framed protocol
 * requires, because a FIFO held read/write never reports EOF; the timeout is
 * what lets an idle agent give its licence back rather than sit forever.
 *
 * UniData does have CallC, so the transport moves into C.  The record work stays
 * in BASIC exactly as before — this replaces four statements, not the design.
 *
 * WHY THIS IS SAFE THROUGH CallC, which marshals every string with strlen and so
 * cannot carry a NUL: the wire format is hex payloads and fixed-width ASCII
 * headers, deliberately (gitproto.h), so nothing crossing this boundary is ever
 * binary.  The encoding chosen to survive NLS translation on UniVerse turns out
 * to be exactly what makes a CallC transport possible on UniData.
 *
 * The file descriptors live here, in statics, rather than being handed back to
 * BASIC as numbers: an fd is meaningless to BASIC, and one session serves one
 * caller, so there is nothing to disambiguate.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_req = -1;      /* requests arrive here  */
static int g_rsp = -1;      /* replies leave here    */
static char *g_buf;         /* grown to fit the largest read so far */
static size_t g_cap;

/* CallC returns a NUL-terminated string; "" is the "nothing" answer and any
   other value is either data or a diagnostic the caller prints. */
static char *ok(void)  { return ""; }

/* $MVGIT_CALLC_LOG — a trace of the wire itself, both ends.
 *
 * Worth its keep: this transport cannot report its own faults.  A malformed
 * frame is not an error anywhere — the reader simply waits for bytes that never
 * come, and a length-framed protocol has no resynchronisation point to complain
 * from.  When the agent's reply header went out as "MVG1%4%10" (nine characters
 * where eighteen were expected) every layer stayed silent and the only visible
 * symptom was a session that "did not answer".  Printing what actually crossed
 * found it immediately.  The driver writes its SEND lines to the same file
 * (mvsession.c), so one setting shows both directions in order.
 *
 * Costs nothing when the variable is unset, which is always, in production. */
static void tlog(const char *fmt, const char *a, const char *b) {
    const char *lp = getenv("MVGIT_CALLC_LOG");
    if (!lp || !*lp) return;
    FILE *f = fopen(lp, "a");
    if (!f) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "[%ld.%03ld pid=%ld req=%d rsp=%d] ", (long)ts.tv_sec,
            ts.tv_nsec / 1000000L, (long)getpid(), g_req, g_rsp);
    fprintf(f, fmt, a ? a : "(null)", b ? b : "(null)");
    fclose(f);
}

/* AGOPEN(reqpath, rsppath) — open both pipes.  Read/write on purpose: it is
   what stops a FIFO reporting EOF between frames, which is the same property
   UniVerse's OPENSEQ has and which the framing is built around. */
char *AGOPEN(char *reqp, char *rspp) {
    if (g_req >= 0) close(g_req);
    if (g_rsp >= 0) close(g_rsp);
    tlog("AGOPEN req=[%s] rsp=[%s]\n", reqp, rspp);
    /* WAIT FOR THE PIPES, briefly.  In the ordinary case the driver made them
       before starting us and they are already there.  In ATTACH mode the order
       is reversed — the verb backgrounds the CLI and then becomes the agent — so
       we may arrive first.  A short retry costs nothing when they exist and is
       the difference between working and a puzzling failure when they do not
       yet. */
    for (int tries = 0; tries < 100; tries++) {
        if (access(reqp ? reqp : "", F_OK) == 0 &&
            access(rspp ? rspp : "", F_OK) == 0) break;
        struct timespec ts = { 0, 100 * 1000 * 1000 };   /* 100ms */
        nanosleep(&ts, NULL);
    }
    g_req = open(reqp ? reqp : "", O_RDWR);
    g_rsp = open(rspp ? rspp : "", O_RDWR);
    tlog("AGOPEN done%s%s\n", "", "");
    if (g_req < 0 || g_rsp < 0) {
        if (g_req >= 0) { close(g_req); g_req = -1; }
        if (g_rsp >= 0) { close(g_rsp); g_rsp = -1; }
        return "AGOPEN: cannot open the pipes";
    }
    return ok();
}

/* AGREAD(count, timeout) — exactly `count` characters, or "" if `timeout`
   seconds pass with the read incomplete.  Exact-count because the protocol says
   how long every frame is and a FIFO will not tell us; timed because an agent
   nobody is talking to must be able to stop waiting and release its licence. */
char *AGREAD(char *count, char *timeout) {
    tlog("AGREAD count=[%s] timeout=[%s]\n", count, timeout);
    long n = count ? atol(count) : 0;
    int secs = timeout ? atoi(timeout) : 0;
    if (n <= 0 || g_req < 0) { tlog("AGREAD bail%s%s\n", "", ""); return ""; }
    if ((size_t)n + 1 > g_cap) {
        size_t nc = (size_t)n + 1;
        char *nb = realloc(g_buf, nc);
        if (!nb) return "";
        g_buf = nb;
        g_cap = nc;
    }
    long got = 0;
    int waited = 0;
    while (got < n) {
        struct pollfd pfd = { g_req, POLLIN, 0 };
        int slice = secs > 0 ? 1000 : -1;          /* 0 = wait indefinitely */
        int pr = poll(&pfd, 1, slice);
        if (pr < 0) { if (errno == EINTR) continue; return ""; }
        if (pr == 0) {
            waited++;
            if (secs > 0 && waited >= secs) { tlog("AGREAD timeout%s%s\n","",""); return ""; }
            continue;
        }
        ssize_t r = read(g_req, g_buf + got, (size_t)(n - got));
        if (r < 0) { if (errno == EINTR) continue; return ""; }
        if (r == 0) return "";
        got += r;
    }
    g_buf[n] = '\0';
    tlog("AGREAD got [%s]%s\n", g_buf, "");
    return g_buf;
}

/* AGWRITE(data) — the whole string, short writes retried. */
char *AGWRITE(char *data) {
    tlog("AGWRITE data=[%s]%s\n", data, "");
    if (g_rsp < 0) return "AGWRITE: not open";
    const char *p = data ? data : "";
    size_t n = strlen(p);
    while (n > 0) {
        ssize_t w = write(g_rsp, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return "AGWRITE: write failed";
        }
        p += w;
        n -= (size_t)w;
    }
    tlog("AGWRITE done%s%s\n", "", "");
    return ok();
}

/* AGCLOSE(unused) — CallC entry points take at least one argument. */
char *AGCLOSE(char *unused) {
    (void)unused;
    if (g_req >= 0) { close(g_req); g_req = -1; }
    if (g_rsp >= 0) { close(g_rsp); g_rsp = -1; }
    free(g_buf);
    g_buf = NULL;
    g_cap = 0;
    return ok();
}
