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
     - MVXGIT_GITD (mvgitd): no record layer at all (gitd_rt.h / gitd_rt.c) —
       the background process does git-object work for a session that keeps its
       own records, so the primitives abort if ever reached.  See gitd_rt.h.

   The engine body is identical either way; only genuine behavioural
   differences (UniData has no on-disk descriptor, generates %INDEXES%
   virtually) are guarded inline with #ifdef MVXGIT_UDT. */
#if defined(MVXGIT_GITD)
#  include "gitd_rt.h"
#elif defined(MVXGIT_UDT)
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
#  define mv_deletefile   mvx_deletefile
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
/* Stage a control blob VERBATIM, with no stickiness — the attribute editor's
   way in (mv_git#15).  Everything else stages through mv_git_stageblob and
   keeps the recorded geometry; this is the one path meant to change it, which
   is why deliberately changing a shipped default is a command and not a side
   effect of resizing a file. */
char *mv_git_stagectl(mv_ctx *ctx, const char *repo, const char *path,
                      const char *content);
/* Put an edited account descriptor back on disk in this platform's own form —
   the portable one is CONVERTED, a native one written verbatim.  The descriptor
   is one thing in two spellings and the engine converts both ways already, so an
   edit landing in only the git side is not an edit to the account.  Does nothing
   where no descriptor lives on disk (UniData, UniVerse). */
char *mv_git_putdesc(mv_ctx *ctx, const char *repo, const char *path,
                     const char *content);
/* The STAGED content of `path` (the index, not HEAD), marks restored.  An
   editor must build on the edit before it, not on the last commit. */
char *mv_git_ixcat(mv_ctx *ctx, const char *repo, const char *path);
/* …with the true byte length, for the same reason CAT has one: a staged blob is
   arbitrary bytes and strlen would truncate it at the first NUL. */
char *mv_git_ixcat_len(mv_ctx *ctx, const char *repo, const char *path,
                       int64_t *outlen);
/* What the index holds that HEAD does not: "<status>  <path>" per delta. */
char *mv_git_staged(mv_ctx *ctx, const char *repo);
/* The unstaged diff with hunk headers — the -u form of mv_git_diff. */
char *mv_git_diff_u(mv_ctx *ctx, const char *repo, const char *file);
/* A unified diff of two @AM-separated CONTENTS, so the BASIC diff bodies render
   through the same libgit2 call the C diff does instead of hand-rolling one. */
char *mv_git_udiff(mv_ctx *ctx, const char *oldtext, const char *newtext,
                   const char *path);

/* Render the canonical portable `.mv-account` descriptor for an account with the
   given identity and default hash backend into `out`; returns its length.  One
   schema shared by mvx-git (converting from the native `.mvx`) and udt-git
   (synthesising from the live UniData account), so both emit identical bytes. */
/* Where this account sits beneath the repository root, as a path prefix — ""
   (the default) when the account IS the root, "acctA/" when it is below one.
   Records commit at <prefix><file>/<id>, so several accounts can share one
   repository without both claiming CUST/C1.  Set once per account, before the
   operation; the engine applies it at the single point where a record's git
   path is built, and strips it again where paths are read back. */
void mv_git_set_prefix(const char *prefix);

/* Forget what was cached about the account currently in scope (its file list).
   Setting a prefix does this implicitly; call it directly when the working
   directory changes without the prefix changing. */
void mv_git_forget_account(void);

/* The stock-account baseline: a file of `<blob-oid> <record-id>` lines naming
   the VOC records a FRESH account of this flavour is born with.  A record
   matching one exactly is the system's, not the account's, and is neither
   staged nor reported (mv_git#46).  Building the file needs a session and is
   the driver's job; applying it is platform-neutral and lives here, which also
   makes it available to both the CLI and the in-session verb.  NULL or a
   missing file means no subtraction — every record is the account's own. */
int mv_account_furniture(const char *name, size_t len);
char *mv_git_filter_furniture(const char *list);
char *mv_git_versions(const char *self);
int mv_agent_cataloged(void);
int mv_git_desc_for(char *path, size_t pcap, char *desc, size_t dcap,
                    const char *prefix, int open);
void mv_git_set_stock(const char *path);

/* The baseline as ids alone (mark- or newline-separated), supplied by BASIC
   where the engine may not read records itself.  Enough for the delete
   protection, which never compares content. */
void mv_git_stock_ids(const char *ids);

int mv_git_desc_open(const char *name, const char *version,
                     const char *description, const char *hash,
                     char *out, size_t cap);

/* Adopt a descriptor onto THIS platform (mv_git#44).

   Adoption is a CONVERSION, not a restoration.  A repository may carry a native
   descriptor from wherever it was committed — `.mvx` (MVX), `.udt` (UniData) —
   and each describes a platform this one is not: `.mvx` names an lmdb backend,
   `.udt` a UniData file type.  Rebuilding the account here therefore means
   rewriting the descriptor to describe THIS platform, and the rewrite is left in
   the working tree as an ordinary git change so the user reviews and commits it.
   A clone must never silently mutate what a repository claims to be.

   `platform` is the native marker's bare name ("uv", "udt", "mvx").  `flavour`
   supplies a field the source could not carry (UniVerse's VOC flavour, mv_git#15)
   and is ignored when empty, so a platform with no such notion never acquires a
   meaningless field.  With `open_form` the portable `.mv-account` is rendered
   instead — the interchange form, and the one to prefer when the account will
   travel again.

   `name_out` receives the descriptor filename to write; when it differs from the
   source's, that is the rename git will report.  Returns the rendered length. */
int mv_git_desc_adopt(const char *src, size_t srclen, const char *platform,
                      const char *flavour, int open_form,
                      char *name_out, size_t name_cap,
                      char *out, size_t cap);

/* Read one field from a descriptor (either form), so a driver can tell what the
   source could not supply without duplicating the parser.  Returns 1 when the
   field is present and non-empty, 0 otherwise. */
int mv_git_desc_field(const char *src, size_t srclen, const char *key,
                      char *out, size_t cap);

/* A %FILE% CONTROL IS EXTENSIBLE, AND ITS FIRST LINE IS THE WHOLE OF THE OLD
   FORMAT.  Line 1 is the file's class — "DIR", or "hash <modulo> STATIC|DYNAMIC"
   — and every line after it is one `key = value` parameter from the attribute
   registry (BP/GIT.ATTR.DEFS, mv_git#15): the UniData dynamic set (minmod,
   split, merge, largerec, blocksize, hashtype) and whatever later backends
   bring.  Growing the control this way rather than widening line 1 is what lets
   a reader that predates a parameter still get the class right, and it is the
   same `key = value` grammar the account descriptor already uses.

   So a reader that wants the class takes LINE ONE, not the trimmed blob.  A
   control now runs to a few hundred bytes rather than the couple of dozen the
   single line needed, which is what MV_GIT_CTL_MAX sizes. */
#define MV_GIT_CTL_MAX 512

/* The open-form %FILE% control committed for `base` (HEAD's <base>.DICT/%FILE%)
   in `out`; returns its length, or -1 if `base` has no committed control yet.
   Generators use it to keep a shipped hash modulo sticky: a re-add preserves the
   committed default instead of overwriting it with the live file's current size,
   so one customer's resize never becomes another's. */
int mv_git_committed_control(const char *repo, const char *base,
                             char *out, size_t cap);

/* %FILE% IS CREATE-TIME METADATA, NOT LIVE STATE.  Given a blob about to be
   staged at `path`, return the content that should actually go in: for a hash
   file's <base>.DICT/%FILE% that HEAD already carries, the committed control,
   otherwise `content` unchanged.  `keep` is scratch of at least MV_GIT_CTL_MAX bytes; the
   returned pointer is either `content` or `keep`, and *len is updated to match.

   A file's geometry is recorded when git first learns the file exists, and then
   it stops moving.  Resizing the live file is local operational tuning — it is
   not a change to the thing being versioned, so it must not show as a diff, and
   it must not ride out to every other clone as a new default.  The geometry is
   consulted in exactly one place: creating the file on an MV system that does
   not have it yet.  (Deliberately changing a shipped default is the attribute
   editor's job — mv_git#15 — not a side effect of a resize.)

   Shared by all three staging entry points (engine subroutine, CallC bridge,
   daemon op) because a copy that missed one of them is precisely the bug this
   fixes: the udt/uv add path went through the one wrapper that lacked the rule,
   so every re-add rewrote the control from the live file. */
const char *mv_git_sticky_control(const char *repo, const char *path,
                                  const char *content, int64_t *len,
                                  char *keep, size_t cap);

/* Batched staging (bulk in-session use): open once, accumulate blobs across many
   mv_git_batch_add calls, write the index once at mv_git_batch_end.  O(n) — for
   staging a whole account without re-opening the repo per record. */
int  mv_git_batch_begin(const char *repo);
void mv_git_batch_add(const char *path, const char *content, int64_t len,
                      int translate);
void mv_git_batch_end(void);
/* How many records the batch skipped because their id cannot be a git path (an
   id containing "/", or one that is "." or "..").  A skipped record is NOT
   versioned, so a front-end must tell the user rather than let it pass. */
long mv_git_batch_skipped(void);

/* Checkout side (git -> account): list every blob path in HEAD, and fetch one
   blob's content with attribute marks restored, so a driver can create files
   and WRITE records on its own platform (the UniData in-session GIT verb). */
char *mv_git_headfiles(mv_ctx *ctx, const char *repo);
char *mv_git_catpath(mv_ctx *ctx, const char *repo, const char *path);
/* As mv_git_catpath, but reporting the content's true byte length.  Record
   content is arbitrary bytes and may contain NULs, so a caller able to carry an
   explicit length must use this instead of measuring with strlen — otherwise a
   binary record is silently truncated on the way back out. */
char *mv_git_catpath_len(mv_ctx *ctx, const char *repo, const char *path,
                         int64_t *outlen);
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
/* Unstage every record of a file the account no longer has — %FILE% gone means
   the file is gone.  Its own entry point because the wholesale add has three
   implementations (C on MVX, BASIC on UniVerse and UniData) and all of them
   must reconcile the same way.
   `live` is the account's file names, @AM-separated, from whoever CAN see them:
   mvgitd has no record backend, so it must be told rather than asked.  Empty
   means "ask the backend" (in-process: MVX and the CLI drivers). */
char *mv_git_prune_gone(mv_ctx *ctx, const char *repo, const char *live);
/* The record ids staged under <file>/, @AM-separated — so a caller that CAN read
   the account can tell which of them are gone.  Neither side can reconcile a
   record deletion alone: BASIC cannot see the index, mvgitd cannot read records. */
char *mv_git_index_ids(mv_ctx *ctx, const char *repo, const char *file);
char *mv_git_log(mv_ctx *ctx, const char *repo, const char *count);
char *mv_git_diff(mv_ctx *ctx, const char *repo, const char *file);
char *mv_git_show(mv_ctx *ctx, const char *repo, const char *file,
                   const char *id);
char *mv_git_branch(mv_ctx *ctx, const char *repo, const char *name);
char *mv_git_checkout(mv_ctx *ctx, const char *repo, const char *name);
char *mv_git_switch(mv_ctx *ctx, const char *repo, const char *name);
char *mv_git_merge(mv_ctx *ctx, const char *repo, const char *branch);
char *mv_git_cherrypick(mv_ctx *ctx, const char *repo, const char *commit);
char *mv_git_restore(mv_ctx *ctx, const char *repo, const char *file);
/* remotes & clone (libgit2, no OS git) — mvx#94 / mvpkg#23 */
char *mv_git_clone(mv_ctx *ctx, const char *url, const char *dir, const char *ref);
char *mv_git_fetch(mv_ctx *ctx, const char *repo, const char *remote);
char *mv_git_push(mv_ctx *ctx, const char *repo, const char *remote, const char *refspec);
char *mv_git_pull(mv_ctx *ctx, const char *repo, const char *remote, const char *branch);
/* Pull WITHOUT writing records: fetch, move the ref, sync the git index — and
   leave materialising to the caller.  For a server that has no record backend
   (mvgitd), where the alternative is calling a record primitive and dying. */
char *mv_git_pullref(mv_ctx *ctx, const char *repo, const char *remote,
                     const char *branch);
char *mv_git_remote(mv_ctx *ctx, const char *repo, const char *action,
                    const char *name, const char *url);
char *mv_git_config(mv_ctx *ctx, const char *repo, const char *key, const char *value);
char *mv_git_addall(mv_ctx *ctx, const char *repo);
char *mv_git_tag(mv_ctx *ctx, const char *repo, const char *op, const char *name,
                 const char *target, const char *message);

#endif /* MVXGIT_H */
