/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* uvsession — see uvsession.h.  The client half of the agent protocol, and the
 * licence discipline that goes with it. */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE          /* realpath */

#include "uvsession.h"
#include "gitproto.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef UV_BIN
#define UV_BIN "uv"
#endif

struct uv_session {
    char  account[MVG_PATH_MAX];
    char  dir[MVG_PATH_MAX];        /* private run dir holding the two FIFOs */
    char  token[MVG_TOKEN_LEN + 1];
    int   req;                      /* we write requests here      */
    int   rsp;                      /* we read replies here        */
    pid_t pid;                      /* the uv process              */
    struct uv_session *next;
};

static uv_session *g_sessions;
static int g_jobs = 1;
static int g_handlers_installed;

void uvs_set_jobs(int n) { g_jobs = n > 0 ? n : 1; }
int  uvs_jobs(void)      { return g_jobs; }
const char *uvs_account(const uv_session *s) { return s ? s->account : ""; }

/* --- hex, the wire encoding -------------------------------------------- */

static const char HEXD[] = "0123456789ABCDEF";

static char *hex_enc(const char *in, long len, long *outlen) {
    char *o = malloc((size_t)len * 2 + 1);
    if (!o) return NULL;
    for (long i = 0; i < len; i++) {
        o[i * 2]     = HEXD[(unsigned char)in[i] >> 4];
        o[i * 2 + 1] = HEXD[(unsigned char)in[i] & 0x0F];
    }
    o[len * 2] = '\0';
    *outlen = len * 2;
    return o;
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static char *hex_dec(const char *in, long len, long *outlen) {
    if (len % 2) return NULL;
    char *o = malloc((size_t)len / 2 + 1);
    if (!o) return NULL;
    for (long i = 0; i < len; i += 2) {
        int h = hexv(in[i]), l = hexv(in[i + 1]);
        if (h < 0 || l < 0) { free(o); return NULL; }
        o[i / 2] = (char)((h << 4) | l);
    }
    o[len / 2] = '\0';
    *outlen = len / 2;
    return o;
}

/* --- exact-count I/O ---------------------------------------------------- */

static int write_all(int fd, const char *p, long n) {
    while (n > 0) {
        ssize_t w = write(fd, p, (size_t)n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= w;
    }
    return 0;
}

/* Reads are for an exact count, for the same reason the BASIC side's are: a
   FIFO held open read/write never reports EOF, so a short read is a partial
   frame to be completed, not an end of stream. */
static int read_all(int fd, char *p, long n) {
    while (n > 0) {
        ssize_t r = read(fd, p, (size_t)n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

/* --- teardown ------------------------------------------------------------ */

static void unlink_dir(const char *dir) {
    char p[MVG_PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/req", dir); unlink(p);
    snprintf(p, sizeof p, "%s/rsp", dir); unlink(p);
    snprintf(p, sizeof p, "%s/run",  dir); unlink(p);
    rmdir(dir);
}

/* Wait up to `ms` for the child, without blocking indefinitely: a session that
   has been asked to QUIT normally goes in well under a second, and one that does
   not must still not hold us — or its licence — forever. */
static int reap(pid_t pid, int ms) {
    const int step = 20;
    for (int waited = 0; waited <= ms; waited += step) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return 1;
        if (r < 0) return 1;                  /* already reaped */
        struct timespec ts = { 0, step * 1000000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

void uvs_close(uv_session *s) {
    if (!s) return;
    /* unlink from the registry first, so a signal during teardown does not
       revisit a session already being torn down */
    for (uv_session **pp = &g_sessions; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }

    /* Ask, rather than kill.  A session that reaches QUIT and STOPs releases its
       licence; one that is signalled may not. */
    if (s->req >= 0) {
        uvs_call(s, "QUIT", 0, NULL, NULL, NULL, NULL);
        close(s->req);
        s->req = -1;
    }
    if (s->rsp >= 0) { close(s->rsp); s->rsp = -1; }

    if (s->pid > 0) {
        if (!reap(s->pid, 3000)) {
            kill(s->pid, SIGTERM);
            if (!reap(s->pid, 2000)) {
                kill(s->pid, SIGKILL);
                reap(s->pid, 1000);
            }
        }
        s->pid = -1;
    }
    unlink_dir(s->dir);
    free(s);
}

void uvs_close_all(void) {
    while (g_sessions) uvs_close(g_sessions);
}

static void on_signal(int sig) {
    uvs_close_all();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_handlers(void) {
    if (g_handlers_installed) return;
    g_handlers_installed = 1;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
    signal(SIGPIPE, SIG_IGN);      /* a dead session is an error, not a death */
    atexit(uvs_close_all);
}

/* --- opening ------------------------------------------------------------- */

static void make_token(char *out) {
    static const char SET[] = "0123456789abcdef";
    unsigned char raw[MVG_TOKEN_LEN / 2];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, raw, sizeof raw) != (ssize_t)sizeof raw) {
        /* Not security against a determined local attacker — filesystem
           permissions do that.  This distinguishes sessions and catches a
           desynchronised stream, so a weaker source is still adequate. */
        unsigned long v = (unsigned long)getpid() ^ (unsigned long)time(NULL);
        for (size_t i = 0; i < sizeof raw; i++) { raw[i] = (unsigned char)v; v = v * 1103515245 + 12345; }
    }
    if (fd >= 0) close(fd);
    for (size_t i = 0; i < sizeof raw; i++) {
        out[i * 2]     = SET[raw[i] >> 4];
        out[i * 2 + 1] = SET[raw[i] & 0x0F];
    }
    out[MVG_TOKEN_LEN] = '\0';
}

uv_session *uvs_open(const char *account, char *err, size_t errcap) {
    install_handlers();

    char real[MVG_PATH_MAX];
    if (!realpath(account && account[0] ? account : ".", real)) {
        snprintf(err, errcap, "no such account: %s", account ? account : ".");
        return NULL;
    }
    /* Already have one?  Reuse it — the licence is already spent. */
    for (uv_session *s = g_sessions; s; s = s->next)
        if (!strcmp(s->account, real)) return s;

    /* Honour the licence cap: retire the oldest session before adding another,
       so the number held never exceeds what the caller allowed. */
    int live = 0;
    for (uv_session *s = g_sessions; s; s = s->next) live++;
    while (live >= g_jobs && g_sessions) {
        uv_session *last = g_sessions;
        while (last->next) last = last->next;
        uvs_close(last);
        live--;
    }

    uv_session *s = calloc(1, sizeof *s);
    if (!s) { snprintf(err, errcap, "out of memory"); return NULL; }
    s->req = s->rsp = -1;
    s->pid = -1;
    snprintf(s->account, sizeof s->account, "%s", real);
    make_token(s->token);

    /* A private directory for the pipes: 0700, since the token travels through
       it and a shared account means the OS uid cannot tell sessions apart. */
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    snprintf(s->dir, sizeof s->dir, "%s/uvgit-%ld-%s", tmp, (long)getpid(), s->token);
    if (mkdir(s->dir, 0700) != 0) {
        snprintf(err, errcap, "cannot create %s: %s", s->dir, strerror(errno));
        free(s);
        return NULL;
    }
    char reqp[MVG_PATH_MAX + 8], rspp[MVG_PATH_MAX + 8], runp[MVG_PATH_MAX + 8];
    snprintf(reqp, sizeof reqp, "%s/req", s->dir);
    snprintf(rspp, sizeof rspp, "%s/rsp", s->dir);
    snprintf(runp, sizeof runp, "%s/run", s->dir);
    if (mkfifo(reqp, 0600) != 0 || mkfifo(rspp, 0600) != 0) {
        snprintf(err, errcap, "cannot create pipes in %s: %s", s->dir, strerror(errno));
        unlink_dir(s->dir);
        free(s);
        return NULL;
    }

    /* The session's whole script: run the agent, then leave.  QUIT is there for
       the case where the agent STOPs early — without it the session would sit at
       the TCL prompt holding a licence. */
    FILE *rf = fopen(runp, "w");
    if (!rf) {
        snprintf(err, errcap, "cannot write %s: %s", runp, strerror(errno));
        unlink_dir(s->dir);
        free(s);
        return NULL;
    }
    fprintf(rf, "RUN BP GIT.AGENT %s %s %s\nQUIT\n", reqp, rspp, s->token);
    fclose(rf);

    /* Open both ends read/write so neither open blocks waiting for a peer, and
       so the pipe survives the agent closing its end between frames — the same
       property UniVerse's own OPENSEQ has, which is why the protocol is
       length-framed in the first place. */
    s->req = open(reqp, O_RDWR);
    s->rsp = open(rspp, O_RDWR);
    if (s->req < 0 || s->rsp < 0) {
        snprintf(err, errcap, "cannot open pipes: %s", strerror(errno));
        if (s->req >= 0) close(s->req);
        if (s->rsp >= 0) close(s->rsp);
        unlink_dir(s->dir);
        free(s);
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errcap, "fork: %s", strerror(errno));
        close(s->req); close(s->rsp);
        unlink_dir(s->dir);
        free(s);
        return NULL;
    }
    if (pid == 0) {
        /* The child must not hold our ends of the pipes: it gets its own through
           the agent's OPENSEQ, and an inherited fd would keep a pipe alive after
           we closed it. */
        close(s->req);
        close(s->rsp);
        if (chdir(real) != 0) _exit(127);
        /* A driver never wants curses.  UniVerse already treats a session with no
           terminal as a phantom, so a LOGIN paragraph's non-interactive guard
           fires by itself; this stops anything that runs anyway from painting. */
        setenv("TERM", "dumb", 1);
        int in = open(runp, O_RDONLY);
        if (in < 0) _exit(127);
        dup2(in, STDIN_FILENO);
        if (in != STDIN_FILENO) close(in);
        /* The session's own chatter (banner, prompts, whatever LOGIN prints) is
           not our protocol and must not be mistaken for it — the protocol has
           its own pipes.  Discard it. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execlp(UV_BIN, UV_BIN, (char *)NULL);
        _exit(127);
    }
    s->pid = pid;
    s->next = g_sessions;
    g_sessions = s;

    /* Prove the agent is actually up before handing the session out.  Without
       this the first real call would be where a missing or uncompiled agent
       surfaced, and it would look like a record failure rather than a setup one. */
    char *body = NULL;
    long blen = 0;
    int st = uvs_call(s, "PING", 0, NULL, NULL, &body, &blen);
    free(body);
    if (st != MVG_OK) {
        snprintf(err, errcap,
                 "the account I/O agent did not answer in %s — is BP/GIT.AGENT "
                 "compiled there?", real);
        uvs_close(s);
        return NULL;
    }
    return s;
}

/* --- one call ------------------------------------------------------------ */

int uvs_call(uv_session *s, const char *op, int nargs,
             const char *const *args, const long *lens,
             char **out, long *outlen) {
    if (out) *out = NULL;
    if (outlen) *outlen = 0;
    if (!s || s->req < 0) return MVG_EPROTO;
    if (nargs < 0) nargs = 0;

    /* body: nargs repetitions of [len(10)][len hex chars] */
    char *body = NULL;
    long blen = 0, bcap = 0;
    for (int i = 0; i < nargs; i++) {
        long alen = lens ? lens[i] : (long)strlen(args[i]);
        long elen = 0;
        char *enc = hex_enc(args[i] ? args[i] : "", alen, &elen);
        if (!enc) { free(body); return MVG_EPROTO; }
        if (blen + MVG_LEN_LEN + elen + 1 > bcap) {
            bcap = (blen + MVG_LEN_LEN + elen + 1) * 2;
            char *nb = realloc(body, (size_t)bcap);
            if (!nb) { free(enc); free(body); return MVG_EPROTO; }
            body = nb;
        }
        blen += snprintf(body + blen, (size_t)(bcap - blen), "%0*ld",
                         MVG_LEN_LEN, elen);
        memcpy(body + blen, enc, (size_t)elen);
        blen += elen;
        free(enc);
    }

    char hdr[MVG_REQ_HDR + 1];
    snprintf(hdr, sizeof hdr, "%s%-*s%-*s%0*d%0*ld",
             MVG_MAGIC,
             MVG_TOKEN_LEN,  s->token,
             MVG_OPCODE_LEN, op,
             MVG_NARGS_LEN,  nargs,
             MVG_LEN_LEN,    blen);
    if (write_all(s->req, hdr, MVG_REQ_HDR) != 0 ||
        (blen && write_all(s->req, body, blen) != 0)) {
        free(body);
        return MVG_EPROTO;
    }
    free(body);

    char rh[MVG_RSP_HDR + 1];
    if (read_all(s->rsp, rh, MVG_RSP_HDR) != 0) return MVG_EPROTO;
    rh[MVG_RSP_HDR] = '\0';
    if (memcmp(rh, MVG_MAGIC, MVG_MAGIC_LEN) != 0) return MVG_EPROTO;

    char nbuf[MVG_LEN_LEN + 1];
    memcpy(nbuf, rh + MVG_MAGIC_LEN, MVG_STATUS_LEN);
    nbuf[MVG_STATUS_LEN] = '\0';
    int status = atoi(nbuf);
    memcpy(nbuf, rh + MVG_MAGIC_LEN + MVG_STATUS_LEN, MVG_LEN_LEN);
    nbuf[MVG_LEN_LEN] = '\0';
    long rlen = atol(nbuf);
    if (rlen < 0 || rlen > MVG_MAX_BODY) return MVG_EPROTO;

    if (rlen > 0) {
        char *enc = malloc((size_t)rlen + 1);
        if (!enc) return MVG_EPROTO;
        if (read_all(s->rsp, enc, rlen) != 0) { free(enc); return MVG_EPROTO; }
        long dlen = 0;
        char *dec = hex_dec(enc, rlen, &dlen);
        free(enc);
        if (!dec) return MVG_EPROTO;
        if (out) *out = dec; else free(dec);
        if (outlen) *outlen = dlen;
    }
    return status;
}

int uvs_calls(uv_session *s, const char *op, int nargs,
              const char *const *args, char **out, long *outlen) {
    return uvs_call(s, op, nargs, args, NULL, out, outlen);
}
