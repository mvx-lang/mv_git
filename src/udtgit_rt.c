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

/* Look up `key` in the session config file, copying its value into out[cap].
   The file lets an admin set the login once so udt-git runs with no credentials
   in the environment or on the command line ("credential-less" — a git service
   account for UniData).  Path: $UDTGIT_CONFIG, else ~/.udtgitrc.  Format: plain
   `key = value` lines (user, password, host, service); '#' begins a comment.
   Returns 1 when found.  The file is read once and cached. */
static int udt_cfg(const char *key, char *out, size_t cap) {
    static char *data = NULL;
    static int loaded = 0;
    if (!loaded) {
        loaded = 1;
        char pbuf[4096];
        const char *path = getenv("UDTGIT_CONFIG");
        if (!path || !*path) {
            const char *home = getenv("HOME");
            if (home && *home) {
                snprintf(pbuf, sizeof pbuf, "%s/.udtgitrc", home);
                path = pbuf;
            }
        }
        FILE *f = path && *path ? fopen(path, "r") : NULL;
        if (f) {
            long n;
            if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 &&
                n < (1L << 20) && fseek(f, 0, SEEK_SET) == 0 &&
                (data = malloc((size_t)n + 1))) {
                size_t got = fread(data, 1, (size_t)n, f);
                data[got] = '\0';
            }
            fclose(f);
        }
    }
    if (!data) return 0;
    size_t klen = strlen(key);
    for (char *p = data; p && *p;) {
        char *eol = strchr(p, '\n');
        char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '#' && strncmp(q, key, klen) == 0) {
            char *r = q + klen;
            while (*r == ' ' || *r == '\t') r++;
            if (*r == '=') {
                r++;
                while (*r == ' ' || *r == '\t') r++;
                char *e = eol ? eol : r + strlen(r);
                while (e > r && (e[-1] == '\r' || e[-1] == ' ' ||
                                 e[-1] == '\t' || e[-1] == '\n'))
                    e--;
                size_t vl = (size_t)(e - r);
                if (vl >= cap) vl = cap - 1;
                memcpy(out, r, vl);
                out[vl] = '\0';
                return 1;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return 0;
}

/* Resolve a session setting: the environment wins, then the config file, then
   `dflt` (NULL for none). */
static const char *udt_setting(const char *env, const char *cfgkey,
                               char *buf, size_t cap, const char *dflt) {
    const char *v = getenv(env);
    if (v && *v) return v;
    if (udt_cfg(cfgkey, buf, cap) && buf[0]) return buf;
    return dflt;
}

/* Open the InterCall session lazily, on the first record operation, so
   session-free commands (init, log) work in a directory that is not yet a live
   UniData account. */
static void udt_ensure_session(mv_ctx *ctx) {
    if (ctx->open) return;
    char ub[256], pb[256], hb[256], sb[64];
    const char *host = udt_setting("UDT_HOST", "host", hb, sizeof hb, "localhost");
    const char *user = udt_setting("UDT_USER", "user", ub, sizeof ub, getenv("USER"));
    const char *pass = udt_setting("UDT_PASSWORD", "password", pb, sizeof pb, "");
    const char *svc  = udt_setting("UDT_SERVICE", "service", sb, sizeof sb, "udcs");
    const char *acct = getenv("MVXACCOUNT");
    if (!user) user = "";
    if (!acct || !*acct) acct = ".";
    long code = 0;
    ctx->session = ic_unidata_session((char *)host, (char *)user, (char *)pass,
                                      (char *)acct, &code, NULL, (char *)svc);
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
    /* Enumerate the account's own files from VOC: an F-type record (field 1 ==
       "F"/"DIR") whose data path (field 2) is a bare name — no '/' and no
       leading '@' — is a local file, as opposed to a Q/F pointer into another
       account or the system.  Emit the file names @AM-separated. */
    long fid = 0, dflag = IK_DATA, nlen = 3, st = 0, code = 0;
    char voc[] = "VOC";
    ic_open(&fid, &dflag, voc, &nlen, &st, &code);
    if (code != 0) { mv_set_str(dst, "", 0); return; }
    long list = 0, c2 = 0;
    ic_select(&fid, &list, &c2);

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) mv_fatal("out of memory");
    char *rec = malloc(UDT_MAX_REC);
    if (!rec) mv_fatal("out of memory");

    for (;;) {
        char id[IC_MAX_RECID_LENGTH + 1];
        long mx = IC_MAX_RECID_LENGTH, act = 0, endflag = 0, lst = 0;
        ic_readnext(&lst, id, &mx, &act, &endflag);
        if (endflag != 0) break;
        long lk = IK_READ, ilen = act, rmax = UDT_MAX_REC, rl = 0, rs = 0, rc = 0;
        ic_read(&fid, &lk, id, &ilen, rec, &rmax, &rl, &rs, &rc);
        if (rc != 0) continue;
        /* field 1 = up to first @AM; field 2 = next @AM-delimited segment */
        long f1e = 0;
        while (f1e < rl && (unsigned char)rec[f1e] != 0xFE) f1e++;
        long f2s = f1e < rl ? f1e + 1 : rl, f2e = f2s;
        while (f2e < rl && (unsigned char)rec[f2e] != 0xFE) f2e++;
        int is_file = (f1e == 1 && rec[0] == 'F') ||
                      (f1e == 3 && strncmp(rec, "DIR", 3) == 0);
        if (!is_file) continue;
        int local = 1;
        if (f2e > f2s && rec[f2s] == '@') local = 0;
        for (long k = f2s; local && k < f2e; k++)
            if (rec[k] == '/') local = 0;
        if (!local) continue;
        /* skip UniData work/system files: _HOLD_ / _PH_ / _EDAMAP_ … (name
           wrapped in underscores) and &SAVEDLISTS& / &PH& … (wrapped in &). */
        if (act >= 2 && ((id[0] == '_' && id[act - 1] == '_') ||
                         (id[0] == '&' && id[act - 1] == '&')))
            continue;
        if (len + (size_t)act + 1 > cap) {
            while (len + (size_t)act + 1 > cap) cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) mv_fatal("out of memory");
        }
        if (len) buf[len++] = (char)0xFE;
        memcpy(buf + len, id, (size_t)act);
        len += (size_t)act;
    }
    ic_close(&fid, &code);
    mv_set_str(dst, buf, (int64_t)len);
    free(buf);
    free(rec);
}

/* --- misc -------------------------------------------------------------- */

int mv_openaccount(void) {
    const char *v = getenv("MVX_OPENACCOUNT");
    return v && *v && strcmp(v, "0") != 0;
}

int mv_voc_class(const char *type, int64_t len) {
    /* UniData VOC type codes.  Verbs and keywords belong to the source system;
       file/Q/remote pointers are platform-specific.  Everything else — PA
       (paragraph), S (sentence), M (menu), PH (phrase), … — is the user's own
       and travels. */
    static const struct { const char *t; int c; } tbl[] = {
        {"V", 1}, {"K", 1},                              /* verb, keyword */
        {"F", 2}, {"LF", 2}, {"DF", 2}, {"DIR", 2},      /* file definitions */
        {"Q", 2}, {"X", 2}, {"R", 2},                    /* account/remote ptrs */
        {NULL, 0}
    };
    for (int i = 0; tbl[i].t; i++) {
        size_t sl = strlen(tbl[i].t);
        if ((size_t)len == sl && strncasecmp(type, tbl[i].t, sl) == 0)
            return tbl[i].c;
    }
    return 0;
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
