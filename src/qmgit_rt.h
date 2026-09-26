/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* qmgit_rt.h — the OpenQM / ScarletDME record backend for the record-git
 * engine (mv_git#243).
 *
 * The engine (mvxgit.c) is one codebase compiled per platform, depending on a
 * fixed, narrow set of record primitives.  For the QM build (qm-git,
 * -DMVXGIT_QM) those names come from here, implemented over QMClient
 * (qmclilib) in qmgit_rt.c.
 *
 * QM IS THE MVX/jBASE CASE, NOT THE UniData ONE.  A standalone process makes
 * its own connection with QMConnectLocal() and then calls the record API
 * itself, so there is no agent session to keep alive, no licence to nurse and
 * no framed protocol — just calls.  That is why this file looks like
 * jbasegit_rt.h and not like mvsession.h.
 *
 * WHAT QMClient CANNOT DO, AND WHY IT MATTERS HERE.  Its record calls take C
 * strings with no length: `void QMWrite(int fno, char *id, char *data)`, and
 * qmclilib.c takes `strlen(data)` internally even though the wire packet it
 * builds is length-framed.  A record containing a NUL is therefore truncated
 * at the first one, silently.  Measured, not assumed: writing 16 bytes
 * (41 42 00 43 01 FF 7F 80 44 00 45 0A 0D FD FC 5A) reads back as 2.
 *
 * The engine's records are arbitrary bytes and may contain NULs (mvxgit.h).
 * So mv_write REFUSES a record with an embedded NUL rather than storing a
 * truncated one — a loud failure instead of a corrupt repository that only
 * shows up as a bad diff weeks later.  The fix is a length-carrying entry
 * point in qmclilib (the packet already has the length); until that exists
 * upstream this is the honest boundary.
 */
#ifndef QMGIT_RT_H
#define QMGIT_RT_H

#include <stdint.h>
#include <stddef.h>

/* --- value type -------------------------------------------------------- */
/* Records and record ids are byte strings; MultiValue marks (@AM=0xFE …) are
   ordinary bytes, exactly as on MVX and jBASE, so the engine's mark<->newline
   translation is unchanged.  `data` is NUL-terminated for convenience but
   `len` is authoritative. */
typedef struct mv_value {
    char   *data;      /* owned; NULL when unassigned */
    int64_t len;
    int     fno;       /* QMClient file number, for values returned by mv_open */
    int     is_file;
} mv_value;

typedef struct mv_ctx mv_ctx;   /* opaque: the QM connection + its open files */

/* --- context ----------------------------------------------------------- */
mv_ctx *mv_ctx_create(void);          /* connection opens lazily */
void    mv_ctx_destroy(mv_ctx *ctx);  /* closes files, disconnects */

/* --- value ops --------------------------------------------------------- */
void    mv_init(mv_value *v);
void    mv_clear(mv_value *v);
void    mv_set_str(mv_value *v, const char *p, int64_t len);   /* copies */
int64_t mv_val_chars(const mv_value *v, char *numbuf, size_t cap,
                     const char **pp);

/* --- record I/O (QMClient) --------------------------------------------- */
int64_t mv_open(mv_ctx *ctx, const mv_value *dict, const mv_value *spec,
                mv_value *fvar);
int64_t mv_read(mv_ctx *ctx, mv_value *rec, const mv_value *fvar,
                const mv_value *id, int64_t lock);
int64_t mv_write(mv_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t keep_lock, int64_t onerr);
int64_t mv_delete_rec(mv_ctx *ctx, const mv_value *fvar, const mv_value *id);
void    mv_select(mv_ctx *ctx, const mv_value *fvar);
int64_t mv_readnext(mv_ctx *ctx, mv_value *id);   /* 0 when the list is done */
int64_t mv_createfile(mv_ctx *ctx, const mv_value *spec, const mv_value *type);
int64_t mv_deletefile(mv_ctx *ctx, const mv_value *spec);
void    mv_filelist(mv_ctx *ctx, mv_value *dst);
int64_t mv_fileclass(mv_ctx *ctx, const char *name, char *out, size_t cap);
int64_t mv_indices(mv_ctx *ctx, const char *name, char *out, size_t cap);

/* --- account / misc ----------------------------------------------------- */
int  mv_openaccount(void);
int  mv_voc_class(const char *type, int64_t len);
void mv_fatal(const char *fmt, ...)
#ifdef __GNUC__
     __attribute__((format(printf, 1, 2), noreturn))
#endif
     ;

/* The QM account this run works in.  qm-git resolves it (from -a, $QMACCOUNT,
   or the current directory's name) before any record op, because QMConnectLocal
   needs an account name and there is no "current account" to inherit the way
   there is inside a session. */
/* Is `name` one of the account's own files?  Answered from the VOC, which is
   the only authority on QM: the filesystem cannot tell a directory file from
   an ordinary directory, and some (BP.OUT) have no dictionary to look for.
   Accepts the engine's `<name>.DICT` spelling for a file's dictionary. */
int  mv_qm_is_file(const char *name);

void        mv_qm_set_account(const char *name);
const char *mv_qm_account(void);

#endif /* QMGIT_RT_H */
