/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* uvgit_rt.c — the record backend for UniVerse (mv_git#47).
 *
 * The peer of udtgit_rt.c, which implements the same contract over UniData's
 * InterCall.  There is no InterCall for us on UniVerse — it is a client SDK we
 * cannot get for Linux, and GCI is licensed and dead in the Trial Edition — so
 * these primitives go through a SESSION instead: BP/GIT.AGENT, running in the
 * account, reached over the framed pipe protocol in uvsession.c.
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
 * out and is replaced, because uvsession replays the opens — see uvsession.c.
 *
 * THE ACCOUNT IS AMBIENT.  The engine never says which account a record lives
 * in; it chdir's and works there, exactly as the MVX and UniData backends
 * assume.  So the session is opened lazily against the current directory on
 * first use and cached, which also means the licence is not taken until a
 * record is actually wanted.
 */

#define _POSIX_C_SOURCE 200809L

#include "gitd_rt.h"
#include "uvsession.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

struct mv_ctx { uv_session *s; };

/* One session per process, opened on first record use against the account we
   are standing in.  The engine has no notion of "which account", so neither can
   this: it is wherever the caller chdir'd to, which is the same rule the other
   backends follow. */
static mv_ctx g_ctx;

static uv_session *session(void) {
    if (!g_ctx.s) {
        char err[512] = "";
        g_ctx.s = uvs_open(".", err, sizeof err);
        if (!g_ctx.s)
            mv_fatal("uv-git: cannot reach the account: %s", err);
    }
    return g_ctx.s;
}

mv_ctx *mv_ctx_create(void) { return &g_ctx; }

/* Finish with the account we are in: close its session and forget it, so the
   next account opens its own.  This is what keeps a multi-account walk to ONE
   licence — and it must clear the cached pointer, since uvs_close frees the
   session it is pointing at. */
void mv_uv_release(void) {
    if (g_ctx.s) {
        uvs_close(g_ctx.s);
        g_ctx.s = NULL;
    }
}

void mv_ctx_destroy(mv_ctx *ctx) {
    /* Deliberately does NOT close the session: the engine creates and destroys
       contexts freely around individual operations, and tearing the session down
       with each one would pay a licence acquisition per record.  uvs_close_all()
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
    return uvs_call(session(), op, nargs, args, lens, out, outlen) == 0;
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
    fputs("uv-git: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    /* Release the licence on the way out — exit() runs the atexit handler
       uvsession installs, so a fatal error does not strand a session. */
    exit(1);
}
