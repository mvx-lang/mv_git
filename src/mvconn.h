/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* mvconn — the CLI's connection to an MV system (mv_git#267).
 *
 * One object per account, created by a driver's main() and handed to whatever
 * needs to talk to the system.  It is where the front ends stop each keeping
 * their own copy of the conversation: before this there were four byte-identical
 * `open_account_on()` readers, four `ask_open_account()` prompts, and eleven
 * places that wrote $MVX_OPENACCOUNT.
 *
 * IT STOPS AT THE ENGINE.  mvxgit.h binds the record primitives at compile time
 * on purpose -- one binary per platform, no runtime indirection, across 659 call
 * sites -- and this does not change that.  It sits in front, where the drivers
 * are, and it will grow the transport (direct C on MVX/jBASE/OpenQM, GIT.AGENT
 * over a pipe on the U2s, which mvsession already is) in later stages of #267.
 *
 * Stage 1 is the open account: asked once, held here, exported once.
 */
#ifndef MVCONN_H
#define MVCONN_H

#include <stddef.h>
#include <strings.h>

typedef struct mv_conn mv_conn;

/* A connection to the account at `account` -- a path, "." for the current
   directory.  Never fails for want of a repository: an account that is not a
   git repository yet is a normal thing to hold a connection to (`clone` and
   `adopt` both start there), and the open-account answer is simply "no". */
mv_conn *mvconn_open(const char *account);
void     mvconn_close(mv_conn *c);
const char *mvconn_account(const mv_conn *c);

/* --- the open account ---------------------------------------------------
 *
 * Three things decide it, in this order: what the operator said on the command
 * line, what the repository says, and -- only when neither has -- what the
 * operator says when asked.  The driver's job is to say which of those it has;
 * the resolution is here.
 */

/* `mvx.openaccount` in the account's own git config.  0 when there is no
   repository, no setting, or the setting is off. */
int  mvconn_config_open_account(mv_conn *c);

/* --open-account (1) / --no-open-account (0).  Wins over everything. */
void mvconn_set_open_account(mv_conn *c, int on);
int  mvconn_have_open_account(const mv_conn *c);   /* has one been set? */

/* The resolved answer: the override if there is one, else the config. */
int  mvconn_open_account(mv_conn *c);

/* Ask, for a driver that has to convert something and has no answer yet.
   $MVXGIT_OPEN_ACCOUNT answers for a script; with no terminal the answer is
   yes, because the alternative is a clone that silently stops being portable.
   The answer is remembered on the connection, so asking twice cannot give two
   different answers.  `prog` names the driver in the message. */
int  mvconn_ask_open_account(mv_conn *c, const char *prog, const char *subject);

/* Where the account's git config actually lives.  Not always
   <account>/.git/config: in a worktree or a submodule `.git` is a FILE naming
   the real directory. */
void mvconn_config_path(mv_conn *c, char *buf, size_t n);

/* Write `mvx.openaccount = true` into the account's own config, so the next
   command in it knows without being told again.  ONE MECHANISM: the drivers
   had three -- libgit2 on MVX and UniVerse, system("git config ...") on jBASE,
   an execv of git on UniData -- which meant the flag could be written by a git
   that is not on PATH, or into a path that is not where the config is. */
int  mvconn_persist_open_account(mv_conn *c);

/* THE ONE PLACE $MVX_OPENACCOUNT IS WRITTEN.  The engine's deep call sites have
   no connection to ask -- that is what the variable is for -- so the boundary
   between "the object knows" and "the engine can see it" is this call, made by
   the driver once it has an account.  Only ever sets, never clears, so an
   explicit setting from the environment survives. */
void mvconn_export_open_account(mv_conn *c);

/* ONE READING OF THE BOOLEAN (#267).  There were three: the CLI's
   $MVXGIT_OPEN_ACCOUNT rejected "0*", "no", "false" and "off"; the engine's
   $MVX_OPENACCOUNT rejected `*v != '0'` in two arms and `strcmp(v,"0")` in the
   other two -- so "false" meant ON to the engine and OFF to the CLI.  Nothing
   reaches that today because every writer writes "1", which is exactly why it
   would have gone unnoticed when something stopped.  Inline so the engine arms,
   which are compiled into binaries that do not link mvconn.c, can share it. */
static inline int mvconn_env_true(const char *v) {
    if (!v || !*v) return 0;
    return !(v[0] == '0' || strcasecmp(v, "no") == 0 ||
             strcasecmp(v, "false") == 0 || strcasecmp(v, "off") == 0);
}

#endif /* MVCONN_H */
