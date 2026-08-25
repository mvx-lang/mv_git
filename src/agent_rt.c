/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* agent_rt.c — the record backend that reaches records through a SESSION
 * (mv_git#47).
 *
 * Used by every MV platform that keeps its records behind a licensed session:
 * BP/GIT.AGENT runs in the account and these primitives talk to it over the
 * framed pipe protocol in mvsession.c.  Nothing here is platform-specific —
 * it talks to a session, not to a product — so UniVerse and UniData share it,
 * differing only in which shell mvsession starts.
 *
 * On UniVerse there is no alternative: InterCall is a client SDK unavailable for
 * Linux and GCI is licensed and dead in the Trial Edition.  On UniData there is
 * one — udtgit_rt.c over InterCall — and this replaces it anyway, because
 * InterCall authenticates with a stored password rather than as the person
 * running the command (mv_git#45).
 *
 * That one substitution is the whole port.  The engine in mvxgit.c is written
 * against this contract and nothing else, so implementing it here puts every
 * engine operation on UniVerse — add, status, checkout, commit, diff — with no
 * verbs and no BASIC git code involved.  The session does account I/O; uv-git
 * keeps libgit2 and does the git work in process.
 *
 * A FILE HANDLE IS A NUMBER, not a pointer.  The agent owns the open files and
 * hands out slot numbers, so mv_value.fid carries the slot and every operation
 * on a file is one round trip naming it.  Handles survive a session that times
 * out and is replaced, because mvsession replays the opens — see mvsession.c.
 *
 * THE ACCOUNT IS AMBIENT.  The engine never says which account a record lives
 * in; it chdir's and works there, exactly as the MVX and UniData backends
 * assume.  So the session is opened lazily against the current directory on
 * first use and cached, which also means the licence is not taken until a
 * record is actually wanted.
 */

#define _POSIX_C_SOURCE 200809L

#include "gitd_rt.h"
#include "mvxgit.h"      /* mv_account_furniture() */
#include "mvsession.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

struct mv_ctx { mv_session *s; };

/* One session per process, opened on first record use against the account we
   are standing in.  The engine has no notion of "which account", so neither can
   this: it is wherever the caller chdir'd to, which is the same rule the other
   backends follow. */
static mv_ctx g_ctx;

static mv_session *session(void) {
    if (!g_ctx.s) {
        char err[512] = "";
        g_ctx.s = mvs_open(".", err, sizeof err);
        if (!g_ctx.s)
            mv_fatal("cannot reach the account: %s", err);
    }
    return g_ctx.s;
}

mv_ctx *mv_ctx_create(void) { return &g_ctx; }

/* Finish with the account we are in: close its session and forget it, so the
   next account opens its own.  This is what keeps a multi-account walk to ONE
   licence — and it must clear the cached pointer, since mvs_close frees the
   session it is pointing at. */
void mv_agent_release(void) {
    if (g_ctx.s) {
        mvs_close(g_ctx.s);
        g_ctx.s = NULL;
    }
}

void mv_ctx_destroy(mv_ctx *ctx) {
    /* Deliberately does NOT close the session: the engine creates and destroys
       contexts freely around individual operations, and tearing the session down
       with each one would pay a licence acquisition per record.  mvs_close_all()
       at exit is what releases it — and the agent's idle timeout is the backstop
       if we never get there. */
    (void)ctx;
}

/* --- value ops ----------------------------------------------------------- */
/* Identical to the other backends: a plain owned buffer with `len`
   authoritative, since a record may contain NULs. */

void mv_init(mv_value *v) {
    if (!v) return;
    v->data = NULL; v->len = 0; v->fid = 0; v->is_file = 0;
}

void mv_clear(mv_value *v) {
    if (!v) return;
    free(v->data);
    mv_init(v);
}

void mv_set_str(mv_value *v, const char *p, int64_t len) {
    if (!v) return;
    free(v->data);
    v->data = NULL; v->len = 0; v->fid = 0; v->is_file = 0;
    if (len < 0) len = 0;
    v->data = malloc((size_t)len + 1);
    if (!v->data) mv_fatal("out of memory assigning a %lld-byte value",
                           (long long)len);
    if (p && len) memcpy(v->data, p, (size_t)len);
    v->data[len] = '\0';        /* convenience only; len is authoritative */
    v->len = len;
}

int64_t mv_val_chars(const mv_value *v, char *numbuf, size_t cap,
                     const char **pp) {
    (void)numbuf; (void)cap;
    if (!v || !v->data) { if (pp) *pp = ""; return 0; }
    if (pp) *pp = v->data;
    return v->len;
}

/* --- helpers ------------------------------------------------------------- */

static const char *sdata(const mv_value *v) {
    return (v && v->data) ? v->data : "";
}

/* A call whose arguments are counted, so record content carrying NULs survives.
   Returns 1 on MVG_OK, 0 otherwise; the reply body (if wanted) is malloc'd. */
static int call(const char *op, int nargs, const char *const *args,
                const long *lens, char **out, long *outlen) {
    return mvs_call(session(), op, nargs, args, lens, out, outlen) == 0;
}

/* --- record I/O ---------------------------------------------------------- */

int64_t mv_open(mv_ctx *ctx, const mv_value *dict, const mv_value *spec,
                mv_value *fvar) {
    (void)ctx;
    const char *a[2];
    int na = 0;
    a[na++] = sdata(spec);
    if (dict) a[na++] = "DICT";
    char *body = NULL;
    if (!call("OPEN", na, a, NULL, &body, NULL)) { free(body); return 0; }
    int slot = body ? atoi(body) : 0;
    free(body);
    if (slot <= 0) return 0;
    mv_clear(fvar);
    fvar->fid = slot;
    fvar->is_file = 1;
    return 1;
}

int64_t mv_read(mv_ctx *ctx, mv_value *rec, const mv_value *fvar,
                const mv_value *id, int64_t lock) {
    (void)ctx; (void)lock;      /* the agent reads without locking; see below */
    char h[24];
    snprintf(h, sizeof h, "%ld", (long)fvar->fid);
    const char *a[2] = { h, sdata(id) };
    long l[2] = { (long)strlen(h), (long)(id ? id->len : 0) };
    char *body = NULL;
    long blen = 0;
    if (!call("READ", 2, a, l, &body, &blen)) { free(body); return 0; }
    mv_set_str(rec, body ? body : "", blen);
    free(body);
    return 1;
}

int64_t mv_write(mv_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t keep_lock, int64_t onerr) {
    (void)ctx; (void)keep_lock; (void)onerr;
    char h[24];
    snprintf(h, sizeof h, "%ld", (long)fvar->fid);
    const char *a[3] = { h, sdata(id), sdata(rec) };
    long l[3] = { (long)strlen(h), (long)(id ? id->len : 0),
                  (long)(rec ? rec->len : 0) };
    return call("WRITE", 3, a, l, NULL, NULL) ? 1 : 0;
}

int64_t mv_delete_rec(mv_ctx *ctx, const mv_value *fvar, const mv_value *id) {
    (void)ctx;
    char h[24];
    snprintf(h, sizeof h, "%ld", (long)fvar->fid);
    const char *a[2] = { h, sdata(id) };
    long l[2] = { (long)strlen(h), (long)(id ? id->len : 0) };
    return call("DELETE", 2, a, l, NULL, NULL) ? 1 : 0;
}

void mv_select(mv_ctx *ctx, const mv_value *fvar) {
    (void)ctx;
    char h[24];
    snprintf(h, sizeof h, "%ld", (long)fvar->fid);
    const char *a[1] = { h };
    call("SELECT", 1, a, NULL, NULL, NULL);
}

int64_t mv_readnext(mv_ctx *ctx, mv_value *id) {
    (void)ctx;
    char *body = NULL;
    long blen = 0;
    if (!call("READNEXT", 0, NULL, NULL, &body, &blen)) { free(body); return 0; }
    /* An empty body is the end of the list, and it is unambiguous: a record id
       is never empty. */
    if (blen <= 0) { free(body); return 0; }
    mv_set_str(id, body, blen);
    free(body);
    return 1;
}

int64_t mv_deletefile(mv_ctx *ctx, const mv_value *spec) {
    (void)ctx;
    const char *a[1] = { sdata(spec) };
    char *body = NULL;
    int ok = call("DELETEFILE", 1, a, NULL, &body, NULL);
    free(body);
    return ok ? 1 : 0;
}

int64_t mv_createfile(mv_ctx *ctx, const mv_value *spec, const mv_value *type) {
    (void)ctx;
    const char *a[2] = { sdata(spec), sdata(type) };
    char *body = NULL;
    int ok = call("CREATEFILE", 2, a, NULL, &body, NULL);
    free(body);
    return ok ? 1 : 0;
}

void mv_filelist(mv_ctx *ctx, mv_value *dst) {
    (void)ctx;
    char *body = NULL;
    long blen = 0;
    if (!call("FILELIST", 0, NULL, NULL, &body, &blen)) {
        mv_set_str(dst, "", 0);
        free(body);
        return;
    }
    /* Drop UniData's account furniture before the list reaches anyone.  The
       agent enumerates the account's VOC and has no view on what a commit
       should leave out; the CallC backend filters in its own enumeration, and
       this is the same gate for the agent transport, which is what the CLI
       actually uses (mv_git#72).  Filtered in place — the list is
       name<VM>class, @AM-separated, and it only ever shrinks. */
    long out = 0;
    for (long i = 0; i < blen; ) {
        long s = i;
        while (i < blen && (unsigned char)body[i] != 0xFE) i++;
        long e = i;                                  /* entry is [s, e) */
        long n = s;
        while (n < e && (unsigned char)body[n] != 0xFD) n++;
        if (!mv_account_furniture(body + s, (size_t)(n - s))) {
            if (out) body[out++] = (char)0xFE;
            memmove(body + out, body + s, (size_t)(e - s));
            out += e - s;
        }
        if (i < blen) i++;                           /* past the @AM */
    }
    blen = out;
    mv_set_str(dst, body ? body : "", blen);
    free(body);
}

int64_t mv_fileclass(mv_ctx *ctx, const char *name, char *out, size_t cap) {
    (void)ctx;
    snprintf(out, cap, "hash");
    const char *a[1] = { name };
    char *body = NULL;
    long blen = 0;
    if (!call("FILECLASS", 1, a, NULL, &body, &blen)) { free(body); return 0; }
    if (body && blen > 0 && (size_t)blen < cap) {
        memcpy(out, body, (size_t)blen);
        out[blen] = '\0';
    }
    free(body);
    return 1;
}

int64_t mv_indices(mv_ctx *ctx, const char *name, char *out, size_t cap) {
    (void)ctx;
    if (cap) out[0] = '\0';
    const char *a[1] = { name };
    char *body = NULL;
    long blen = 0;
    if (!call("INDICES", 1, a, NULL, &body, &blen)) { free(body); return 0; }
    if (body && blen > 0 && (size_t)blen < cap) {
        memcpy(out, body, (size_t)blen);
        out[blen] = '\0';
    }
    free(body);
    return 1;
}

/* --- misc ---------------------------------------------------------------- */

int mv_openaccount(void) {
    const char *v = getenv("MVX_OPENACCOUNT");
    return v && *v && strcmp(v, "0") != 0;
}

/* Master-VOC classification is a pure table lookup with no record access, and it
   must stay byte-for-byte the same judgement as the other backends since it
   decides what travels in a commit.  0 = keep (the user's own procs), 1 = always
   drop (a verb/keyword the destination supplies), 2 = drop in the open
   interchange only (a platform file/Q/remote pointer, which travels instead as
   <file>.DICT/%FILE%). */
int mv_voc_class(const char *type, int64_t len) {
    static const struct { const char *t; int c; } tbl[] = {
        {"V", 1}, {"K", 1},                              /* verb, keyword */
        /* UniData's LOCAL catalog (`CATALOG BP PROG LOCAL`), which writes
           `C` / <abs path into CTLG> / <file> <prog>.  Class 1, like a verb:
           cataloguing on the far side recreates it, and what the record holds
           is an ABSOLUTE path on the machine that committed it, pointing into
           CTLG -- which is furniture and never travels.  So a clone anywhere
           else got a VOC entry naming a directory that does not exist
           (mv_git#137).  UniVerse writes `V` for the same act and has no C-type
           records at all (measured on 14.2.1: zero in a stock account), so the
           shared U2 table can carry it. */
        {"C", 1},                                        /* udt local catalog */
        {"F", 2}, {"LF", 2}, {"DF", 2}, {"DIR", 2},      /* file definitions */
        {"Q", 2}, {"X", 2}, {"R", 2},                    /* account/remote ptrs */
        {NULL, 0}
    };
    if (!type || len <= 0) return 0;
    for (int i = 0; tbl[i].t; i++) {
        size_t sl = strlen(tbl[i].t);
        if ((size_t)len == sl && strncasecmp(type, tbl[i].t, sl) == 0)
            return tbl[i].c;
    }
    return 0;
}

void mv_fatal(const char *fmt, ...) {
    va_list ap;
    /* Name the driver the way the user invoked it, without a per-platform
       string here: the shell mvsession was told to start is exactly what
       distinguishes uv-git from udt-git. */
    const char *sh = mvs_shell();
    fprintf(stderr, "%s-git: ", (sh && *sh) ? sh : "mv");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    /* Release the licence on the way out — exit() runs the atexit handler
       mvsession installs, so a fatal error does not strand a session. */
    exit(1);
}
