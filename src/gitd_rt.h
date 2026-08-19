/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* gitd_rt.h — the record backend for the background-process build of the engine
 * (-DMVXGIT_GITD).
 *
 * There isn't one, and that is the design rather than a gap.  The background
 * process exists to run libgit2 for a BASIC session that cannot (mv_git#40);
 * the session keeps the records.  That is Model B, exactly as on UniData
 * CallC — BASIC drives the record loop on the live session and hands content
 * over, C does the git-object work, and C never calls back.  Since the process
 * is not in the session, it has no licence, no file handles, and no business
 * touching records.
 *
 * So these primitives are present only to satisfy the engine's link, and every
 * one of them aborts with a diagnostic naming the operation.  The engine's
 * Model B ops (init/stage/stageblob/commit/status/log/branch/files/cat/diff/…)
 * are pure git-object work and never reach them; the ops that DO use records
 * (materialize, addall, adddisk, add) are the ones BASIC replaces.  Aborting
 * rather than silently returning empty is deliberate: if a future engine change
 * routes a served opcode through a record primitive, it must fail loudly at the
 * first call, not quietly commit an empty tree.
 *
 * The value type mirrors the UniData backend's: a plain owned byte buffer with
 * `len` authoritative, since records may contain NULs.
 */
#ifndef GITD_RT_H
#define GITD_RT_H

#include <stdint.h>
#include <stddef.h>

typedef struct mv_value {
    char   *data;   /* owned; NULL when unassigned */
    int64_t len;
    long    fid;    /* unused here; kept so the struct matches the other backends */
    int     is_file;
} mv_value;

typedef struct mv_ctx mv_ctx;   /* opaque, and in this build empty */

mv_ctx *mv_ctx_create(void);
void    mv_ctx_destroy(mv_ctx *ctx);

/* Value ops are real — the engine uses them to move content in and out. */
void    mv_init(mv_value *v);
void    mv_clear(mv_value *v);
void    mv_set_str(mv_value *v, const char *p, int64_t len);   /* copies */
int64_t mv_val_chars(const mv_value *v, char *numbuf, size_t cap,
                     const char **pp);

/* Record I/O — all of these abort; see the file comment. */
int64_t mv_open(mv_ctx *ctx, const mv_value *dict, const mv_value *spec,
                mv_value *fvar);
int64_t mv_read(mv_ctx *ctx, mv_value *rec, const mv_value *fvar,
                const mv_value *id, int64_t lock);
int64_t mv_write(mv_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t keep_lock, int64_t onerr);
int64_t mv_delete_rec(mv_ctx *ctx, const mv_value *fvar, const mv_value *id);
void    mv_select(mv_ctx *ctx, const mv_value *fvar);
int64_t mv_readnext(mv_ctx *ctx, mv_value *id);
int64_t mv_createfile(mv_ctx *ctx, const mv_value *spec, const mv_value *type);
/* Delete the file named by `spec`, with its dictionary.  Non-zero if it is gone
   afterwards (including "it was not there").  Used when a checkout removes a
   file's %FILE% control — the control is the file's existence in git. */
int64_t mv_deletefile(mv_ctx *ctx, const mv_value *spec);
void    mv_filelist(mv_ctx *ctx, mv_value *dst);
int64_t mv_fileclass(mv_ctx *ctx, const char *name, char *out, size_t cap);
int64_t mv_indices(mv_ctx *ctx, const char *name, char *out, size_t cap);

int  mv_openaccount(void);
int  mv_voc_class(const char *type, int64_t len);
void mv_fatal(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

#endif /* GITD_RT_H */
