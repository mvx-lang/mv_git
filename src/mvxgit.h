/*
 * MVX — a native compiler and runtime for Pick/MultiValue BASIC.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Plain-C record-git API (#58).
 *
 * The record-git engine reads and writes hash-file records directly to/from
 * git objects in the account's own git repository; its working tree is the
 * live records, so there is no export copy.  The BASIC GIT verb reaches it
 * through the subroutine ABI (mvx_sub_GIT*); this header exposes the same
 * operations as ordinary C functions so the mvx-git executable can drive the
 * identical engine instead of shelling out to git and mvx-git-adopt.
 *
 * Each call returns a malloc'd output string the caller frees.  Lines are
 * separated by the attribute mark 0xFE (@AM) — the mvx-git executable renders
 * those as newlines.  `ctx` is a runtime context bound to the account
 * (MVXACCOUNT); `repo` is the repository path, normally ".git".
 */
#ifndef MVXGIT_H
#define MVXGIT_H

/* The record-git engine is one codebase compiled per platform.  It depends on a
   fixed, narrow set of record primitives (the value type mv_value, the context
   mv_ctx, and open/read/write/select/readnext/delete/createfile/filelist plus
   mv_init/clear/set_str/val_chars, mv_openaccount and mv_fatal) and never
   touches mv_value's internals.  The concrete implementation of those names is
   chosen at compile time — no runtime indirection:

     - default (mvx-git): the MVX runtime, libmvxrt (mvx_runtime.h).
     - MVXGIT_UDT (udt-git): the same names over Rocket UniData's InterCall API
       (udtgit_rt.h / udtgit_rt.c).

   The engine body is identical either way; only genuine behavioural
   differences (UniData has no on-disk descriptor, generates %INDEXES%
   virtually) are guarded inline with #ifdef MVXGIT_UDT. */
#if defined(MVXGIT_UDT)
#  include "udtgit_rt.h"
#else
#  include "mvx_runtime.h"
/* The shared engine speaks a platform-neutral record API (mv_*); on MVX those
   names map to the runtime's mvx_* symbols at compile time — direct calls, no
   indirection.  (The value ops mv_init/mv_clear/mv_set_str/mv_val_chars and the
   type mv_value are already the runtime's own mv_* names.) */
#  define mv_ctx          mvx_ctx
#  define mv_ctx_create   mvx_ctx_create
#  define mv_ctx_destroy  mvx_ctx_destroy
#  define mv_open         mvx_open
#  define mv_read         mvx_read
#  define mv_readnext     mvx_readnext
#  define mv_write        mvx_write
#  define mv_delete_rec   mvx_delete_rec
#  define mv_select       mvx_select
#  define mv_createfile   mvx_createfile
#  define mv_filelist     mvx_filelist
#  define mv_openaccount  mvx_openaccount
#  define mv_fatal        mvx_fatal
#  define mv_voc_class    mvx_voc_class
#endif

char *mv_git_init(mv_ctx *ctx, const char *repo);
/* textconv filter (tidy diffs): beautify a record blob at `path` to stdout for
   diff display only — the stored blob is untouched.  0 on success. */
int mv_git_textconv(const char *path);
char *mv_git_add(mv_ctx *ctx, const char *repo, const char *file,
                  const char *id);
/* Stage a git submodule directory `name` as a gitlink (not as records). */
char *mv_git_addsub(mv_ctx *ctx, const char *repo, const char *name);
/* Stage a raw blob at git `path` with `content` — for synthesising open-account
   controls (.mv-account, <file>.DICT/%FILE%) that have no backing record. */
char *mv_git_stageblob(mv_ctx *ctx, const char *repo, const char *path,
                       const char *content);

/* Render the canonical portable `.mv-account` descriptor for an account with the
   given identity and default hash backend into `out`; returns its length.  One
   schema shared by mvx-git (converting from the native `.mvx`) and udt-git
   (synthesising from the live UniData account), so both emit identical bytes. */
int mv_git_desc_open(const char *name, const char *version,
                     const char *description, const char *hash,
                     char *out, size_t cap);

/* The open-form %FILE% control committed for `base` (HEAD's <base>.DICT/%FILE%)
   in `out`; returns its length, or -1 if `base` has no committed control yet.
   Generators use it to keep a shipped hash modulo sticky: a re-add preserves the
   committed default instead of overwriting it with the live file's current size,
   so one customer's resize never becomes another's. */
int mv_git_committed_control(const char *repo, const char *base,
                             char *out, size_t cap);

/* Batched staging (bulk in-session use): open once, accumulate blobs across many
   mv_git_batch_add calls, write the index once at mv_git_batch_end.  O(n) — for
   staging a whole account without re-opening the repo per record. */
int  mv_git_batch_begin(const char *repo);
void mv_git_batch_add(const char *path, const char *content, int64_t len,
                      int translate);
void mv_git_batch_end(void);

/* Checkout side (git -> account): list every blob path in HEAD, and fetch one
   blob's content with attribute marks restored, so a driver can create files
   and WRITE records on its own platform (the UniData in-session GIT verb). */
char *mv_git_headfiles(mv_ctx *ctx, const char *repo);
char *mv_git_catpath(mv_ctx *ctx, const char *repo, const char *path);
/* Stage the on-disk working tree exactly as `git add -A` would (modes,
   .gitignore, top-level files, deletions).  Step one of `mvx-git add -A`. */
char *mv_git_adddisk(mv_ctx *ctx, const char *repo);
/* Normalise the staged index to the open account format (%FILE% -> DIR/hash,
   .mvx -> .mv-account) — the git objects carry the open form; disk stays
   native.  Run after `add` when `mvx.openaccount` is set. */
char *mv_git_openform(mv_ctx *ctx, const char *repo);
/* Materialise a native account directly from the repo's HEAD tree (the clone
   path): MV files -> backend, .mv-account/.mvx -> .mvx, plain files -> disk.
   The open form never touches disk; no external adopt tool is run. */
char *mv_git_materialize(mv_ctx *ctx, const char *repo);
char *mv_git_rm(mv_ctx *ctx, const char *repo, const char *file,
                 const char *id);
char *mv_git_commit(mv_ctx *ctx, const char *repo, const char *msg);
char *mv_git_status(mv_ctx *ctx, const char *repo);
char *mv_git_log(mv_ctx *ctx, const char *repo, const char *count);
char *mv_git_diff(mv_ctx *ctx, const char *repo, const char *file);
char *mv_git_show(mv_ctx *ctx, const char *repo, const char *file,
                   const char *id);
char *mv_git_branch(mv_ctx *ctx, const char *repo, const char *name);
char *mv_git_checkout(mv_ctx *ctx, const char *repo, const char *name);
char *mv_git_merge(mv_ctx *ctx, const char *repo, const char *branch);
char *mv_git_cherrypick(mv_ctx *ctx, const char *repo, const char *commit);
char *mv_git_restore(mv_ctx *ctx, const char *repo, const char *file);
/* remotes & clone (libgit2, no OS git) — mvx#94 / mvpkg#23 */
char *mv_git_clone(mv_ctx *ctx, const char *url, const char *dir, const char *ref);
char *mv_git_fetch(mv_ctx *ctx, const char *repo, const char *remote);
char *mv_git_push(mv_ctx *ctx, const char *repo, const char *remote, const char *refspec);
char *mv_git_pull(mv_ctx *ctx, const char *repo, const char *remote, const char *branch);
char *mv_git_remote(mv_ctx *ctx, const char *repo, const char *action,
                    const char *name, const char *url);
char *mv_git_config(mv_ctx *ctx, const char *repo, const char *key, const char *value);
char *mv_git_addall(mv_ctx *ctx, const char *repo);

#endif /* MVXGIT_H */
