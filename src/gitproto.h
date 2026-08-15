/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* gitproto.h — the wire protocol between a BASIC session and the per-user
 * background process (mv_git#40).
 *
 * WHY THIS EXISTS.  On UniVerse, BASIC cannot call libgit2: GCI is licensed and
 * non-functional in the Trial Edition.  So the libgit2 work moves out of the
 * session into a background process the user owns, reached over named pipes.
 * It is Model B — the same division of labour as the UniData CallC bridge
 * (gitcallcb.c): BASIC drives the record loop on the live session, C does the
 * git-object work, and C never calls back into BASIC.
 *
 * FRAMING — length-prefixed, never delimiter-terminated.  Three facts force
 * this, each established by probe on UniVerse 14.2.1:
 *
 *   1. The payload is binary.  Git objects are zlib streams, so 0x0A occurs at
 *      arbitrary offsets and WRITESEQ/READSEQ (newline-terminated) would shred
 *      them.  Record content is binary for the same reason plus the marks.
 *   2. READBLK does not short-return at EOF on a FIFO.  OPENSEQ holds the pipe
 *      read/write, so the reader is also a writer, EOF never arrives, and a
 *      read for more bytes than are present HANGS.  Every read must therefore
 *      be for an exact, known count — which is what the length header supplies.
 *   3. There is consequently no EOF to terminate a delimited frame on, so
 *      delimiter framing is not merely lossy here, it is unimplementable.
 *
 * Headers are fixed-width ASCII so the BASIC side can build and parse them with
 * ordinary string operations, and so a hexdump of a stuck pipe is readable.
 *
 * EVERYTHING ON THE WIRE IS TEXT.  Payloads are hex-encoded, so every byte that
 * crosses the pipe is in the invariant ASCII subset.  MultiValue platforms do
 * not carry arbitrary bytes comfortably: NUL is the port's own binary marker and
 * terminates a C string wherever data crosses into C, and — the deciding
 * reason — UniVerse may have NLS or codepage translation active on sequential
 * I/O, which can alter ANY non-ASCII byte in transit.  Record marks (0xFE, 0xFD,
 * 0xFC) are non-ASCII, so raw record content is exactly what such a layer would
 * corrupt.  Hex is immune to all of it, and it decodes with the platform's own
 * builtin conversion rather than a per-byte BASIC loop.
 *
 * The cost is that a payload doubles in size.  Base64 would cost a third
 * instead, but needs a hand-rolled codec in BASIC; hex was chosen because the
 * conversion is builtin and therefore runs at native speed on bulk data.  If
 * payload size ever outweighs that, the encoding is confined to one function at
 * each end.
 *
 * REQUEST  MVG1 | token(32) | opcode(16) | nargs(2) | bodylen(10)   = 64 bytes
 *          body: nargs repetitions of [len(10)][len hex chars]
 * RESPONSE MVG1 | status(4) | bodylen(10)                           = 18 bytes
 *          body: bodylen hex chars (engine output, or raw record content)
 *
 * Lengths count ENCODED characters — what to read off the pipe — so the reader
 * never has to know the decoded size in advance.  An error reply's body is the
 * message, hex-encoded like any other.
 *
 * Arguments are individually length-prefixed rather than separated, because no
 * byte is safe as a separator in record content.  That holds at the BASIC
 * interface too: GIT.RPC takes a DIMENSIONED array, not an @AM-separated
 * string, since an @AM-separated list would split a record at its own marks.
 *
 * The token is checked on every request, not just at connect.  It does two
 * jobs: it distinguishes sessions that share one OS uid (the shared-account
 * case — filesystem permissions cannot, since they are the same user), and it
 * makes a desynchronised stream fail closed.  A length-framed protocol has no
 * resynchronisation point: once the two ends disagree about where a frame
 * starts, every subsequent byte is garbage, and a checked token turns silent
 * corruption into a clean rejection.
 */
#ifndef GITPROTO_H
#define GITPROTO_H

#define MVG_MAGIC        "MVG1"
#define MVG_MAGIC_LEN    4
#define MVG_TOKEN_LEN    32
#define MVG_OPCODE_LEN   16
#define MVG_NARGS_LEN    2
#define MVG_LEN_LEN      10
#define MVG_STATUS_LEN   4

#define MVG_REQ_HDR      (MVG_MAGIC_LEN + MVG_TOKEN_LEN + MVG_OPCODE_LEN + \
                          MVG_NARGS_LEN + MVG_LEN_LEN)          /* 64 */
#define MVG_RSP_HDR      (MVG_MAGIC_LEN + MVG_STATUS_LEN + MVG_LEN_LEN) /* 18 */

/* Offsets into the request header. */
#define MVG_OFF_MAGIC    0
#define MVG_OFF_TOKEN    (MVG_OFF_MAGIC + MVG_MAGIC_LEN)        /* 4  */
#define MVG_OFF_OPCODE   (MVG_OFF_TOKEN + MVG_TOKEN_LEN)        /* 36 */
#define MVG_OFF_NARGS    (MVG_OFF_OPCODE + MVG_OPCODE_LEN)      /* 52 */
#define MVG_OFF_BODYLEN  (MVG_OFF_NARGS + MVG_NARGS_LEN)        /* 54 */

/* Status codes.  0 is success; the body then carries the engine's output. */
#define MVG_OK           0
#define MVG_EPROTO       1   /* bad magic, malformed header, bad length   */
#define MVG_EAUTH        2   /* token mismatch — wrong session, or desync */
#define MVG_EOPCODE      3   /* unknown opcode                            */
#define MVG_EARGS        4   /* wrong argument count for the opcode       */
#define MVG_EFAIL        5   /* the engine reported a failure             */

/* A single request may not exceed this; a guard against a desynchronised
   stream being read as a colossal length and exhausting memory. */
#define MVG_MAX_BODY     (256L * 1024L * 1024L)

/* Limits on the run-state directory, whose default is $HOME/.mvgit (0700). */
#define MVG_PATH_MAX     1024

#endif /* GITPROTO_H */
