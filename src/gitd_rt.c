/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* gitd_rt.c — the (absent) record backend for the background process.
 *
 * See gitd_rt.h: the background process runs libgit2 on behalf of a session
 * that keeps its own records, so it has no record layer at all.  The value ops
 * are real, because the engine moves content through mv_value; the record
 * primitives abort with a diagnostic naming the caller. */

#define _POSIX_C_SOURCE 200809L

#include "gitd_rt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strncasecmp, for the VOC type table */

struct mv_ctx { int unused; };

mv_ctx *mv_ctx_create(void) {
    static struct mv_ctx one;   /* no session to open; one shared empty ctx */
    return &one;
}

void mv_ctx_destroy(mv_ctx *ctx) { (void)ctx; }

/* --- value ops (real) ---------------------------------------------------- */

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

/* --- record I/O (absent by design) --------------------------------------- */

/* One place to say why, so every diagnostic reads the same and points at the
   actual mistake: a served opcode reached a record primitive, which means the
   division of labour has been broken, not that the user did something wrong. */
static void norecords(const char *what) {
    mv_fatal("the git background process has no record access, but '%s' was "
             "called.\n"
             "This build (MVXGIT_GITD) does git-object work only — the BASIC "
             "session owns the records and passes their content over the pipe "
             "(Model B).  An opcode that needs record I/O must be served in "
             "BASIC instead.", what);
}

int64_t mv_open(mv_ctx *ctx, const mv_value *dict, const mv_value *spec,
                mv_value *fvar) {
    (void)ctx; (void)dict; (void)spec; (void)fvar; norecords("mv_open");
}
int64_t mv_read(mv_ctx *ctx, mv_value *rec, const mv_value *fvar,
                const mv_value *id, int64_t lock) {
    (void)ctx; (void)rec; (void)fvar; (void)id; (void)lock; norecords("mv_read");
}
int64_t mv_write(mv_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t keep_lock, int64_t onerr) {
    (void)ctx; (void)rec; (void)fvar; (void)id; (void)keep_lock; (void)onerr;
    norecords("mv_write");
}
int64_t mv_delete_rec(mv_ctx *ctx, const mv_value *fvar, const mv_value *id) {
    (void)ctx; (void)fvar; (void)id; norecords("mv_delete_rec");
}
void mv_select(mv_ctx *ctx, const mv_value *fvar) {
    (void)ctx; (void)fvar; norecords("mv_select");
}
int64_t mv_readnext(mv_ctx *ctx, mv_value *id) {
    (void)ctx; (void)id; norecords("mv_readnext");
}
int64_t mv_createfile(mv_ctx *ctx, const mv_value *spec, const mv_value *type) {
    (void)ctx; (void)spec; (void)type; norecords("mv_createfile");
}

int64_t mv_deletefile(mv_ctx *ctx, const mv_value *spec) {
    (void)ctx; (void)spec; norecords("mv_deletefile");
    return 0;
}
void mv_filelist(mv_ctx *ctx, mv_value *dst) {
    (void)ctx; (void)dst; norecords("mv_filelist");
}
int64_t mv_fileclass(mv_ctx *ctx, const char *name, char *out, size_t cap) {
    (void)ctx; (void)name; (void)out; (void)cap; norecords("mv_fileclass");
}
int64_t mv_indices(mv_ctx *ctx, const char *name, char *out, size_t cap) {
    (void)ctx; (void)name; (void)out; (void)cap; norecords("mv_indices");
}

/* --- misc ---------------------------------------------------------------- */

/* The open account format is the interchange form, and the pipe only ever
   carries the open form — the session materialises native shapes itself. */
int mv_openaccount(void) { return 1; }

/* Master-VOC classification is a pure table lookup on the type code with no
   record access, so it is served here rather than aborted — and it must stay
   byte-for-byte the same judgement as the other backends, since it decides what
   travels in a commit.  Kept identical to udtgit_rt.c's table deliberately:
   0 = keep (the user's own procs), 1 = always drop (a verb/keyword the
   destination supplies), 2 = drop in the open interchange only (a platform
   file/Q/remote pointer, which travels as <file>.DICT/%FILE%). */
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
    fputs("mvgitd: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}
