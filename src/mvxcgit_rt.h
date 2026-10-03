/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* mvxcgit_rt — the record layer over the MVX CLIENT LIBRARY (#267 stage 2).
 *
 * The engine depends on a fixed, narrow set of record primitives and never
 * touches mv_value's internals.  For the default MVX build those names come
 * from libmvxrt (mvx_runtime.h) -- mvx-git reaching into the runtime it happens
 * to be linked against.  For -DMVXGIT_MVXC they come from here, over libmvxc:
 * the same contract mv-connect and every language binding uses, so mvx-git stops
 * being a special case with privileged access to MVX's internals.
 *
 * WHY THIS IS AN ARM AND NOT A REMAP.  libmvxc's value is an opaque heap handle
 * (mvxc_val *, from mvxc_new) and the engine's is a stack local, 85 of them, with
 * mv_init/mv_clear around each.  Those cannot be the same type.  So this arm
 * defines its own mv_value -- a plain owned byte buffer, exactly as the UniData
 * and jBASE arms do -- and converts at the boundary.  That is also why the engine
 * body does not change: the impedance lives here, in one file.
 *
 * A "file variable" -- the value mv_open fills and that read/write/select/delete
 * take back -- carries the mvxc_file handle in `f`.  The engine passes it through
 * opaquely, so overloading the value struct is safe, which is the same trick
 * udtgit_rt.h plays with its InterCall handle.
 */
#ifndef MVXCGIT_RT_H
#define MVXCGIT_RT_H

#include <stddef.h>
#include <stdint.h>

#include <mvxc.h>

typedef struct mv_value {
    char       *data;     /* owned; NULL when unassigned */
    int64_t     len;
    mvxc_file  *f;        /* the open file, for values returned by mv_open */
    int         is_file;
} mv_value;

typedef struct mv_ctx mv_ctx;   /* opaque: the session and the files it opened */

/* --- context ----------------------------------------------------------- */
/* Connects with NULL, not with an account name -- deliberately.  Passing a name
   makes mvxc_connect LOGTO it, which runs the account's LOGIN, and mvx-git has
   already resolved and entered its account by the time the engine runs ($MVXACCOUNT
   is set).  A second LOGIN on every `mvx-git status` would be a behaviour change
   nobody asked for. */
mv_ctx *mv_ctx_create(void);
void    mv_ctx_destroy(mv_ctx *ctx);

/* --- value ops --------------------------------------------------------- */
/* RENAMED SYMBOLS, AND THIS ARM IS THE ONLY ONE THAT HAS TO.
 *
 * libmvxrt EXPORTS mv_init, mv_clear, mv_set_str and mv_val_chars -- they are
 * its own value ops, the ones this arm is replacing.  The udt and jbase arms can
 * define those names freely because their binaries never link the runtime at
 * all; this one links it transitively through libmvxc, and then the definitions
 * in the EXECUTABLE interpose on the runtime's own internal calls.  The runtime
 * builds its select list with mv_init and mv_copy, so it called THIS file's
 * mv_init on ITS mv_value -- two different structs, same name -- and SELECT came
 * back with thirty unassigned ids.  A whole account walked to nothing.
 *
 * So the symbols are ours and the seam names are macros onto them.  The engine
 * still says mv_init; nothing is exported that the runtime also exports. */
void    mvxcgit_init(mv_value *v);
void    mvxcgit_clear(mv_value *v);
void    mvxcgit_set_str(mv_value *v, const char *p, int64_t len);   /* copies */
int64_t mvxcgit_val_chars(const mv_value *v, char *numbuf, size_t cap,
                          const char **pp);
#define mv_init       mvxcgit_init
#define mv_clear      mvxcgit_clear
#define mv_set_str    mvxcgit_set_str
#define mv_val_chars  mvxcgit_val_chars

/* --- record I/O -------------------------------------------------------- */
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
int64_t mv_deletefile(mv_ctx *ctx, const mv_value *spec);
void    mv_filelist(mv_ctx *ctx, mv_value *dst);

/* --- misc -------------------------------------------------------------- */
/* Bind `file` to backend `want`, ASKING when this host does not have it -- which
   is why it is here and not in the library.  mvxc_bind_file refuses to prompt
   (mvx#302): the caller owns the screen.  mvx-git IS the caller and it is a
   command line, so the question belongs here, in the arm, and the answer goes
   back through the library. */
int mv_bind_driver(const char *file, const char *want);

/* The abort of last resort, when there is no guard to unwind to and no memory to
   describe why.  The runtime's mvx_fatal is what the default arm uses; a client
   has no business reaching for it, and nothing here is ever called from BASIC. */
void mv_hard_fatal(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

/* mv_openaccount, mv_voc_class and mv_fatal are MVX BEHAVIOUR, not transport,
   and are shared with the default arm -- mvxgit.c defines all three under
   MVXGIT_MVXRT, which this build also sets.  Declared there. */

#endif /* MVXCGIT_RT_H */
