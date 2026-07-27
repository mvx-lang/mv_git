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
 * identical engine instead of shelling out to git and mvx-convert-acct.
 *
 * Each call returns a malloc'd output string the caller frees.  Lines are
 * separated by the attribute mark 0xFE (@AM) — the mvx-git executable renders
 * those as newlines.  `ctx` is a runtime context bound to the account
 * (MVXACCOUNT); `repo` is the repository path, normally ".git".
 */
#ifndef MVXGIT_H
#define MVXGIT_H

#include "mvx_runtime.h"

char *mvx_git_init(mvx_ctx *ctx, const char *repo);
char *mvx_git_add(mvx_ctx *ctx, const char *repo, const char *file,
                  const char *id);
/* Stage a git submodule directory `name` as a gitlink (not as records). */
char *mvx_git_addsub(mvx_ctx *ctx, const char *repo, const char *name);
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
