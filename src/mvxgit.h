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
   mvx_ctx, and open/read/write/select/readnext/delete/createfile/filelist plus
   mv_init/clear/set_str/val_chars, mvx_openaccount and mvx_fatal) and never
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
#endif

char *mvx_git_init(mvx_ctx *ctx, const char *repo);
char *mvx_git_add(mvx_ctx *ctx, const char *repo, const char *file,
                  const char *id);
/* Stage a git submodule directory `name` as a gitlink (not as records). */
char *mvx_git_addsub(mvx_ctx *ctx, const char *repo, const char *name);
/* Stage the on-disk working tree exactly as `git add -A` would (modes,
   .gitignore, top-level files, deletions).  Step one of `mvx-git add -A`. */
char *mvx_git_adddisk(mvx_ctx *ctx, const char *repo);
/* Normalise the staged index to the open account format (%FILE% -> DIR/hash,
   .mvx -> .mv-account) — the git objects carry the open form; disk stays
   native.  Run after `add` when `mvx.openaccount` is set. */
char *mvx_git_openform(mvx_ctx *ctx, const char *repo);
/* Materialise a native account directly from the repo's HEAD tree (the clone
   path): MV files -> backend, .mv-account/.mvx -> .mvx, plain files -> disk.
   The open form never touches disk; no external adopt tool is run. */
char *mvx_git_materialize(mvx_ctx *ctx, const char *repo);
char *mvx_git_rm(mvx_ctx *ctx, const char *repo, const char *file,
                 const char *id);
char *mvx_git_commit(mvx_ctx *ctx, const char *repo, const char *msg);
char *mvx_git_status(mvx_ctx *ctx, const char *repo);
char *mvx_git_log(mvx_ctx *ctx, const char *repo, const char *count);
char *mvx_git_diff(mvx_ctx *ctx, const char *repo, const char *file);
char *mvx_git_show(mvx_ctx *ctx, const char *repo, const char *file,
                   const char *id);
char *mvx_git_branch(mvx_ctx *ctx, const char *repo, const char *name);
char *mvx_git_checkout(mvx_ctx *ctx, const char *repo, const char *name);
char *mvx_git_merge(mvx_ctx *ctx, const char *repo, const char *branch);
char *mvx_git_cherrypick(mvx_ctx *ctx, const char *repo, const char *commit);
char *mvx_git_restore(mvx_ctx *ctx, const char *repo, const char *file);

#endif /* MVXGIT_H */
