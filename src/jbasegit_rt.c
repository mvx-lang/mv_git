/* jbasegit_rt.c — the mv_* record layer over jBASE's Jedi* API.
 * Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
 *
 * See jbasegit_rt.h for why this looks like the MVX backend rather than the
 * UniData one.  Conventions below were established by experiment, because the
 * jBASE headers document none of them (mv_git#114):
 *
 *   JediOpen        0 on success, fills **FilePointer; FilePath may be NULL
 *   JediReadRecord  0 on success; allocates via the mallocptr you pass
 *   JediWriteRecord 0 on success
 *   JediSelect      0 on success, fills **SelectPtr
 *   JediReadnext    ALWAYS 0, even past the end.  End of list is
 *                   *RecordKeyPtr == NULL, and *RecordKeyLenPtr is not
 *                   touched -- so loop on the key pointer, never the return
 *                   code, or you read a stale key for ever.
 */
#include "jbasegit_rt.h"

#include <jsystem.h>
#include <jedi.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>   /* strcasecmp */

/* jBASE's own headers define VAR, STRING and friends; nothing below needs
   them, and the engine never sees them. */

struct mv_ctx {
    DPSTRUCT *dp;
    int       owned;    /* we made the session, so we destroy it */
};

/* A session handed in by the DEFC path, if any.  See mv_jbase_use_session. */
static DPSTRUCT *jb_borrowed_dp = NULL;

void mv_jbase_use_session(void *dp) { jb_borrowed_dp = (DPSTRUCT *)dp; }

void mv_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("jb-git: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* --- value ops --------------------------------------------------------- */

void mv_init(mv_value *v) {
    v->data = NULL;
    v->len = 0;
    v->fd = NULL;
    v->sel = NULL;
    v->is_file = 0;
}

void mv_clear(mv_value *v) {
    if (v->is_file) {
        /* the borrowed session may be gone by now on the DEFC path, so only
           tear down what we still have a session for */
        if (v->sel && jb_borrowed_dp)
            JediSelectEnd(jb_borrowed_dp, (JediFileDescriptor *)v->fd,
                          (struct JediSelectPtr *)v->sel);
        if (v->fd && jb_borrowed_dp)
            JediClose(jb_borrowed_dp, (JediFileDescriptor *)v->fd, 0);
    }
    free(v->data);
    mv_init(v);
}

void mv_set_str(mv_value *v, const char *p, int64_t len) {
    char *nb = malloc((size_t)len + 1);
    if (!nb) mv_fatal("out of memory");
    if (len) memcpy(nb, p, (size_t)len);
    nb[len] = '\0';
    free(v->data);
    v->data = nb;
    v->len = len;
    /* setting bytes makes this a plain string, never a file handle */
    v->fd = NULL;
    v->sel = NULL;
    v->is_file = 0;
}

int64_t mv_val_chars(const mv_value *v, char *numbuf, size_t cap,
                     const char **pp) {
    (void)numbuf;
    (void)cap;
    *pp = v->data ? v->data : "";
    return v->len;
}

/* --- context / session ------------------------------------------------- */

/* The session, made on demand.  A DEFC caller has already been given one by
   jBASE and passes it in; a standalone process asks for one. */
static DPSTRUCT *jb_session(mv_ctx *ctx) {
    if (ctx->dp) return ctx->dp;
    if (jb_borrowed_dp) {
        ctx->dp = jb_borrowed_dp;
        ctx->owned = 0;
        return ctx->dp;
    }
    ctx->dp = (DPSTRUCT *)JBASESessionObjectFactory();
    if (!ctx->dp) mv_fatal("cannot start a jBASE session");
    ctx->owned = 1;
    jb_borrowed_dp = ctx->dp;   /* so mv_clear can close files */
    return ctx->dp;
}

mv_ctx *mv_ctx_create(void) {
    mv_ctx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) mv_fatal("out of memory");
    return ctx;   /* the session opens lazily on first record op */
}

void mv_ctx_destroy(mv_ctx *ctx) {
    if (!ctx) return;
    if (ctx->dp && ctx->owned) {
        if (jb_borrowed_dp == ctx->dp) jb_borrowed_dp = NULL;
        JBASESessionObjectDestroy(ctx->dp);
    }
    free(ctx);
}

/* --- record I/O -------------------------------------------------------- */

int64_t mv_open(mv_ctx *ctx, const mv_value *dict, const mv_value *spec,
                mv_value *fvar) {
    DPSTRUCT *dp = jb_session(ctx);
    const char *name = spec && spec->data ? spec->data : "";
    /* A dictionary opens as the file's DICT: jBASE spells that "DICT <file>"
       in BASIC, and the Jedi layer takes the same path form udt does. */
    char path[512];
    if (dict) snprintf(path, sizeof path, "DICT %s", name);
    else      snprintf(path, sizeof path, "%s", name);

    JediFileDescriptor *fd = NULL;
    if (JediOpen(dp, &fd, path, NULL, 0) != 0 || !fd) return 0;

    free(fvar->data);
    fvar->data = NULL;
    fvar->len = 0;
    fvar->fd = fd;
    fvar->sel = NULL;
    fvar->is_file = 1;
    return 1;
}

int64_t mv_read(mv_ctx *ctx, mv_value *rec, const mv_value *fvar,
                const mv_value *id, int64_t lock) {
    (void)lock;
    DPSTRUCT *dp = jb_session(ctx);
    if (!fvar->is_file || !fvar->fd) return 0;

    char *buf = NULL;
    int len = 0;
    int rc = JediReadRecord(dp, (JediFileDescriptor *)fvar->fd, 0,
                            id->data ? id->data : (char *)"",
                            (int)id->len, &buf, &len, 0, malloc);
    if (rc != 0) { mv_set_str(rec, "", 0); return 0; }
    mv_set_str(rec, buf ? buf : "", len > 0 ? len : 0);
    free(buf);
    return 1;
}

int64_t mv_write(mv_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t keep_lock, int64_t onerr) {
    (void)keep_lock;
    (void)onerr;
    DPSTRUCT *dp = jb_session(ctx);
    if (!fvar->is_file || !fvar->fd) return 0;
    int rc = JediWriteRecord(dp, (JediFileDescriptor *)fvar->fd, 0,
                             id->data ? id->data : (char *)"", (int)id->len,
                             rec->data ? rec->data : (char *)"", (int)rec->len,
                             0);
    return rc == 0;
}

int64_t mv_delete_rec(mv_ctx *ctx, const mv_value *fvar, const mv_value *id) {
    DPSTRUCT *dp = jb_session(ctx);
    if (!fvar->is_file || !fvar->fd) return 0;
    int rc = JediDeleteRecord(dp, (JediFileDescriptor *)fvar->fd, 0,
                              id->data ? id->data : (char *)"", (int)id->len);
    return rc == 0;
}

/* The select list belongs to the file value it was started on; mv_readnext
   drains whichever one is current, which matches how the engine uses it (one
   select at a time, over the file it just opened). */
static mv_value *jb_cur_sel = NULL;

void mv_select(mv_ctx *ctx, const mv_value *fvar) {
    DPSTRUCT *dp = jb_session(ctx);
    mv_value *f = (mv_value *)fvar;    /* the list is state on the file value */
    if (!f->is_file || !f->fd) return;
    if (f->sel) {
        JediSelectEnd(dp, (JediFileDescriptor *)f->fd,
                      (struct JediSelectPtr *)f->sel);
        f->sel = NULL;
    }
    struct JediSelectPtr *sel = NULL;
    if (JediSelect(dp, (JediFileDescriptor *)f->fd, &sel) != 0) return;
    f->sel = sel;
    jb_cur_sel = f;
}

int64_t mv_readnext(mv_ctx *ctx, mv_value *id) {
    DPSTRUCT *dp = jb_session(ctx);
    mv_value *f = jb_cur_sel;
    if (!f || !f->sel || !f->fd) return 0;

    char *key = NULL;
    int klen = 0;
    /* JediReadnext returns 0 even past the end: a NULL key is the only
       end-of-list signal, and klen is left untouched.  See the header. */
    JediReadnext(dp, (JediFileDescriptor *)f->fd,
                 (struct JediSelectPtr *)f->sel, &key, &klen);
    if (!key) {
        JediSelectEnd(dp, (JediFileDescriptor *)f->fd,
                      (struct JediSelectPtr *)f->sel);
        f->sel = NULL;
        jb_cur_sel = NULL;
        return 0;
    }
    mv_set_str(id, key, klen > 0 ? klen : 0);
    return 1;
}

/* --- files ------------------------------------------------------------- */
/* First pass: the file-level operations are what a clone and a checkout need,
   and they are the part most likely to want jBASE-specific spelling.  Left
   deliberately minimal and honest about it rather than guessed at. */

/* Run a sentence through jBASE's own command processor.
 *
 * NOT JediFileOp, though it exists and is exported.  Its flags do not mean what
 * they are named: CREATE with flags=0 reported JEDI_FILEOP_FILE_EXISTS_DATA on
 * an empty directory and made only the `]D` dictionary, after which opening the
 * file resolved to that dictionary and a record written to the "file" went into
 * it.  DATA_ONLY did create the data section and no dictionary; DICT_ONLY then
 * reported the data existing.  Undocumented, and a file half-created that way is
 * worse than no file, so this drives the sentence jBASE itself uses -- which
 * gets the type and geometry right by construction.  udtgit_rt.c reaches for
 * UniData's ECL the same way and for the same reason (mv_git#114). */
static int jb_run_sentence(const char *sentence) {
    char cmd[1200];
    /* jsh reads TCL from stdin; the account is the working directory, which is
       already where the engine is operating. */
    snprintf(cmd, sizeof cmd,
             "printf '%%s\\n' \"%s\" | jsh >/dev/null 2>&1", sentence);
    int rc = system(cmd);
    if (getenv("JBGIT_DEBUG"))
        fprintf(stderr, "jb-git: %s -> %d\n", sentence, rc);
    return rc == 0;
}

/* `type` is NULL/"" for the account default (a hash file) or "DIR".
 *
 * jBASE spells the types JP (hash) and UD (Unix directory) -- and NOT JD, which
 * is also a regular file rather than a directory and would silently give a
 * directory file the wrong shape. */
int64_t mv_createfile(mv_ctx *ctx, const mv_value *spec, const mv_value *type) {
    (void)ctx;
    if (!spec || !spec->data || !spec->data[0]) return 0;
    const char *t = type && type->data ? type->data : "";
    int isdir = (strcasecmp(t, "DIR") == 0);
    char sentence[600];
    snprintf(sentence, sizeof sentence, "CREATE-FILE %s 1 11%s",
             spec->data, isdir ? " TYPE=UD" : "");
    if (!jb_run_sentence(sentence)) return 0;
    /* Believe the filesystem, not the exit status: jsh reports a create in its
       output rather than its return code. */
    struct stat st;
    return stat(spec->data, &st) == 0;
}

int64_t mv_deletefile(mv_ctx *ctx, const mv_value *spec) {
    (void)ctx;
    if (!spec || !spec->data || !spec->data[0]) return 0;
    struct stat st;
    if (stat(spec->data, &st) != 0) return 1;      /* already gone */
    char sentence[600];
    snprintf(sentence, sizeof sentence, "DELETE-FILE %s", spec->data);
    jb_run_sentence(sentence);
    return stat(spec->data, &st) != 0;             /* gone afterwards? */
}

/* The account's files, as `name<VM>type` rows separated by @AM.
 *
 * NOT JediGetAllFiles: that enumerates the files a session already has OPEN,
 * which is not the same question.  A jBASE account is a directory and its files
 * are entries in it — a hash file is a single regular file (CUST, 49152 bytes),
 * a directory file is a real directory, and a dictionary is a sibling named
 * `<file>]D`.  So the account's file list is a directory scan, which is also
 * what the DIR-backed MVX path does. */
void mv_filelist(mv_ctx *ctx, mv_value *dst) {
    (void)ctx;
    char buf[65536];
    size_t n = 0;
    DIR *d = opendir(".");
    if (!d) { mv_set_str(dst, "", 0); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        if (nm[0] == '.') continue;                    /* . .. .git .jbase */
        size_t l = strlen(nm);
        /* THE MASTER FILE IS ITS OWN DICTIONARY, and the rule below hid it.
           jBASE's MD exists ONLY as `MD]D`: `jstat MD` reports File .../MD]D,
           and `CT MD` and `CT DICT MD` return the same records, so there is no
           `MD` data file on disk to enumerate.  Skipping every `]D` name as a
           dictionary therefore made the account's own master file invisible to
           the walk -- which is why the master-file direction of a commit had
           never run here at all (mv_git#114).
           Emit it under the name the account uses.  Every OTHER `]D` really is
           the dictionary of the file beside it, and still skips below. */
        if (l == 4 && !strcmp(nm, "MD]D")) {
            static const char md[] = "MD", mdtype[] = "hash";
            size_t need = sizeof md - 1 + 1 + sizeof mdtype - 1 + 1;
            if (n + need >= sizeof buf) break;
            if (n) buf[n++] = (char)0xFE;
            memcpy(buf + n, md, sizeof md - 1); n += sizeof md - 1;
            buf[n++] = (char)0xFD;
            memcpy(buf + n, mdtype, sizeof mdtype - 1); n += sizeof mdtype - 1;
            continue;
        }
        if (l > 2 && !strcmp(nm + l - 2, "]D")) continue;  /* a dictionary */
        /* ...and so is <file>.DICT, which is what an OPEN-FORM clone puts on
           disk.  jBASE has no VOC, so this walk is a directory scan and a
           dictionary looks exactly like a data file: enumerated as one, it was
           given a dictionary of its OWN, so `add -A` invented
           CLIENTS.DICT.DICT/%FILE% for every file in the account and rewrote
           the real CLIENTS.DICT/%FILE% from `hash 2 DYNAMIC` to `DIR`.
           `status` never saw any of it, because %FILE% is synthesised at add
           time rather than read -- so a freshly cloned account read clean and
           committed twelve changes anyway. */
        if (l > 5 && !strcmp(nm + l - 5, ".DICT")) continue;
        struct stat st;
        if (stat(nm, &st) != 0) continue;
        const char *type;
        if (S_ISDIR(st.st_mode)) type = "DIR";
        else if (S_ISREG(st.st_mode)) {
            /* A regular file is only an MV file if it has a dictionary beside
               it; otherwise it is somebody's README. */
            char dict[512];
            snprintf(dict, sizeof dict, "%s]D", nm);
            struct stat ds;
            if (stat(dict, &ds) != 0) continue;
            type = "hash";
        } else continue;
        size_t need = strlen(nm) + 1 + strlen(type) + 1;
        if (n + need >= sizeof buf) break;
        if (n) buf[n++] = (char)0xFE;
        memcpy(buf + n, nm, strlen(nm)); n += strlen(nm);
        buf[n++] = (char)0xFD;
        memcpy(buf + n, type, strlen(type)); n += strlen(type);
    }
    closedir(d);
    mv_set_str(dst, buf, (int64_t)n);
}

/* A live file's class in the open form: "DIR", or "hash <modulo> DYNAMIC".
 *
 * FIRST PASS: the modulo is reported as 1 because jBASE keeps the geometry in
 * the file header and reading it means parsing a private format.  A clone
 * therefore recreates a file at default size rather than its true one, which is
 * a sizing regression and not a correctness one -- but it IS a lie about
 * geometry, so it is written down here rather than left to be discovered.
 * TODO(#114): read the real modulo (JediIOCTL / JediStatDynamicObject). */
int64_t mv_fileclass(mv_ctx *ctx, const char *name, char *out, size_t cap) {
    (void)ctx;
    struct stat st;
    if (stat(name, &st) != 0) { if (cap) out[0] = '\0'; return 0; }
    if (S_ISDIR(st.st_mode)) snprintf(out, cap, "DIR");
    else snprintf(out, cap, "hash 1 DYNAMIC");
    return 1;
}

int64_t mv_indices(mv_ctx *ctx, const char *name, char *out, size_t cap) {
    (void)ctx; (void)name;
    if (cap) out[0] = '\0';
    return 0;   /* TODO(#114) */
}

/* --- misc -------------------------------------------------------------- */

int mv_openaccount(void) {
    const char *e = getenv("MVX_OPENACCOUNT");
    return e && *e && *e != '0';
}

/* jBASE's MD is not a VOC, and most of what mv_voc_class classifies elsewhere
   simply is not there.  Measured on 6.2.1.1, a fresh CREATE-ACCOUNT MD holds 240
   records:
     t x204   the stock verb definitions
     A x8  Z x5  I x4   dictionary descriptors (I carries SUBR(...))
     Q x2  C x2         pointers, and a catalog entry
   plus a handful whose attribute 1 is data rather than a type code.

   CLASS 2 IS Q AND ONLY Q.  `CREATE-FILE CUST` writes NO MD record at all --
   CUST and its CUST]D appear on disk and MD is untouched -- so a jBASE file is
   found by the directory scan in mv_filelist(), never by a master-file pointer.
   `SET-FILE` does write, and what it writes is a Q pointer.  So the "a pointer
   to a file this repository carries" rule has exactly one spelling here.

   CLASS 1 IS DELIBERATELY EMPTY.  There is no V: a verb is a `t` record, and a
   fresh account already has 204 of them.  Dropping every `t` would be the right
   answer for those and the wrong one for a verb the user wrote, and which of
   those is true depends on whether jBASE's catalog rewrites the MD record --
   not yet measured.  The stock records are handled by the stock-record
   subtraction (mv_git#46) rather than by class, so leaving class 1 empty loses
   nothing today and guesses nothing.  Returning 0 for everything was the old
   behaviour; this records WHY it is nearly right rather than calling it a TODO
   (mv_git#114). */
int mv_voc_class(const char *type, int64_t len) {
    if (len == 1 && (type[0] == 'Q' || type[0] == 'q')) return 2;
    return 0;
}
