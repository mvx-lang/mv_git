/* jbasegit_rt.h — the record layer the shared engine sits on, for jBASE.
 * Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
 *
 * The same mv_* contract udtgit_rt.h declares, implemented over jBASE's Jedi*
 * record API rather than UniData's InterCall.  See mv_git#114 for how the
 * platform was surveyed; the short version is that jBASE is the MVX case, not
 * the UniData one:
 *
 *   - a DEFC entry point is handed the session (DPSTRUCT *dp), and
 *   - a standalone process makes its own with JBASESessionObjectFactory()
 *
 * and either way C can then call JediOpen / JediReadRecord / JediWriteRecord /
 * JediDeleteRecord / JediSelect / JediReadnext / JediClose itself.  So there is
 * no BASIC-driven loop (UniData's Model B, which exists because C there gets no
 * file API) and no background process with a wire protocol (UniVerse, which has
 * no in-process C at all).  The engine owns the operation.
 */
#ifndef JBASEGIT_RT_H
#define JBASEGIT_RT_H

#include <stdint.h>
#include <stddef.h>

/* --- value type -------------------------------------------------------- */
/* Byte strings, exactly as on MVX and UniData: MultiValue marks (@AM = 0xFE
   and friends) are ordinary bytes, so the engine's mark<->newline translation
   is unchanged.  `data` is NUL-terminated for convenience; `len` is
   authoritative, because a record may contain NULs.

   A value returned by mv_open carries an open file instead: `fd` is the
   JediFileDescriptor* and `sel` any in-flight select list over it.  Kept void*
   so the engine — which never looks inside an mv_value — does not drag jBASE's
   headers in behind it. */
typedef struct mv_value {
    char   *data;      /* owned; NULL when unassigned */
    int64_t len;
    void   *fd;        /* JediFileDescriptor*, for values from mv_open */
    void   *sel;       /* struct JediSelectPtr*, while a select is running */
    int     is_file;
} mv_value;

typedef struct mv_ctx mv_ctx;   /* opaque: the jBASE session + its open files */

/* --- context ----------------------------------------------------------- */
mv_ctx *mv_ctx_create(void);
void    mv_ctx_destroy(mv_ctx *ctx);

/* --- value ops --------------------------------------------------------- */
void    mv_init(mv_value *v);
void    mv_clear(mv_value *v);
void    mv_set_str(mv_value *v, const char *p, int64_t len);   /* copies */
int64_t mv_val_chars(const mv_value *v, char *numbuf, size_t cap,
                     const char **pp);

/* --- record I/O (Jedi*) ------------------------------------------------ */
int64_t mv_open(mv_ctx *ctx, const mv_value *dict, const mv_value *spec,
                mv_value *fvar);
int64_t mv_read(mv_ctx *ctx, mv_value *rec, const mv_value *fvar,
                const mv_value *id, int64_t lock);
int64_t mv_write(mv_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t keep_lock, int64_t onerr);
int64_t mv_delete_rec(mv_ctx *ctx, const mv_value *fvar, const mv_value *id);
void    mv_select(mv_ctx *ctx, const mv_value *fvar);
int64_t mv_readnext(mv_ctx *ctx, mv_value *id);   /* 0 when the list is done */

/* --- files ------------------------------------------------------------- */
int64_t mv_createfile(mv_ctx *ctx, const mv_value *spec, const mv_value *type);
int64_t mv_deletefile(mv_ctx *ctx, const mv_value *spec);
void    mv_filelist(mv_ctx *ctx, mv_value *dst);
int64_t mv_fileclass(mv_ctx *ctx, const char *name, char *out, size_t cap);
int64_t mv_indices(mv_ctx *ctx, const char *name, char *out, size_t cap);

/* --- misc -------------------------------------------------------------- */
int  mv_openaccount(void);
int  mv_voc_class(const char *type, int64_t len);
void mv_fatal(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

/* Hand the record layer a session that already exists — the DEFC path, where
   jBASE gives the C function its own dp and making a second one would be both
   wasteful and wrong.  Call before the first record op; the standalone path
   leaves it alone and mv_ctx_create makes one. */
void mv_jbase_use_session(void *dp);

#endif /* JBASEGIT_RT_H */
