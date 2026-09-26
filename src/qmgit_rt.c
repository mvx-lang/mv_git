/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* qmgit_rt.c — the OpenQM / ScarletDME record backend (mv_git#243).
 *
 * See qmgit_rt.h for why this is shaped like jbasegit_rt.c and not like
 * mvsession.c, and for the NUL limitation mv_write enforces.
 *
 * Everything here goes through QMClient (qmclilib).  That API is supported,
 * ships with stock ScarletDME, and needs none of the CCALL fixes the
 * in-session verb will want — so the CLI is buildable today.
 */
#include "qmgit_rt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <limits.h>
#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif
#include <unistd.h>

#include "qmclilib.h"

/* --- context ----------------------------------------------------------- */

#define MAXOPEN 64

struct open_file {
    char name[256];   /* the spec as the engine asked for it, e.g. "DICT BP" */
    int  fno;
};

struct mv_ctx {
    int connected;
    struct open_file files[MAXOPEN];
    int  nfiles;
    int  sel_active;
};

static char g_account[256];

void mv_qm_set_account(const char *name) {
    if (!name) { g_account[0] = '\0'; return; }
    snprintf(g_account, sizeof g_account, "%s", name);
}

const char *mv_qm_account(void) { return g_account; }

/* QMConnectLocal needs an account name: outside a session there is no current
   account to inherit.  Connecting is deferred to the first record op so that
   `qm-git --version` and the forwarded git commands cost nothing. */
/* THE CONNECTION IS PER PROCESS, NOT PER CONTEXT.  QMConnectLocal() has no
   handle: it connects the process.  Keeping the flag on mv_ctx meant the
   file-name cache below — which the engine reaches through is_mv_file(), with
   no context to hand — could run before anything had connected, find nothing,
   and cache that.  Every QM file then looked like an ordinary directory. */
static int g_connected;

static int qm_connect(void) {
    if (g_connected) return 1;
    if (!g_account[0])
        mv_fatal("no QM account: pass -a <account> or set $QMACCOUNT");
    if (!QMConnectLocal(g_account)) {
        char *e = QMError();
        mv_fatal("cannot connect to QM account %s: %s",
                 g_account, (e && *e) ? e : "QMConnectLocal failed");
    }
    g_connected = 1;
    return 1;
}

static int ensure_connected(mv_ctx *ctx) {
    qm_connect();
    ctx->connected = 1;
    return 1;
}

mv_ctx *mv_ctx_create(void) {
    mv_ctx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) mv_fatal("out of memory");
    return ctx;   /* the connection opens lazily on first record op */
}

void mv_ctx_destroy(mv_ctx *ctx) {
    if (!ctx) return;
    if (ctx->connected) {
        for (int i = 0; i < ctx->nfiles; i++) QMClose(ctx->files[i].fno);
        QMDisconnect();
        g_connected = 0;
    }
    free(ctx);
}

/* --- value ops --------------------------------------------------------- */

void mv_init(mv_value *v) {
    if (!v) return;
    v->data = NULL; v->len = 0; v->fno = -1; v->is_file = 0;
}

void mv_clear(mv_value *v) {
    if (!v) return;
    free(v->data);
    mv_init(v);
}

void mv_set_str(mv_value *v, const char *p, int64_t len) {
    if (!v) return;
    char *n = malloc((size_t)len + 1);
    if (!n) mv_fatal("out of memory");
    if (len > 0 && p) memcpy(n, p, (size_t)len);
    n[len] = '\0';
    free(v->data);
    v->data = n;
    v->len = len;
    v->is_file = 0;
    v->fno = -1;
}

int64_t mv_val_chars(const mv_value *v, char *numbuf, size_t cap,
                     const char **pp) {
    (void)numbuf; (void)cap;
    if (!v || !v->data) { if (pp) *pp = ""; return 0; }
    if (pp) *pp = v->data;
    return v->len;
}

/* --- record I/O -------------------------------------------------------- */

/* The engine asks for a file by name, repeatedly.  QMOpen costs a round trip
   and a server-side file table slot, so opens are cached per context and the
   same spec always returns the same file number. */
static int open_cached(mv_ctx *ctx, const char *spec) {
    for (int i = 0; i < ctx->nfiles; i++)
        if (strcmp(ctx->files[i].name, spec) == 0) return ctx->files[i].fno;
    int fno = QMOpen((char *)spec);
    if (fno < 0) return -1;
    if (ctx->nfiles < MAXOPEN) {
        snprintf(ctx->files[ctx->nfiles].name,
                 sizeof ctx->files[0].name, "%s", spec);
        ctx->files[ctx->nfiles].fno = fno;
        ctx->nfiles++;
    }
    return fno;
}

int64_t mv_open(mv_ctx *ctx, const mv_value *dict, const mv_value *spec,
                mv_value *fvar) {
    if (!ctx || !spec || !spec->data || !spec->data[0] || !fvar) return 0;
    ensure_connected(ctx);
    char sp[600];
    int want_dict = dict && dict->data && dict->data[0];
    if (want_dict) snprintf(sp, sizeof sp, "DICT %s", spec->data);
    else           snprintf(sp, sizeof sp, "%s", spec->data);

    int fno = open_cached(ctx, sp);
    if (fno < 0) return 0;

    mv_clear(fvar);
    fvar->fno = fno;
    fvar->is_file = 1;
    /* Carry the spec as the value's bytes too: the engine prints a file
       variable in diagnostics, and "DICT BP" is more use than a number. */
    size_t spn = strlen(sp);
    fvar->data = malloc(spn + 1);
    if (!fvar->data) mv_fatal("out of memory");
    memcpy(fvar->data, sp, spn + 1);
    fvar->len = (int64_t)spn;
    return 1;
}

/* ABSENCE IS REPORTED IN `err`, NOT BY A NULL RETURN.  qmclilib's read_record
   always hands back a buffer: on SV_ELSE — no such record — it is simply
   empty, and the status goes in the out-parameter.  Testing the pointer
   therefore said "found" for every record that was not there, so a record
   deleted from the account read back as an empty one and `status` never
   reported a deletion.  Only SV_OK means the record exists; a genuinely empty
   record also comes back SV_OK with zero bytes, so the two stay distinct.

   Same shape as the QMReadNext trap above: this API signals absence
   out-of-band, and a NULL check is not it. */
int64_t mv_read(mv_ctx *ctx, mv_value *rec, const mv_value *fvar,
                const mv_value *id, int64_t lock) {
    if (!ctx || !rec || !fvar || !fvar->is_file || !id || !id->data) return 0;
    ensure_connected(ctx);
    int err = SV_OK;
    char *p = lock ? QMReadu(fvar->fno, id->data, 0, &err)
                   : QMRead(fvar->fno, id->data, &err);
    if (p == NULL || err != SV_OK) {
        if (p) QMFree(p);
        mv_set_str(rec, "", 0);
        return 0;
    }
    mv_set_str(rec, p, (int64_t)strlen(p));
    QMFree(p);
    return 1;
}

/* A record with an embedded NUL cannot survive QMClient: qmclilib takes
   strlen(data) even though its wire packet is length-framed.  Storing the
   truncation would corrupt the repository silently and surface much later as a
   wrong diff, so refuse it here, where the cause is still visible. */
static void refuse_binary(const mv_value *rec, const mv_value *id) {
    if (!rec || !rec->data) return;
    if ((int64_t)strlen(rec->data) == rec->len) return;
    mv_fatal("record '%s' contains a NUL at offset %lld of %lld: QMClient "
             "cannot carry it (QMWrite takes no length), and writing it would "
             "truncate the record. See qmgit_rt.h.",
             (id && id->data) ? id->data : "?",
             (long long)strlen(rec->data), (long long)rec->len);
}

int64_t mv_write(mv_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t keep_lock, int64_t onerr) {
    (void)onerr;
    if (!ctx || !rec || !fvar || !fvar->is_file || !id || !id->data) return 0;
    ensure_connected(ctx);
    refuse_binary(rec, id);
    const char *data = rec->data ? rec->data : "";
    if (keep_lock) QMWriteu(fvar->fno, id->data, (char *)data);
    else           QMWrite(fvar->fno, id->data, (char *)data);
    return QMStatus() == SV_OK;
}

int64_t mv_delete_rec(mv_ctx *ctx, const mv_value *fvar, const mv_value *id) {
    if (!ctx || !fvar || !fvar->is_file || !id || !id->data) return 0;
    ensure_connected(ctx);
    QMDelete(fvar->fno, id->data);
    return QMStatus() == SV_OK;
}

/* One select list is enough: the engine drains a select fully before starting
   another.  List 0 is the default list, the one QMReadNext reads without
   being told. */
#define SEL_LIST 0
/* Our own scans (the file-name cache, mv_filelist) run on a DIFFERENT list.
   The engine calls backend_has_file() and is_mv_file() from inside loops that
   are already draining list 0 with mv_readnext, so priming a cache on list 0
   would silently truncate the caller's select. */
#define SEL_INTERNAL 9

void mv_select(mv_ctx *ctx, const mv_value *fvar) {
    if (!ctx || !fvar || !fvar->is_file) return;
    ensure_connected(ctx);
    if (ctx->sel_active) QMClearSelect(SEL_LIST);
    QMSelect(fvar->fno, SEL_LIST);
    ctx->sel_active = 1;
}

/* ONLY NULL ENDS THE LIST.  QMReadNext returns an EMPTY STRING for entries it
   cannot hand back, scattered through the list rather than only at the end, and
   returns NULL when the list is genuinely exhausted (qmclilib.c: the default
   branch of its server_error switch).  Treating the first empty as the end
   truncated every select silently — a select of QMSYS's NEWVOC stopped at 166
   of 478 records, so the stock baseline it builds was missing BASIC and a
   checkout deleted the verb that compiles programs (mv_git#243). */
int64_t mv_readnext(mv_ctx *ctx, mv_value *id) {
    if (!ctx || !id) return 0;
    ensure_connected(ctx);
    for (;;) {
        char *p = QMReadNext(SEL_LIST);
        if (p == NULL) {
            ctx->sel_active = 0;
            mv_set_str(id, "", 0);
            return 0;
        }
        if (p[0] == '\0') { QMFree(p); continue; }   /* skipped, not the end */
        mv_set_str(id, p, (int64_t)strlen(p));
        QMFree(p);
        return 1;
    }
}

/* --- files ------------------------------------------------------------- */

/* Run a TCL sentence and discard its output.  QM reports most file operations
   in what they print rather than in a status, so callers check the world
   afterwards rather than trusting this. */
static void qm_sentence(mv_ctx *ctx, const char *sentence) {
    ensure_connected(ctx);
    int err = 0;
    char *out = QMExecute((char *)sentence, &err);
    if (out) QMFree(out);
}

/* A QM dynamic file is a DIRECTORY on disk holding group files: ~0, ~1, …
   A QM directory file is a directory of plain records.  So the two are not
   told apart by stat() — only by whether group 0 is inside.
   ScarletDME#99 proposes changing that prefix from '~' to '%', so accept
   either rather than break on the day it lands. */
static int is_hash_dir(const char *path) {
    char p[PATH_MAX];
    struct stat st;
    if (strlen(path) + 4 >= sizeof p) return 0;
    snprintf(p, sizeof p, "%s/~0", path);
    if (stat(p, &st) == 0) return 1;
    snprintf(p, sizeof p, "%s/%%0", path);
    if (stat(p, &st) == 0) return 1;
    return 0;
}

/* IS THIS THE ACCOUNT'S OWN FILE, OR SOMEBODY ELSE'S?
 *
 * A fresh QM account's VOC has ten "F" entries and only one of them is a file
 * the account owns.  The rest are pointers:
 *
 *     $ACC     F <AM> .                    the account directory itself
 *     SYSCOM   F <AM> @QMSYS/SYSCOM        a file in another account
 *     ERRMSG   F <AM> @QMSYS/ERRMSG        likewise
 *     VOC      F <AM> VOC                  the account's own
 *
 * QM writes all of them as type F rather than Q, so the type does not separate
 * them — the PATH does.  Handing the lot to the engine made it treat QMSYS's
 * files as account content: in open-account mode it staged a %FILE% control
 * for each, and every later status reported them deleted, which buried the
 * real results under `D $ACC.DICT/%FILE%` (mv_git#243).
 *
 * So: a relative path that is not "." is ours; "@..." and absolute paths are
 * not. */
static int path_is_local(const char *p) {
    if (!p || !*p) return 0;
    if (p[0] == '@') return 0;                    /* @ACCOUNT/FILE pointer */
    if (p[0] == '/') return 0;                    /* elsewhere on disk */
    if (strcmp(p, ".") == 0) return 0;            /* the account itself */
    return 1;
}

/* The VOC entry for a file is "F" <AM> data-path <AM> dict-path, with the
   paths relative to the account directory. */
static int voc_file_path(mv_ctx *ctx, const char *name, char *out, size_t cap) {
    int fno = open_cached(ctx, "VOC");
    if (fno < 0) return 0;
    int err = 0;
    char *rec = QMRead(fno, (char *)name, &err);
    if (!rec) return 0;
    int ok = 0;
    if (rec[0] == 'F' || rec[0] == 'f') {
        const char *a2 = strchr(rec, (char)0xFE);
        if (a2) {
            a2++;
            const char *end = strchr(a2, (char)0xFE);
            size_t n = end ? (size_t)(end - a2) : strlen(a2);
            if (n && n < cap) { memcpy(out, a2, n); out[n] = '\0'; ok = 1; }
        }
    }
    QMFree(rec);
    return ok;
}

/* Pull one `Label : value' out of a verb's display output.  ANALYZE.FILE
   paints a screen -- there are escape sequences in there -- so match on the
   label and read the digits after the colon rather than trying to parse by
   position. */
static int64_t qm_report_number(const char *text, const char *label) {
    const char *p = text ? strstr(text, label) : NULL;
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return -1;
    return (int64_t)strtoll(p, NULL, 10);
}

static int64_t qm_analyze_modulus(mv_ctx *ctx, const char *name) {
    ensure_connected(ctx);
    char sentence[600];
    snprintf(sentence, sizeof sentence, "ANALYZE.FILE %s", name);
    int err = 0;
    char *out = QMExecute(sentence, &err);
    int64_t mod = qm_report_number(out, "Current modulus");
    if (out) QMFree(out);
    return mod;
}

int64_t mv_fileclass(mv_ctx *ctx, const char *name, char *out, size_t cap) {
    if (cap) out[0] = '\0';
    if (!ctx || !name || !*name) return 0;
    ensure_connected(ctx);
    char path[1024];
    if (voc_file_path(ctx, name, path, sizeof path)) {
        if (!path_is_local(path)) return 0;        /* a pointer, not our file */
    } else {
        snprintf(path, sizeof path, "%s", name);   /* not in VOC: try the name */
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    if (is_hash_dir(path)) {
        /* THE REAL MODULUS, so a clone recreates the file at its true size
           rather than at the default and then splits its way back up.
           dh_modulus() would give it directly, but QMClient does not export
           the dh_ layer -- that is the in-session verb's route, and it waits
           on ScarletDME#109.  ANALYZE.FILE reports it, so ask the account:

               Type              : Dynamic, version 2
               Minimum modulus   : 1
               Current modulus   : 17

           CURRENT, not minimum, and for the same reason UniData reads
           FINFO_MODULUS rather than the creation parameter: a file that has
           split is that size now, and recreating it at the size it was first
           made would make the clone do the splitting again. */
        int64_t mod = qm_analyze_modulus(ctx, name);
        if (mod > 0) snprintf(out, cap, "hash %lld DYNAMIC", (long long)mod);
        else         snprintf(out, cap, "hash 1 DYNAMIC");
    } else {
        snprintf(out, cap, "DIR");
    }
    return 1;
}

/* The account's alternate keys for `name', as @AM-separated FIELD NAMES --
   the form %INDEXES% carries and that mvx-git's rebuild reads back.
   On QM an index is created against a dictionary item and takes its name, so
   the index name IS the field name.

   LIST.INDEX prints them, and `ALL' is what makes it list rather than stop and
   ask for one:

       Number of indices = 2

       Index name...... En Type Nulls S/M Fmt Field/Expression
       NAME              Y  D    Yes   S   L  1

       TOWN              Y  D    Yes   S   L  2

   so: skip to the column header, then take the first token of every non-blank
   line after it. */
int64_t mv_indices(mv_ctx *ctx, const char *name, char *out, size_t cap) {
    if (cap) out[0] = '\0';
    if (!ctx || !name || !*name || cap == 0) return 0;
    ensure_connected(ctx);
    char sentence[600];
    snprintf(sentence, sizeof sentence, "LIST.INDEX %s ALL", name);
    int err = 0;
    char *text = QMExecute(sentence, &err);
    if (!text) return 0;

    /* QMExecute hands the display back as ONE string with the lines separated
       by an attribute mark, not by newlines -- so split on the mark (and on a
       newline too, rather than depend on which). */
    const char *p = strstr(text, "Index name");
    size_t n = 0;
    while (p && *p) {
        while (*p && (unsigned char)*p != 0xFE && *p != '\n') p++;
        if (!*p) break;
        p++;                                  /* past the separator */
        const char *e = p;
        while (*e && (unsigned char)*e != 0xFE && *e != '\n') e++;
        size_t len = (size_t)(e - p);
        size_t t = 0;
        while (t < len && p[t] != ' ' && p[t] != '\t' && p[t] != '\r') t++;
        if (t > 0) {                          /* a line starting in column 1 */
            if (n + t + 1 >= cap) break;
            if (n) out[n++] = (char)0xFE;
            memcpy(out + n, p, t);
            n += t;
        }
        p = e;
    }
    out[n] = '\0';
    QMFree(text);
    return n > 0;
}

int64_t mv_createfile(mv_ctx *ctx, const mv_value *spec, const mv_value *type) {
    if (!ctx || !spec || !spec->data || !spec->data[0]) return 0;
    ensure_connected(ctx);
    const char *t = (type && type->data) ? type->data : "";
    int isdir = (strcasecmp(t, "DIR") == 0);
    char sentence[700];
    snprintf(sentence, sizeof sentence, "CREATE-FILE %s%s",
             spec->data, isdir ? " DIRECTORY" : "");
    qm_sentence(ctx, sentence);
    /* Believe the account, not the sentence's output. */
    char cls[64];
    return mv_fileclass(ctx, spec->data, cls, sizeof cls);
}

int64_t mv_deletefile(mv_ctx *ctx, const mv_value *spec) {
    if (!ctx || !spec || !spec->data || !spec->data[0]) return 0;
    ensure_connected(ctx);
    char sentence[700];
    snprintf(sentence, sizeof sentence, "DELETE-FILE %s", spec->data);
    qm_sentence(ctx, sentence);
    char cls[64];
    return mv_fileclass(ctx, spec->data, cls, sizeof cls) == 0;  /* gone */
}

/* The account's files as "name<VM>type" rows, @AM-separated — read out of the
   VOC rather than the directory, because the VOC is what the account actually
   considers a file. */
void mv_filelist(mv_ctx *ctx, mv_value *dst) {
    mv_set_str(dst, "", 0);
    if (!ctx) return;
    ensure_connected(ctx);
    int fno = open_cached(ctx, "VOC");
    if (fno < 0) return;

    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    if (!buf) mv_fatal("out of memory");

    QMSelect(fno, SEL_INTERNAL);
    for (;;) {
        char *id = QMReadNext(SEL_INTERNAL);
        if (!id) break;                       /* NULL alone ends the list */
        if (!id[0]) { QMFree(id); continue; } /* an entry QM could not return */
        int err = 0;
        char *rec = QMRead(fno, id, &err);
        int local = 0;
        if (rec && (rec[0] == 'F' || rec[0] == 'f')
                && (rec[1] == '\0' || rec[1] == (char)0xFE)) {
            const char *a2 = strchr(rec, (char)0xFE);
            if (a2) {
                char pbuf[1024];
                a2++;
                const char *e2 = strchr(a2, (char)0xFE);
                size_t pn = e2 ? (size_t)(e2 - a2) : strlen(a2);
                if (pn && pn < sizeof pbuf) {
                    memcpy(pbuf, a2, pn); pbuf[pn] = '\0';
                    local = path_is_local(pbuf);
                }
            }
        }
        if (local) {
            char cls[64];
            if (mv_fileclass(ctx, id, cls, sizeof cls)) {
                const char *type = (strcmp(cls, "DIR") == 0) ? "DIR" : "hash";
                size_t need = strlen(id) + strlen(type) + 3;
                if (n + need >= cap) {
                    cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) mv_fatal("out of memory");
                    buf = nb;
                }
                if (n) buf[n++] = (char)0xFE;
                memcpy(buf + n, id, strlen(id)); n += strlen(id);
                buf[n++] = (char)0xFD;
                memcpy(buf + n, type, strlen(type)); n += strlen(type);
            }
        }
        if (rec) QMFree(rec);
        QMFree(id);
    }
    ctx->sel_active = 0;
    mv_set_str(dst, buf, (int64_t)n);
    if (getenv("MVGIT_QM_DEBUG")) {
        char cwd[PATH_MAX];
        fprintf(stderr, "[qm] mv_filelist acct=%s cwd=%s -> %lld bytes: ",
                g_account, getcwd(cwd, sizeof cwd) ? cwd : "?", (long long)n);
        for (size_t k = 0; k < n; k++)
            fputc(((unsigned char)buf[k] == 0xFE) ? ' ' :
                  ((unsigned char)buf[k] == 0xFD) ? ':' : buf[k], stderr);
        fputc('\n', stderr);
    }
    free(buf);
}

/* --- account / misc ----------------------------------------------------- */

int mv_openaccount(void) {
    const char *e = getenv("MVX_OPENACCOUNT");
    return e && *e && *e != '0';
}

int mv_voc_class(const char *type, int64_t len) {
    /* QM VOC type codes.  Verbs and keywords belong to the system; file and
       pointer definitions are platform-specific.  Everything else — PA, S, M,
       PH, … — is the user's own and travels with the account. */
    static const struct { const char *t; int c; } tbl[] = {
        {"V", 1}, {"K", 1}, {"I", 1},                 /* verb, keyword, intrinsic */
        {"F", 2}, {"Q", 2}, {"R", 2}, {"DIR", 2},     /* file / pointer defs */
        {NULL, 0}
    };
    for (int i = 0; tbl[i].t; i++) {
        size_t sl = strlen(tbl[i].t);
        if ((size_t)len == sl && strncasecmp(type, tbl[i].t, sl) == 0)
            return tbl[i].c;
    }
    return 0;
}

void mv_fatal(const char *fmt, ...) {
    va_list ap;
    fputs("qm-git: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}


/* --- is this name one of the account's files? -------------------------
 *
 * The engine's is_mv_file() has no context and has to decide from the name
 * alone, but on QM the filesystem cannot answer it.  A dynamic file is a
 * directory of group files, a directory file is a directory of records, and
 * some directory files have no dictionary at all -- BP.OUT, which QM's BASIC
 * compiler creates for object code, is exactly that.  Testing for `<name>.DIC`
 * therefore classified BP.OUT as an ordinary directory and every account that
 * had ever compiled a program reported `D BP.OUT.DICT/%FILE%` for ever.
 *
 * The VOC is the authority, so ask it, once, and cache.  `<name>.DICT` is the
 * engine's spelling for a file's dictionary, so it answers for the file. */
static char **g_names;
static size_t g_names_n, g_names_cap;

static void names_add(const char *nm) {
    if (g_names_n == g_names_cap) {
        size_t nc = g_names_cap ? g_names_cap * 2 : 32;
        char **nn = realloc(g_names, nc * sizeof *nn);
        if (!nn) mv_fatal("out of memory");
        g_names = nn; g_names_cap = nc;
    }
    size_t l = strlen(nm);
    char *c = malloc(l + 1);
    if (!c) mv_fatal("out of memory");
    memcpy(c, nm, l + 1);
    g_names[g_names_n++] = c;
}

static void names_prime(void) {
    static int done;
    if (done) return;
    qm_connect();
    int fno = QMOpen((char *)"VOC");
    if (fno < 0) return;      /* not cached: a failed scan must not become
                                 "this account has no files" for the run */
    done = 1;
    QMSelect(fno, SEL_INTERNAL);
    for (;;) {
        char *id = QMReadNext(SEL_INTERNAL);
        if (!id) break;                       /* NULL alone ends the list */
        if (!id[0]) { QMFree(id); continue; } /* an entry QM could not return */
        int err = 0;
        char *rec = QMRead(fno, id, &err);
        if (rec && (rec[0] == 'F' || rec[0] == 'f')
                && (rec[1] == '\0' || rec[1] == (char)0xFE)) {
            const char *a2 = strchr(rec, (char)0xFE);
            if (a2) {
                char pbuf[1024];
                a2++;
                const char *e2 = strchr(a2, (char)0xFE);
                size_t pn = e2 ? (size_t)(e2 - a2) : strlen(a2);
                if (pn && pn < sizeof pbuf) {
                    memcpy(pbuf, a2, pn); pbuf[pn] = '\0';
                    if (path_is_local(pbuf)) names_add(id);
                }
            }
        }
        if (rec) QMFree(rec);
        QMFree(id);
    }
    QMClose(fno);
}

int mv_qm_is_file(const char *name) {
    if (!name || !*name) return 0;
    if (!g_account[0]) return 0;               /* nothing to ask yet */
    char base[300];
    size_t nl = strlen(name);
    if (nl > 5 && nl - 5 < sizeof base && strcmp(name + nl - 5, ".DICT") == 0) {
        memcpy(base, name, nl - 5);
        base[nl - 5] = '\0';
        name = base;
    }
    names_prime();
    for (size_t i = 0; i < g_names_n; i++)
        if (strcmp(g_names[i], name) == 0) return 1;
    return 0;
}
