/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* gitd.c — mvgitd, the per-user background process that runs libgit2 on behalf
 * of a BASIC session that cannot (mv_git#40).
 *
 * It is deliberately NOT a daemon.  It belongs to the user who started it, is
 * started lazily by the GIT verb rather than installed, serves only that user,
 * and exits on its own once idle — so there is nothing to administer, no boot
 * script, and no root.
 *
 * Access control is the filesystem: the run directory is 0700 and every pipe
 * inside it 0600, so another OS user cannot open them at all.  The token layered
 * on top does the job permissions cannot — it separates sessions sharing one uid
 * (the shared-account case) and makes a desynchronised stream fail closed rather
 * than corrupt.  See gitproto.h for the framing and why it must be length-based.
 *
 * Modes:
 *   mvgitd --connect <sid>   ensure a server is running, create this session's
 *                            pipe pair, announce it.  What the GIT verb calls;
 *                            idempotent and fast.
 *   mvgitd --serve           the server loop (spawned by --connect, not by hand)
 *   mvgitd --stop            ask a running server to exit
 *   mvgitd --status          report whether one is running
 *
 * Concurrency: the server forks a child per session, so two sessions never share
 * a pipe or an index.  Each child owns its session's pipes for its lifetime and
 * removes them on the way out.
 */

#define _POSIX_C_SOURCE 200809L

#include "mvxgit.h"
#include "gitproto.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How long the server lingers with no sessions before exiting, and how long a
   child waits on a silent client before giving up.  These are BACKSTOPS, not the
   mechanism: a child normally leaves as soon as it notices its session has gone
   (see session_alive), and these bound the case where that check cannot help —
   a client that is alive but has stopped talking.  A killed session leaves a
   FIFO behind either way, since a FIFO outlives the process that served it. */
#define IDLE_EXIT_SECS    300
#define SESSION_IDLE_SECS 900

/* How often a child looks up to see whether its session is still there. */
#define LIVENESS_POLL_SECS 5

static char g_dir[MVG_PATH_MAX];      /* run directory, 0700 */
static char g_token[MVG_TOKEN_LEN + 1];

/* --- small helpers -------------------------------------------------------- */

static void die(const char *what) {
    fprintf(stderr, "mvgitd: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void pathcat(char *out, size_t cap, const char *leaf) {
    snprintf(out, cap, "%s/%s", g_dir, leaf);
}

/* Resolve the run directory ($MVGIT_DIR, else $HOME/.mvgit) and create it 0700.
   0700 is the access control, so a pre-existing directory with looser bits is
   tightened rather than trusted. */
static void resolve_dir(void) {
    const char *d = getenv("MVGIT_DIR");
    if (d && *d) {
        snprintf(g_dir, sizeof g_dir, "%s", d);
    } else {
        const char *h = getenv("HOME");
        if (!h || !*h) { fprintf(stderr, "mvgitd: $HOME is not set\n"); exit(1); }
        snprintf(g_dir, sizeof g_dir, "%s/.mvgit", h);
    }
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST) die(g_dir);
    if (chmod(g_dir, 0700) != 0) die(g_dir);
}

/* Read the token, creating it if absent.  O_EXCL makes creation the race
   winner's job, so two sessions connecting at once cannot end up with different
   tokens for the same server. */
static void ensure_token(void) {
    char p[MVG_PATH_MAX];
    pathcat(p, sizeof p, "token");

    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        unsigned char raw[MVG_TOKEN_LEN / 2];
        int rfd = open("/dev/urandom", O_RDONLY);
        if (rfd < 0) die("/dev/urandom");
        if (read(rfd, raw, sizeof raw) != (ssize_t)sizeof raw) die("/dev/urandom");
        close(rfd);
        char hex[MVG_TOKEN_LEN + 1];
        for (size_t i = 0; i < sizeof raw; i++)
            snprintf(hex + i * 2, 3, "%02x", raw[i]);
        if (write(fd, hex, MVG_TOKEN_LEN) != MVG_TOKEN_LEN) die(p);
        close(fd);
    } else if (errno != EEXIST) {
        die(p);
    }

    fd = open(p, O_RDONLY);
    if (fd < 0) die(p);
    ssize_t n = read(fd, g_token, MVG_TOKEN_LEN);
    close(fd);
    if (n != MVG_TOKEN_LEN) {
        fprintf(stderr, "mvgitd: token at %s is malformed (%zd bytes)\n", p, n);
        exit(1);
    }
    g_token[MVG_TOKEN_LEN] = '\0';
}

/* Read/write exactly n bytes, or fail.  Short reads are normal on a pipe, so
   both loop; neither ever asks for more than the frame declared. */
static int read_full(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return 0;                        /* peer gone */
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 1;
}

static int write_full(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 1;
}

/* --- payload encoding ----------------------------------------------------- *
 * Everything on the wire is hex, so no non-ASCII byte ever crosses the pipe.
 * See gitproto.h for why: NUL, and UniVerse's optional NLS/codepage translation
 * on sequential I/O, which would corrupt record marks.  The BASIC end uses the
 * platform's builtin MX conversion, so this is the only codec written by hand.
 */

static const char HEXD[] = "0123456789ABCDEF";

/* Encode `len` bytes as 2*len hex characters, NUL-terminated. */
static char *hex_encode(const char *in, long len, long *outlen) {
    char *out = malloc((size_t)len * 2 + 1);
    if (!out) return NULL;
    for (long i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        out[i * 2]     = HEXD[c >> 4];
        out[i * 2 + 1] = HEXD[c & 0x0F];
    }
    out[len * 2] = '\0';
    if (outlen) *outlen = len * 2;
    return out;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Decode hex to bytes.  Returns NULL on an odd length or a non-hex character —
   both mean the stream is not what it claims, so the caller rejects the frame
   rather than guessing at a partial decode. */
static char *hex_decode(const char *in, long len, long *outlen) {
    if (len % 2) return NULL;
    char *out = malloc((size_t)(len / 2) + 1);
    if (!out) return NULL;
    for (long i = 0; i < len; i += 2) {
        int hi = hex_val(in[i]), lo = hex_val(in[i + 1]);
        if (hi < 0 || lo < 0) { free(out); return NULL; }
        out[i / 2] = (char)((hi << 4) | lo);
    }
    out[len / 2] = '\0';        /* convenience; the length is authoritative */
    if (outlen) *outlen = len / 2;
    return out;
}

/* Fixed-width zero-padded decimal, the header's only number format. */
static void put_num(char *dst, int width, long v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%0*ld", width, v);
    memcpy(dst, tmp, (size_t)width);
}

static long get_num(const char *src, int width) {
    char tmp[32];
    if (width >= (int)sizeof tmp) return -1;
    memcpy(tmp, src, (size_t)width);
    tmp[width] = '\0';
    for (int i = 0; i < width; i++)
        if (tmp[i] < '0' || tmp[i] > '9') return -1;
    return strtol(tmp, NULL, 10);
}

/* --- the served operations ------------------------------------------------ */

/* Same division of labour as the UniData CallC bridge (gitcallcb.c): every op
   here is pure git-object work.  The difference is that CallC could not marshal
   binary or output buffers, so it had to route results through <repo>/gitmsg and
   <repo>/gitcat side-channel files; a framed pipe carries an explicit length, so
   results come back inline and those files are not needed. */

static const char *rp(const char *repo) {
    return repo && repo[0] ? repo : ".git";
}

/* An op's result.  `len` is carried explicitly rather than recovered with
   strlen, because a committed record is arbitrary bytes and may contain NULs —
   measuring it would truncate the reply and lose data silently.  ok() measures
   text output for convenience; ok_n() is for anything that can be binary. */
typedef struct { char *out; long len; int status; } opres;

static opres ok(char *out) {
    opres r; r.out = out; r.len = out ? (long)strlen(out) : 0; r.status = MVG_OK;
    return r;
}
static opres ok_n(char *out, long len) {
    opres r; r.out = out; r.len = len; r.status = MVG_OK; return r;
}

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Argument vector: a[i] is NUL-terminated, l[i] is its true length (record
   content may contain NULs, so length is authoritative for anything staged). */
typedef struct { char **a; long *l; int n; } args;

static const char *A(const args *g, int i) { return i < g->n ? g->a[i] : ""; }

static opres op_ping(const args *g) {
    (void)g;
    return ok(dup_str("gitd-ok"));
}

/* FLUSH — write the accumulated index to disk now.
 *
 * Staging is batched: mv_git_batch_add builds an in-memory index and only
 * mv_git_batch_end writes it (git_index_write).  That index is the handover to
 * commit, which opens the repo itself and reads it from DISK — so it must be
 * written before the staging command returns.  Nothing else does it in time:
 * commit is a separate command, and where a background process serves one
 * session per command it is a different process with an empty index of its own.
 *
 * Flushing per request instead would mean an index write per record, so the
 * verb calls this once when it has finished staging — the only point that knows
 * the command is over. */
static opres op_flush(const args *g) {
    (void)g;
    long skipped = mv_git_batch_skipped();
    mv_git_batch_end();
    if (skipped > 0) {
        char m[160];
        snprintf(m, sizeof m,
                 "%ld record(s) NOT staged: their id cannot be a git path "
                 "(contains '/', or is '.' or '..')", skipped);
        return ok(dup_str(m));
    }
    return ok(NULL);
}

static opres op_init(const args *g) {
    mv_git_batch_end();                   /* discard any stale open batch */
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_init(ctx, rp(A(g, 0)));
    mv_ctx_destroy(ctx);
    return ok(r);
}

/* STAGE(repo, file, id, record) — one record into the batched index.  The
   session has already READ it; marks become newlines in the blob (translate=1). */
static opres op_stage(const args *g) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", A(g, 1), A(g, 2));
    mv_git_batch_begin(rp(A(g, 0)));      /* idempotent */
    mv_git_batch_add(path, g->a[3], g->l[3], 1);
    return ok(NULL);
}

/* STAGEBLOB(repo, path, content) — a raw blob (the open-account controls).
   %FILE% goes through mv_git_sticky_control, so a resize is not a commit. */
static opres op_stageblob(const args *g) {
    const char *p = A(g, 1);
    const char *use = A(g, 2);
    char keep[MV_GIT_CTL_MAX];
    int64_t ulen = (int64_t)g->l[2];
    use = mv_git_sticky_control(rp(A(g, 0)), p, use, &ulen, keep, sizeof keep);
    mv_git_batch_begin(rp(A(g, 0)));
    mv_git_batch_add(p, use, ulen, 0);
    return ok(NULL);
}

static opres op_commit(const args *g) {
    mv_git_batch_end();                   /* flush the accumulated index */
    mv_ctx *ctx = mv_ctx_create();
    char *r = mv_git_commit(ctx, rp(A(g, 0)), A(g, 1));
    mv_ctx_destroy(ctx);
    return ok(r);
}

/* The remaining ops are uniform thin bridges over the engine. */
#define BRIDGE1(fn, call) static opres fn(const args *g) {                     \
    mv_ctx *ctx = mv_ctx_create();                                             \
    char *r = call;                                                            \
    mv_ctx_destroy(ctx);                                                       \
    return ok(r); }

BRIDGE1(op_status,     mv_git_status(ctx, rp(A(g,0))))
BRIDGE1(op_prune,      mv_git_prune_gone(ctx, rp(A(g,0)), A(g,1)))
BRIDGE1(op_indexids,   mv_git_index_ids(ctx, rp(A(g,0)), A(g,1)))
BRIDGE1(op_log,        mv_git_log(ctx, rp(A(g,0)), A(g,1)[0] ? A(g,1) : "20"))
BRIDGE1(op_branch,     mv_git_branch(ctx, rp(A(g,0)), A(g,1)))
BRIDGE1(op_files,      mv_git_headfiles(ctx, rp(A(g,0))))
/* CAT returns committed record content, which is arbitrary bytes and may
   contain NULs — so it must carry an explicit length rather than be measured
   with strlen, or a binary record comes back truncated. */
static opres op_cat(const args *g) {
    mv_ctx *ctx = mv_ctx_create();
    int64_t n = 0;
    char *r = mv_git_catpath_len(ctx, rp(A(g, 0)), A(g, 1), &n);
    mv_ctx_destroy(ctx);
    return ok_n(r, (long)n);
}
BRIDGE1(op_diff,       mv_git_diff(ctx, rp(A(g,0)), A(g,1)))
BRIDGE1(op_show,       mv_git_show(ctx, rp(A(g,0)), A(g,1), A(g,2)))
BRIDGE1(op_rm,         mv_git_rm(ctx, rp(A(g,0)), A(g,1), A(g,2)))
BRIDGE1(op_addsub,     mv_git_addsub(ctx, rp(A(g,0)), A(g,1)))
BRIDGE1(op_merge,      mv_git_merge(ctx, rp(A(g,0)), A(g,1)))
BRIDGE1(op_cherrypick, mv_git_cherrypick(ctx, rp(A(g,0)), A(g,1)))
BRIDGE1(op_switch,     mv_git_switch(ctx, rp(A(g,0)), A(g,1)))
BRIDGE1(op_checkout,   mv_git_checkout(ctx, rp(A(g,0)), A(g,1)))
BRIDGE1(op_restore,    mv_git_restore(ctx, rp(A(g,0)), A(g,1)))
BRIDGE1(op_tag,        mv_git_tag(ctx, rp(A(g,0)), A(g,1), A(g,2), A(g,3), A(g,4)))
BRIDGE1(op_config,     mv_git_config(ctx, rp(A(g,0)), A(g,1), A(g,2)))
BRIDGE1(op_remote,     mv_git_remote(ctx, rp(A(g,0)), A(g,1), A(g,2), A(g,3)))
BRIDGE1(op_clone,      mv_git_clone(ctx, A(g,1), A(g,2), A(g,3)))
BRIDGE1(op_fetch,      mv_git_fetch(ctx, rp(A(g,0)), A(g,1)))
BRIDGE1(op_push,       mv_git_push(ctx, rp(A(g,0)), A(g,1), A(g,2)))
BRIDGE1(op_pull,       mv_git_pull(ctx, rp(A(g,0)), A(g,1), A(g,2)))
BRIDGE1(op_pullref,    mv_git_pullref(ctx, rp(A(g,0)), A(g,1), A(g,2)))

static const struct {
    const char *name;
    int         nargs;
    opres     (*fn)(const args *);
} OPS[] = {
    { "PING",        0, op_ping       },
    { "FLUSH",       1, op_flush      },
    { "INIT",        1, op_init       },
    { "STAGE",       4, op_stage      },
    { "STAGEBLOB",   3, op_stageblob  },
    { "COMMIT",      2, op_commit     },
    { "STATUS",      1, op_status     },
    { "PRUNE",       2, op_prune      },
    { "INDEXIDS",    2, op_indexids   },
    { "LOG",         2, op_log        },
    { "BRANCH",      2, op_branch     },
    { "FILES",       1, op_files      },
    { "CAT",         2, op_cat        },
    { "DIFF",        2, op_diff       },
    { "SHOW",        3, op_show       },
    { "RM",          3, op_rm         },
    { "ADDSUB",      2, op_addsub     },
    { "MERGE",       2, op_merge      },
    { "CHERRYPICK",  2, op_cherrypick },
    { "SWITCH",      2, op_switch     },
    { "CHECKOUT",    2, op_checkout   },
    { "RESTORE",     2, op_restore    },
    { "TAG",         5, op_tag        },
    { "CONFIG",      3, op_config     },
    { "REMOTE",      4, op_remote     },
    { "CLONE",       4, op_clone      },
    { "FETCH",       2, op_fetch      },
    { "PUSH",        3, op_push       },
    { "PULL",        3, op_pull       },
    { "PULLREF",     3, op_pullref    },
    { NULL, 0, NULL }
};

/* --- session service ------------------------------------------------------ */

/* Send a reply.  `body`/`len` are raw bytes; they go out hex-encoded, and the
   header's length is the ENCODED length — what the reader must take off the
   pipe.  Every reply goes through here, so nothing can reach the wire un-encoded
   by accident. */
static int send_rsp(int fd, int status, const char *body, long len) {
    char hdr[MVG_RSP_HDR];
    long enclen = 0;
    char *enc = NULL;

    if (len > 0) {
        enc = hex_encode(body, len, &enclen);
        if (!enc) return -1;
    }
    memcpy(hdr, MVG_MAGIC, MVG_MAGIC_LEN);
    put_num(hdr + MVG_MAGIC_LEN, MVG_STATUS_LEN, status);
    put_num(hdr + MVG_MAGIC_LEN + MVG_STATUS_LEN, MVG_LEN_LEN, enclen);

    int rc = 0;
    if (write_full(fd, hdr, sizeof hdr) != 1) rc = -1;
    else if (enclen && write_full(fd, enc, (size_t)enclen) != 1) rc = -1;
    free(enc);
    return rc;
}

static int send_err(int fd, int status, const char *msg) {
    return send_rsp(fd, status, msg, (long)strlen(msg));
}

/* Is the session that asked for this child still there?  kill(pid, 0) tests for
   the process without touching it; EPERM counts as alive, since a process we may
   not signal still exists.  A pid of 0 means the caller did not tell us (an
   older client, or a platform where it cannot know), and then the answer is
   always "alive" — the timers alone govern, exactly as before. */
static int session_alive(long pid) {
    if (pid <= 0) return 1;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return errno == EPERM;
}

static void free_args(args *g) {
    for (int i = 0; i < g->n; i++) free(g->a[i]);
    free(g->a); free(g->l);
    g->a = NULL; g->l = NULL; g->n = 0;
}

/* Serve one session until BYE, the client vanishing, or the idle bound.
 *
 * `cwd` is the session's own working directory, handed over at connect.  The
 * child moves there before serving anything, because the engine's model is that
 * the current directory IS the account and the repository is `.git` inside it —
 * exactly how mvx-git and udt-git run.  Without this the daemon would serve from
 * wherever it happened to be started first, which is nobody's account.
 *
 * `spid` is the session process itself, and it is how "exit when the user logs
 * out" actually works.  There is no other signal available: the pipes are held
 * O_RDWR so that a client closing between commands does not end the session,
 * and that same choice means a client's close delivers no EOF.  So the child
 * watches the process instead — when it is gone, wrap up at once rather than
 * sitting out the idle timer. */
static void serve_session(const char *sid, const char *cwd, long spid) {
    char reqp[MVG_PATH_MAX], rspp[MVG_PATH_MAX], leaf[256];

    snprintf(leaf, sizeof leaf, "s%s.req", sid); pathcat(reqp, sizeof reqp, leaf);
    snprintf(leaf, sizeof leaf, "s%s.rsp", sid); pathcat(rspp, sizeof rspp, leaf);

    if (cwd && *cwd && chdir(cwd) != 0) {
        fprintf(stderr, "mvgitd: session %s: cannot enter %s: %s\n",
                sid, cwd, strerror(errno));
        return;
    }

    /* O_RDWR on both ends rather than O_RDONLY/O_WRONLY: it does not block
       waiting for a peer, and it keeps a writer on the request pipe so a client
       that closes between commands does not deliver EOF and end the session
       early.  Framing, not EOF, delimits messages here. */
    int req = open(reqp, O_RDWR);
    if (req < 0) return;
    int rsp = open(rspp, O_RDWR);
    if (rsp < 0) { close(req); return; }

    time_t idle_deadline = time(NULL) + SESSION_IDLE_SECS;

    for (;;) {
        /* Wait in short hops rather than one long sleep, so a logout is noticed
           within LIVENESS_POLL_SECS instead of whenever the idle timer happens
           to expire.  The deadline is absolute, so hopping does not extend it. */
        struct pollfd pfd = { .fd = req, .events = POLLIN };
        int pr = poll(&pfd, 1, LIVENESS_POLL_SECS * 1000);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) {
            if (!session_alive(spid)) break;       /* the user has gone */
            if (time(NULL) >= idle_deadline) break;/* alive but silent: backstop */
            continue;
        }
        idle_deadline = time(NULL) + SESSION_IDLE_SECS;

        char hdr[MVG_REQ_HDR];
        int rr = read_full(req, hdr, sizeof hdr);
        if (rr <= 0) break;

        if (memcmp(hdr + MVG_OFF_MAGIC, MVG_MAGIC, MVG_MAGIC_LEN) != 0) {
            /* No resynchronisation point exists in a length-framed stream, so a
               bad magic means the two ends have lost each other: say so and end
               the session rather than read further garbage. */
            send_err(rsp, MVG_EPROTO, "bad frame magic — stream desynchronised");
            break;
        }
        if (memcmp(hdr + MVG_OFF_TOKEN, g_token, MVG_TOKEN_LEN) != 0) {
            send_err(rsp, MVG_EAUTH, "token mismatch");
            break;
        }

        char opname[MVG_OPCODE_LEN + 1];
        memcpy(opname, hdr + MVG_OFF_OPCODE, MVG_OPCODE_LEN);
        opname[MVG_OPCODE_LEN] = '\0';
        for (int i = MVG_OPCODE_LEN - 1; i >= 0 && opname[i] == ' '; i--)
            opname[i] = '\0';

        long nargs   = get_num(hdr + MVG_OFF_NARGS,   MVG_NARGS_LEN);
        long bodylen = get_num(hdr + MVG_OFF_BODYLEN, MVG_LEN_LEN);
        if (nargs < 0 || bodylen < 0 || bodylen > MVG_MAX_BODY) {
            send_err(rsp, MVG_EPROTO, "malformed frame header");
            break;
        }

        /* Read the whole body before dispatching, so a rejected request still
           leaves the stream positioned at the next frame. */
        char *body = NULL;
        if (bodylen) {
            body = malloc((size_t)bodylen);
            if (!body) { send_err(rsp, MVG_EPROTO, "out of memory"); break; }
            if (read_full(req, body, (size_t)bodylen) <= 0) { free(body); break; }
        }

        if (strcmp(opname, "BYE") == 0)      { free(body); send_rsp(rsp, MVG_OK, NULL, 0); break; }
        if (strcmp(opname, "SHUTDOWN") == 0) { free(body); send_rsp(rsp, MVG_OK, NULL, 0);
                                               kill(getppid(), SIGTERM); break; }

        /* Split the body into its length-prefixed arguments. */
        args g = { NULL, NULL, 0 };
        int malformed = 0;
        if (nargs > 0) {
            g.a = calloc((size_t)nargs, sizeof *g.a);
            g.l = calloc((size_t)nargs, sizeof *g.l);
            if (!g.a || !g.l) { free(body); free(g.a); free(g.l);
                                send_err(rsp, MVG_EPROTO, "out of memory"); break; }
            long off = 0;
            for (long i = 0; i < nargs; i++) {
                if (off + MVG_LEN_LEN > bodylen) { malformed = 1; break; }
                long al = get_num(body + off, MVG_LEN_LEN);   /* encoded length */
                off += MVG_LEN_LEN;
                if (al < 0 || off + al > bodylen) { malformed = 1; break; }
                /* Decode here, so everything above the framing works in raw
                   bytes and the encoding stays confined to the wire. */
                g.a[i] = hex_decode(body + off, al, &g.l[i]);
                if (!g.a[i]) { malformed = 1; break; }
                g.n = (int)(i + 1);
                off += al;
            }
        }
        free(body);
        if (malformed) {
            free_args(&g);
            send_err(rsp, MVG_EPROTO, "malformed argument framing");
            break;
        }

        int found = 0;
        for (int i = 0; OPS[i].name; i++) {
            if (strcmp(opname, OPS[i].name) != 0) continue;
            found = 1;
            if (g.n != OPS[i].nargs) {
                char m[128];
                snprintf(m, sizeof m, "%s takes %d arguments, got %d",
                         OPS[i].name, OPS[i].nargs, g.n);
                send_err(rsp, MVG_EARGS, m);
                break;
            }
            opres r = OPS[i].fn(&g);
            send_rsp(rsp, r.status, r.out ? r.out : "", r.out ? r.len : 0);
            free(r.out);
            break;
        }
        if (!found) {
            char m[128];
            snprintf(m, sizeof m, "unknown opcode '%s'", opname);
            send_err(rsp, MVG_EOPCODE, m);
        }
        free_args(&g);
    }

    /* A batch left open by a session that died must not leak into the next. */
    mv_git_batch_end();
    close(req); close(rsp);
    unlink(reqp); unlink(rspp);
}

/* --- the server loop ------------------------------------------------------ */

static volatile sig_atomic_t g_stop = 0;
static void on_term(int s) { (void)s; g_stop = 1; }

static void server_loop(int lockfd) {
    char ctlp[MVG_PATH_MAX], pidp[MVG_PATH_MAX];
    pathcat(ctlp, sizeof ctlp, "control");
    pathcat(pidp, sizeof pidp, "pid");

    unlink(ctlp);
    if (mkfifo(ctlp, 0600) != 0) die(ctlp);

    /* Record the pid in the file whose lock we hold, so --status/--stop can find
       us and a second server can tell we are alive. */
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%ld\n", (long)getpid());
    if (ftruncate(lockfd, 0) != 0) { /* non-fatal */ }
    if (lseek(lockfd, 0, SEEK_SET) == 0)
        { if (write(lockfd, buf, (size_t)n) != n) { /* non-fatal */ } }

    signal(SIGTERM, on_term);
    signal(SIGINT,  on_term);
    signal(SIGPIPE, SIG_IGN);
    /* SIG_DFL, not SIG_IGN: the default disposition still lets a finished child
       become a zombie, which is what lets us reap it and keep an accurate count.
       SIG_IGN would discard them silently and leave us unable to tell whether
       anyone is still working. */
    signal(SIGCHLD, SIG_DFL);

    /* O_RDWR keeps a writer on the control FIFO at all times, so poll() blocks
       rather than spinning on EOF between clients. */
    int ctl = open(ctlp, O_RDWR);
    if (ctl < 0) die(ctlp);

    time_t last = time(NULL);
    char line[MVG_PATH_MAX + 64];       /* HELLO + session key + account path */
    size_t used = 0;
    int children = 0;

    while (!g_stop) {
        struct pollfd pfd = { .fd = ctl, .events = POLLIN };
        int pr = poll(&pfd, 1, 10 * 1000);
        if (pr < 0) { if (errno == EINTR) continue; break; }

        /* Reap anything that has finished, so `children` is a true count. */
        while (children > 0 && waitpid(-1, NULL, WNOHANG) > 0) children--;

        if (pr == 0) {
            /* Idle out only when nobody is working.  Leaving while a child is
               mid-operation would drop the lock and unlink the control FIFO, and
               the next session would then start a SECOND server — so "one
               process per user" would quietly stop holding, and two children
               could run concurrent index batches in the same account. */
            if (children == 0 && time(NULL) - last >= IDLE_EXIT_SECS) break;
            continue;
        }

        char c;
        ssize_t r = read(ctl, &c, 1);
        if (r <= 0) continue;
        last = time(NULL);

        if (c != '\n') {
            if (used < sizeof line - 1) line[used++] = c;
            continue;
        }
        line[used] = '\0';
        used = 0;

        /* The only control verb: HELLO <sid> <pid> <cwd>, whose pipes --connect
           has already created.  `pid` is the session process, watched so the
           child can leave when the user logs out; `cwd` is the account
           directory, which the child enters before serving.  The path is taken
           as the whole remainder, so a directory containing spaces survives.
           A child owns the session from here. */
        if (strncmp(line, "HELLO ", 6) != 0) continue;
        char *sid = line + 6;
        if (!*sid) continue;
        char *rest = strchr(sid, ' ');
        long spid = 0;
        char *cwd = NULL;
        if (rest) {
            *rest++ = '\0';
            spid = strtol(rest, NULL, 10);
            cwd = strchr(rest, ' ');
            if (cwd) cwd++;
        }

        pid_t pid = fork();
        if (pid == 0) {
            /* Drop BOTH inherited descriptors.  The lock especially: flock is
               held per open-file-description, so a child that outlived its
               parent would keep the pid file locked, server_running() would
               report a server that no longer exists, and the next session would
               write HELLO into a control FIFO with no reader and block. */
            close(ctl);
            close(lockfd);
            serve_session(sid, cwd, spid);
            _exit(0);
        }
        if (pid > 0) children++;
    }

    unlink(ctlp);
    close(ctl);
}

/* --- lazy start ----------------------------------------------------------- */

/* Is a server running?  The lock on the pid file is the liveness test, not the
   file's existence: a killed server leaves the file (and its FIFOs) behind, and
   a stale FIFO is indistinguishable from a live one until you try to use it. */
static int server_running(void) {
    char pidp[MVG_PATH_MAX];
    pathcat(pidp, sizeof pidp, "pid");
    int fd = open(pidp, O_RDWR | O_CREAT, 0600);
    if (fd < 0) die(pidp);
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {   /* we got it: nobody is serving */
        flock(fd, LOCK_UN);
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

/* Start a server and wait for it to be ready.  Two sessions may race here; the
   loser fails the lock inside the child and exits quietly, which is why the
   readiness test is "the control FIFO exists and someone holds the lock" rather
   than "my child is alive". */
static void ensure_server(void) {
    if (server_running()) return;

    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        /* Detach: new session, no controlling terminal, no inherited stdio —
           the shell call that started us must return immediately. */
        setsid();
        char logp[MVG_PATH_MAX];
        pathcat(logp, sizeof logp, "log");
        int devnull = open("/dev/null", O_RDONLY);
        int log = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
        if (log >= 0) { dup2(log, 1); dup2(log, 2); close(log); }

        char pidp[MVG_PATH_MAX];
        pathcat(pidp, sizeof pidp, "pid");
        int lockfd = open(pidp, O_RDWR | O_CREAT, 0600);
        if (lockfd < 0) _exit(1);
        if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) _exit(0);  /* lost the race */

        server_loop(lockfd);
        flock(lockfd, LOCK_UN);
        close(lockfd);
        _exit(0);
    }

    /* Wait for readiness, bounded — never block a session indefinitely on a
       server that failed to come up. */
    char ctlp[MVG_PATH_MAX];
    pathcat(ctlp, sizeof ctlp, "control");
    for (int i = 0; i < 100; i++) {         /* ~5s */
        struct stat st;
        if (stat(ctlp, &st) == 0 && server_running()) return;
        /* nanosleep, not usleep: POSIX 2008 removed the latter, so under
           _POSIX_C_SOURCE=200809L it is not even declared. */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50L * 1000L * 1000L };
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "mvgitd: the background process did not start; see %s/log\n",
            g_dir);
    exit(1);
}

/* --connect <sid>: ensure a server, create this session's pipes, announce it.
   The pipes are made HERE rather than by the server so that when this command
   returns, the session can open them immediately with no second rendezvous. */
static int do_connect(const char *sid, long spid) {
    ensure_token();
    ensure_server();

    char reqp[MVG_PATH_MAX], rspp[MVG_PATH_MAX], ctlp[MVG_PATH_MAX], leaf[256];

    /* The pipe pair is named for this CONNECTION, not just the session: the
       session id plus our own pid.  A session that reconnects (a second RUN, a
       new account) therefore gets fresh pipes instead of colliding with a child
       still holding the old pair open — and since the caller learns the paths
       from our output, nothing needs to guess the name.  The old child idles
       out and removes its own pipes. */
    char key[128];
    snprintf(key, sizeof key, "%s-%ld", sid, (long)getpid());

    snprintf(leaf, sizeof leaf, "s%s.req", key); pathcat(reqp, sizeof reqp, leaf);
    snprintf(leaf, sizeof leaf, "s%s.rsp", key); pathcat(rspp, sizeof rspp, leaf);
    pathcat(ctlp, sizeof ctlp, "control");

    unlink(reqp); unlink(rspp);
    if (mkfifo(reqp, 0600) != 0) die(reqp);
    if (mkfifo(rspp, 0600) != 0) die(rspp);

    /* Hand over our own working directory with the greeting.  This command runs
       inside the session's account (the GIT verb shells out from there), so our
       cwd IS the account — which is what the engine expects to be sitting in.
       Discovering it here means the BASIC side never has to know or send it. */
    char cwd[MVG_PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';

    int ctl = open(ctlp, O_WRONLY);
    if (ctl < 0) die(ctlp);
    char msg[MVG_PATH_MAX + 64];
    int n = snprintf(msg, sizeof msg, "HELLO %s %ld %s\n", key, spid, cwd);
    if (write_full(ctl, msg, (size_t)n) != 1) die(ctlp);
    close(ctl);

    /* The session needs both paths and the token; print them so a caller that
       can capture output need not know the directory layout. */
    printf("%s\n%s\n%s\n%s\n", g_dir, reqp, rspp, g_token);
    return 0;
}

static int do_stop(void) {
    char pidp[MVG_PATH_MAX];
    pathcat(pidp, sizeof pidp, "pid");
    FILE *f = fopen(pidp, "r");
    if (!f) { fprintf(stderr, "mvgitd: not running\n"); return 1; }
    long pid = 0;
    if (fscanf(f, "%ld", &pid) != 1) pid = 0;
    fclose(f);
    if (pid <= 0) { fprintf(stderr, "mvgitd: not running\n"); return 1; }
    if (kill((pid_t)pid, SIGTERM) != 0) { fprintf(stderr, "mvgitd: not running\n"); return 1; }
    printf("stopped %ld\n", pid);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "usage: mvgitd --connect <session-id> [session-pid] | --serve | --stop | --status\n"
        "\n"
        "The per-user background process that runs libgit2 for a BASIC session.\n"
        "Started lazily by the GIT verb; exits by itself once idle.\n"
        "Run state lives in $MVGIT_DIR, else $HOME/.mvgit (0700).\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    resolve_dir();

    if (strcmp(argv[1], "--connect") == 0) {
        if (argc < 3) { usage(); return 2; }
        /* The optional third argument is the SESSION's pid — the process the
           child should watch, so it can leave when the user logs out.  The
           caller supplies it because we cannot infer it reliably: this command
           is run by a shell the session forked, so our own parent is that
           shell, not the session.  Omitted, the timers alone govern. */
        long spid = (argc > 3) ? strtol(argv[3], NULL, 10) : 0;
        return do_connect(argv[2], spid);
    }
    if (strcmp(argv[1], "--serve") == 0) {
        ensure_token();
        char pidp[MVG_PATH_MAX];
        pathcat(pidp, sizeof pidp, "pid");
        int lockfd = open(pidp, O_RDWR | O_CREAT, 0600);
        if (lockfd < 0) die(pidp);
        if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
            fprintf(stderr, "mvgitd: already running\n");
            return 1;
        }
        server_loop(lockfd);
        return 0;
    }
    if (strcmp(argv[1], "--stop") == 0) return do_stop();
    if (strcmp(argv[1], "--status") == 0) {
        int up = server_running();
        printf("%s\n", up ? "running" : "stopped");
        return up ? 0 : 1;
    }
    usage();
    return 2;
}
