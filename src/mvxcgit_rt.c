/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* mvxcgit_rt — see mvxcgit_rt.h.  The record layer over libmvxc. */

#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "mvxcgit_rt.h"
#include "mvxgit.h"

struct mv_ctx {
    mvxc_session *s;
    mvxc_val     *scratch;   /* one value, reused: see mv_read */
};

/* mv_bind_driver takes no context, because the runtime's mvx_bind_driver does
   not either: a binding is ACCOUNT state, not session state, and the engine has
   exactly one context open at a time.  Held here so the seam can keep the
   signature the engine already calls. */
static mv_ctx *g_ctx;

/* --- context ----------------------------------------------------------- */

mv_ctx *mv_ctx_create(void) {
    mv_ctx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;
    mvxc_status st;
    /* NULL, not the account name.  A name makes the library LOGTO it and run
       the account's LOGIN; mvx-git has already entered its account by now
       ($MVXACCOUNT), and a LOGIN on every `mvx-git status` is not this change's
       business. */
    ctx->s = mvxc_connect(NULL, &st);
    if (!ctx->s) { free(ctx); return NULL; }
    ctx->scratch = mvxc_new();
    if (!ctx->scratch) { mvxc_disconnect(ctx->s); free(ctx); return NULL; }
    g_ctx = ctx;
    return ctx;
}

void mv_ctx_destroy(mv_ctx *ctx) {
    if (!ctx) return;
    if (g_ctx == ctx) g_ctx = NULL;
    mvxc_free(ctx->scratch);
    mvxc_disconnect(ctx->s);
    free(ctx);
}

/* --- value ops --------------------------------------------------------- */

void mv_init(mv_value *v) {
    v->data = NULL;
    v->len = 0;
    v->f = NULL;
    v->is_file = 0;
}

void mv_clear(mv_value *v) {
    if (v->is_file && v->f) mvxc_close(v->f);
    free(v->data);
    v->data = NULL;
    v->len = 0;
    v->f = NULL;
    v->is_file = 0;
}

void mv_set_str(mv_value *v, const char *p, int64_t len) {
    /* NUL-TERMINATED as well as counted.  The engine hands these straight to
       calls that take a C string -- a file name, a record id -- while also
       relying on the length for record bodies that contain marks and may
       contain a NUL. */
    char *nb = malloc((size_t)len + 1);
    if (!nb) mv_fatal("mvx-git: out of memory");
    if (len) memcpy(nb, p, (size_t)len);
    nb[len] = '\0';
    free(v->data);
    v->data = nb;
    v->len = len;
    v->is_file = 0;          /* setting bytes makes it a string, not a handle */
    v->f = NULL;
}

int64_t mv_val_chars(const mv_value *v, char *numbuf, size_t cap,
                     const char **pp) {
    (void)numbuf; (void)cap;      /* the engine only handles string values */
    *pp = v->data ? v->data : "";
    return v->len;
}

static const char *cstr(const mv_value *v) {
    return (v && v->data) ? v->data : "";
}

/* --- record I/O -------------------------------------------------------- */

int64_t mv_open(mv_ctx *ctx, const mv_value *dict, const mv_value *spec,
                mv_value *fvar) {
    if (!ctx) return 0;
    mvxc_status st;
    /* `dict` is a flag, not a name: the engine passes non-NULL to mean "the
       dictionary of this file", which is what the other arms read it as too. */
    mvxc_file *f = mvxc_open(ctx->s, cstr(spec), dict ? "DICT" : NULL, &st);
    if (!f) return 0;
    mv_clear(fvar);
    fvar->f = f;
    fvar->is_file = 1;
    return 1;
}

int64_t mv_read(mv_ctx *ctx, mv_value *rec, const mv_value *fvar,
                const mv_value *id, int64_t lock) {
    if (!ctx || !fvar->f) return 0;
    mvxc_status st;

    if (lock) {
        mvxc_val *v = mvxc_readu(fvar->f, cstr(id), 1, &st);
        if (!v) return 0;
        size_t n = 0;
        const char *p = mvxc_bytes(v, &n);
        mv_set_str(rec, p ? p : "", (int64_t)n);
        mvxc_free(v);
        return 1;
    }

    /* ONE VALUE, REUSED.  This is the hot path -- the engine reads every record
       in an account -- and mvxc_read_into exists for exactly it: a malloc per
       record against none (mvx#294). */
    st = mvxc_read_into(fvar->f, cstr(id), ctx->scratch);
    if (st != MVXC_OK) return 0;
    size_t n = 0;
    const char *p = mvxc_bytes(ctx->scratch, &n);
    mv_set_str(rec, p ? p : "", (int64_t)n);
    return 1;
}

int64_t mv_write(mv_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t keep_lock, int64_t onerr) {
    if (!ctx || !fvar->f) return 0;
    (void)keep_lock;
    /* onerr asked the RUNTIME to return instead of aborting; the library does
       that unconditionally, which is its whole promise. */
    (void)onerr;
    mvxc_val *v = mvxc_from_bytes(cstr(rec), (size_t)rec->len);
    if (!v) mv_fatal("mvx-git: out of memory");
    mvxc_status st = mvxc_write(fvar->f, cstr(id), v);
    mvxc_free(v);
    return st == MVXC_OK ? 1 : 0;
}

int64_t mv_delete_rec(mv_ctx *ctx, const mv_value *fvar, const mv_value *id) {
    if (!ctx || !fvar->f) return 0;
    mvxc_status st = mvxc_delete(fvar->f, cstr(id));
    return (st == MVXC_OK || st == MVXC_NOTFOUND) ? 1 : 0;
}

void mv_select(mv_ctx *ctx, const mv_value *fvar) {
    if (!ctx || !fvar->f) return;
    mvxc_select(fvar->f);
}

int64_t mv_readnext(mv_ctx *ctx, mv_value *id) {
    if (!ctx) return 0;
    const char *next = mvxc_next(ctx->s);
    if (!next) return 0;
    /* Copied at once: the id belongs to the SESSION and the next call replaces
       it, which the library says plainly. */
    mv_set_str(id, next, (int64_t)strlen(next));
    return 1;
}

int64_t mv_createfile(mv_ctx *ctx, const mv_value *spec, const mv_value *type) {
    if (!ctx) return 0;
    const char *tp = type ? cstr(type) : NULL;
    if (tp && !*tp) tp = NULL;                 /* "" means the account default */
    return mvxc_create_file(ctx->s, cstr(spec), tp) == MVXC_OK ? 1 : 0;
}

int64_t mv_deletefile(mv_ctx *ctx, const mv_value *spec) {
    if (!ctx) return 0;
    const char *name = cstr(spec);
    if (!*name) return 0;
    mvxc_status st = mvxc_delete_file(ctx->s, name);
    /* Gone afterwards is the question, so "it was not there" is a yes. */
    return (st == MVXC_OK || st == MVXC_NOTFOUND) ? 1 : 0;
}

void mv_filelist(mv_ctx *ctx, mv_value *dst) {
    mv_set_str(dst, "", 0);
    if (!ctx) return;
    mvxc_val *fl = mvxc_files(ctx->s);
    if (!fl) return;
    size_t n = 0;
    const char *p = mvxc_bytes(fl, &n);
    /* Already the shape the engine wants: one attribute per file, name and type
       as its first two values. */
    mv_set_str(dst, p ? p : "", (int64_t)n);
    mvxc_free(fl);
}

/* --- misc -------------------------------------------------------------- */

/* THE QUESTION LIVES HERE, NOT IN THE LIBRARY (mvx#302).  mvxc_bind_file will
   not prompt -- the caller owns the screen -- so it answers MVXC_NOTFOUND and
   hands back what this host would offer instead.  mvx-git IS the caller, and it
   is a command line, so this is where the user gets asked.  The shape of the
   question is the runtime's, because a clone should not feel different for being
   driven through the client library. */
static char g_sub_all[64];       /* answered "always": stop asking */
static int  g_sub_none;          /* answered "quit": stop trying */

static int bind_to(const char *file, const char *drv) {
    char ignored[64];
    return mvxc_bind_file(g_ctx->s, file, drv, ignored, sizeof ignored)
           == MVXC_OK;
}

int mv_bind_driver(const char *file, const char *want) {
    if (!file || !*file || !want || !*want) return 0;
    if (!g_ctx) return 0;
    if (g_sub_none) return 0;

    char instead[64] = "";
    switch (mvxc_bind_file(g_ctx->s, file, want, instead, sizeof instead)) {
    case MVXC_OK:       return 1;
    case MVXC_NOTFOUND: break;                  /* ask below */
    default:
        /* $MVXDRIVER names a backend this host does not have.  Supplied up
           front, so it is an answer already given and wrong, not a question. */
        fprintf(stderr, "%s: %s\n", file, mvxc_error(g_ctx->s));
        return 0;
    }

    if (g_sub_all[0]) return bind_to(file, g_sub_all);

    fprintf(stderr, "\n%s: backend \"%s\" is not available on this host\n",
            file, want);
    if (!instead[0]) {
        fprintf(stderr, "  and there is nothing here to use instead\n");
        return 0;
    }

    /* NO TERMINAL MEANS NO GUESSING.  Not a prompt into a closed pipe, and not
       a silent default that puts the file somewhere nobody chose. */
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "  not a terminal - set MVXDRIVER=<name> to choose one, "
                        "or run this where it can ask\n");
        return 0;
    }

    for (;;) {
        fprintf(stderr, "  create it on \"%s\" instead?"
                        "  [y]es  [n]o, skip  [a]lways  [q]uit"
                        "  (or type a driver name): ", instead);
        fflush(stderr);
        char line[128];
        if (!fgets(line, sizeof line, stdin)) { fprintf(stderr, "\n"); return 0; }
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (!*p || strcasecmp(p, "y") == 0 || strcasecmp(p, "yes") == 0)
            return bind_to(file, instead);
        if (strcasecmp(p, "n") == 0 || strcasecmp(p, "no") == 0) return 0;
        if (strcasecmp(p, "q") == 0 || strcasecmp(p, "quit") == 0) {
            g_sub_none = 1;
            return 0;
        }
        if (strcasecmp(p, "a") == 0 || strcasecmp(p, "all") == 0 ||
            strcasecmp(p, "always") == 0) {
            snprintf(g_sub_all, sizeof g_sub_all, "%s", instead);
            return bind_to(file, instead);
        }
        /* A name, so nobody is stuck with the one on offer. */
        if (bind_to(file, p)) return 1;
        fprintf(stderr, "  \"%s\" is not a driver here\n", p);
    }
}

void mv_hard_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    /* The default arm calls the runtime's mvx_fatal here, whose behaviour is
       what a program CALLed from BASIC expects.  Nothing reaches this arm from
       BASIC -- it is the command line -- and there is no guard left to unwind
       to, so there is nothing to do but stop. */
    abort();
}
