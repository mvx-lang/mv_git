/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* mvsession — an MV session held open for account I/O (mv_git#47).
 *
 * One session per account, running BP/GIT.AGENT, spoken to over the same framed
 * hex protocol BASIC already used for mvgitd (gitproto.h) with the roles
 * swapped: here the C side is the client and the session is the server.
 *
 * Shared by UniVerse and UniData: both keep their records behind a session, so
 * both reach them the same way, and the only difference is which shell to start
 * (mvs_set_shell).  MVX needs none of this — its engine IS its runtime and reads
 * records directly.
 *
 * A SESSION IS A LICENCE.  That is the constraint the whole interface is shaped
 * around.  A session is opened only when there is work in that account, closed
 * as soon as the work is done, and released cleanly rather than killed — a
 * session that dies abruptly can leave its licence held.  Callers working across
 * several accounts should therefore open, use and close one before moving to the
 * next, which is what mvs_jobs() defaults to allowing: exactly one at a time.
 */
#ifndef UVSESSION_H
#define UVSESSION_H

#include <stddef.h>

typedef struct mv_session mv_session;

/* The MV shell to exec for a session: "uv" on UniVerse, "udt" on UniData.  This
   is the only platform-specific thing in the session layer — everything else,
   from the framing to the licence discipline, is about talking to a session
   rather than about which product is running it.  A driver must set it before
   opening a session; there is no default, because a wrong one fails as an agent
   that never answers rather than as the mistake it is. */
void mvs_set_shell(const char *bin);
const char *mvs_shell(void);

/* Cap on sessions open at once — the number of licences this run may hold.
   Defaults to 1.  mvs_open() blocks (by closing the least recently used session)
   rather than exceeding it, so the cap is a guarantee, not a hint. */
void mvs_set_jobs(int n);
int  mvs_jobs(void);

/* How long an idle session waits for its next request before giving up and
   exiting, in seconds; 0 restores the default of 30.  A session is a licence, so
   one nobody is using should not keep costing one — but reconnecting is not free
   either, hence a window rather than closing after every call.  A session that
   times out is reopened transparently on the next call, so this is a tuning
   knob, not a behaviour change. */
void mvs_set_idle(int seconds);
int  mvs_idle(void);

/* Reset a session's idle timer without doing anything else — for a caller that
   knows it will want the session shortly but has nothing to ask yet. */
int  mvs_keepalive(mv_session *s);

/* Open a session in `account` and start the agent.  Returns NULL on failure,
   with a reason in `err`.  Re-opening an account that already has a live
   session returns the existing one. */
mv_session *mvs_open(const char *account, char *err, size_t errcap);

/* One request/reply.  `args`/`lens` carry `nargs` arguments; binary is fine,
   they are hex-encoded in transit.  Returns the protocol status (0 = OK); on
   return *out holds the reply body, malloc'd for the caller to free, and
   *outlen its length.  Either out or outlen may be NULL to discard the body. */
int mvs_call(mv_session *s, const char *op, int nargs,
             const char *const *args, const long *lens,
             char **out, long *outlen);

/* Convenience for the common all-strings case. */
int mvs_calls(mv_session *s, const char *op, int nargs,
              const char *const *args, char **out, long *outlen);

/* Finish with this session: QUIT, wait for it to exit, and only escalate to a
   signal if it will not go.  Releases the licence. */
void mvs_close(mv_session *s);

/* Close every open session.  Installed on SIGINT/SIGTERM so an interrupted run
   does not leave licences held; also safe to call at exit. */
void mvs_close_all(void);

/* The account a session belongs to (for diagnostics). */
const char *mvs_account(const mv_session *s);

/* `<driver>-git agent [OPCODE [arg...]]` — the session-layer diagnostic, shared
   by both drivers because it is about the SESSION, not either platform.  `i`
   indexes the `agent` word itself.  Returns a process exit status. */
int mv_agent_cmd(int argc, char **argv, int i);

/* Put an account I/O agent into the CURRENT DIRECTORY's account and compile it:
   the file to hold it, a PLATFORM.H to $INCLUDE, the source, the compile.  For
   an account where nothing is installed yet — a fresh `clone`, or a stock
   account stood up to learn a baseline from.  0 if the agent compiled. */
int mv_agent_seed(void);

#endif /* UVSESSION_H */
