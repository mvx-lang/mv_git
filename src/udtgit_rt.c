/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* udtgit_rt.c — the UniData record backend (udt-git).
 *
 * Implements the record primitives the record-git engine calls, over Rocket
 * UniData's InterCall C API (intcall.h).  Compiled only for the UniData build
 * (-DMVXGIT_UDT); the MVX build uses libmvxrt instead.  The engine is identical
 * either way — this file just binds its record calls to InterCall.
 *
 * Session: opened once per process against the local UniData server as a
 * UniObjects client (ic_unidata_session).  Credentials come from the
 * environment; the account is $MVXACCOUNT (the directory mvx-git/udt-git chdir
 * into), so ic_open resolves file names within it.
 *
 *   UDT_HOST      server host           (default "localhost")
 *   UDT_USER      login user            (default: $USER)
 *   UDT_PASSWORD  login password        (default: none)
 *   UDT_SERVICE   UniRPC service        (default "udcs")
 *
 * MultiValue marks are the same bytes as on MVX (I_FM/@AM = 0xFE, …), so the
 * engine's mark<->newline blob translation needs no change. */

#include "udtgit_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      /* strncasecmp */
#include <stdarg.h>

/* UniData InterCall.  intcall.h uses LPSTR (char*) / LPLONG (long*) and passes
   everything by address; status `code` is 0 on success. */
#include "intcall.h"

#ifndef IK_DATA
#  define IK_DATA 0
#endif
#ifndef IK_DICT
#  define IK_DICT 1
#endif
#ifndef IK_READ
#  define IK_READ 0
#endif
#ifndef IK_WRITE
#  define IK_WRITE 0
#endif

#define UDT_MAX_REC (16 * 1024 * 1024)   /* read buffer ceiling */
#define UDT_SELECT_LIST 0                /* the one select list the engine uses */

struct mv_ctx {
    long session;
    int  open;
};

/* Open the InterCall session lazily, on the first record operation, so
   session-free commands (init, log) work in a directory that is not yet a live
   UniData account. */
static void udt_ensure_session(mv_ctx *ctx) {
    if (ctx->open) return;
    char *host = getenv("UDT_HOST");
    char *user = getenv("UDT_USER");
    char *pass = getenv("UDT_PASSWORD");
    char *svc  = getenv("UDT_SERVICE");
    char *acct = getenv("MVXACCOUNT");
    if (!host || !*host) host = "localhost";
    if (!user || !*user) user = getenv("USER");
    if (!user) user = "";
    if (!pass) pass = "";
    if (!svc || !*svc) svc = "udcs";
    if (!acct || !*acct) acct = ".";
    long code = 0;
    ctx->session = ic_unidata_session(host, user, pass, acct, &code, NULL, svc);
    if (code != 0)
        mv_fatal("cannot open UniData session on %s account %s (code %ld)",
                  host, acct, code);
    ctx->open = 1;
}

/* --- value ops --------------------------------------------------------- */

void mv_init(mv_value *v) {
    v->data = NULL;
    v->len = 0;
    v->fid = 0;
    v->is_file = 0;
}

void mv_clear(mv_value *v) {
    if (v->is_file && v->fid) {
        long code;
        ic_close(&v->fid, &code);     /* free the InterCall channel */
    }
    free(v->data);
    v->data = NULL;
    v->len = 0;
    v->fid = 0;
    v->is_file = 0;
}

void mv_set_str(mv_value *v, const char *p, int64_t len) {
    char *nb = malloc((size_t)len + 1);
    if (!nb) mv_fatal("udt-git: out of memory");
    if (len) memcpy(nb, p, (size_t)len);
    nb[len] = '\0';
    free(v->data);
    v->data = nb;
    v->len = len;
    /* setting bytes makes this a plain string, never a file handle */
    v->is_file = 0;
    v->fid = 0;
}

int64_t mv_val_chars(const mv_value *v, char *numbuf, size_t cap,
                     const char **pp) {
    (void)numbuf;
    (void)cap;
    *pp = v->data ? v->data : "";
    return v->len;
}

/* --- context / session ------------------------------------------------- */

mv_ctx *mv_ctx_create(void) {
    mv_ctx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) mv_fatal("out of memory");
    return ctx;   /* the session opens lazily on first record op */
}

void mv_ctx_destroy(mv_ctx *ctx) {
    if (!ctx) return;
    if (ctx->open) {
        long code;
        ic_quit(&code);
    }
    free(ctx);
}

/* --- record I/O -------------------------------------------------------- */

int64_t mv_open(mv_ctx *ctx, const mv_value *dict, const mv_value *spec,
                 mv_value *fvar) {
    udt_ensure_session(ctx);
    long dict_flag = dict ? IK_DICT : IK_DATA;
    long namelen = (long)spec->len;
    long file_id = 0, status = 0, code = 0;
    ic_open(&file_id, &dict_flag, spec->data ? spec->data : "", &namelen,
            &status, &code);
    if (code != 0) return 0;
    mv_clear(fvar);
    fvar->fid = file_id;
    fvar->is_file = 1;
    return 1;
}

int64_t mv_read(mv_ctx *ctx, mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t lock) {
    udt_ensure_session(ctx);
    long fid = fvar->fid;
    long lk = (long)lock ? (long)lock : IK_READ;
    long idlen = (long)id->len;
    long maxlen = UDT_MAX_REC;
    long reclen = 0, status = 0, code = 0;
    char *buf = malloc(UDT_MAX_REC);
    if (!buf) mv_fatal("udt-git: out of memory");
    ic_read(&fid, &lk, id->data ? id->data : "", &idlen, buf, &maxlen,
            &reclen, &status, &code);
    if (code != 0) {   /* code 30001 = record not found; any code != 0 = absent */
        free(buf);
        return 0;
    }
    mv_set_str(rec, buf, reclen);
    free(buf);
    return 1;
}

int64_t mv_write(mv_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                  const mv_value *id, int64_t keep_lock, int64_t onerr) {
    udt_ensure_session(ctx);
    (void)onerr;
    long fid = fvar->fid;
    long lk = (long)keep_lock;
    long idlen = (long)id->len;
    long reclen = (long)rec->len;
    long status = 0, code = 0;
    ic_write(&fid, &lk, id->data ? id->data : "", &idlen,
             rec->data ? rec->data : "", &reclen, &status, &code);
    return code == 0 ? 1 : 0;
}

int64_t mv_delete_rec(mv_ctx *ctx, const mv_value *fvar, const mv_value *id) {
    udt_ensure_session(ctx);
    long fid = fvar->fid;
    long lk = IK_READ;
    long idlen = (long)id->len;
    long status = 0, code = 0;
    ic_delete(&fid, &lk, id->data ? id->data : "", &idlen, &status, &code);
    return code == 0 ? 1 : 0;
}

void mv_select(mv_ctx *ctx, const mv_value *fvar) {
    udt_ensure_session(ctx);
    long fid = fvar->fid;
    long list = UDT_SELECT_LIST;
    long code = 0;
    ic_select(&fid, &list, &code);
}

int64_t mv_readnext(mv_ctx *ctx, mv_value *id) {
    udt_ensure_session(ctx);
    long list = UDT_SELECT_LIST;
    long maxlen = IC_MAX_RECID_LENGTH;   /* buffer size (input) */
    long actlen = 0;                     /* actual id length (output) */
    long endflag = 0;                    /* 0 = got a record, non-zero = end */
    char idbuf[IC_MAX_RECID_LENGTH + 1];
    ic_readnext(&list, idbuf, &maxlen, &actlen, &endflag);
    if (endflag != 0) return 0;          /* end of the select list */
    mv_set_str(id, idbuf, actlen);
    return 1;
}

int64_t mv_createfile(mv_ctx *ctx, const mv_value *spec, const mv_value *type) {
    udt_ensure_session(ctx);
    /* UniData CREATE.FILE via ECL.  A directory file for "DIR"; otherwise a
       dynamic hash file (the account default).  Verified/refined against the
       demo account when materialize is exercised. */
    const char *name = spec->data ? spec->data : "";
    int is_dir = type && type->data && type->len == 3 &&
                 strncasecmp(type->data, "DIR", 3) == 0;
    char cmd[1024];
    if (is_dir)
        snprintf(cmd, sizeof cmd, "CREATE.FILE %s 1 DIR", name);
    else
        snprintf(cmd, sizeof cmd, "CREATE.FILE %s DYNAMIC", name);
    long cmdlen = (long)strlen(cmd);
    long status = 0, code = 0, out = 0, outmax = 0, r1 = 0, r2 = 0;
    ic_execute(cmd, &cmdlen, NULL, &outmax, &out, &r1, &status, &code);
    (void)r2;
    return code == 0 ? 1 : 0;
}

void mv_filelist(mv_ctx *ctx, mv_value *dst) {
    udt_ensure_session(ctx);
    /* TODO: enumerate the account's files (LISTF / VOC F-pointers) as
       "name<VM>type" @AM rows.  Not needed for INIT/ADD/COMMIT/STATUS; filled
       in when the clone/materialize path is exercised on UniData. */
    mv_set_str(dst, "", 0);
}

/* --- misc -------------------------------------------------------------- */

int mv_openaccount(void) {
    const char *v = getenv("MVX_OPENACCOUNT");
    return v && *v && strcmp(v, "0") != 0;
}

void mv_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "udt-git: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}
