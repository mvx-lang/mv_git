/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* qmgit_dh.c — the IN-SESSION record layer for OpenQM / ScarletDME
 * (mv_git#243).  Same contract as qmgit_rt.c and chosen at link time instead
 * of it: qm-git (the CLI) links the QMClient one, libqmgit.so (the verb) links
 * this.
 *
 * WHY A SECOND ONE.  QMClient is a CLIENT: it opens its own connection, with
 * its own file table and its own view of the account.  From inside a session
 * that is the wrong thing twice over -- it costs a second connection, and it
 * cannot see work the calling session has done but not committed, which is
 * exactly what an in-session `GIT ADD' is for.  This layer calls QM's own dh_
 * file layer directly, in the session's own process, so the verb reads the
 * records of the session that called it.
 *
 * WHAT IT NEEDS FROM ScarletDME, and why the CLI came first.  Reaching dh_
 * from a CCALL'd library needs three fixes that are not in stock ScarletDME:
 *
 *   geneb/ScarletDME#109  link qm with -rdynamic, or the library cannot
 *                         resolve dh_open at all and dlopen returns NULL
 *   geneb/ScarletDME#111  CCALL truncated every pointer argument to 32 bits,
 *                         so passing a buffer faulted the process
 *   geneb/ScarletDME#113  CCALL sized string 2's allocation from string 1,
 *                         overflowing the heap
 *
 * All three are measured, not guessed: a record read back through this layer
 * was byte-identical to the session's own READ, and a 4-way concurrent write
 * left 1000 records and a correct AK index.
 */
#include "qmgit_rt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>

#include "qm.h"
#include "syscom.h"   /* SelectList() -- the select list descriptors */

/* These live in dh_int.h, which is not part of the public set.  Declared
   rather than included: dh_int.h also carries Public globals, and pulling it
   in here would define a second copy of them in this shared library. */
extern bool dh_delete(DH_FILE *dh_file, char id[], int16_t id_len);
extern bool dh_create_file(char path[], int16_t group_size,
                           int32_t min_modulus, int32_t big_rec_size,
                           int16_t merge_load, int16_t split_load,
                           u_int32_t flags, int16_t version);
/* ...and the AK matrix's shape, from the same header.  op_indices1() walks it
   with exactly these two. */
#define AKD_NAME 1
#define AKD_COLS 6
extern int16_t lock_record(FILE_VAR *, char *id, int16_t id_len, bool update,
                           u_int32_t txn_id, bool no_wait);
extern bool unlock_record(FILE_VAR *, char *id, int16_t id_len);

#define MAXOPEN 64

struct open_file {
    char     name[256];
    DH_FILE *dh;            /* a DYNAMIC file... */
    char     dir[1024];     /* ...or a DIRECTORY file, whose records are files */
};

struct mv_ctx {
    struct open_file files[MAXOPEN];
    int nfiles;
    /* The select is walked with dh_select/dh_complete_select on a list number
       of our own, so it cannot disturb a select the SESSION is in the middle
       of -- the verb is called from BASIC that may itself be inside one. */
    int   sel_list;
    int   sel_active;
    int   sel_filled;
    char *sel_buf;          /* the completed list, @AM-separated */
    int64_t sel_len, sel_at;
    DIR    *sel_dir;        /* a directory file's select is a directory walk */
};

#define SEL_LIST 9          /* ours; 0..7 belong to the session */

static char g_account[256];

void mv_qm_set_account(const char *name) {
    snprintf(g_account, sizeof g_account, "%s", name ? name : "");
}
const char *mv_qm_account(void) { return g_account; }

/* --- context ----------------------------------------------------------- */

mv_ctx *mv_ctx_create(void) {
    mv_ctx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) mv_fatal("out of memory");
    ctx->sel_list = SEL_LIST;
    return ctx;
}

void mv_ctx_destroy(mv_ctx *ctx) {
    if (!ctx) return;
    if (ctx->sel_dir) closedir(ctx->sel_dir);
    free(ctx->sel_buf);
    for (int i = 0; i < ctx->nfiles; i++)
        if (ctx->files[i].dh) dh_close(ctx->files[i].dh);
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
    v->data = n; v->len = len; v->is_file = 0; v->fno = -1;
}

int64_t mv_val_chars(const mv_value *v, char *numbuf, size_t cap,
                     const char **pp) {
    (void)numbuf; (void)cap;
    if (!v || !v->data) { if (pp) *pp = ""; return 0; }
    if (pp) *pp = v->data;
    return v->len;
}

/* --- resolving a name to a path ----------------------------------------
 * dh_open() takes a PATH; the engine names files the way the account does.
 * The VOC is the map, exactly as it is for the QMClient layer -- and a session
 * runs in its account's directory, so `VOC' resolves without help.
 *
 * `F' records are "F" <AM> data-path <AM> dict-path.  A path of "." is the
 * account itself and an "@ACCOUNT/FILE" one belongs to somebody else; neither
 * is this account's file (see qmgit_rt.c, which learned that the hard way). */
static int voc_owns(const char *name);      /* defined below, used by both */

static int path_is_local(const char *p) {
    if (!p || !*p) return 0;
    if (p[0] == '@' || p[0] == '/') return 0;
    return strcmp(p, ".") != 0;
}

static char *chunk_bytes(STRING_CHUNK *rec, int64_t *len) {
    int64_t n = 0;
    for (STRING_CHUNK *p = rec; p; p = p->next) n += p->bytes;
    char *buf = malloc((size_t)n + 1);
    if (!buf) mv_fatal("out of memory");
    int64_t at = 0;
    for (STRING_CHUNK *p = rec; p; p = p->next) {
        memcpy(buf + at, p->data, (size_t)p->bytes);
        at += p->bytes;
    }
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

static STRING_CHUNK *mkchunk(const char *s, int64_t len) {
    int16_t actual = 0;
    STRING_CHUNK *c = s_alloc((int32_t)len, &actual);
    if (!c) return NULL;
    if (len > actual) len = actual;          /* one chunk is enough here */
    memcpy(c->data, s, (size_t)len);
    c->bytes = (int16_t)len;
    c->string_len = (int32_t)len;
    c->ref_ct = 1;
    c->next = NULL;
    return c;
}

/* attribute `n' (1-based) of a record, copied out */
static int rec_attr(const char *rec, int64_t rl, int n, char *out, size_t cap) {
    int at = 1;
    int64_t i = 0, st = 0;
    while (i <= rl) {
        if (i == rl || (unsigned char)rec[i] == 0xFE) {
            if (at == n) {
                size_t len = (size_t)(i - st);
                if (len >= cap) return 0;
                memcpy(out, rec + st, len);
                out[len] = '\0';
                return 1;
            }
            at++; st = i + 1;
        }
        i++;
    }
    return 0;
}

/* @ACCOUNT/FILE -> an OS path.  A VOC F record may point at another account's
   file -- @QMSYS/NEWVOC is the stock VOC template every account carries a
   pointer to, and the stock baseline is built by READING it (mv_git#46).  So
   "does this account own it" and "where is it" are two questions, and only the
   first is path_is_local().  QMClient answered the second for the CLI by
   resolving pointers itself; in-process we resolve them here.
   QMSYS is the one that matters and its location is in the config file; any
   other account pointer is left alone rather than guessed at. */
static int resolve_at(const char *p, char *out, size_t cap) {
    if (p[0] != '@') return 0;
    const char *slash = strchr(p, '/');
    if (!slash) return 0;
    size_t alen = (size_t)(slash - (p + 1));
    if (alen != 5 || strncasecmp(p + 1, "QMSYS", 5) != 0) return 0;
    char qmsys[512] = "/usr/qmsys";
    FILE *cf = fopen("/etc/scarlet.conf", "r");
    if (cf) {
        char ln[600];
        while (fgets(ln, sizeof ln, cf))
            if (strncasecmp(ln, "QMSYS=", 6) == 0) {
                char *v = ln + 6, *e = v;
                while (*e && *e != '\n' && *e != '\r') e++;
                *e = '\0';
                snprintf(qmsys, sizeof qmsys, "%s", v);
                break;
            }
        fclose(cf);
    }
    snprintf(out, cap, "%s/%s", qmsys, slash + 1);
    return 1;
}

/* The path for `name', or its dictionary's when `dict'.  0 if there is no
   usable path -- the account owns no such file and it is not a pointer we can
   follow. */
static int voc_path(const char *name, int dict, char *out, size_t cap) {
    DH_FILE *voc = dh_open("VOC");
    if (!voc) return 0;
    STRING_CHUNK *r = dh_read(voc, (char *)name, (int16_t)strlen(name), NULL);
    int ok = 0;
    if (r) {
        int64_t rl = 0;
        char *rec = chunk_bytes(r, &rl);
        char type[16];
        if (rec_attr(rec, rl, 1, type, sizeof type) &&
            (type[0] == 'F' || type[0] == 'f') &&
            rec_attr(rec, rl, dict ? 3 : 2, out, cap)) {
            if (path_is_local(out)) ok = 1;
            else {
                char at[1024];
                if (resolve_at(out, at, sizeof at)) {
                    snprintf(out, cap, "%s", at);
                    ok = 1;
                }
            }
        }
        free(rec);
        s_free(r);
    }
    dh_close(voc);
    return ok;
}

/* --- directory files ----------------------------------------------------
 *
 * A QM DIRECTORY file is an OS directory and its records are ordinary files,
 * so none of the dh_ calls above apply to one.  That is not an edge case: BP
 * is a directory file in every account, and so is QMSYS's NEWVOC -- which is
 * what the stock baseline is read from.
 *
 * THE RECORD ID IS NOT THE FILENAME.  QM maps ids that cannot be filenames:
 * a leading `.' becomes `%d', a leading `~' becomes `%t', and each of
 * df_restricted_chars becomes `%' plus its partner in df_substitute_chars.
 * map_t1_id() does it and QM exports it, so call THAT rather than keep a
 * second copy of the table in step with theirs. */
extern bool map_t1_id(char *id, int16_t id_len, char *mapped_id);

/* ...and back again, for a directory walk.  There is no unmap_t1_id in QM --
   nothing there needs one, because a select reads the ids from elsewhere --
   so this is the only place the tables are read directly. */
static void unmap_t1_id(const char *fn, char *id, size_t cap) {
    size_t n = 0;
    const char *p = fn;
    if (p[0] == '%' && p[1] == 'd') { if (n + 1 < cap) id[n++] = '.'; p += 2; }
    else if (p[0] == '%' && p[1] == 't') { if (n + 1 < cap) id[n++] = '~'; p += 2; }
    while (*p && n + 1 < cap) {
        if (*p == '%' && p[1]) {
            const char *r = strchr(df_substitute_chars, p[1]);
            if (r) { id[n++] = df_restricted_chars[r - df_substitute_chars];
                     p += 2; continue; }
        }
        id[n++] = *p++;
    }
    id[n] = '\0';
}

/* <dir>/<mapped id>, or 0 if the id cannot be a filename at all. */
static int dir_record_path(const char *dir, const char *id,
                           char *out, size_t cap) {
    char mapped[600];
    if (!map_t1_id((char *)id, (int16_t)strlen(id), mapped)) return 0;
    return snprintf(out, cap, "%s/%s", dir, mapped) < (int)cap;
}

/* Returns the cache slot, opening the file if need be.  A DYNAMIC file gets a
   DH_FILE; a DIRECTORY file gets its path and no DH_FILE, because there is no
   DH structure in one to open. */
static struct open_file *open_cached(mv_ctx *ctx, const char *spec,
                                     const char *path) {
    for (int i = 0; i < ctx->nfiles; i++)
        if (strcmp(ctx->files[i].name, spec) == 0) return &ctx->files[i];
    if (ctx->nfiles >= MAXOPEN) return NULL;

    struct open_file *f = &ctx->files[ctx->nfiles];
    memset(f, 0, sizeof *f);
    struct stat sb;
    char probe[1100];
    int isdir_file = 0;
    if (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        snprintf(probe, sizeof probe, "%s/~0", path);
        int hash = (stat(probe, &sb) == 0);
        if (!hash) {
            snprintf(probe, sizeof probe, "%s/%%0", path);
            hash = (stat(probe, &sb) == 0);
        }
        isdir_file = !hash;
    }
    if (isdir_file) {
        snprintf(f->dir, sizeof f->dir, "%s", path);
    } else {
        f->dh = dh_open((char *)path);
        if (!f->dh) return NULL;
    }
    snprintf(f->name, sizeof f->name, "%s", spec);
    ctx->nfiles++;
    return f;
}

/* --- record I/O --------------------------------------------------------- */

int64_t mv_open(mv_ctx *ctx, const mv_value *dict, const mv_value *spec,
                mv_value *fvar) {
    if (!ctx || !spec || !spec->data || !spec->data[0] || !fvar) return 0;
    int want_dict = dict && dict->data && dict->data[0];
    char key[600], path[1024];
    snprintf(key, sizeof key, "%s%s", want_dict ? "DICT " : "", spec->data);
    if (!voc_path(spec->data, want_dict, path, sizeof path)) return 0;
    if (!open_cached(ctx, key, path)) return 0;
    mv_clear(fvar);
    fvar->is_file = 1;
    fvar->fno = -1;
    fvar->data = malloc(strlen(key) + 1);
    if (!fvar->data) mv_fatal("out of memory");
    memcpy(fvar->data, key, strlen(key) + 1);
    fvar->len = (int64_t)strlen(key);
    return 1;
}

static struct open_file *fvar_slot(mv_ctx *ctx, const mv_value *fvar) {
    if (!ctx || !fvar || !fvar->is_file || !fvar->data) return NULL;
    for (int i = 0; i < ctx->nfiles; i++)
        if (strcmp(ctx->files[i].name, fvar->data) == 0) return &ctx->files[i];
    return NULL;
}

/* --- mark mapping, for directory files ONLY ------------------------------
 *
 * A DIRECTORY FILE'S RECORD IS A TEXT FILE, and QM writes it as one: its
 * attribute marks are NEWLINES on disk.  Reading one back turns them into
 * field marks again, and a single trailing newline is the end of the file
 * rather than a final empty attribute.  op_dio3.c does exactly this either
 * side of the OS read and write, and nothing else -- value and subvalue marks
 * are ordinary bytes in a directory file and are left alone.
 *
 * SKIPPING IT LOOKS LIKE IT WORKS, WHICH IS WHY IT HAS TO BE SAID.  Reading
 * raw gives a "record" whose attributes are separated by newlines; the engine
 * stores a blob with newlines for marks, so the two agree and `ADD' and
 * `STATUS' are perfectly happy.  It is the WRITE that breaks: a checkout hands
 * back a record with real field marks in it, those go to disk verbatim, and
 * every record in BP becomes one 0xFE-riddled line that QM can still read but
 * nothing else can.  In the suite that surfaced as every source in the account
 * reading as modified after the first `GIT CHECKOUT'. */
static char *dir_marks_in(const char *buf, size_t n, size_t *out) {
    /* A trailing newline is the file's terminator, not an empty attribute. */
    if (n && buf[n - 1] == '\n') n--;
    char *d = malloc(n ? n : 1);
    if (!d) mv_fatal("out of memory");
    for (size_t i = 0; i < n; i++) d[i] = (buf[i] == '\n') ? (char)0xFE : buf[i];
    *out = n;
    return d;
}

/* Field marks back to newlines, plus the terminating newline QM adds. */
static char *dir_marks_out(const char *buf, size_t n, size_t *out) {
    char *d = malloc(n + 1);
    if (!d) mv_fatal("out of memory");
    for (size_t i = 0; i < n; i++)
        d[i] = ((unsigned char)buf[i] == 0xFE) ? '\n' : buf[i];
    d[n] = '\n';
    *out = n + 1;
    return d;
}

int64_t mv_read(mv_ctx *ctx, mv_value *rec, const mv_value *fvar,
                const mv_value *id, int64_t lock) {
    (void)lock;
    struct open_file *f = fvar_slot(ctx, fvar);
    if (!f || !id || !id->data) return 0;
    if (f->dir[0]) {                      /* a directory file: it IS a file */
        char path[1700];
        if (!dir_record_path(f->dir, id->data, path, sizeof path)) return 0;
        FILE *fp = fopen(path, "rb");
        if (!fp) { mv_set_str(rec, "", 0); return 0; }
        char *buf = NULL; size_t cap = 0, n = 0;
        for (;;) {
            if (n + 4096 > cap) { cap = cap ? cap * 2 : 8192;
                char *nb = realloc(buf, cap);
                if (!nb) { free(buf); fclose(fp); mv_fatal("out of memory"); }
                buf = nb; }
            size_t got = fread(buf + n, 1, 4096, fp);
            n += got;
            if (got < 4096) break;
        }
        fclose(fp);
        size_t mn = 0;
        char *mapped = dir_marks_in(buf ? buf : "", n, &mn);
        mv_set_str(rec, mapped, (int64_t)mn);
        free(mapped);
        free(buf);
        return 1;
    }
    DH_FILE *dh = f->dh;
    dh_err = 0;
    STRING_CHUNK *r = dh_read(dh, id->data, (int16_t)strlen(id->data), NULL);
    if (!r) { mv_set_str(rec, "", 0); return 0; }
    int64_t rl = 0;
    char *buf = chunk_bytes(r, &rl);
    s_free(r);
    mv_set_str(rec, buf, rl);
    free(buf);
    return 1;
}

/* NO strlen ON THE RECORD.  dh_write takes a length, so a record carrying a
   NUL travels intact here -- unlike the QMClient layer, whose API cannot. */
int64_t mv_write(mv_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t keep_lock, int64_t onerr) {
    (void)keep_lock; (void)onerr;
    struct open_file *f = fvar_slot(ctx, fvar);
    if (!f || !id || !id->data) return 0;
    if (f->dir[0]) {
        char path[1700];
        if (!dir_record_path(f->dir, id->data, path, sizeof path)) return 0;
        FILE *fp = fopen(path, "wb");
        if (!fp) return 0;
        size_t want = 0;
        char *mapped = dir_marks_out(rec && rec->data ? rec->data : "",
                                     rec ? (size_t)rec->len : 0, &want);
        size_t put = fwrite(mapped, 1, want, fp);
        free(mapped);
        fclose(fp);
        return put == want;
    }
    DH_FILE *dh = f->dh;
    STRING_CHUNK *c = mkchunk(rec && rec->data ? rec->data : "",
                              rec ? rec->len : 0);
    if (!c) return 0;
    dh_err = 0;
    bool ok = dh_write(dh, id->data, (int16_t)strlen(id->data), c);
    s_free(c);
    return ok ? 1 : 0;
}

int64_t mv_delete_rec(mv_ctx *ctx, const mv_value *fvar, const mv_value *id) {
    struct open_file *f = fvar_slot(ctx, fvar);
    if (!f || !id || !id->data) return 0;
    if (f->dir[0]) {
        char path[1700];
        if (!dir_record_path(f->dir, id->data, path, sizeof path)) return 0;
        return unlink(path) == 0 || errno == ENOENT;
    }
    DH_FILE *dh = f->dh;
    dh_err = 0;
    return dh_delete(dh, id->data, (int16_t)strlen(id->data)) ? 1 : 0;
}

void mv_select(mv_ctx *ctx, const mv_value *fvar) {
    struct open_file *f = fvar_slot(ctx, fvar);
    if (!f) return;
    if (ctx->sel_active && !ctx->sel_dir) dh_end_select(ctx->sel_list);
    if (ctx->sel_dir) { closedir(ctx->sel_dir); ctx->sel_dir = NULL; }
    free(ctx->sel_buf); ctx->sel_buf = NULL;
    ctx->sel_filled = 0; ctx->sel_at = 0; ctx->sel_len = 0;
    if (f->dir[0]) {
        ctx->sel_dir = opendir(f->dir);     /* the records ARE the entries */
        ctx->sel_active = (ctx->sel_dir != NULL);
        return;
    }
    dh_select(f->dh, (int16_t)ctx->sel_list);
    ctx->sel_active = 1;
}

/* dh_select fills a SELECT LIST, group by group; dh_complete_select finishes
   the job and leaves the ids in the list descriptor as an @AM-separated
   string.  So complete it once and then walk the string -- there is no
   "next id" call in the dh_ layer, because the interpreter's own READNEXT
   consumes the descriptor rather than asking the file. */
static void sel_fill(mv_ctx *ctx) {
    if (ctx->sel_filled) return;
    dh_complete_select((int16_t)ctx->sel_list);
    DESCRIPTOR *d = SelectList(ctx->sel_list);
    if (d && d->type == STRING && d->data.str.saddr) {
        STRING_CHUNK *s = d->data.str.saddr;
        int64_t n = 0;
        ctx->sel_buf = chunk_bytes(s, &n);
        ctx->sel_len = n;
    } else {
        ctx->sel_buf = NULL;
        ctx->sel_len = 0;
    }
    ctx->sel_at = 0;
    ctx->sel_filled = 1;
}

int64_t mv_readnext(mv_ctx *ctx, mv_value *id) {
    if (!ctx || !id) return 0;
    if (!ctx->sel_active) { mv_set_str(id, "", 0); return 0; }
    if (ctx->sel_dir) {
        struct dirent *e;
        while ((e = readdir(ctx->sel_dir)) != NULL) {
            if (e->d_name[0] == '.' &&
                (e->d_name[1] == '\0' ||
                 (e->d_name[1] == '.' && e->d_name[2] == '\0'))) continue;
            char rid[600];
            unmap_t1_id(e->d_name, rid, sizeof rid);
            mv_set_str(id, rid, (int64_t)strlen(rid));
            return 1;
        }
        closedir(ctx->sel_dir);
        ctx->sel_dir = NULL;
        ctx->sel_active = 0;
        mv_set_str(id, "", 0);
        return 0;
    }
    sel_fill(ctx);
    while (ctx->sel_buf && ctx->sel_at < ctx->sel_len) {
        int64_t st = ctx->sel_at;
        while (ctx->sel_at < ctx->sel_len &&
               (unsigned char)ctx->sel_buf[ctx->sel_at] != 0xFE) ctx->sel_at++;
        int64_t len = ctx->sel_at - st;
        if (ctx->sel_at < ctx->sel_len) ctx->sel_at++;   /* past the mark */
        if (len > 0) { mv_set_str(id, ctx->sel_buf + st, len); return 1; }
    }
    free(ctx->sel_buf);
    ctx->sel_buf = NULL;
    ctx->sel_active = 0;
    ctx->sel_filled = 0;
    mv_set_str(id, "", 0);
    return 0;
}

/* --- files -------------------------------------------------------------- */

/* --- making and unmaking a file ----------------------------------------
 *
 * A FILE IS THREE THINGS, and dh_create_file() is only the first: the data
 * file's groups on disk, a DICTIONARY beside it, and a VOC entry so the
 * account knows the name at all.  Create the groups alone and nothing can
 * open it; that is why this layer refused to do it until now rather than
 * leaving a half-made file behind.
 *
 * The geometry is QM's own, measured from a file CREATE-FILE had just made
 * (ANALYZE.FILE: group size 2, large record 1638, minimum modulus 1, load
 * factors 80 split / 50 merge, version 2) rather than copied from a header
 * constant that might only be a default for something else.
 *
 * CREATEF, the BASIC verb, does a great deal more than this -- multifiles,
 * shared dictionaries, case folding, the lot.  This is deliberately only the
 * shape mv_git makes: a data file, its dictionary, and the pointer to them.  */
#define QMDH_GROUP_SIZE   2
#define QMDH_MIN_MODULUS  1
#define QMDH_BIG_REC      1638
#define QMDH_MERGE_LOAD  50
#define QMDH_SPLIT_LOAD  80
#define QMDH_VERSION      2

static int dh_make(const char *path) {
    return dh_create_file((char *)path, QMDH_GROUP_SIZE, QMDH_MIN_MODULUS,
                          QMDH_BIG_REC, QMDH_MERGE_LOAD, QMDH_SPLIT_LOAD,
                          0, QMDH_VERSION) ? 1 : 0;
}

/* "F" <AM> data-path <AM> dict-path -- the form every F record in a QM VOC
   has, and the one voc_path() above reads back. */
static int voc_write_file_entry(const char *name, const char *dpath,
                                const char *xpath) {
    DH_FILE *voc = dh_open("VOC");
    if (!voc) return 0;
    char rec[1200];
    int n = snprintf(rec, sizeof rec, "F%c%s%c%s",
                     (char)0xFE, dpath, (char)0xFE, xpath);
    STRING_CHUNK *c = mkchunk(rec, n);
    int ok = 0;
    if (c) {
        ok = dh_write(voc, (char *)name, (int16_t)strlen(name), c) ? 1 : 0;
        s_free(c);
    }
    dh_close(voc);
    return ok;
}

/* The record-key item every QM dictionary carries, in the shape CREATE-FILE
   writes it: D / 0 / / <file> / 10L / S. */
static void dict_write_id_item(const char *xpath, const char *name) {
    DH_FILE *dx = dh_open((char *)xpath);
    if (!dx) return;
    char rec[600];
    int n = snprintf(rec, sizeof rec, "D%c0%c%c%s%c10L%cS",
                     (char)0xFE, (char)0xFE, (char)0xFE, name,
                     (char)0xFE, (char)0xFE);
    STRING_CHUNK *c = mkchunk(rec, n);
    if (c) { dh_write(dx, "@ID", 3, c); s_free(c); }
    dh_close(dx);
}

int64_t mv_createfile(mv_ctx *ctx, const mv_value *spec, const mv_value *type) {
    if (!ctx || !spec || !spec->data || !spec->data[0]) return 0;
    const char *name = spec->data;
    const char *t = (type && type->data) ? type->data : "";
    int isdir = (strcasecmp(t, "DIR") == 0);

    char dpath[1024], xpath[1100];
    snprintf(dpath, sizeof dpath, "%s", name);
    snprintf(xpath, sizeof xpath, "%s.DIC", name);

    if (isdir) {
        if (mkdir(dpath, 0777) != 0 && errno != EEXIST) return 0;
    } else if (!dh_make(dpath)) {
        return 0;
    }
    if (!dh_make(xpath)) return 0;
    dict_write_id_item(xpath, name);
    if (!voc_write_file_entry(name, dpath, xpath)) return 0;

    char cls[64];
    return mv_fileclass(ctx, name, cls, sizeof cls);
}

/* Unmaking is the same three things in reverse, and the VOC entry goes LAST
   on the way in and FIRST on the way out: while it is there the account can
   still open a file whose groups have gone. */
static void rm_tree(const char *path) {
    char cmd[2200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    (void)system(cmd);
}

int64_t mv_deletefile(mv_ctx *ctx, const mv_value *spec) {
    if (!ctx || !spec || !spec->data || !spec->data[0]) return 0;
    const char *name = spec->data;
    char dpath[1024], xpath[1100];
    if (!voc_path(name, 0, dpath, sizeof dpath)) return 1;   /* already gone */
    if (!voc_path(name, 1, xpath, sizeof xpath))
        snprintf(xpath, sizeof xpath, "%s.DIC", name);

    DH_FILE *voc = dh_open("VOC");
    if (voc) {
        dh_delete(voc, (char *)name, (int16_t)strlen(name));
        dh_close(voc);
    }
    /* Drop it from our own cache too, or a later open hands back a DH_FILE
       for groups that are no longer there. */
    for (int i = 0; i < ctx->nfiles; i++) {
        if (strcmp(ctx->files[i].name, name) == 0 ||
            (strncmp(ctx->files[i].name, "DICT ", 5) == 0 &&
             strcmp(ctx->files[i].name + 5, name) == 0)) {
            if (ctx->files[i].dh) dh_close(ctx->files[i].dh);
            ctx->files[i].dh = NULL;
            ctx->files[i].dir[0] = '\0';
            ctx->files[i].name[0] = '\0';
        }
    }
    rm_tree(dpath);
    rm_tree(xpath);
    char cls[64];
    return mv_fileclass(ctx, name, cls, sizeof cls) == 0;
}

int64_t mv_fileclass(mv_ctx *ctx, const char *name, char *out, size_t cap) {
    (void)ctx;
    if (cap) out[0] = '\0';
    char path[1024];
    if (!name || !*name || !voc_path(name, 0, path, sizeof path)) return 0;
    struct stat sb;
    if (stat(path, &sb) != 0 || !S_ISDIR(sb.st_mode)) return 0;
    char probe[1100];
    snprintf(probe, sizeof probe, "%s/~0", path);
    int hash = (stat(probe, &sb) == 0);
    if (!hash) {
        snprintf(probe, sizeof probe, "%s/%%0", path);
        hash = (stat(probe, &sb) == 0);
    }
    if (!hash) { snprintf(out, cap, "DIR"); return 1; }
    /* The modulus straight from the file, which is the whole advantage of
       being in-process: the CLI has to parse ANALYZE.FILE for this. */
    DH_FILE *dh = dh_open(path);
    int32_t mod = dh ? dh_modulus(dh) : 0;
    if (dh) dh_close(dh);
    if (mod > 0) snprintf(out, cap, "hash %ld DYNAMIC", (long)mod);
    else         snprintf(out, cap, "hash 1 DYNAMIC");
    return 1;
}

void mv_filelist(mv_ctx *ctx, mv_value *dst) {
    (void)ctx;
    mv_set_str(dst, "", 0);
    DH_FILE *voc = dh_open("VOC");
    if (!voc) return;
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    if (!buf) mv_fatal("out of memory");
    dh_select(voc, (int16_t)SEL_LIST);
    dh_complete_select((int16_t)SEL_LIST);
    DESCRIPTOR *d = SelectList(SEL_LIST);
    if (d && d->type == STRING && d->data.str.saddr) {
        int64_t il = 0;
        char *ids = chunk_bytes(d->data.str.saddr, &il);
        int64_t i = 0;
        while (i < il) {
            int64_t st = i;
            while (i < il && (unsigned char)ids[i] != 0xFE) i++;
            char nm[256];
            size_t nl = (size_t)(i - st);
            if (i < il) i++;
            if (nl == 0 || nl >= sizeof nm) continue;
            memcpy(nm, ids + st, nl); nm[nl] = '\0';
            char cls[64];
            if (!voc_owns(nm)) continue;          /* @QMSYS pointers are not ours */
            if (!mv_fileclass(ctx, nm, cls, sizeof cls)) continue;
            const char *type = (strcmp(cls, "DIR") == 0) ? "DIR" : "hash";
            size_t need = nl + strlen(type) + 3;
            if (n + need >= cap) { cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) mv_fatal("out of memory");
                buf = nb; }
            if (n) buf[n++] = (char)0xFE;
            memcpy(buf + n, nm, nl); n += nl;
            buf[n++] = (char)0xFD;
            memcpy(buf + n, type, strlen(type)); n += strlen(type);
        }
        free(ids);
    }
    dh_end_select((int16_t)SEL_LIST);
    dh_close(voc);
    mv_set_str(dst, buf, (int64_t)n);
    free(buf);
}

/* The account's alternate keys, @AM-separated -- read from the file's OWN AK
   table rather than from a verb's printed output, which is what the CLI has to
   parse.  `ak_data' is a matrix of AKD_COLS columns per index and AKD_NAME
   holds the name; op_indices1() walks it exactly this way, which is where the
   shape comes from rather than from guesswork. */
int64_t mv_indices(mv_ctx *ctx, const char *name, char *out, size_t cap) {
    (void)ctx;
    if (cap) out[0] = '\0';
    char path[1024];
    if (!name || !*name || !voc_path(name, 0, path, sizeof path)) return 0;
    DH_FILE *dh = dh_open(path);
    if (!dh) return 0;
    size_t n = 0;
    ARRAY_HEADER *ak = dh->ak_data;
    if (ak) {
        for (int32_t i = 0; i < ak->rows; i++) {
            DESCRIPTOR *d = Element(ak, (i * AKD_COLS) + AKD_NAME);
            if (!d || d->type != STRING || !d->data.str.saddr) continue;
            STRING_CHUNK *sc = d->data.str.saddr;
            size_t len = (size_t)sc->bytes;          /* always one chunk */
            if (len == 0 || n + len + 1 >= cap) continue;
            if (n) out[n++] = (char)0xFE;
            memcpy(out + n, sc->data, len);
            n += len;
        }
    }
    out[n] = '\0';
    dh_close(dh);
    return n > 0;
}

/* OWNERSHIP, not reachability: a pointer to @QMSYS is openable but is not this
   account's file, and treating it as one put QMSYS's files in the commit. */
static int voc_owns(const char *name) {
    DH_FILE *voc = dh_open("VOC");
    if (!voc) return 0;
    STRING_CHUNK *r = dh_read(voc, (char *)name, (int16_t)strlen(name), NULL);
    int own = 0;
    if (r) {
        int64_t rl = 0;
        char *rec = chunk_bytes(r, &rl);
        char type[16], p[1024];
        if (rec_attr(rec, rl, 1, type, sizeof type) &&
            (type[0] == 'F' || type[0] == 'f') &&
            rec_attr(rec, rl, 2, p, sizeof p))
            own = path_is_local(p);
        free(rec);
        s_free(r);
    }
    dh_close(voc);
    return own;
}

int mv_qm_is_file(const char *name) {
    char path[1024];
    (void)path;
    size_t nl = name ? strlen(name) : 0;
    char base[300];
    if (nl > 5 && nl - 5 < sizeof base && strcmp(name + nl - 5, ".DICT") == 0) {
        memcpy(base, name, nl - 5); base[nl - 5] = '\0';
        name = base;
    }
    return name && *name && voc_owns(name);
}

int mv_openaccount(void) {
    const char *e = getenv("MVX_OPENACCOUNT");
    return e && *e && *e != '0';
}

int mv_voc_class(const char *type, int64_t len) {
    static const struct { const char *t; int c; } tbl[] = {
        {"V", 1}, {"K", 1}, {"I", 1},
        {"F", 2}, {"Q", 2}, {"R", 2}, {"DIR", 2},
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
    fputs("qm-git(verb): ", stderr);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    /* NOT exit(): we are inside the caller's QM session, and taking the
       process down takes their session with it. */
    abort();
}
