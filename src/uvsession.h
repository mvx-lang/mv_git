/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* uvsession — a UniVerse session held open for account I/O (mv_git#47).
 *
 * One session per account, running BP/GIT.AGENT, spoken to over the same framed
 * hex protocol BASIC already used for mvgitd (gitproto.h) with the roles
 * swapped: here the C side is the client and the session is the server.
 *
 * A SESSION IS A LICENCE.  That is the constraint the whole interface is shaped
 * around.  A session is opened only when there is work in that account, closed
 * as soon as the work is done, and released cleanly rather than killed — a
 * session that dies abruptly can leave its licence held.  Callers working across
 * several accounts should therefore open, use and close one before moving to the
 * next, which is what uvs_jobs() defaults to allowing: exactly one at a time.
 */
#ifndef UVSESSION_H
#define UVSESSION_H

#include <stddef.h>

typedef struct uv_session uv_session;

/* Cap on sessions open at once — the number of licences this run may hold.
   Defaults to 1.  uvs_open() blocks (by closing the least recently used session)
   rather than exceeding it, so the cap is a guarantee, not a hint. */
void uvs_set_jobs(int n);
int  uvs_jobs(void);

/* Open a session in `account` and start the agent.  Returns NULL on failure,
   with a reason in `err`.  Re-opening an account that already has a live
   session returns the existing one. */
uv_session *uvs_open(const char *account, char *err, size_t errcap);

/* One request/reply.  `args`/`lens` carry `nargs` arguments; binary is fine,
   they are hex-encoded in transit.  Returns the protocol status (0 = OK); on
   return *out holds the reply body, malloc'd for the caller to free, and
   *outlen its length.  Either out or outlen may be NULL to discard the body. */
int uvs_call(uv_session *s, const char *op, int nargs,
             const char *const *args, const long *lens,
             char **out, long *outlen);

/* Convenience for the common all-strings case. */
int uvs_calls(uv_session *s, const char *op, int nargs,
              const char *const *args, char **out, long *outlen);

/* Finish with this session: QUIT, wait for it to exit, and only escalate to a
   signal if it will not go.  Releases the licence. */
void uvs_close(uv_session *s);

/* Close every open session.  Installed on SIGINT/SIGTERM so an interrupted run
   does not leave licences held; also safe to call at exit. */
void uvs_close_all(void);

/* The account a session belongs to (for diagnostics). */
const char *uvs_account(const uv_session *s);

#endif /* UVSESSION_H */
