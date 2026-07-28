/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* udtgit_rt.h — the UniData record backend for the record-git engine.
 *
 * The engine (mvxgit.c) is one codebase compiled per platform.  It depends on a
 * fixed, narrow set of record primitives and never touches mv_value's
 * internals.  For the MVX build those names come from libmvxrt (mvx_runtime.h);
 * for the UniData build (udt-git, -DMVXGIT_UDT) they come from here, implemented
 * over Rocket UniData's InterCall C API (intcall.h) in udtgit_rt.c.  This is a
 * compile-time choice — no runtime indirection: the exact same engine object
 * code shape, only the record layer differs.
 *
 * The value type is a plain owned byte buffer.  A "file variable" (the value a
 * successful mvx_open fills and that read/write/select/delete take back) also
 * carries the InterCall file handle in `fid`; the engine only ever passes it
 * through opaquely, so overloading the value struct is safe. */
#ifndef UDTGIT_RT_H
#define UDTGIT_RT_H

#include <stdint.h>
#include <stddef.h>

/* --- value type -------------------------------------------------------- */
/* Records and record ids are byte strings (MultiValue marks @AM=0xFE … are
   ordinary bytes, exactly as on MVX, so the engine's mark<->newline xlate is
   unchanged).  `data` is NUL-terminated for convenience but `len` is
   authoritative (records may contain NULs). */
typedef struct mv_value {
    char   *data;   /* owned; NULL when unassigned */
    int64_t len;
    long    fid;    /* InterCall file handle, for values returned by mvx_open */
    int     is_file;
} mv_value;

typedef struct mvx_ctx mvx_ctx;   /* opaque: InterCall session + open files */

/* --- context ----------------------------------------------------------- */
mvx_ctx *mvx_ctx_create(void);          /* opens the InterCall session */
void     mvx_ctx_destroy(mvx_ctx *ctx); /* closes files, quits the session */

/* --- value ops --------------------------------------------------------- */
void    mv_init(mv_value *v);
void    mv_clear(mv_value *v);
void    mv_set_str(mv_value *v, const char *p, int64_t len);   /* copies */
/* Return the value's bytes: sets *pp to the buffer (numbuf/cap unused for the
   string values the engine handles) and returns the length. */
int64_t mv_val_chars(const mv_value *v, char *numbuf, size_t cap,
                     const char **pp);

/* --- record I/O (InterCall) -------------------------------------------- */
/* Open file `spec` (its dictionary when `dict` is non-NULL) into `fvar`;
   returns non-zero on success. */
int64_t mvx_open(mvx_ctx *ctx, const mv_value *dict, const mv_value *spec,
                 mv_value *fvar);
/* Read record `id` from `fvar` into `rec`; returns non-zero when it exists. */
int64_t mvx_read(mvx_ctx *ctx, mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t lock);
int64_t mvx_write(mvx_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                  const mv_value *id, int64_t keep_lock, int64_t onerr);
int64_t mvx_delete_rec(mvx_ctx *ctx, const mv_value *fvar, const mv_value *id);
/* Begin a select over `fvar`; ids are then drained with mvx_readnext. */
void    mvx_select(mvx_ctx *ctx, const mv_value *fvar);
int64_t mvx_readnext(mvx_ctx *ctx, mv_value *id);   /* 0 when the list is done */
/* Create file `spec`; `type` is NULL for the account default (a dynamic hash
   file) or "DIR" for a directory file. */
int64_t mvx_createfile(mvx_ctx *ctx, const mv_value *spec, const mv_value *type);
/* Fill `dst` with the account's files as "name<VM>type" rows, @AM-separated. */
void    mvx_filelist(mvx_ctx *ctx, mv_value *dst);

/* --- misc -------------------------------------------------------------- */
int  mvx_openaccount(void);   /* open account format on? ($MVX_OPENACCOUNT) */
void mvx_fatal(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

#endif /* UDTGIT_RT_H */
