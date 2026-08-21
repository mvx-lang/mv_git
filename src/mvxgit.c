/*
 * MVX — a native compiler and runtime for Pick/MultiValue BASIC.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.  There is NO WARRANTY, to
 * the extent permitted by law; see the LICENSE file for details.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Native git for hash-file records, via libgit2 — modelled on real git.
 *
 * The working tree is the live records in an MVX file; the index (at
 * <repo>/index) is git's staging area; and commits are commits, in the
 * account's own git repository (its .git).  A record maps to the git path
 * "<file>/<id>", so ordinary git tooling and hosts read the history.
 * Attribute marks translate to newlines in the blob (and back on restore) so
 * diffs are line-oriented.
 *
 * These are cataloged subroutines (the MVX subroutine ABI), CALLed by
 * the BASIC GIT verb.  libgit2 links into this library alone; records
 * flow through the runtime storage API called from here — nothing
 * touches a filesystem working tree.  Library calls, not exec: any
 * privilege tier.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* expose gmtime_r in <time.h> under -std=c11 */
#endif

#include "mvxgit.h"      /* selects the record backend at compile time */

#include <ctype.h>
#include <dirent.h>
#include <fnmatch.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <strings.h>      /* strcasecmp / strncasecmp */
#include <sys/stat.h>
#include <unistd.h>

/* --- helpers ----------------------------------------------------------- */

static void ensure_init(void) {
    static int done;
    if (!done) { git_libgit2_init(); done = 1; }
}

static void arg_str(const mv_value *v, char *out, size_t cap) {
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(v, nb, sizeof nb, &p);
    if ((size_t)n >= cap) n = (int64_t)cap - 1;
    memcpy(out, p, (size_t)n);
    out[n] = '\0';
}

static void fail(mv_value *out, const char *what) {
    const git_error *e = git_error_last();
    char buf[512];
    snprintf(buf, sizeof buf, "git: %s: %s", what,
             e && e->message ? e->message : "failed");
    mv_set_str(out, buf, (int64_t)strlen(buf));
}

static char *xlate(const char *p, int64_t n, char from, char to,
                   int64_t *outn) {
    char *b = malloc(n ? (size_t)n : 1);
    if (!b) mv_fatal("out of memory in git translate");
    for (int64_t i = 0; i < n; i++) b[i] = p[i] == from ? to : p[i];
    *outn = n;
    return b;
}

typedef struct { char *d; size_t len, cap; } sbuf;

static void sb_put(sbuf *s, const char *p, size_t n) {
    if (s->len + n + 1 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        while (s->cap < s->len + n + 1) s->cap *= 2;
        s->d = realloc(s->d, s->cap);
        if (!s->d) mv_fatal("out of memory in git output");
    }
    memcpy(s->d + s->len, p, n);
    s->len += n;
}

static void sb_line(sbuf *s, const char *line) {
    if (s->len) { char am = (char)0xFE; sb_put(s, &am, 1); }
    sb_put(s, line, strlen(line));
}

static void sb_out(sbuf *s, mv_value *dst, const char *empty) {
    if (s->d) mv_set_str(dst, s->d, (int64_t)s->len);
    else mv_set_str(dst, empty, (int64_t)strlen(empty));
    free(s->d);
}

#ifdef MVXGIT_UDT
/* UniData D-item <-> open-dict attribute remap (mvx#25 open-dict interchange).
   A UniData dictionary D/I item is  TYP LOC CONV NAME FORMAT SM ASSOC — single/
   multi at attribute 6, association at 7; the canonical open form is mvx-shaped,
   with ASSOC at 6 and SM at 7.  Swapping attributes 6 and 7 converts EITHER way
   (an involution), so one function serves both commit (native->open) and
   materialise (open->native).  Trailing empty attributes are trimmed so a
   no-association item does not grow an attribute across a round trip.  Only D/I
   items are remapped; every other record (PH, %FILE%, %INDEXES%, …) passes
   through unchanged (returns NULL).  On MVX this is absent — its D-items already
   ARE the open form.  Returns a malloc'd buffer + *outlen, or NULL. */
static char *dict_item_swap(const char *rec, int64_t len, int64_t *outlen) {
    const char *att[32];
    int64_t alen[32];
    int na = 0;
    const char *s = rec, *e = rec + len;
    while (na < 32) {
        const char *m = memchr(s, (char)0xFE, (size_t)(e - s));
        att[na] = s;
        alen[na] = (m ? m : e) - s;
        na++;
        if (!m) break;
        s = m + 1;
    }
    if (na == 0 || alen[0] != 1 || (att[0][0] != 'D' && att[0][0] != 'I'))
        return NULL;                    /* only D/I items carry SM/assoc */
    int slots = na < 7 ? 7 : na;
    sbuf sb = {0};
    for (int i = 0; i < slots; i++) {
        int src = i == 5 ? 6 : (i == 6 ? 5 : i);   /* swap attrs 6 and 7 */
        if (i) { char am = (char)0xFE; sb_put(&sb, &am, 1); }
        if (src < na) sb_put(&sb, att[src], (size_t)alen[src]);
    }
    while (sb.len && (unsigned char)sb.d[sb.len - 1] == 0xFE) sb.len--;  /* trim tail */
    *outlen = (int64_t)sb.len;
    if (!sb.d) { char *z = malloc(1); return z; }   /* empty but non-NULL */
    return sb.d;
}
#endif

/* --- ignore rules ------------------------------------------------------
   The account's GITIGNORE record (a file in the account root, one glob
   per line) keeps bulk data out of history: ignore "ORDERS" to skip a
   million order records while "ORDERS.DICT" (the dictionary) stays
   trackable.  A pattern matches either the file name or the full
   "file/record" path. */
static char g_ign[128][256];
static int g_nign, g_ign_loaded;

static void load_ignores(void) {
    if (g_ign_loaded) return;
    g_ign_loaded = 1;
    g_nign = 0;
    const char *acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";
    char path[4096];
    snprintf(path, sizeof path, "%s/GITIGNORE", acct);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char ln[512];
    while (fgets(ln, sizeof ln, fp) && g_nign < 128) {
        size_t n = strlen(ln);
        while (n && (ln[n - 1] == '\n' || ln[n - 1] == '\r' ||
                     ln[n - 1] == ' '))
            ln[--n] = '\0';
        char *p = ln;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        snprintf(g_ign[g_nign++], 256, "%s", p);
    }
    fclose(fp);
}

/* --- the account's place in its repository (mv_git#44) --------------------
 *
 * A repository may hold several accounts, and then a record cannot be committed
 * at `<file>/<id>`: two accounts would both claim `CUST/C1` and silently
 * overwrite each other.  Each account's records therefore live under its own
 * directory, `<account>/<file>/<id>`, which is the layout the mvx repository
 * already uses.
 *
 * The engine works INSIDE one account at a time — it chdir's there and every
 * record primitive is account-relative — so the prefix is applied at exactly one
 * boundary: where a record's git path is built.  The caller sets it once per
 * account; empty (the default) means the account IS the repository root, which
 * is every single-account package and therefore unchanged. */
static char g_prefix[256];
void mv_git_forget_account(void);

void mv_git_set_prefix(const char *p) {
    /* The prefix changes exactly when the ACCOUNT changes, so this is also the
       point at which anything cached about the account stops being true.  The
       backend's file list is the one that matters: reused across a multi-account
       walk it would answer account B with account A's files. */
    mv_git_forget_account();
    if (!p || !*p) { g_prefix[0] = '\0'; return; }
    size_t n = strlen(p);
    if (n && p[n - 1] == '/')
        snprintf(g_prefix, sizeof g_prefix, "%s", p);
    else
        snprintf(g_prefix, sizeof g_prefix, "%s/", p);
}

/* The git path of a record: `<prefix><file>/<id>`. */
static void record_path(char *out, size_t cap, const char *fn, const char *id) {
    snprintf(out, cap, "%s%s/%s", g_prefix, fn, id);
}

/* The account-relative part of a repo path, or NULL when the path belongs to a
   different account.  Used where a name has to travel back the other way —
   status derives which FILES to scan from the paths already in the index. */
static const char *unprefix(const char *path) {
    if (!g_prefix[0]) return path;
    size_t n = strlen(g_prefix);
    return strncmp(path, g_prefix, n) == 0 ? path + n : NULL;
}

/* True for a path that is a dictionary's %FILE% control (…/…DICT/%FILE%). */
static int is_file_control(const char *path) {
    size_t pl = strlen(path);
    const char *suf = ".DICT/%FILE%";
    size_t sl = strlen(suf);
    return pl >= sl && strcmp(path + pl - sl, suf) == 0;
}

/* True when a control's content is in the OPEN interchange form — the portable
   class "DIR" or "hash …", possibly followed by `key = value` parameter lines —
   as opposed to an account's own native %FILE% record (FILE<VM>type<VM>conn).
   The two share a path and mean different things, which is why anything holding
   one has to be able to tell them apart. */
static int is_open_control(const char *c, int64_t cl) {
    if (!c) return 0;
    if (cl >= 3 && strncasecmp(c, "DIR", 3) == 0) return 1;
    if (cl >= 4 && strncasecmp(c, "hash", 4) == 0) return 1;
    return 0;
}

static int ignored(const char *file, const char *path) {
    load_ignores();
    for (int i = 0; i < g_nign; i++) {
        if (fnmatch(g_ign[i], file, 0) == 0) return 1;
        if (fnmatch(g_ign[i], path, FNM_PATHNAME) == 0) return 1;
    }
    return 0;
}

/* True when a record's git PATH is excluded by .gitignore.  `ignored()` above
   consults the account's own GIT IGNORE list (bulk MV data); this consults git's
   ordinary .gitignore, so a record git's plain `add` never enumerates — an lmdb
   record, or a directory-file record whose path a rule names (e.g. `VOC/LIB`) —
   is honoured the same way, in both `add` and `status`.  A NULL repo or a rule
   lookup error means "not ignored". */
static int git_path_ignored(git_repository *repo, const char *path) {
    int ig = 0;
    return repo && git_ignore_path_is_ignored(&ig, repo, path) == 0 && ig;
}

/* Seed $MVX_OPENACCOUNT from a repo's mvx.openaccount config so the engine
   honours open-account mode when driven by the GIT verb.  The CLI sets this env
   via apply_open_env before calling the engine; the verb path (and D3, which has
   no CLI at all) did not — so read it straight from .git/config here.  Only ever
   SETS (never clears), leaving an explicit env from the CLI untouched.  Call at
   the top of a sub, before any mv_openaccount() (which reads the env, uncached). */
static void openaccount_sync(const char *rp) {
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) return;
    git_config *cfg = NULL;
    int on = 0;
    if (git_repository_config(&cfg, repo) == 0) {
        int b = 0;
        if (git_config_get_bool(&b, cfg, "mvx.openaccount") == 0) {
            on = b;
        } else {
            git_buf s = GIT_BUF_INIT;    /* get_bool differs across libgit2 builds */
            if (git_config_get_string_buf(&s, cfg, "mvx.openaccount") == 0 && s.ptr)
                on = strcasecmp(s.ptr, "true") == 0 || strcmp(s.ptr, "1") == 0;
            git_buf_dispose(&s);
        }
        git_config_free(cfg);
    }
    git_repository_free(repo);
    if (on) setenv("MVX_OPENACCOUNT", "1", 1);
}

/* A well-known BUILD provisioning pointer: a `CATALOG` item in VOC/MD pointing
   at the account's local CATALOG directory.  BUILD (re)creates it when it
   provisions an account — alongside the CATALOG binaries and index B-trees it
   rebuilds — so it is derived local plumbing, never part of the committed
   portable form.  So a wholesale add (`add -A` or `add VOC`) skips it and status
   never reports it as untracked — a fresh open clone reads clean after BUILD
   runs (mvx#77).  An explicit `add VOC CATALOG` still stages it (the escape
   hatch), the same shape as the compiled-object skip (mv_git#9). */
/* Is this record a compiled object rather than content?
 *
 * UniData writes the object of <PROG> as _<PROG> in the SAME file — an
 * underscore PREFIX, measured on 8.3: compiling BP/GIT.AGENT produces
 * BP/_GIT.AGENT.  (It is not a ".O" suffix; that is UniVerse, where the object
 * lives in a separate FILE named <X>.O and is excluded a level up, by file.)
 * Checked by name and only when the source really is there, so a record
 * legitimately called _SOMETHING is never lost.
 *
 * $MVX_GIT_SKIP_OBJECTS keeps the old content test as a backstop for whatever
 * the naming rule does not cover: an object is binary, and no record this tool
 * should version contains a NUL.
 *
 * ADD AND STATUS BOTH CALL THIS, which is the whole point of it being a
 * function.  Every time those two have disagreed about what to skip, the result
 * was the same bug: status reports a record that add will never stage, so the
 * account is dirty forever and no commit can clear it.
 */
static int record_is_object(mv_ctx *ctx, mv_value *fvar, const char *idb,
                            const char *cp, int64_t clen) {
    if (idb[0] == '_' && idb[1]) {
        mv_value bid, brec;
        mv_init(&bid); mv_init(&brec);
        mv_set_str(&bid, idb + 1, (int64_t)(strlen(idb) - 1));
        int have_src = mv_read(ctx, &brec, fvar, &bid, 0);
        mv_clear(&bid); mv_clear(&brec);
        if (have_src) return 1;
    }
    static int backstop = -1;
    if (backstop < 0) backstop = getenv("MVX_GIT_SKIP_OBJECTS") != NULL;
    if (backstop && cp && clen > 0 && memchr(cp, 0, (size_t)clen)) return 1;
    return 0;
}

static int is_provision_pointer(const char *file, const char *id) {
    if (strcasecmp(id, "CATALOG") != 0) return 0;
    return strcasecmp(file, "VOC") == 0 || strcasecmp(file, "MD") == 0;
}

/* Open an MVX file by its git name: a trailing ".DICT" opens the
   dictionary of the base file, so dictionaries are trackable as
   "<file>.DICT". */
static int open_named(mv_ctx *ctx, const char *name, mv_value *fvar) {
    size_t n = strlen(name);
    mv_value spec;
    mv_init(&spec);
    if (n > 5 && strcmp(name + n - 5, ".DICT") == 0) {
        mv_value dictv;
        mv_init(&dictv);
        mv_set_str(&dictv, "DICT", 4);
        mv_set_str(&spec, name, (int64_t)(n - 5));
        int r = mv_open(ctx, &dictv, &spec, fvar);
        mv_clear(&dictv);
        mv_clear(&spec);
        return r;
    }
    mv_set_str(&spec, name, (int64_t)n);
    int r = mv_open(ctx, NULL, &spec, fvar);
    mv_clear(&spec);
    return r;
}

/* Init the account's git repository (initial branch main).  The path is the
   account's ".git" directory, so this is an ordinary (non-bare) repo that git
   and hosts like GitHub recognise — the account directory is its working tree.
   The working tree "checks out incorrectly" under plain git (records are the
   real working tree, not files), which is fine: the .mvx descriptor marks the
   directory as an MVX account, and the account tooling rebuilds the records. */
static int init_bare_main(git_repository **repo, const char *path) {
    /* A non-bare repo is initialised at its working directory (the account
       root), where git puts the gitdir at ".git".  Callers pass the gitdir
       (".git"); its working tree is the parent, i.e. the current directory the
       caller has already entered. */
    (void)path;
    git_repository_init_options o = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    o.flags = GIT_REPOSITORY_INIT_MKPATH;
    o.initial_head = "main";
    return git_repository_init_ext(repo, ".", &o);
}

/* Open (init if needed) the bare repo, and its persistent index. */
static int repo_open(const char *path, git_repository **repo,
                     git_index **index) {
    if (git_repository_open(repo, path) != 0 &&
        init_bare_main(repo, path) != 0)
        return -1;
    if (index) {
        char ipath[4200];
        /* Use the RESOLVED git dir — git_repository_path handles a submodule's
           gitlink `.git` FILE; "<path>/index" would be ".git/index" and fail
           with ENOTDIR when .git is a file rather than a directory. */
        const char *gd = git_repository_path(*repo);
        snprintf(ipath, sizeof ipath, "%sindex", gd ? gd : "");
        if (git_index_open(index, ipath) != 0) {
            git_repository_free(*repo);
            return -1;
        }
        git_index_read(*index, 0);      /* load if present */
    }
    return 0;
}

static git_tree *head_tree(git_repository *repo) {
    git_object *h = NULL;
    git_commit *c = NULL;
    git_tree *t = NULL;
    if (git_revparse_single(&h, repo, "HEAD") == 0 &&
        git_commit_lookup(&c, repo, git_object_id(h)) == 0)
        git_commit_tree(&t, c);
    if (c) git_commit_free(c);
    if (h) git_object_free(h);
    return t;
}

/* Copy the open-form %FILE% control committed for `base` (HEAD's
   <base>.DICT/%FILE%) into out[cap]; returns its length, or -1 if `base` has no
   committed control in `head`.  Reads from an already-open repo/tree. */
static int head_control(git_repository *repo, git_tree *head, const char *base,
                        char *out, size_t cap) {
    if (!head) return -1;
    char path[600];
    snprintf(path, sizeof path, "%s.DICT/%%FILE%%", base);
    git_tree_entry *te = NULL;
    if (git_tree_entry_bypath(&te, head, path) != 0) return -1;
    int n = -1;
    git_blob *b = NULL;
    if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) == 0) {
        const char *c = git_blob_rawcontent(b);
        int64_t cl = (int64_t)git_blob_rawsize(b);
        if (cl >= 0 && (size_t)cl < cap) {
            memcpy(out, c, (size_t)cl);
            out[cl] = '\0';
            n = (int)cl;
        }
        git_blob_free(b);
    }
    git_tree_entry_free(te);
    return n;
}

/* Blob oid of a record's current content (translated, not stored). */
static int record_oid(const char *content, int64_t len, git_oid *oid) {
    int64_t bl;
    char *b = xlate(content, len, (char)0xFE, '\n', &bl);
    int rc = git_odb_hash(oid, b, (size_t)bl, GIT_OBJECT_BLOB);
    free(b);
    return rc;
}

/* --- the stock account baseline (mv_git#46) -------------------------------
 *
 * An account created on UniVerse is born with a VOC full of records nobody
 * wrote: 847 of them for the PICK flavour, 840 for Ideal, 851 for Reality — the
 * verbs, keywords and pointers the system supplies.  Committing them makes a
 * repository where 92% of the content is furniture, buries the real change in a
 * diff, and — worse — restores one release's stock VOC over another's on
 * checkout, silently.
 *
 * So a record identical to the one a fresh account of the same flavour would
 * have is not this account's content, and is not committed.  What IS committed
 * is what the account added, and what it changed: the comparison is by content,
 * not just by name, so an edited stock verb still travels.
 *
 * THE BASELINE IS SUPPLIED, NOT DISCOVERED.  Building it means standing up an
 * account of the right flavour and reading its VOC, which needs a session and is
 * therefore platform work; the driver does it and hands the file over here.  All
 * the engine needs is a set of (id, content) pairs to recognise, which is
 * platform-neutral — and being a plain file, it is equally available to the
 * verb route through mvgitd and to the CLI, which matters now that both are
 * kept (DECISIONS.md).
 *
 * The file is per clone, not committed: the stock VOC belongs to a particular
 * UniVerse release, and a committed baseline would subtract 14.2's furniture
 * from a 14.3 account.  Format is one `<oid> <id>` per line, `#` comments.
 *
 * NOT HANDLED: an account that deliberately DELETES a stock record.  It looks
 * identical to one that never had it, so checkout puts it back.  Recording that
 * needs a tombstone, and it is deliberately out of this first cut.
 */
typedef struct { char oid[41]; char id[256]; } stock_rec;
static stock_rec *g_stock;
static int g_stock_n, g_stock_cap, g_stock_loaded;
static char g_stock_path[4096];

void mv_git_set_stock(const char *path) {
    free(g_stock);
    g_stock = NULL;
    g_stock_n = g_stock_cap = 0;
    g_stock_loaded = 0;
    snprintf(g_stock_path, sizeof g_stock_path, "%s", path ? path : "");
}

/* The baseline as IDS ALONE, handed over by BASIC.
 *
 * In a session the engine cannot read records at all — that is the licence
 * guard (-DMVXGIT_INSESSION, mv_git#54): a record primitive here would open a
 * SECOND session into the account we are already inside.  So in-session it
 * cannot build the baseline itself, and without one a checkout treats every
 * stock VOC record as "extra" and DELETES it — a pull silently stripping the
 * destination's own VOC, CT and all.
 *
 * Ids are enough for that job.  Deletion asks `is_stock_id`, which never looks
 * at content, and ids are plain ASCII — so the whole baseline is one call
 * instead of a per-record protocol.  BASIC can reach the master VOC (GIT.ADD
 * already does, through a temporary F-pointer) and hands the list over here.
 *
 * Content-sensitive callers keep working the way they already do: in-session
 * `add` compares against the live master in BASIC, and the CLI builds the full
 * <oid> <id> file. */
void mv_git_stock_ids(const char *ids) {
    free(g_stock);
    g_stock = NULL;
    g_stock_n = g_stock_cap = 0;
    g_stock_path[0] = '\0';
    g_stock_loaded = 1;                 /* supplied, so never load a file over it */
    if (!ids) return;
    const char *p = ids;
    while (*p) {
        const char *e = p;
        while (*e && (unsigned char)*e != 0xFE && *e != '\n') e++;
        if (e > p) {
            if (g_stock_n >= g_stock_cap) {
                g_stock_cap = g_stock_cap ? g_stock_cap * 2 : 1024;
                stock_rec *ns = realloc(g_stock, (size_t)g_stock_cap * sizeof *ns);
                if (!ns) return;
                g_stock = ns;
            }
            g_stock[g_stock_n].oid[0] = '\0';
            int n = (int)(e - p);
            if (n > 255) n = 255;
            memcpy(g_stock[g_stock_n].id, p, (size_t)n);
            g_stock[g_stock_n].id[n] = '\0';
            g_stock_n++;
        }
        if (!*e) break;
        p = e + 1;
    }
}

static void stock_load(void) {
    if (g_stock_loaded) return;
    g_stock_loaded = 1;
    if (!g_stock_path[0]) return;
    FILE *f = fopen(g_stock_path, "r");
    if (!f) return;
    char ln[512];
    while (fgets(ln, sizeof ln, f)) {
        if (ln[0] == '#' || ln[0] == '\n') continue;
        size_t n = strlen(ln);
        while (n && (ln[n-1] == '\n' || ln[n-1] == '\r')) ln[--n] = '\0';
        if (n < 42 || ln[40] != ' ') continue;
        if (g_stock_n >= g_stock_cap) {
            g_stock_cap = g_stock_cap ? g_stock_cap * 2 : 1024;
            stock_rec *ns = realloc(g_stock, (size_t)g_stock_cap * sizeof *ns);
            if (!ns) { fclose(f); return; }
            g_stock = ns;
        }
        memcpy(g_stock[g_stock_n].oid, ln, 40);
        g_stock[g_stock_n].oid[40] = '\0';
        snprintf(g_stock[g_stock_n].id, sizeof g_stock[g_stock_n].id, "%s", ln + 41);
        g_stock_n++;
    }
    fclose(f);
}

/* True when an id belongs to the stock account, whatever its content.
   Used where the question is "may this be deleted", not "should it be staged":
   a stock record the user has EDITED differs in content, so it IS committed and
   the caller finds it there — meaning an id-only test never protects anything
   that should have been removed, and it costs no extra record read. */
static int is_stock_id(const char *id) {
    stock_load();
    for (int i = 0; i < g_stock_n; i++)
        if (strcmp(g_stock[i].id, id) == 0) return 1;
    return 0;
}

/* True when this record is exactly what a fresh account of the same flavour
   would hold — same id, same content — and so is not the account's own. */
static int is_stock_record(const char *id, const char *content, int64_t len) {
    stock_load();
    if (!g_stock_n) return 0;
    git_oid oid;
    if (record_oid(content, len, &oid) != 0) return 0;
    char hex[GIT_OID_HEXSZ + 1];
    git_oid_fmt(hex, &oid);
    hex[GIT_OID_HEXSZ] = '\0';
    for (int i = 0; i < g_stock_n; i++)
        if (strcmp(g_stock[i].id, id) == 0 &&
            strcmp(g_stock[i].oid, hex) == 0)
            return 1;
    return 0;
}

#if defined(MVXGIT_UDT)
/* UniData supplies its baseline as a FILE, so nothing has to be stood up.
 *
 * `newacct` copies the master VOC into the new account verbatim — its own
 * strings say `cp %ssys/VOC` / "cp MASTERVOC to VOC" — so $UDTHOME/sys/VOC IS
 * the stock account's VOC, exact for the release actually running.  Measured on
 * 8.3: a fresh account's 617 VOC records match the master with ZERO content
 * differences, leaving 7 records that are genuinely the account's own.
 *
 * Reaching it needs no new machinery.  UniData has no OPENPATH and will not
 * open a hash file by path, but a VOC F-pointer makes any path an ordinary file
 * to OPEN — and WRITE/OPEN/SELECT/READNEXT/READ/DELETE are already the record
 * contract, so this works identically for the CLI (agent_rt.c) and the
 * in-session verb (udtgit_rt.c) with one implementation.  The pointer is
 * removed again immediately; it exists only for the length of the scan.
 *
 * Contrast UniVerse, where the template is per FLAVOUR and not readable as a
 * file, so uv-git stands up a throwaway account instead (build_stock).  Same
 * baseline, same file format, different way of getting there — which is why the
 * engine takes a supplied file rather than knowing about either. */
#define STOCK_PTR "%GITSTOCK%"

static int stock_build_udt(mv_ctx *ctx, const char *master, const char *mdict,
                           const char *out) {
    mv_value voc, ptr, id, rec, stk;
    mv_init(&voc); mv_init(&ptr); mv_init(&id); mv_init(&rec); mv_init(&stk);
    int n = -1;
    if (!open_named(ctx, "VOC", &voc)) goto done;

    char body[9000];
    int bl = snprintf(body, sizeof body, "F%c%s%c%s", 0xFE, master, 0xFE, mdict);
    mv_set_str(&rec, body, (int64_t)bl);
    mv_set_str(&id, STOCK_PTR, (int64_t)strlen(STOCK_PTR));
    if (!mv_write(ctx, &rec, &voc, &id, 0, 0)) goto done;

    if (open_named(ctx, STOCK_PTR, &stk)) {
        FILE *f = fopen(out, "w");
        if (f) {
            fprintf(f, "# stock VOC for this UniData release — generated here,\n"
                       "# never committed: it belongs to %s (mv_git#46).\n",
                    master);
            n = 0;
            mv_select(ctx, &stk);
            while (mv_readnext(ctx, &id)) {
                if (!mv_read(ctx, &rec, &stk, &id, 0)) continue;
                const char *cp;
                char nb[40];
                int64_t cl = mv_val_chars(&rec, nb, sizeof nb, &cp);
                git_oid oid;
                if (record_oid(cp, cl, &oid) != 0) continue;
                char hex[GIT_OID_HEXSZ + 1];
                git_oid_fmt(hex, &oid);
                hex[GIT_OID_HEXSZ] = '\0';
                char sid[256];
                arg_str(&id, sid, sizeof sid);
                fprintf(f, "%s %s\n", hex, sid);
                n++;
            }
            fclose(f);
        }
    }
    /* Always, including on every failure above: a stray pointer would be
       committed as if it were the account's own record. */
    mv_set_str(&id, STOCK_PTR, (int64_t)strlen(STOCK_PTR));
    mv_delete_rec(ctx, &voc, &id);
done:
    mv_clear(&voc); mv_clear(&ptr); mv_clear(&id); mv_clear(&rec); mv_clear(&stk);
    return n;
}

/* Point the engine at this clone's baseline, building it the first time.  Once
   per process, and cached per clone beside git's own local-only state. */
static void stock_ensure_udt(mv_ctx *ctx, const char *rp) {
    static int done;
    if (done || g_stock_path[0]) return;
    done = 1;
    const char *home = getenv("UDTHOME");
    if (!home || !home[0]) return;
    char master[4096], mdict[4096];
    snprintf(master, sizeof master, "%s/sys/VOC", home);
    snprintf(mdict, sizeof mdict, "%s/sys/D_VOC", home);
    if (access(master, R_OK) != 0) return;

    git_repository *r = NULL;
    if (git_repository_open(&r, rp) != 0) return;
    char dir[4096], path[4300];
    snprintf(dir, sizeof dir, "%smvgit", git_repository_path(r));
    git_repository_free(r);
    mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/stock-udt", dir);

    if (access(path, R_OK) != 0) {
        fprintf(stderr, "git: learning what a stock UniData account holds "
                        "(once per clone)\n");
        if (stock_build_udt(ctx, master, mdict, path) < 0) {
            unlink(path);
            fprintf(stderr, "git: could not read %s; commits will carry the "
                            "system's own VOC records (mv_git#46)\n", master);
            return;
        }
    }
    mv_git_set_stock(path);
}
#else
#define stock_ensure_udt(ctx, rp) ((void)0)
#endif

/* --- GITINIT(repo, out) ------------------------------------------------ */
void mvx_sub_GITINIT(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) == 0) {
        git_repository_free(repo);
        mv_set_str(argv[1], "already a repository", 20);
        return;
    }
    if (init_bare_main(&repo, rp) != 0) {
        fail(argv[1], "init");
        return;
    }
    git_repository_free(repo);
    mv_set_str(argv[1], "initialised empty git repository", 32);
}

/* Stage the current content of one MVX file's records (or one record)
   into the index.  GITADD(repo, file, record-or-empty, out) */
void mvx_sub_GITADD(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 4) return;
    ensure_init();
    char rp[4096], fn[256], only[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], fn, sizeof fn);
    arg_str(argv[2], only, sizeof only);
    /* `GIT ADD BP *` means the whole file, not a record called "*".  MV has no
       shell to expand it, so it arrives literally and used to be looked up as an
       id — which found nothing and staged nothing.  Folding it to "no record
       named" here makes it the wholesale add it reads as, which also means the
       object-file exclusion applies to it: `ADD BP *` skips objects exactly as
       `ADD BP` and `ADD -A` do, and only naming an object stages one. */
    if (strcmp(only, "*") == 0) only[0] = '\0';
    openaccount_sync(rp);               /* verb path: honour mvx.openaccount */

    /* Committing the master VOC keeps the user's own items — paragraphs,
       sentences, menus, phrases (portable PROCs that must run on the other
       platform) — but never the system entries a fresh account auto-creates.
       Verbs (V, a native binary pointer) and keywords (K) are ALWAYS dropped,
       open format or not: they belong to the source system and the destination
       supplies its own — true even between two UniData versions.  The open
       interchange additionally drops the platform-specific file/Q/remote
       pointers (F/LF/DF/DIR/Q/X/R), since the portable file type travels as the
       synthesised <file>.DICT/%FILE%; a native commit keeps those so it can be
       checked out into another instance of the same platform. */
    int is_voc = strcasecmp(fn, "VOC") == 0 || strcasecmp(fn, "MD") == 0;
    int voc_open = is_voc && mv_openaccount();
#ifdef MVXGIT_UDT
    /* An open-account dictionary commits its D/I items in the canonical (mvx-
       shaped) open form: UniData's SM/assoc attribute order is remapped here. */
    size_t fnlen = strlen(fn);
    int dict_open = fnlen > 5 && strcmp(fn + fnlen - 5, ".DICT") == 0 &&
                    mv_openaccount();
#endif
    /* A blanket `add -A` sets $MVX_GIT_SKIP_OBJECTS so compiled BASIC objects
       (binary records holding a NUL — derived, rebuilt on the target by
       CATALOG/BUILD) are not committed by default; an explicit `add <file>`
       does not set it and stages everything (mv_git#9). */
    int skip_objects = getenv("MVX_GIT_SKIP_OBJECTS") != NULL;

    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[3], "open"); return; }

    if (ignored(fn, fn)) {
        char out[300];
        snprintf(out, sizeof out, "%s is in GITIGNORE, nothing staged", fn);
        mv_set_str(argv[3], out, (int64_t)strlen(out));
        git_index_free(index);
        git_repository_free(repo);
        return;
    }

    mv_value fvar, id, rec;
    mv_init(&fvar); mv_init(&id); mv_init(&rec);
    int64_t n = 0, skipped = 0;
    if (open_named(ctx, fn, &fvar)) {
        int one = only[0] != '\0';
        int have = 1;
        if (one) {
            mv_set_str(&id, only, (int64_t)strlen(only));
            have = mv_read(ctx, &rec, &fvar, &id, 0);
        } else {
            mv_select(ctx, &fvar);
        }
        while (have) {
            if (!one) {
                if (!mv_readnext(ctx, &id)) break;
                if (!mv_read(ctx, &rec, &fvar, &id, 0)) continue;
            }
            if (is_voc) {               /* drop auto-created system VOC items */
                const char *vp;
                char vnb[40];
                int64_t vl = mv_val_chars(&rec, vnb, sizeof vnb, &vp);
                int64_t f1 = 0;
                while (f1 < vl && (unsigned char)vp[f1] != 0xFE) f1++;
                /* The platform classifies its own VOC type codes (they differ
                   across UniData / MVX / UniVerse): 1 = always drop (system
                   verb/keyword the target supplies), 2 = drop in the open
                   interchange only (platform file/pointer — %FILE% carries the
                   portable form), 0 = keep (the user's own procs). */
                /* All of these exclusions are about what a WHOLESALE add
                   should not sweep up.  Naming a record explicitly
                   (`add VOC SORT`) is a deliberate act and overrides every one
                   of them: the user has said they want this record versioned,
                   and second-guessing that is how a tool becomes untrustworthy. */
                int cls = mv_voc_class(vp, f1);
                if (!one && (cls == 1 || (cls == 2 && voc_open))) {
                    skipped++;
                    continue;
                }
                /* Identical to what a fresh account of this flavour holds, so
                   it is the system's record and not this account's (mv_git#46).
                   Skipped on a WHOLESALE add only: naming a record explicitly
                   (`add VOC SOMEVERB`) is a deliberate act and stages it, the
                   same rule provisioning pointers follow just below. */
                if (!one) {
                    char sid[256];
                    arg_str(&id, sid, sizeof sid);
                    if (is_stock_record(sid, vp, vl)) {
                        skipped++;
                        continue;
                    }
                }
            }
            char idb[256], nb[40];
            arg_str(&id, idb, sizeof idb);
            {
                /* Two ignore namespaces, and they are not the same path.  The
                   account's own GIT IGNORE list is account-relative, so it sees
                   `<file>/<id>`; .gitignore is repository-relative, so it sees
                   the prefixed form. */
                char acctp[600], repop[600];
                snprintf(acctp, sizeof acctp, "%s/%s", fn, idb);
                record_path(repop, sizeof repop, fn, idb);
                if (ignored(fn, acctp) || git_path_ignored(repo, repop)) {
                    skipped++; if (one) break; else continue; }
            }
            if (!one && is_provision_pointer(fn, idb)) {
                skipped++;      /* BUILD provisioning pointer: skip on a wholesale
                                   `add -A`/`add VOC`; explicit `add VOC CATALOG`
                                   (one) still stages it */
                continue;
            }
            const char *cp;
            int64_t clen = mv_val_chars(&rec, nb, sizeof nb, &cp);
            /* Objects are skipped by `add -A`, staged by an EXPLICIT add —
               the same rule as every other exclusion here. */
            if (!one && record_is_object(ctx, &fvar, idb, cp, clen)) {
                skipped++;
                continue;
            }
            /* %FILE% IS CREATE-TIME METADATA, AND ON MVX IT IS ALSO A REAL
               RECORD — which is the one place those two facts collide.  A
               wholesale add would restage the account's own native control
               (FILE<VM>type) straight over the OPEN control git is holding, and
               the open one is the versioned artefact: it is what a clone onto
               another platform reads, and what the attribute editor writes.  So
               an edit made before an `add -A` would be gone, and status would go
               clean as though the edit had landed.

               Leave an open control where it is.  A file that has none is
               unaffected — its native record still stages, and GITOPENFORM
               converts it — so this only ever declines to overwrite geometry git
               already holds, which is the sticky rule (mv_git#15) reaching the
               one staging path that is a record rather than a blob. */
            {
                char ctlp[600];
                record_path(ctlp, sizeof ctlp, fn, idb);
                if (is_file_control(ctlp)) {
                    const git_index_entry *pe =
                        git_index_get_bypath(index, ctlp, 0);
                    git_blob *pb = NULL;
                    int keep = 0;
                    if (pe && git_blob_lookup(&pb, repo, &pe->id) == 0) {
                        keep = is_open_control(git_blob_rawcontent(pb),
                                               (int64_t)git_blob_rawsize(pb));
                        git_blob_free(pb);
                    }
                    if (keep) { if (one) break; else continue; }
                }
            }
            const char *sc = cp;
            int64_t sl = clen;
            char *dtmp = NULL;
#ifdef MVXGIT_UDT
            if (dict_open) {
                int64_t dl;
                dtmp = dict_item_swap(cp, clen, &dl);
                if (dtmp) { sc = dtmp; sl = dl; }
            }
#endif
            int64_t bl;
            char *blob = xlate(sc, sl, (char)0xFE, '\n', &bl);
            free(dtmp);
            git_oid boid;
            if (git_blob_create_from_buffer(&boid, repo, blob,
                                            (size_t)bl) == 0) {
                git_index_entry e;
                memset(&e, 0, sizeof e);
                char path[600];
                record_path(path, sizeof path, fn, idb);
                e.path = path;
                e.mode = GIT_FILEMODE_BLOB;
                e.id = boid;
                git_index_add(index, &e);
                n++;
            }
            free(blob);
            if (one) break;
        }
    }
    mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);

    /* RECONCILE DELETIONS.  Staging only ever ADDED, so a record deleted from
       the account stayed in the index and went on being committed — and a clone
       of that commit brought it back.  Deleted data returning is worse than
       deleted data being missed, and `status` already reported the deletion
       correctly, so the two disagreed about the same account.
       The test is the one status uses — the record cannot be read — so they now
       agree by construction.  Only on a wholesale add of a file: naming one
       record is about that record, not about everything else in the file. */
    int64_t removed = 0;
    if (!only[0]) {
        char rpfx[600];
        int pn = snprintf(rpfx, sizeof rpfx, "%s%s/", g_prefix, fn);
        mv_value fv2, id2, rec2;
        mv_init(&fv2); mv_init(&id2); mv_init(&rec2);
        if (pn > 0 && open_named(ctx, fn, &fv2)) {
            size_t pl = (size_t)pn;
            for (size_t i = git_index_entrycount(index); i-- > 0; ) {
                const git_index_entry *e = git_index_get_byindex(index, i);
                if (!e || strncmp(e->path, rpfx, pl) != 0) continue;
                char path[600], rid[300];
                snprintf(path, sizeof path, "%s", e->path);
                snprintf(rid, sizeof rid, "%s", e->path + pl);
                /* synthesised, never a record — see stage_file_control */
                if (strcmp(rid, "%FILE%") == 0) continue;
                mv_set_str(&id2, rid, (int64_t)strlen(rid));
                if (!mv_read(ctx, &rec2, &fv2, &id2, 0)) {
                    git_index_remove_bypath(index, path);
                    removed++;
                }
            }
        }
        mv_clear(&fv2); mv_clear(&id2); mv_clear(&rec2);
    }

    int rc = git_index_write(index);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[3], "write index"); return; }
    char out[96];
    if (removed) {
        snprintf(out, sizeof out, "staged %lld record(s), %lld removed",
                 (long long)n, (long long)removed);
        mv_set_str(argv[3], out, (int64_t)strlen(out));
        return;
    }
    if (skipped)
        snprintf(out, sizeof out, "staged %lld record(s), %lld ignored",
                 (long long)n, (long long)skipped);
    else
        snprintf(out, sizeof out, "staged %lld record(s)", (long long)n);
    mv_set_str(argv[3], out, (int64_t)strlen(out));
}

/* `git add` with git's own semantics: stage every on-disk file in the working
   tree — honouring .gitignore, executable bits, and top-level as well as nested
   paths — and stage deletions, exactly as stock git would.  This is step one of
   a drop-in `mvx-git add -A`; the caller layers step two on top (staging the
   records of VOC files that are not plain on-disk files, e.g. LMDB hash files).
   Uses the repository-owned index so libgit2 can stat the working tree and
   apply the ignore rules.  GITADDDISK(repo, out) */
/* Declared here, defined with the status helpers: whether the BACKEND calls this
   name one of the account's files. */
static int backend_has_file(mv_ctx *ctx, const char *name);
static int tracked_file_gone(mv_ctx *ctx, git_index *index, const char *top);
static int is_mv_file(const char *n);
static void backend_files_reset(void);
static void split_top(const char *path, char *out, size_t cap);

/* The record-git model tracks records as git blobs, never the binary LMDB
   store — so the account's mvxdata.lmdb must never be staged, even when no
   .gitignore lists it.

   Nor may this pass stage anything that belongs to an MV FILE.  Those are
   records, and the record pass stages them with record semantics; letting the
   plain-file pass also stage them means the same path is written twice by two
   readers whose bytes need not agree, and the loser shows up as permanently
   modified.  On MVX the question never arose, because an MV file's records are
   not ordinary files on disk.  On UniVerse a directory file — BP, BP.O — is
   exactly that, which is how `M BP.O/...` survived every commit.

   Returns >0 to skip the path. */
static int addall_skip(const char *path, const char *matched, void *payload) {
    (void)matched;
    if (strncmp(path, "mvxdata.lmdb", 12) == 0) return 1;
    /* git hands us a REPOSITORY-relative path; file names are account-relative.
       Below a repository root those differ by the account's prefix, and comparing
       the wrong one means every test here silently fails to match — which is how
       a second account's records got staged twice, once as records and once as
       plain files. */
    const char *rel = unprefix(path);
    if (!rel) return 1;                     /* another account's territory */
    char top[256];
    split_top(rel, top, sizeof top);
    /* At the repository root — no prefix — a top-level directory that is itself
       an account belongs to that account's own pass.  Without this the root pass
       would sweep every account's records up as ordinary blobs, which is exactly
       what the per-account passes are for. */
    if (!g_prefix[0] && strcmp(top, rel) != 0) {
        char probe[600];
        struct stat psb;
        snprintf(probe, sizeof probe, "%s/VOC", top);
        if (stat(probe, &psb) == 0) return 1;
        snprintf(probe, sizeof probe, "%s/.mvx", top);
        if (stat(probe, &psb) == 0) return 1;
    }
    /* A platform WORK file is never content.  &SAVEDLISTS&, &PH&, _HOLD_ … are
       scratch areas the system writes to as a side effect of ordinary use — and
       in our case as a side effect of US: every SELECT this tool issues rewrites
       a &SAVEDLISTS& entry, so tracking it means the account is dirty the moment
       anything looks at it.  The account scan has always skipped these names
       (wrapped in & or _); the plain-file pass must skip them too, because on
       UniVerse they are real directories on disk and git would otherwise sweep
       them up. */
    {
        size_t tl = strlen(top);
        if (tl >= 2 && ((top[0] == '&' && top[tl - 1] == '&') ||
                        (top[0] == '_' && top[tl - 1] == '_')))
            return 1;
    }
    /* only a path INSIDE a file is a record; the file's own entry is not */
    if (strcmp(top, rel) != 0 && backend_has_file((mv_ctx *)payload, top))
        return 1;
    /* A FILE'S CONTROL IS NEVER TAKEN FROM DISK.  <file>.DICT/%FILE% is
       create-time metadata, and on MVX it is also an ordinary file on disk — so
       git's own working-tree pass swept it up and staged the account's NATIVE
       control over the OPEN one git was holding.  The open control is the
       versioned artefact: it is what a clone onto another platform reads, and
       what the attribute editor writes.  So an attribute edit made before an
       `add -A` was silently gone, and status went clean as though it had landed.

       Skipping it costs nothing.  A file that needs a control still gets one
       from the record pass, which stages the native %FILE% record, and
       GITOPENFORM converts that to the open class.  The disk pass was only ever
       a third way to the same path, arriving last and knowing least. */
    if (is_file_control(rel)) return 1;
    return 0;
}

void mvx_sub_GITADDDISK(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0 &&
        init_bare_main(&repo, rp) != 0) { fail(argv[1], "open"); return; }
    git_index *index = NULL;
    if (git_repository_index(&index, repo) != 0) {
        git_repository_free(repo);
        fail(argv[1], "index");
        return;
    }
    git_strarray all = {NULL, 0};              /* empty pathspec ⇒ everything */
    int rc = git_index_add_all(index, &all, GIT_INDEX_ADD_DEFAULT,
                               addall_skip, ctx);
    /* update_all needs the SAME filter.  It refreshes every already-tracked path
       from the working tree, so without the callback it re-stages record paths
       as plain blobs — quietly undoing the record staging that put them there.
       In a multi-account repository that made each account's pass overwrite the
       previous account's records, and the loser showed up as permanently
       modified. */
    if (rc == 0) rc = git_index_update_all(index, &all, addall_skip, ctx);
    if (rc == 0) rc = git_index_write(index);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[1], "add"); return; }
    mv_set_str(argv[1], "staged working tree", 19);
}

/* Smallest prime >= max(n, 2). */
static long next_prime(long n) {
    if (n < 2) return 2;
    for (;; n++) {
        int prime = 1;
        for (long d = 2; d * d <= n; d++)
            if (n % d == 0) { prime = 0; break; }
        if (prime) return n;
    }
}

/* Records the open form aims to place per hash group when guessing a modulo. */
#define OPENFORM_RECS_PER_GROUP 7

/* Guess a hash-file modulo from the number of data records staged for `base`
   (index entries under "<base>/").  MVX/LMDB has no modulo of its own, so the
   open form carries this as a size hint for a materialise onto a modulo-based
   platform (UniData/UV); a dynamic target refines it under load.  Returns 0 for
   an empty file (leave the control a bare "hash").  The index is sorted, so the
   records for a file are contiguous from the prefix match. */
static long guess_modulo(git_index *index, const char *base) {
    char prefix[600];
    snprintf(prefix, sizeof prefix, "%s/", base);
    size_t pos = 0;
    if (git_index_find_prefix(&pos, index, prefix) != 0) return 0;
    size_t plen = strlen(prefix), ic = git_index_entrycount(index), n = 0;
    for (size_t i = pos; i < ic; i++) {
        const git_index_entry *e = git_index_get_byindex(index, i);
        if (strncmp(e->path, prefix, plen) != 0) break;
        n++;
    }
    if (n == 0) return 0;
    return next_prime((long)(n / OPENFORM_RECS_PER_GROUP));
}

/* --- account descriptor conversion: .mvx <-> .mv-account (mvx#73) ---------
 *
 * The on-disk native descriptor `.mvx` and the portable git-side `.mv-account`
 * share one `key = value` grammar but hold different subsets.  `.mv-account` is
 * the portable superset: identity (name/version/description) plus the open-form
 * markers `openaccount` and the default `hash` backend.
 *
 * Security policy (`permit`/`deny`, #80) has TWO layers (mvx_perm.c):
 *   - the account's `.mvx` is the VENDOR declaration (source #1) — the shell
 *     command surface a package ships, e.g. `permit prog:MVPKG = mkdir tar ...`.
 *     This IS the package and MUST travel with the account, so `.mv-account`
 *     carries it and checkout re-seeds it into the native `.mvx` — enforced on
 *     mvx at the restricted tier; on UniData/other systems it is a declaration
 *     the git editor manages, not enforced.
 *   - `.mvx-private/permissions` (source #2) is the LOCAL admin's policy —
 *     git-ignored, host-specific, and NEVER carried here.
 * So the engine converts at the boundary: commit projects `.mvx` (identity +
 * vendor permits) down to the portable form, checkout rebuilds `.mvx` from it.
 * One schema, one writer, shared with udt-git via mv_git_desc_open(). */
typedef struct {
    char name[128];
    char version[32];
    char description[256];
    char hash[32];        /* default hash backend; empty -> "lmdb" on open form */
    int  openaccount;     /* open-form version; 0 if the source carried none */
    char flavour[32];     /* VOC flavour, UniVerse only (mv_git#15).  A UniVerse
                             account is created with a flavour — PICK, IN2,
                             Ideal … — and it governs how the account's own VOC
                             behaves.  Recreating an account without it produces
                             something that looks right and behaves differently,
                             so it has to travel with the descriptor.  Empty for
                             platforms that have no such notion. */
    char permits[2048];   /* vendor permit/deny lines, verbatim (each \n-terminated) */
} acct_desc;

static void desc_rtrim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1]=='\n' || s[n-1]=='\r' || s[n-1]==' ' || s[n-1]=='\t'))
        s[--n] = '\0';
}

/* Parse a legible descriptor (either `.mvx` or `.mv-account`) into `d`.  The
   portable identity keys fill the struct; `permit`/`deny` lines are captured
   VERBATIM into d->permits (the vendor security policy, which travels with the
   account); comment and `file` lines are ignored. */
static void desc_parse(const char *buf, size_t len, acct_desc *d) {
    memset(d, 0, sizeof *d);
    size_t i = 0;
    while (i < len) {
        size_t s = i;
        while (i < len && buf[i] != '\n') i++;
        size_t ll = i - s;
        if (i < len) i++;                        /* consume the newline */
        char line[512];
        if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, buf + s, ll);
        line[ll] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || !*p) continue;
        /* Vendor security policy travels with the account: capture permit/deny
           lines verbatim (they carry an '=', but no portable key matches, so
           they'd otherwise be dropped).  Local admin policy lives in
           .mvx-private/permissions and never reaches here. */
        if ((strncmp(p, "permit", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) ||
            (strncmp(p, "deny",   4) == 0 && (p[4] == ' ' || p[4] == '\t'))) {
            desc_rtrim(p);
            size_t have = strlen(d->permits);
            if (have + strlen(p) + 2 < sizeof d->permits)
                snprintf(d->permits + have, sizeof d->permits - have, "%s\n", p);
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p, *val = eq + 1;
        desc_rtrim(key);
        while (*val == ' ' || *val == '\t') val++;
        desc_rtrim(val);
        if      (strcasecmp(key, "name") == 0)
            snprintf(d->name, sizeof d->name, "%s", val);
        else if (strcasecmp(key, "version") == 0)
            snprintf(d->version, sizeof d->version, "%s", val);
        else if (strcasecmp(key, "description") == 0)
            snprintf(d->description, sizeof d->description, "%s", val);
        else if (strcasecmp(key, "hash") == 0)
            snprintf(d->hash, sizeof d->hash, "%s", val);
        else if (strcasecmp(key, "openaccount") == 0)
            d->openaccount = atoi(val);
        else if (strcasecmp(key, "flavour") == 0 || strcasecmp(key, "flavor") == 0)
            snprintf(d->flavour, sizeof d->flavour, "%s", val);
    }
}

/* Render the canonical portable `.mv-account` form of `d`; returns its length. */
static int desc_render_open(const acct_desc *d, char *out, size_t cap) {
    const char *name = d->name[0]    ? d->name    : "account";
    const char *ver  = d->version[0] ? d->version : "1";
    const char *hash = d->hash[0]    ? d->hash    : "lmdb";
    int oa = d->openaccount > 0 ? d->openaccount : 1;
    int n = snprintf(out, cap,
        "# .mv-account - open (portable) account descriptor\n"
        "name = %s\nversion = %s\nopenaccount = %d\nhash = %s\n",
        name, ver, oa, hash);
    if (n > 0 && (size_t)n < cap && d->description[0])
        n += snprintf(out + n, cap - (size_t)n, "description = %s\n",
                      d->description);
    /* Only emitted when the source had one, so an account from a platform with
       no flavour does not acquire a meaningless field. */
    if (n > 0 && (size_t)n < cap && d->flavour[0])
        n += snprintf(out + n, cap - (size_t)n, "flavour = %s\n", d->flavour);
    /* vendor permit/deny lines travel with the account (the package's declared
       shell surface); the local admin layer stays in .mvx-private. */
    if (n > 0 && (size_t)n < cap && d->permits[0])
        n += snprintf(out + n, cap - (size_t)n, "%s", d->permits);
    return n;
}

/* Render a NATIVE descriptor for `d` — identity + the VENDOR permit/deny lines
   (re-seeded on checkout so mvx enforces them at the restricted tier).  The
   LOCAL admin policy is re-established from .mvx-private, never carried here.
   `tag` names the platform in the header comment: the native form is per-platform
   (`.mvx`, `.uv`, `.udt`) and the file should say which one it describes. */
static int desc_render_native_as(const acct_desc *d, const char *tag,
                                char *out, size_t cap) {
    const char *name = d->name[0]    ? d->name    : "account";
    const char *ver  = d->version[0] ? d->version : "1";
    int n = snprintf(out, cap,
        "# %s account descriptor\nname = %s\nversion = %s\n", tag, name, ver);
    if (n > 0 && (size_t)n < cap && d->description[0])
        n += snprintf(out + n, cap - (size_t)n, "description = %s\n",
                      d->description);
    /* Carried here too, even though MVX has no flavour of its own: an account
       that passes through MVX — cloned, worked on, pushed back — must not lose
       it, or returning to UniVerse would recreate the account with the wrong
       VOC behaviour.  Preservation, not use. */
    if (n > 0 && (size_t)n < cap && d->flavour[0])
        n += snprintf(out + n, cap - (size_t)n, "flavour = %s\n", d->flavour);
    if (n > 0 && (size_t)n < cap && d->permits[0])
        n += snprintf(out + n, cap - (size_t)n, "%s", d->permits);
    return n;
}

/* The native MVX descriptor — the form checkout re-seeds onto disk as `.mvx`. */
static int desc_render_native(const acct_desc *d, char *out, size_t cap) {
    return desc_render_native_as(d, "MVX", out, cap);
}

/* Public: render the canonical portable descriptor for an account with the
   given identity + default hash backend (udt-git synthesises it from the live
   UniData account; mvx-git converts from `.mvx`).  Returns the length. */
int mv_git_desc_open(const char *name, const char *version,
                     const char *description, const char *hash,
                     char *out, size_t cap) {
    acct_desc d;
    memset(&d, 0, sizeof d);
    if (name)        snprintf(d.name, sizeof d.name, "%s", name);
    if (version)     snprintf(d.version, sizeof d.version, "%s", version);
    if (description) snprintf(d.description, sizeof d.description, "%s",
                             description);
    if (hash)        snprintf(d.hash, sizeof d.hash, "%s", hash);
    d.openaccount = 1;
    return desc_render_open(&d, out, cap);
}

/* Public: adopt a descriptor onto this platform — see mvxgit.h for why this is a
   conversion the user reviews rather than something a clone does silently. */
int mv_git_desc_adopt(const char *src, size_t srclen, const char *platform,
                      const char *flavour, int open_form,
                      char *name_out, size_t name_cap,
                      char *out, size_t cap) {
    acct_desc d;
    desc_parse(src ? src : "", src ? srclen : 0, &d);
    /* Supplied only when the source lacked it, so adopting an account that
       already names its flavour never overwrites what it says. */
    if (flavour && flavour[0])
        snprintf(d.flavour, sizeof d.flavour, "%s", flavour);
    if (open_form) {
        d.openaccount = 1;
        if (name_out) snprintf(name_out, name_cap, ".mv-account");
        return desc_render_open(&d, out, cap);
    }
    /* Declining the open form keeps the account native — but native to HERE, not
       to wherever it came from.  On UniVerse and UniData the native marker exists
       only inside the repository (the live account IS its VOC), so the descriptor's
       whole job is to say what to rebuild and how. */
    d.openaccount = 0;
    if (name_out)
        snprintf(name_out, name_cap, ".%s",
                 (platform && platform[0]) ? platform : "mvx");
    {
        char tag[32];
        const char *p = (platform && platform[0]) ? platform : "mvx";
        size_t i = 0;
        for (; p[i] && i < sizeof tag - 1; i++)
            tag[i] = (char)toupper((unsigned char)p[i]);
        tag[i] = '\0';
        return desc_render_native_as(&d, tag, out, cap);
    }
}

/* Public: read one field from a descriptor without duplicating the parser. */
int mv_git_desc_field(const char *src, size_t srclen, const char *key,
                      char *out, size_t cap) {
    acct_desc d;
    const char *v = NULL;
    desc_parse(src ? src : "", src ? srclen : 0, &d);
    if      (!strcasecmp(key, "name"))        v = d.name;
    else if (!strcasecmp(key, "version"))     v = d.version;
    else if (!strcasecmp(key, "description")) v = d.description;
    else if (!strcasecmp(key, "hash"))        v = d.hash;
    else if (!strcasecmp(key, "flavour") ||
             !strcasecmp(key, "flavor"))      v = d.flavour;
    if (!v || !v[0]) { if (cap) out[0] = '\0'; return 0; }
    snprintf(out, cap, "%s", v);
    return 1;
}

/* Normalise the staged index to the open account format — the record-git
   cross-platform interchange (mvx#73).  The working tree stays a native account
   on disk; only the git objects carry the open form, so this rewrites staged
   blobs after `add`: a dictionary's `%FILE%` control (`FILE <VM> type <VM>
   conn`) becomes the portable class `DIR` or `hash` (a hashed file carries a
   guessed modulo, "hash <modulo> DYNAMIC", so a clone onto a modulo-based
   platform sizes the file instead of always making a tiny default), and the
   native account descriptor `.mvx` is converted (not renamed) to the portable
   `.mv-account`.  Runs only when the account opts in (`mvx.openaccount`).
   GITOPENFORM(repo, out) */
void mvx_sub_GITOPENFORM(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[1], "open"); return; }
    git_tree *ht = head_tree(repo);   /* for the sticky committed modulo */

    /* Collect changes first — adding to the index mid-scan invalidates the
       entry pointers git_index_get_byindex returns. */
    struct fix { char path[600]; git_oid id; } *fix = NULL;
    size_t nfix = 0, capfix = 0;
    int have_mvx = 0;
    git_oid mvx_blob;
    const char *suffix = ".DICT/%FILE%";
    size_t sl = strlen(suffix);

    size_t ic = git_index_entrycount(index);
    for (size_t i = 0; i < ic; i++) {
        const git_index_entry *e = git_index_get_byindex(index, i);
        if (e->mode == GIT_FILEMODE_COMMIT) continue;   /* submodule gitlink */
        size_t pl = strlen(e->path);

        if (strcmp(e->path, ".mvx") == 0) {   /* .mvx -> .mv-account (convert) */
            git_blob *b = NULL;
            if (git_blob_lookup(&b, repo, &e->id) == 0) {
                acct_desc d;
                desc_parse(git_blob_rawcontent(b),
                           (size_t)git_blob_rawsize(b), &d);
                char open[1024];
                int ol = desc_render_open(&d, open, sizeof open);
                if (ol > 0 && git_blob_create_from_buffer(&mvx_blob, repo,
                        open, (size_t)ol) == 0)
                    have_mvx = 1;
                git_blob_free(b);
            }
            continue;
        }
        if (pl < sl || strcmp(e->path + pl - sl, suffix) != 0)
            continue;                                   /* not a %FILE% control */
        git_blob *b = NULL;
        if (git_blob_lookup(&b, repo, &e->id) != 0) continue;
        const char *c = git_blob_rawcontent(b);
        int64_t cl = (int64_t)git_blob_rawsize(b);
        const char *cls = NULL;                         /* native -> DIR/hash */
        if (cl >= 5 && memcmp(c, "FILE", 4) == 0 && (unsigned char)c[4] == 0xFD) {
            const char *t = c + 5, *end = c + cl;
            const char *m2 = memchr(t, 0xFD, (size_t)(end - t));
            const char *te = m2 ? m2 : end;
            while (te > t && (te[-1] == '\n' || te[-1] == '\r' ||
                              te[-1] == ' ' || te[-1] == '\t'))
                te--;                                   /* trim trailing ws */
            cls = (te - t == 3 && strncasecmp(t, "dir", 3) == 0) ? "DIR" : "hash";
        }
        git_blob_free(b);
        if (!cls) continue;
        /* For a hashed file the control carries a modulo the target sizes to.
           Keep an already-committed modulo STICKY — the shipped default must not
           track this working copy's current size, or one customer's growth/resize
           would become everyone's — so preserve HEAD's control and only guess for
           a file with none committed yet. */
        char spec[MV_GIT_CTL_MAX];
        const char *cont = cls;
        if (strcmp(cls, "hash") == 0) {
            char base[600];
            snprintf(base, sizeof base, "%.*s", (int)(pl - sl), e->path);
            char committed[MV_GIT_CTL_MAX];
            if (head_control(repo, ht, base, committed, sizeof committed) >= 0 &&
                strncmp(committed, "hash", 4) == 0) {
                snprintf(spec, sizeof spec, "%s", committed);   /* sticky default */
                cont = spec;
            } else {
                long mod = guess_modulo(index, base);           /* new file: seed */
                if (mod > 0) {
                    snprintf(spec, sizeof spec, "hash %ld DYNAMIC", mod);
                    cont = spec;
                }
            }
        }
        git_oid nb;
        if (git_blob_create_from_buffer(&nb, repo, cont, strlen(cont)) != 0)
            continue;
        if (nfix == capfix) {
            capfix = capfix ? capfix * 2 : 16;
            fix = realloc(fix, capfix * sizeof *fix);
            if (!fix) mv_fatal("out of memory in openform");
        }
        snprintf(fix[nfix].path, sizeof fix[nfix].path, "%s", e->path);
        fix[nfix].id = nb;
        nfix++;
    }

    for (size_t i = 0; i < nfix; i++) {
        git_index_entry e;
        memset(&e, 0, sizeof e);
        e.path = fix[i].path;
        e.mode = GIT_FILEMODE_BLOB;
        e.id = fix[i].id;
        git_index_add(index, &e);
    }
    if (have_mvx) {
        git_index_entry e;
        memset(&e, 0, sizeof e);
        e.path = ".mv-account";
        e.mode = GIT_FILEMODE_BLOB;
        e.id = mvx_blob;
        git_index_add(index, &e);
        git_index_remove_bypath(index, ".mvx");
    }
    free(fix);
    int rc = git_index_write(index);
    if (ht) git_tree_free(ht);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[1], "openform"); return; }
    char out[80];
    snprintf(out, sizeof out, "open form: %zu file(s) normalised",
             nfix + (size_t)have_mvx);
    mv_set_str(argv[1], out, (int64_t)strlen(out));
}

/* Invoke an engine sub with n string args + an output slot; discard the output. */
static void addall_call(void (*fn)(mv_ctx *, int32_t, mv_value **),
                        mv_ctx *ctx, const char **args, int n) {
    mv_value v[6], *av[6];
    for (int i = 0; i < n; i++) {
        mv_init(&v[i]);
        mv_set_str(&v[i], args[i], (int64_t)strlen(args[i]));
        av[i] = &v[i];
    }
    mv_init(&v[n]); av[n] = &v[n];
    fn(ctx, n + 1, av);
    for (int i = 0; i <= n; i++) mv_clear(&v[i]);
}

/* <acct>/<n>.DICT/%FILE% present -> an on-disk MV file (directory-backed). */
static int addall_is_mv_file(const char *acct, const char *n) {
    struct stat sb;
    char p[4096];
    snprintf(p, sizeof p, "%s/%s.DICT/%%FILE%%", acct, n);
    return stat(p, &sb) == 0;
}

/* GITADDALL(repo, out) — the whole-account `add -A`, IN THE ENGINE so it works
   where there is no CLI to fall back to (D3).  Same three passes as the former
   CLI-only add_all: git's own add of plain files (honouring .gitignore), then
   every MV file's records — on-disk directory files AND whatever the BACKEND
   reports (mv_filelist), staged with their .DICT — then the open-form
   normalisation.  A gitignored MV file is skipped (its records never enter the
   open form).

   Both file sources are needed because the platforms disagree about where a file
   even IS.  On MVX a file may be a directory on disk carrying <name>.DICT/%FILE%,
   which pass 2 finds by looking; on UniVerse and UniData a file is a hash file
   that pass 2 cannot see at all, and the only authority is the account's VOC —
   which is exactly what mv_filelist answers.  Pass 3 used to run only when an
   lmdb store was present, which quietly made it MVX-only: on UniVerse it left
   every record unstaged while reporting success, because pass 1 had swept the
   directory files as ordinary blobs and nothing had asked the backend.  So the
   backend is now always consulted, and pass 3 skips what pass 2 already did. */
void mvx_sub_GITSTAGEBLOB(mv_ctx *ctx, int32_t argc, mv_value **argv);

/* Compiled BASIC objects, which differ per platform and must not be committed.
 *
 *   UniVerse  objects live in a SEPARATE FILE named after the source file:
 *             BP -> BP.O.  So a file whose name is <X>.O, where <X> is also a
 *             file, is an object file and none of its records are content.
 *   UniData   objects live INSIDE the source file, as records named _<PROG>
 *             alongside <PROG> — an underscore PREFIX (measured on 8.3:
 *             compiling BP/GIT.AGENT writes BP/_GIT.AGENT).  So a record id
 *             starting "_" whose base id exists in the same file is an object.
 *   MVX       neither — objects go to CATALOG/, which is not a record file, so
 *             these rules simply never fire.
 *
 * Naming the companion is what makes this exact.  The previous rule was "the
 * record contains a NUL", which is a guess in both directions: it drops a
 * legitimate record that happens to hold one, and keeps an object that happens
 * not to.  Worse, it was gated on an environment variable that ONLY udt-git
 * set, so UniVerse was committing its compiled objects outright.
 *
 * Wholesale adds only.  Naming a record explicitly stages it regardless, the
 * same rule every other exclusion follows. */
/* Stage `<file>.DICT/%FILE%` — the file's own CREATE.FILE parameters.
 *
 * This is the `.gitkeep` of a MultiValue file, and rather more: it makes a file
 * with no records exist in the commit at all, and it carries the geometry a
 * clone needs to recreate the file CORRECTLY rather than as some default.
 *
 * On MVX the control is a real record on disk, so `add` picks it up like any
 * other and this is a no-op.  On UniVerse and UniData nothing produces one — the
 * dictionary is a separate hash file with no such entry — so the geometry had no
 * carrier and a clone could only guess.  GITOPENFORM converts a control that
 * already exists; it never creates one, which is why turning the open form on
 * did not help.
 *
 * Staged in EVERY form, not just the open one: a native commit needs it just as
 * much, because it is the only thing that says what to create.
 *
 * Only where the backend can describe a file.  mv_fileclass is part of the
 * UniVerse and UniData contracts and is deliberately absent from MVX's, because
 * there the control IS a record and `add` already stages it — synthesising a
 * second one would be inventing content over the account's own. */
static void stage_file_control(mv_ctx *ctx, const char *rp, const char *name) {
#if !defined(MVXGIT_GITD) && !defined(MVXGIT_UDT)
    (void)ctx; (void)rp; (void)name;
    return;                     /* MVX: the file's control is its own record */
#else
    char cls[128] = "";
    if (mv_fileclass(ctx, name, cls, sizeof cls) <= 0 || !cls[0]) return;
    char path[600];
    snprintf(path, sizeof path, "%s.DICT/%%FILE%%", name);
    mv_value a0, a1, a2, out;
    mv_init(&a0); mv_init(&a1); mv_init(&a2); mv_init(&out);
    mv_set_str(&a0, rp, (int64_t)strlen(rp));
    mv_set_str(&a1, path, (int64_t)strlen(path));
    mv_set_str(&a2, cls, (int64_t)strlen(cls));
    mv_value *av[4] = { &a0, &a1, &a2, &out };
    mvx_sub_GITSTAGEBLOB(ctx, 4, av);
    mv_clear(&a0); mv_clear(&a1); mv_clear(&a2); mv_clear(&out);
#endif
}

/* The names pass 2 already staged, so pass 3 does not stage them twice.  Small
   and linear on purpose: an account has tens of files, not thousands. */
typedef struct { char (*n)[256]; int c, cap; } addall_set;

static void addall_seen(addall_set *s, const char *name) {
    if (s->c >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 32;
        void *p = realloc(s->n, (size_t)nc * 256);
        if (!p) return;
        s->n = p; s->cap = nc;
    }
    snprintf(s->n[s->c++], 256, "%s", name);
}

static int addall_was_seen(const addall_set *s, const char *name) {
    for (int i = 0; i < s->c; i++)
        if (!strcmp(s->n[i], name)) return 1;
    return 0;
}

void mvx_sub_GITADDALL(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    openaccount_sync(rp);               /* verb path: honour mvx.openaccount */
    stock_ensure_udt(ctx, rp);          /* subtract the system's own VOC (#46) */
    const char *acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";
    int64_t nfiles = 0;
    addall_set seen = {0};

    /* 1. git's own add — plain files, honouring .gitignore, deletions, submodules.
          Restricted to this account: a prefixed run stages only what is under its
          own directory, and the repository's own top-level files are staged once,
          by the caller, at the root.  Otherwise each account in a multi-account
          repository would re-stage every other account's records as plain
          blobs — silently undoing their record staging. */
    { const char *a[] = {rp}; addall_call(mvx_sub_GITADDDISK, ctx, a, 1); }

    git_repository *repo = NULL;
    git_repository_open(&repo, rp);              /* for the .gitignore checks */

    /* 2. on-disk MV files (a directory with <name>.DICT/%FILE%) */
    DIR *d = opendir(acct);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;
        char p[4096];
        struct stat sb;
        snprintf(p, sizeof p, "%s/%s", acct, n);
        if (stat(p, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
        if (!addall_is_mv_file(acct, n)) continue;
        if (git_path_ignored(repo, n)) continue;
        const char *a[] = {rp, n, ""};
        addall_call(mvx_sub_GITADD, ctx, a, 3);
        stage_file_control(ctx, rp, n);
        addall_seen(&seen, n);
        nfiles++;
    }
    if (d) closedir(d);

    /* 3. every file the BACKEND knows about — lmdb-backed on MVX, the VOC scan
          on UniVerse and UniData.  Always consulted: a platform whose files are
          hash files has no other way to be seen, and pass 2 cannot find them. */
    {
        mv_value fl;
        mv_init(&fl);
        mv_filelist(ctx, &fl);                   /* name<VM>type, @AM-separated */
        char nb[40];
        const char *p;
        int64_t len = mv_val_chars(&fl, nb, sizeof nb, &p), i = 0;
        while (i < len) {
            int64_t s = i;
            while (i < len && (unsigned char)p[i] != 0xFE &&
                   (unsigned char)p[i] != 0xFD) i++;
            int64_t nl = i - s;
            if (nl > 0 && nl < 256) {
                char name[256];
                memcpy(name, p + s, (size_t)nl);
                name[nl] = '\0';
                /* UniVerse: <X>.O is the object file of <X>; skip it whole. */
                int is_obj = 0;
                {
                    size_t nl2 = strlen(name);
                    if (nl2 > 2 && strcmp(name + nl2 - 2, ".O") == 0) {
                        char base[256];
                        snprintf(base, sizeof base, "%.*s", (int)(nl2 - 2), name);
                        is_obj = backend_has_file(ctx, base);
                    }
                }
                if (!is_obj && !addall_was_seen(&seen, name) &&
                    !git_path_ignored(repo, name)) {
                    const char *a[] = {rp, name, ""};
                    addall_call(mvx_sub_GITADD, ctx, a, 3);
                    char dn[300];
                    snprintf(dn, sizeof dn, "%s.DICT", name);
                    const char *a2[] = {rp, dn, ""};
                    addall_call(mvx_sub_GITADD, ctx, a2, 3);
                    stage_file_control(ctx, rp, name);
                    nfiles++;
                }
            }
            while (i < len && (unsigned char)p[i] != 0xFE) i++;
            if (i < len) i++;
        }
        mv_clear(&fl);
    }
    free(seen.n);

    if (repo) {
        git_repository_free(repo);
        repo = NULL;
    }
    /* 3b. Files that are GONE — see mv_git_prune_gone. */
    { char *pr = mv_git_prune_gone(ctx, rp, ""); free(pr); }

    /* 4. open-account normalisation of the staged index */
    if (mv_openaccount()) {
        const char *a[] = {rp};
        addall_call(mvx_sub_GITOPENFORM, ctx, a, 1);
    }

    char out[128];
    snprintf(out, sizeof out, "staged %lld file(s)", (long long)nfiles);
    mv_set_str(argv[1], out, (int64_t)strlen(out));
}

/* Drop from the index every record of a file the account no longer has.
 *
 * Passes that stage walk what EXISTS, so a deleted file is never visited and its
 * records sit in the index for ever — committed again on every commit, and
 * brought back whole by a clone of that commit.  %FILE% is the file's existence
 * in git, so when it goes the file goes: <file>/ and <file>.DICT/ both.
 *
 * ITS OWN ENTRY POINT because the wholesale add is not one implementation.  On
 * MVX it is the C GITADDALL; on UniVerse and UniData it is BASIC (GITUDT.ADD)
 * walking files and staging each record.  A reconcile living inside GITADDALL
 * therefore never ran on the two platforms that need it most — `add -A` left the
 * deleted file staged and status reported it deleted for ever.  One function,
 * called by both, is the only arrangement that cannot drift.
 */
/* The account's live files, as told to us by the CALLER.
 *
 * mvgitd is built MVXGIT_NORECORDS: it has no record backend, so
 * backend_has_file() there is always 0 and every file looks deleted.  Letting it
 * decide emptied the whole index.  The records live in the SESSION, so the
 * session is the only thing that knows what exists — on UniVerse and UniData the
 * BASIC add hands its file list in.  Empty means "no list given, ask the
 * backend", which is the in-process case (MVX, and the CLI drivers). */
static const char *g_live = NULL;
static int64_t     g_livelen = 0;

static int live_listed(const char *name) {
    if (!g_live || g_livelen <= 0) return -1;      /* no list — cannot say */
    size_t nl = strlen(name);
    int64_t i = 0;
    while (i < g_livelen) {
        int64_t st = i;
        /* Entries may be name<VM>type — stop at the VM, as every other reader of
           a file list here does.  Comparing the whole field matched nothing and
           made every file look deleted. */
        while (i < g_livelen && (unsigned char)g_live[i] != 0xFE &&
               (unsigned char)g_live[i] != 0xFD) i++;
        if ((size_t)(i - st) == nl && memcmp(g_live + st, name, nl) == 0) return 1;
        while (i < g_livelen && (unsigned char)g_live[i] != 0xFE) i++;
        if (i < g_livelen) i++;
    }
    return 0;
}

void mvx_sub_GITPRUNE(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 3) return;
    ensure_init();
    backend_files_reset();      /* judge liveness against the account as it IS */
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    char lnb[40];
    g_live = NULL;
    g_livelen = mv_val_chars(argv[1], lnb, sizeof lnb, &g_live);
    openaccount_sync(rp);
    git_repository *repo = NULL;
    git_index *gidx = NULL;
    if (repo_open(rp, &repo, &gidx) != 0) { fail(argv[2], "open"); return; }
    /* DECIDE FIRST, THEN REMOVE.  The test is "is <base>.DICT/%FILE% still in
       the index" — an entry this pass is itself deleting.  Removing as we went
       dropped %FILE% and every later lookup then said "git never tracked this",
       so the file's own records survived. */
    char (*gonetop)[256] = NULL;
    size_t ng = 0, gcap = 0;
    for (size_t i = 0; i < git_index_entrycount(gidx); i++) {
        const git_index_entry *e = git_index_get_byindex(gidx, i);
        if (!e || e->mode == GIT_FILEMODE_COMMIT) continue;
        const char *rel = unprefix(e->path);
        if (!rel || !strchr(rel, '/')) continue;
        char top[256];
        split_top(rel, top, sizeof top);
        size_t k = 0;
        for (; k < ng; k++) if (!strcmp(gonetop[k], top)) break;
        if (k < ng) continue;                     /* already decided */
        if (!tracked_file_gone(ctx, gidx, top)) continue;
        if (ng == gcap) {
            size_t nc = gcap ? gcap * 2 : 8;
            char (*t)[256] = realloc(gonetop, nc * sizeof *gonetop);
            if (!t) break;
            gonetop = t;
            gcap = nc;
        }
        snprintf(gonetop[ng++], 256, "%s", top);
    }
    long dropped = 0;
    for (size_t i = git_index_entrycount(gidx); i-- > 0 && ng; ) {
        const git_index_entry *e = git_index_get_byindex(gidx, i);
        if (!e || e->mode == GIT_FILEMODE_COMMIT) continue;
        const char *rel = unprefix(e->path);
        if (!rel || !strchr(rel, '/')) continue;
        char top[256], path[700];
        split_top(rel, top, sizeof top);
        snprintf(path, sizeof path, "%s", e->path);
        for (size_t k = 0; k < ng; k++) {
            if (strcmp(gonetop[k], top)) continue;
            git_index_remove_bypath(gidx, path);
            dropped++;
            break;
        }
    }
    free(gonetop);
    if (dropped) git_index_write(gidx);
    git_index_free(gidx);
    git_repository_free(repo);
    char out[128];
    /* Silent when there was nothing to unstage.  A caller that echoes this
       prints it on EVERY blanket add otherwise — and an always-non-empty
       result is also what let a stale side channel be mistaken for a real
       one (mv_git#58). */
    if (dropped == 0)
        out[0] = '\0';
    else
        snprintf(out, sizeof out, "%ld record(s) of deleted file(s) unstaged",
             dropped);
    g_live = NULL; g_livelen = 0;
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* Stage a git submodule as a gitlink (mode 0160000, id = the submodule's
   current HEAD) rather than recursing into it as a directory file — so an
   account can carry submodules (e.g. docs -> the repo wiki).  The submodule is
   added with plain `git submodule add` (which writes .gitmodules and clones
   it); this keeps the gitlink current on add/commit.  GITADDSUB(repo, name,
   out) */
void mvx_sub_GITADDSUB(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], name[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], name, sizeof name);

    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[2], "open"); return; }

    /* the submodule's HEAD is the commit the gitlink records */
    git_repository *sub = NULL;
    git_oid head;
    int rc = git_repository_open(&sub, name);
    if (rc == 0) rc = git_reference_name_to_id(&head, sub, "HEAD");
    if (sub) git_repository_free(sub);
    if (rc != 0) {
        git_index_free(index);
        git_repository_free(repo);
        fail(argv[2], "submodule HEAD");
        return;
    }
    git_index_entry e;
    memset(&e, 0, sizeof e);
    e.path = name;
    e.mode = GIT_FILEMODE_COMMIT;           /* 0160000 gitlink */
    e.id = head;
    rc = git_index_add(index, &e);
    if (rc == 0) rc = git_index_write(index);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[2], "stage submodule"); return; }
    char out[300];
    snprintf(out, sizeof out, "staged submodule %s", name);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* Unstage/remove tracking of a record.  GITRM(repo, file, record, out) */
void mvx_sub_GITRM(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 4) return;
    ensure_init();
    char rp[4096], fn[256], recid[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], fn, sizeof fn);
    arg_str(argv[2], recid, sizeof recid);

    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[3], "open"); return; }
    char path[600];
    record_path(path, sizeof path, fn, recid);
    int rc;
    if (recid[0]) rc = git_index_remove_bypath(index, path);
    else rc = git_index_remove_directory(index, fn, 0);
    if (rc == 0) rc = git_index_write(index);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[3], "rm"); return; }
    mv_set_str(argv[3], "removed from tracking", 21);
}

/* GITINDEXIDS(repo, file, out) — the record ids currently staged under <file>/,
 * @AM-separated.
 *
 * NEITHER SIDE CAN RECONCILE ALONE, which is why this exists.  Staging a deleted
 * record's removal needs two facts: what git has, and what the account still
 * has.  The BASIC add can read the account but cannot see the index; mvgitd can
 * see the index but has no record backend at all.  So the engine answers "what
 * do I have for this file" and the caller — which can READ — decides.
 */
void mvx_sub_GITINDEXIDS(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], fn[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], fn, sizeof fn);
    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[2], "open"); return; }
    char pfx[600];
    int pn = snprintf(pfx, sizeof pfx, "%s%s/", g_prefix, fn);
    sbuf b = {0};
    if (pn > 0) {
        for (size_t i = 0; i < git_index_entrycount(index); i++) {
            const git_index_entry *e = git_index_get_byindex(index, i);
            if (!e || e->mode == GIT_FILEMODE_COMMIT) continue;
            if (strncmp(e->path, pfx, (size_t)pn) != 0) continue;
            const char *id = e->path + pn;
            if (!*id || strchr(id, '/')) continue;     /* not a plain record id */
            sb_line(&b, id);
        }
    }
    git_index_free(index);
    git_repository_free(repo);
    sb_out(&b, argv[2], "");
}

/* GITCOMMIT(repo, message, out) — commit the staged index. */
void mvx_sub_GITCOMMIT(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], msg[4096];
    arg_str(argv[0], rp, sizeof rp);
    openaccount_sync(rp);
    arg_str(argv[1], msg, sizeof msg);

    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[2], "open"); return; }

    git_oid tree_oid, commit_oid;
    git_tree *tree = NULL, *ptree = NULL;
    git_signature *sig = NULL;
    git_object *headobj = NULL;
    git_commit *parent = NULL;
    int rc = git_index_write_tree_to(&tree_oid, index, repo);
    if (rc != 0) {
        /* Say what actually failed.  The HEAD probe below is EXPECTED to fail on
           a first commit, and libgit2 keeps that message in giterr_last(), so
           reporting the last error here blames a missing HEAD for a problem that
           is usually an unstorable path in the index. */
        const git_error *e = git_error_last();
        char m[512];
        snprintf(m, sizeof m, "cannot build a tree from the staged index: %s",
                 (e && e->message) ? e->message : "unknown");
        fail(argv[2], m);
        git_index_free(index); git_repository_free(repo);
        return;
    }
    rc = git_tree_lookup(&tree, repo, &tree_oid);

    if (git_revparse_single(&headobj, repo, "HEAD") == 0 &&
        git_commit_lookup(&parent, repo, git_object_id(headobj)) == 0 &&
        git_commit_tree(&ptree, parent) == 0) {
        /* nothing to commit if the tree is unchanged */
        if (rc == 0 && git_oid_equal(git_tree_id(ptree), &tree_oid)) {
            git_tree_free(ptree); git_commit_free(parent);
            git_object_free(headobj);
            if (tree) git_tree_free(tree);
            git_index_free(index); git_repository_free(repo);
            mv_set_str(argv[2], "nothing to commit", 17);
            return;
        }
    }
    if (rc == 0 && git_signature_default(&sig, repo) != 0)
        rc = git_signature_now(&sig, "MVX", "mvx@localhost");
    if (rc == 0) {
        const git_commit *parents[1];
        int np = 0;
        if (parent) { parents[0] = parent; np = 1; }
        rc = git_commit_create(&commit_oid, repo, "HEAD", sig, sig,
                               NULL, msg, tree, np, parents);
    }
    if (sig) git_signature_free(sig);
    if (ptree) git_tree_free(ptree);
    if (parent) git_commit_free(parent);
    if (headobj) git_object_free(headobj);
    if (tree) git_tree_free(tree);
    git_index_free(index);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[2], "commit"); return; }
    char sha[8], out[80];
    git_oid_tostr(sha, sizeof sha, &commit_oid);
    snprintf(out, sizeof out, "[%s] %s", sha, msg);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* Distinct top-level file names across index + HEAD tree. */
typedef struct { char (*n)[256]; size_t c, cap; } nameset;

static void ns_add(nameset *s, const char *name) {
    for (size_t i = 0; i < s->c; i++)
        if (strcmp(s->n[i], name) == 0) return;
    if (s->c == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->n = realloc(s->n, s->cap * sizeof *s->n);
        if (!s->n) mv_fatal("out of memory in git status");
    }
    snprintf(s->n[s->c++], 256, "%s", name);
}

static void split_top(const char *path, char *top, size_t cap) {
    const char *slash = strchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : strlen(path);
    if (n >= cap) n = cap - 1;
    memcpy(top, path, n);
    top[n] = '\0';
}

/* Whether a top-level name is an MV file (its records are stored canonically,
   so status/commit read them through the driver) rather than a plain file or
   directory git tracks verbatim.  An on-disk directory is an MV file only if it
   carries a dictionary control (<name>.DICT/%FILE%) — a proper DIR file always
   has one; a plain directory (server/, test/, a .DICT itself, a submodule) does
   not.  A name with no on-disk directory but tracked in the index is an
   LMDB-backed hash file, so it is an MV file.  Pure stat(), run in the account
   (the engine has chdir'd there): it never opens the store, so status on a
   directory-only account cannot conjure an mvxdata.lmdb. */
/* The account's own files as the BACKEND reports them, cached for the process.
 *
 * The disk test below cannot answer this on every platform.  It recognises an MV
 * file by the MVX convention — a directory carrying <name>.DICT/%FILE% — and on
 * UniVerse the dictionary is a separate file called D_<name>, so a directory
 * file like BP or BP.O looks like an ordinary directory of ordinary files.  It
 * is then read TWICE with different byte semantics: once by git's plain-file
 * pass, once as records, and whichever staged last leaves the other permanently
 * disagreeing.  That is what left `M BP.O/...` in status after every commit.
 *
 * The VOC is the authority on what is a file, and mv_filelist is how the backend
 * says so.  Not available in the recordless build, which has no backend at all —
 * hence the guard rather than a call that would abort. */
static nameset g_bfiles;
static int g_bfiles_done;

static int backend_has_file(mv_ctx *ctx, const char *name) {
#ifdef MVXGIT_NORECORDS
    (void)ctx; (void)name;
    return 0;
#else
    if (!g_bfiles_done) {
        g_bfiles_done = 1;
        mv_value fl;
        mv_init(&fl);
        mv_filelist(ctx, &fl);              /* name<VM>type, @AM-separated */
        char nb[40];
        const char *p;
        int64_t len = mv_val_chars(&fl, nb, sizeof nb, &p), i = 0;
        while (i < len) {
            int64_t st = i;
            while (i < len && (unsigned char)p[i] != 0xFE &&
                   (unsigned char)p[i] != 0xFD) i++;
            int64_t nl = i - st;
            if (nl > 0 && nl < 256) {
                char nm[256];
                memcpy(nm, p + st, (size_t)nl);
                nm[nl] = '\0';
                ns_add(&g_bfiles, nm);
            }
            while (i < len && (unsigned char)p[i] != 0xFE) i++;
            if (i < len) i++;
        }
        mv_clear(&fl);
    }
    for (size_t k = 0; k < g_bfiles.c; k++)
        if (!strcmp(g_bfiles.n[k], name)) return 1;
    return 0;
#endif
}


/* Is this file actually THERE?
 *
 * Not is_mv_file(), which answers "could this name be a record file" and
 * returns 1 for anything without an on-disk directory — it can never report a
 * file gone, which is exactly what a deletion needs to know.  A file is live if
 * it is a directory carrying its own %FILE% control (MVX, directory-backed) or
 * the backend still lists it (a hash file on UniVerse/UniData, an LMDB file on
 * MVX). */
static int file_is_live(mv_ctx *ctx, const char *name) {
    struct stat sb;
    if (stat(name, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        char ctl[600];
        snprintf(ctl, sizeof ctl, "%s.DICT/%%FILE%%", name);
        if (stat(ctl, &sb) == 0) return 1;
    }
    return backend_has_file(ctx, name);
}

/* Did git track `top` as an MV FILE that the account no longer has?
 *
 * `%FILE%` is a file's EXISTENCE in git — a checkout creates the file from it —
 * so an index entry for <base>.DICT/%FILE% is git saying "this is a file", and
 * the file no longer being live is the account saying "it is gone".
 *
 * Deleting a file therefore behaves like removing a directory: every record
 * under <file>/ AND <file>.DICT/ is a deletion, the %FILE% control included.
 * Without this a DELETE.FILE was invisible — every entry whose file could not
 * be opened was skipped, so its records stayed in the index and in HEAD, and a
 * clone of that commit brought the entire file back.
 */
static char g_gone_memo[256];
static int  g_gone_ans = -1;
static int tracked_file_gone(mv_ctx *ctx, git_index *index, const char *top) {
    if (!top || !top[0] || !index) return 0;
    char base[256];
    snprintf(base, sizeof base, "%s", top);
    size_t bl = strlen(base);
    if (bl > 5 && strcmp(base + bl - 5, ".DICT") == 0) base[bl - 5] = '\0';
    if (!base[0]) return 0;
    /* Index entries are sorted by path, so a file's records arrive together and
       a one-entry memo makes the scan below effectively linear. */
    if (g_gone_ans >= 0 && strcmp(g_gone_memo, base) == 0) return g_gone_ans;
    int listed = live_listed(base);
    int alive = listed >= 0 ? listed : file_is_live(ctx, base);
    int ans = 0;
    if (!alive) {
        /* GIT TRACKED IT AS A FILE if the index holds CONTENT for it — a
           record, or a dictionary item — as opposed to nothing but its %FILE%
           control.  Every MV file has a dictionary and no plain directory does,
           so the .DICT subtree is the mark that works in both the open and the
           native form.  (It also keeps an ordinary tracked directory — docs/,
           say — from ever looking like a deleted file.)

           THE CONTROL ALONE IS A DECLARATION, NOT A DELETION.  Attributes live
           in the git objects, not in the account, so a control may perfectly
           well describe a file that is not on this machine: setting a file's
           geometry BEFORE the file exists, so a clone builds it right the first
           time, is a use of the attribute editor rather than a mistake.  Keying
           on "anything under the dictionary" counted that control as evidence
           the file had once been live, and pruned the declaration away on the
           next `add -A` — the edit simply evaporated.

           The cost is narrow and worth naming: a file that was genuinely
           deleted while EMPTY leaves its control behind, because nothing
           distinguishes it from a declaration.  `GIT RM` removes it. */
        char dpfx[600], rpfx[600], ctl[600];
        int dn = snprintf(dpfx, sizeof dpfx, "%s%s.DICT/", g_prefix, base);
        int rn = snprintf(rpfx, sizeof rpfx, "%s%s/", g_prefix, base);
        snprintf(ctl, sizeof ctl, "%s%s.DICT/%%FILE%%", g_prefix, base);
        if (dn > 0 && rn > 0) {
            for (size_t i = 0; i < git_index_entrycount(index); i++) {
                const git_index_entry *e = git_index_get_byindex(index, i);
                if (!e) continue;
                if (strcmp(e->path, ctl) == 0) continue;   /* the declaration */
                if (strncmp(e->path, dpfx, (size_t)dn) == 0 ||
                    strncmp(e->path, rpfx, (size_t)rn) == 0) { ans = 1; break; }
            }
        }
    }
    snprintf(g_gone_memo, sizeof g_gone_memo, "%s", base);
    g_gone_ans = ans;
    return ans;
}

/* Drop what was cached about the account we were in.  Called when the prefix
   changes (a different account) and by the CLI between accounts. */
/* Forget what the ACCOUNT looked like — its file list and the deletion memo.
 * Both are answers about live state, and mvgitd serves many commands from one
 * process, so a cache that outlives a command is a cache that reports a file
 * created a moment ago as absent (and one just deleted as present).  Cheap: one
 * FILELIST per command. */
static void backend_files_reset(void) {
    free(g_bfiles.n);
    g_bfiles.n = NULL;
    g_bfiles.c = g_bfiles.cap = 0;
    g_bfiles_done = 0;
    g_gone_ans = -1;
}

void mv_git_forget_account(void) {
    g_gone_ans = -1;                 /* the memo belongs to the old account */
    free(g_bfiles.n);
    g_bfiles.n = NULL;
    g_bfiles.c = g_bfiles.cap = 0;
    g_bfiles_done = 0;
}

static int is_mv_file(const char *name) {
    struct stat sb;
    if (stat(name, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        char ctl[600];
        snprintf(ctl, sizeof ctl, "%s.DICT/%%FILE%%", name);
        return stat(ctl, &sb) == 0;
    }
    return 1;   /* no on-disk directory ⇒ LMDB-backed */
}

/* The open-account CLASS of a %FILE% control's content: the native FILE<VM>type,
   the bare open "DIR"/"hash", and the extended "hash <modulo> DYNAMIC|STATIC"
   all reduce to "DIR" or "hash".  Returns the length written to `out`, or -1 if
   the content is not a recognisable control.  status/diff normalise both the
   live on-disk control and the committed blob through this and compare classes,
   so a clean account reads clean — and the modulo, a sticky default (mv_git#8),
   is never mistaken for a live change. */
static int control_open(const char *c, int64_t cl, char *out, size_t cap) {
    while (cl > 0 && (c[cl - 1] == '\n' || c[cl - 1] == '\r' ||
                      c[cl - 1] == ' ' || c[cl - 1] == '\t'))
        cl--;
    if (cl == 3 && strncasecmp(c, "DIR", 3) == 0) { snprintf(out, cap, "DIR"); return 3; }
    /* "hash", or "hash <modulo> DYNAMIC|STATIC" — the class is just "hash". */
    if (cl >= 4 && strncasecmp(c, "hash", 4) == 0) { snprintf(out, cap, "hash"); return 4; }
    if (cl >= 5 && memcmp(c, "FILE", 4) == 0 && (unsigned char)c[4] == 0xFD) {
        const char *t = c + 5, *end = c + cl;
        const char *m2 = memchr(t, 0xFD, (size_t)(end - t));
        const char *te = m2 ? m2 : end;
        if (te - t == 3 && strncasecmp(t, "dir", 3) == 0) { snprintf(out, cap, "DIR"); return 3; }
        snprintf(out, cap, "hash");
        return 4;
    }
    return -1;
}

/* GITSTATUS(repo, out) — real-git short status across tracked files. */
/* Defined with the staging helpers below; status needs the same judgement. */
static int path_storable(const char *path);

void mvx_sub_GITSTATUS(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    backend_files_reset();      /* the account as it IS, not as it was cached */
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    openaccount_sync(rp);
    /* status must apply exactly what add applies, or a record add never stages
       reads as untracked forever. */
    stock_ensure_udt(ctx, rp);
    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[1], "open"); return; }
    git_tree *ht = head_tree(repo);
    sbuf s = {0, 0, 0};

    /* staged: HEAD tree vs index */
    git_diff *sd = NULL;
    if (git_diff_tree_to_index(&sd, repo, ht, index, NULL) == 0) {
        size_t n = git_diff_num_deltas(sd);
        for (size_t i = 0; i < n; i++) {
            const git_diff_delta *d = git_diff_get_delta(sd, i);
            char line[700];
            snprintf(line, sizeof line, "%c  %s",
                     git_diff_status_char(d->status), d->new_file.path);
            sb_line(&s, line);
        }
        git_diff_free(sd);
    }

    /* working (records) vs index: modified / untracked / deleted */
    nameset files = {0, 0, 0};
    size_t ic = git_index_entrycount(index);
    for (size_t i = 0; i < ic; i++) {
        const git_index_entry *e = git_index_get_byindex(index, i);
        if (e->mode == GIT_FILEMODE_COMMIT) continue;   /* submodule gitlink */
        /* Index paths are repository-relative and this account is only part of
           the repository: strip our prefix, and ignore what belongs to another
           account — its records are not ours to compare. */
        const char *rel = unprefix(e->path);
        if (!rel) continue;
        char top[256];
        split_top(rel, top, sizeof top);
        ns_add(&files, top);
    }
    for (size_t f = 0; f < files.c; f++) {
        const char *fn = files.n[f];
        if (!is_mv_file(fn) && !backend_has_file(ctx, fn))
            continue;                     /* plain path — git diffs it, below */
        mv_value fvar, id, rec;
        mv_init(&fvar); mv_init(&id); mv_init(&rec);
        if (open_named(ctx, fn, &fvar)) {
            /* The same two exclusions `add` applies, because a record add
               deliberately never stages must not then be reported as untracked —
               it would be untracked forever, and no amount of committing would
               clear it.  This is what made `status` dirty immediately after a
               commit on UniVerse: a PICK-flavour VOC is full of type K and V
               records (keywords and verbs the destination supplies its own copies
               of), add dropped every one, and status listed every one. */
            int is_voc = strcasecmp(fn, "VOC") == 0 || strcasecmp(fn, "MD") == 0;
            int voc_open = is_voc && mv_openaccount();
            mv_select(ctx, &fvar);
            while (mv_readnext(ctx, &id)) {
                if (!mv_read(ctx, &rec, &fvar, &id, 0)) continue;
                char idb[256], nb[40], path[600];
                arg_str(&id, idb, sizeof idb);
                if (is_voc) {
                    const char *vp;
                    char vnb[40];
                    int64_t vl = mv_val_chars(&rec, vnb, sizeof vnb, &vp);
                    int64_t f1 = 0;
                    while (f1 < vl && (unsigned char)vp[f1] != 0xFE) f1++;
                    int cls = mv_voc_class(vp, f1);
                    if (cls == 1 || (cls == 2 && voc_open)) continue;
                    /* An object FILE is not committed, so neither is its VOC
                       pointer — and reporting the pointer would leave it
                       untracked forever, since no add will ever stage it. */
                    {
                        size_t il = strlen(idb);
                        if (il > 2 && strcmp(idb + il - 2, ".O") == 0) {
                            char b[256];
                            snprintf(b, sizeof b, "%.*s", (int)(il - 2), idb);
                            if (backend_has_file(ctx, b)) continue;
                        }
                    }
                }
                /* A compiled object is not staged by any wholesale add, so
                   reporting it would leave it untracked forever — the same
                   reason, and the same function, as on the add side. */
                {
                    const char *ocp;
                    char onb[40];
                    int64_t ocl = mv_val_chars(&rec, onb, sizeof onb, &ocp);
                    if (record_is_object(ctx, &fvar, idb, ocp, ocl)) continue;
                }
                record_path(path, sizeof path, fn, idb);
                /* An id that cannot be a git path was never staged either
                   (mv_git#42) — reporting it as untracked would be reporting
                   something no commit can ever fix. */
                if (!path_storable(path)) continue;
                const git_index_entry *entry =
                    git_index_get_bypath(index, path, 0);
                const char *cp;
                int64_t clen = mv_val_chars(&rec, nb, sizeof nb, &cp);
                /* A stock record that nobody staged is the system's furniture,
                   and `add -A` skips it — so reporting it would leave it
                   untracked forever, the trap type K and V fell into (#51).
                   One that IS staged was staged deliberately, and from then on
                   it is ordinary tracked content: changes to it must show. */
                if (is_voc && !entry && is_stock_record(idb, cp, clen))
                    continue;
                git_oid woid;
                char ofb[16];
                int ofl;
                if (mv_openaccount() && strcmp(idb, "%FILE%") == 0 &&
                    (ofl = control_open(cp, clen, ofb, sizeof ofb)) >= 0) {
                    /* %FILE%: compare the CLASS against the committed blob — the
                       committed modulo is a sticky default (mv_git#8), not a live
                       diff, so a matching class (hash/DIR) is clean. */
                    if (entry) {
                        char comm[16];
                        int cml = -1;
                        git_blob *cb = NULL;
                        if (git_blob_lookup(&cb, repo, &entry->id) == 0) {
                            cml = control_open(git_blob_rawcontent(cb),
                                               (int64_t)git_blob_rawsize(cb),
                                               comm, sizeof comm);
                            git_blob_free(cb);
                        }
                        if (cml == ofl && memcmp(ofb, comm, (size_t)ofl) == 0)
                            continue;   /* same class — clean */
                    }
                    record_oid(ofb, ofl, &woid);   /* class change / untracked */
                } else {
                    record_oid(cp, clen, &woid);
                }
                if (!entry) {
                    if (ignored(fn, path)) continue;   /* MVX GIT IGNORE list */
                    if (git_path_ignored(repo, path)) continue;   /* .gitignore */
                    if (is_provision_pointer(fn, idb)) continue;  /* BUILD-derived */
                    char line[700];
                    snprintf(line, sizeof line, "?? %s", path);
                    sb_line(&s, line);
                } else if (!git_oid_equal(&entry->id, &woid)) {
                    char line[700];
                    snprintf(line, sizeof line, " M %s", path);
                    sb_line(&s, line);
                }
            }
        }
        mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
    }
    /* deleted: index entry whose record no longer exists */
    for (size_t i = 0; i < ic; i++) {
        const git_index_entry *e = git_index_get_byindex(index, i);
        if (e->mode == GIT_FILEMODE_COMMIT) continue;   /* submodule gitlink */
        const char *rel = unprefix(e->path);
        if (!rel) continue;               /* another account's record */
        char top[256];
        split_top(rel, top, sizeof top);
        /* The whole file may be gone — DELETE.FILE, or a checkout that dropped
           its %FILE%.  Then everything under it is deleted, not skipped. */
        int gone_file = tracked_file_gone(ctx, index, top);
        if (!gone_file && !is_mv_file(top) && !backend_has_file(ctx, top))
            continue;                     /* plain path — git tracks deletions */
        /* A record path is `<file>/<id>`.  An entry with no slash is a plain
           file at the account root, and git tracks its own deletions — but its
           name may still BE an MV file's (VOCLIB, VOC …), in which case the
           read below looks for a record of that name inside that file, fails,
           and reports the file itself as a deleted record. */
        const char *recid = strchr(rel, '/');
        if (!recid) continue;
        recid++;
        /* `%FILE%` is SYNTHESISED from the live file's geometry, not read from
           it (see stage_file_control) — on UniVerse and UniData no such record
           exists to find.  Looking for one and not finding it is expected, so
           reporting it deleted would make every account permanently dirty the
           moment its files were described. */
        if (!gone_file && strcmp(recid, "%FILE%") == 0) continue;
        if (gone_file) {                  /* the file itself went: all of it */
            char line[700];
            snprintf(line, sizeof line, " D %s", e->path);
            sb_line(&s, line);
            continue;
        }
        mv_value fvar, id, rec;
        mv_init(&fvar); mv_init(&id); mv_init(&rec);
        int isrec = 0, gone = 0;
        if (open_named(ctx, top, &fvar)) {
            isrec = 1;                      /* a real MV file; the entry is a record */
            mv_set_str(&id, recid, (int64_t)strlen(recid));
            gone = !mv_read(ctx, &rec, &fvar, &id, 0);
        }
        mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
        /* only a record of an MV file can be reported deleted; a plain file
           (README, .gitmodules, …) that isn't an MV record is left to git */
        if (isrec && gone) {
            char line[700];
            snprintf(line, sizeof line, " D %s", e->path);
            sb_line(&s, line);
        }
    }
    free(files.n);

    /* plain (non-MV) paths: diff the working tree against the index for tracked
       files exactly as git would — modified or deleted — skipping MV files,
       whose records are diffed above (their git-native working bytes differ
       from the canonical staged blob, so git's own diff would mis-report them).
       Untracked files are left out: the record-git model reports through its
       own GITIGNORE, not the working tree at large. */
    git_diff *wd = NULL;
    git_diff_options wo = GIT_DIFF_OPTIONS_INIT;
    if (git_diff_index_to_workdir(&wd, repo, index, &wo) == 0) {
        size_t n = git_diff_num_deltas(wd);
        for (size_t i = 0; i < n; i++) {
            const git_diff_delta *d = git_diff_get_delta(wd, i);
            /* A submodule gitlink is git's to track, not a record: git resolves
               it against the submodule's own HEAD, but this workdir diff cannot,
               so a clean (or un-checked-out) submodule surfaces here as ` D name`
               (mv_git#1).  The other two status loops already skip gitlinks; do
               the same so a submodule is never reported as a deleted record.
               A genuinely restaged gitlink still shows via the tree↔index diff
               above, exactly as git reports it. */
            if (d->old_file.mode == GIT_FILEMODE_COMMIT ||
                d->new_file.mode == GIT_FILEMODE_COMMIT)
                continue;
            char top[256];
            {
                const char *rel = unprefix(d->new_file.path);
                split_top(rel ? rel : d->new_file.path, top, sizeof top);
            }
            /* the descriptor is committed at `.mv-account` (portable form) but on
               disk is native `.mvx` — project the on-disk `.mvx` down to the open
               form and compare that against the committed blob, so a local
               permit/deny edit is invisible while a real identity change shows. */
            if (mv_openaccount() && strcmp(d->new_file.path, ".mv-account") == 0) {
                FILE *df = fopen(".mvx", "rb");
                int clean = 0;
                if (df) {
                    char buf[65536];
                    size_t bn = fread(buf, 1, sizeof buf, df);
                    fclose(df);
                    acct_desc ad;
                    desc_parse(buf, bn, &ad);
                    char open[1024];
                    int ol = desc_render_open(&ad, open, sizeof open);
                    git_oid woid;
                    if (ol > 0 &&
                        git_odb_hash(&woid, open, (size_t)ol,
                                     GIT_OBJECT_BLOB) == 0 &&
                        git_oid_equal(&woid, &d->old_file.id))
                        clean = 1;
                }
                if (clean) continue;
                sb_line(&s, " M .mv-account");
                continue;
            }
            if (is_mv_file(top) || backend_has_file(ctx, top)) continue;
            /* an open account's on-disk %FILE% is native (FILE<VM>type) while
               the committed blob is the open form (DIR/hash); compare in
               open-space and skip when they match. */
            if (mv_openaccount() && is_file_control(d->new_file.path)) {
                FILE *cf = fopen(d->new_file.path, "rb");
                if (cf) {
                    char nat[256];
                    size_t nn = fread(nat, 1, sizeof nat, cf);
                    fclose(cf);
                    /* Compare CLASS only: the live on-disk control is native
                       (its class), the committed blob is the open form carrying a
                       sticky modulo (mv_git#8) — normalise both and skip when the
                       class matches; only a hash<->DIR change is a real diff. */
                    char live[16], comm[16];
                    int ll = control_open(nat, (int64_t)nn, live, sizeof live);
                    int cml = -1;
                    git_blob *cb = NULL;
                    if (git_blob_lookup(&cb, repo, &d->old_file.id) == 0) {
                        cml = control_open(git_blob_rawcontent(cb),
                                           (int64_t)git_blob_rawsize(cb),
                                           comm, sizeof comm);
                        git_blob_free(cb);
                    }
                    if (ll >= 0 && cml == ll && memcmp(live, comm, (size_t)ll) == 0)
                        continue;   /* same class — clean */
                }
            }
            char line[700];
            snprintf(line, sizeof line, " %c %s",
                     git_diff_status_char(d->status), d->new_file.path);
            sb_line(&s, line);
        }
        git_diff_free(wd);
    }

    if (ht) git_tree_free(ht);
    git_index_free(index);
    git_repository_free(repo);
    sb_out(&s, argv[1], "nothing to commit, working tree clean");
}

/* GITLOG(repo, count, out) */
void mvx_sub_GITLOG(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], cnt[32];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], cnt, sizeof cnt);
    int64_t want = atoll(cnt);
    if (want <= 0) want = 20;
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) {
        mv_set_str(argv[2], "no history", 10);
        return;
    }
    git_revwalk *w = NULL;
    sbuf s = {0, 0, 0};
    if (git_revwalk_new(&w, repo) == 0 && git_revwalk_push_head(w) == 0) {
        git_oid oid;
        int64_t seen = 0;
        while (seen < want && git_revwalk_next(&oid, w) == 0) {
            git_commit *c = NULL;
            if (git_commit_lookup(&c, repo, &oid) != 0) break;
            /* Standard `git log` layout: a full-SHA "commit" header, Author,
               Date in the author's own timezone, a blank line, then the message
               indented four spaces, then a blank separator. */
            char full[41];
            git_oid_tostr(full, sizeof full, &oid);
            const git_signature *au = git_commit_author(c);
            char line[4300];
            snprintf(line, sizeof line, "commit %s", full);
            sb_line(&s, line);
            if (au) {
                snprintf(line, sizeof line, "Author: %s <%s>",
                         au->name ? au->name : "", au->email ? au->email : "");
                sb_line(&s, line);
                time_t local = (time_t)(au->when.time + (int64_t)au->when.offset * 60);
                struct tm tmv;
                gmtime_r(&local, &tmv);
                char dbuf[64];
                strftime(dbuf, sizeof dbuf, "%a %b %e %H:%M:%S %Y", &tmv);
                int off = au->when.offset, ao = off < 0 ? -off : off;
                snprintf(line, sizeof line, "Date:   %s %c%02d%02d", dbuf,
                         off < 0 ? '-' : '+', ao / 60, ao % 60);
                sb_line(&s, line);
            }
            sb_line(&s, "");
            const char *msg = git_commit_message(c);
            for (const char *p = msg ? msg : ""; *p; ) {
                const char *nl = strchr(p, '\n');
                int len = nl ? (int)(nl - p) : (int)strlen(p);
                snprintf(line, sizeof line, "    %.*s", len, p);
                sb_line(&s, line);
                if (!nl) break;
                p = nl + 1;
            }
            sb_line(&s, "");
            git_commit_free(c);
            seen++;
        }
    }
    if (w) git_revwalk_free(w);
    git_repository_free(repo);
    sb_out(&s, argv[2], "no history");
}

/* diff line callback: accumulate +/-/space lines into an sbuf. */
static int diff_line_cb(const git_diff_delta *d, const git_diff_hunk *h,
                        const git_diff_line *l, void *payload) {
    (void)d; (void)h;
    sbuf *s = payload;
    char pfx = l->origin;               /* '+', '-', ' ', 'F','H' */
    /* records never end in a newline; drop the "no newline" markers */
    if (pfx == GIT_DIFF_LINE_CONTEXT_EOFNL ||
        pfx == GIT_DIFF_LINE_ADD_EOFNL ||
        pfx == GIT_DIFF_LINE_DEL_EOFNL)
        return 0;
    char line[4200];
    int n;
    if (pfx == '+' || pfx == '-' || pfx == ' ')
        n = snprintf(line, sizeof line, "%c%.*s", pfx,
                     (int)l->content_len, l->content);
    else
        n = snprintf(line, sizeof line, "%.*s",
                     (int)l->content_len, l->content);
    /* content includes its own newline; strip for line-per-attribute */
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    sb_line(s, line);
    return 0;
}

/* hunk header callback: the "@@ -a,b +c,d @@" line a unified diff carries.
   Passed only for the -u form; without it libgit2 still emits context, which is
   why the compact output already looked nearly unified — what it never had was
   any way to tell WHERE in the record the change was. */
static int diff_hunk_cb(const git_diff_delta *d, const git_diff_hunk *h,
                        void *payload) {
    (void)d;
    sbuf *s = payload;
    char line[300];
    int n = snprintf(line, sizeof line, "%.*s", (int)h->header_len, h->header);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    sb_line(s, line);
    return 0;
}

/* GITUDIFF(oldtext, newtext, path, out) — a unified diff of two CONTENTS, with
   hunk headers, rendered exactly the way the record diff renders one.

   The two sides arrive @AM-separated (that is what CAT and IXCAT return) and go
   back the same way, one output line per attribute.  It exists so the BASIC
   diff bodies — the staged diff on every platform, and the unstaged one on
   UniData and UniVerse — render through the SAME libgit2 call the C diff uses
   rather than growing a second, hand-written diff that would drift from it. */
void mvx_sub_GITUDIFF(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 4) return;
    ensure_init();
    char path[700];
    const char *op, *np;
    char onb[40], nnb[40];
    int64_t ol = mv_val_chars(argv[0], onb, sizeof onb, &op);
    int64_t nl = mv_val_chars(argv[1], nnb, sizeof nnb, &np);
    arg_str(argv[2], path, sizeof path);
    int64_t obl = 0, nbl = 0;
    char *ob = xlate(op, ol, (char)0xFE, '\n', &obl);
    char *nb = xlate(np, nl, (char)0xFE, '\n', &nbl);
    sbuf s = {0, 0, 0};
    git_diff_buffers(ob, (size_t)obl, path, nb, (size_t)nbl, path, NULL,
                     NULL, NULL, diff_hunk_cb, diff_line_cb, &s);
    free(ob); free(nb);
    sb_out(&s, argv[3], "");
}

/* GITDIFF(repo, file, out) — unstaged record changes (working vs index)
   as a unified diff.  file "" diffs all tracked files. */
static void diff_run(mv_ctx *ctx, int32_t argc, mv_value **argv, int unified);

/* The compact form (no hunk headers) and the -u form share one body; only the
   hunk callback differs, and a second copy of this walk is the last thing this
   file needs. */
void mvx_sub_GITDIFF(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    diff_run(ctx, argc, argv, 0);
}
void mvx_sub_GITDIFFU(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    diff_run(ctx, argc, argv, 1);
}
static void diff_run(mv_ctx *ctx, int32_t argc, mv_value **argv, int unified) {
    if (argc < 3) return;
    ensure_init();
    char rp[4096], only[256];
    arg_str(argv[0], rp, sizeof rp);
    openaccount_sync(rp);
    arg_str(argv[1], only, sizeof only);
    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[2], "open"); return; }
    sbuf s = {0, 0, 0};

    size_t ic = git_index_entrycount(index);
    for (size_t i = 0; i < ic; i++) {
        const git_index_entry *e = git_index_get_byindex(index, i);
        char top[256];
        split_top(e->path, top, sizeof top);
        if (only[0] && strcmp(only, top) != 0) continue;
        const char *recid = strchr(e->path, '/');
        recid = recid ? recid + 1 : e->path;

        mv_value fvar, id, rec;
        mv_init(&fvar); mv_init(&id); mv_init(&rec);
        int have = 0;
        if (open_named(ctx, top, &fvar)) {
            mv_set_str(&id, recid, (int64_t)strlen(recid));
            have = mv_read(ctx, &rec, &fvar, &id, 0);
        }
        git_blob *old = NULL;
        if (git_blob_lookup(&old, repo, &e->id) == 0) {
            char nb[40];
            const char *cp = "";
            int64_t clen = 0, bl = 0;
            char *buf = NULL;
            int open = mv_openaccount();
            char ofb[16];
            int ofl;
            if (open && strcmp(e->path, ".mv-account") == 0) {
                /* descriptor: on disk it is native `.mvx`; diff its portable
                   projection (open form) against the committed `.mv-account`, so
                   a local permit/deny edit is not shown as a change */
                FILE *df = fopen(".mvx", "rb");
                if (df) {
                    char raw[65536];
                    size_t rn = fread(raw, 1, sizeof raw, df);
                    fclose(df);
                    acct_desc ad;
                    desc_parse(raw, rn, &ad);
                    buf = malloc(1024);
                    if (buf) bl = desc_render_open(&ad, buf, 1024);
                }
            } else if (open && strcmp(recid, "%FILE%") == 0 && have &&
                       (clen = mv_val_chars(&rec, nb, sizeof nb, &cp),
                        (ofl = control_open(cp, clen, ofb, sizeof ofb)) >= 0)) {
                /* an open account's on-disk %FILE% is native (FILE<VM>type) while
                   the committed blob is the open form carrying a sticky modulo
                   (mv_git#8) — compare CLASS only.  When the class matches, use
                   the committed bytes so the blob-compare below reports no change
                   (the modulo is not a live diff); otherwise show the class
                   change against the live class. */
                char comm[16];
                int cml = control_open(git_blob_rawcontent(old),
                                       (int64_t)git_blob_rawsize(old),
                                       comm, sizeof comm);
                if (cml == ofl && memcmp(ofb, comm, (size_t)ofl) == 0) {
                    bl = (int64_t)git_blob_rawsize(old);
                    buf = malloc((size_t)bl);
                    if (buf) memcpy(buf, git_blob_rawcontent(old), (size_t)bl);
                } else {
                    buf = malloc((size_t)ofl);
                    if (buf) { memcpy(buf, ofb, (size_t)ofl); bl = ofl; }
                }
            } else if (is_mv_file(top) && have) {
                /* A genuine MV record: `add` staged it as the read form with
                   marks->newlines, so diff that same form. */
                clen = mv_val_chars(&rec, nb, sizeof nb, &cp);
                buf = xlate(cp, clen, (char)0xFE, '\n', &bl);
            } else {
                /* Not an MV record — a directory-backed dictionary item (mv_git#6),
                   a plain file (.mvx, README), or a file under a plain
                   subdirectory.  `add` staged its blob from the raw on-disk bytes,
                   which keep the dir driver's trailing terminator that mvx_read
                   strips; diff those raw bytes so a clean item reads clean.  A
                   missing file leaves buf empty — a real deletion. */
                FILE *rf = fopen(e->path, "rb");
                if (rf) {
                    fseek(rf, 0, SEEK_END);
                    long fsz = ftell(rf);
                    fseek(rf, 0, SEEK_SET);
                    if (fsz >= 0 && (buf = malloc((size_t)fsz + 1)))
                        bl = (int64_t)fread(buf, 1, (size_t)fsz, rf);
                    fclose(rf);
                }
            }
            git_oid woid;
            int changed = 1;
            if (git_odb_hash(&woid, buf ? buf : "", (size_t)bl,
                             GIT_OBJECT_BLOB) == 0)
                changed = !git_oid_equal(&woid, &e->id);
            if (changed) {
                char hdr[700];
                snprintf(hdr, sizeof hdr, "diff %s", e->path);
                sb_line(&s, hdr);
                git_diff_blob_to_buffer(old, e->path, buf, (size_t)bl,
                                        e->path, NULL, NULL, NULL,
                                        unified ? diff_hunk_cb : NULL,
                                        diff_line_cb, &s);
            }
            free(buf);
            git_blob_free(old);
        }
        mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
    }
    git_index_free(index);
    git_repository_free(repo);
    sb_out(&s, argv[2], "no changes");
}

/* GITSHOW(repo, file, record, out) — committed content of a record. */
void mvx_sub_GITSHOW(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 4) return;
    ensure_init();
    char rp[4096], fn[256], recid[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], fn, sizeof fn);
    arg_str(argv[2], recid, sizeof recid);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[3], "open"); return; }
    git_tree *t = head_tree(repo);
    char path[600];
    record_path(path, sizeof path, fn, recid);
    git_tree_entry *te = NULL;
    git_blob *blob = NULL;
    if (t && git_tree_entry_bypath(&te, t, path) == 0 &&
        git_blob_lookup(&blob, repo, git_tree_entry_id(te)) == 0) {
        const char *cp = git_blob_rawcontent(blob);
        int64_t clen = (int64_t)git_blob_rawsize(blob), rl;
        char *r = xlate(cp, clen, '\n', (char)0xFE, &rl);
        mv_set_str(argv[3], r, rl);
        free(r);
        git_blob_free(blob);
    } else {
        mv_set_str(argv[3], "", 0);
    }
    if (te) git_tree_entry_free(te);
    if (t) git_tree_free(t);
    git_repository_free(repo);
}

/* Collect every blob path in a tree walk (records, dict items, open-form
   controls) into an @AM-separated list — the checkout side iterates it. */
static int gitfiles_cb(const char *root, const git_tree_entry *e, void *pl) {
    if (git_tree_entry_type(e) == GIT_OBJECT_BLOB) {
        char line[900];
        snprintf(line, sizeof line, "%s%s", root, git_tree_entry_name(e));
        sb_line((sbuf *)pl, line);
    }
    return 0;
}

/* GITFILES(repo, out) — every blob path in HEAD, @AM-separated. */
void mvx_sub_GITFILES(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[1], "open"); return; }
    git_tree *t = head_tree(repo);
    sbuf s = {0, 0, 0};
    if (t) {
        git_tree_walk(t, GIT_TREEWALK_PRE, gitfiles_cb, &s);
        git_tree_free(t);
    }
    git_repository_free(repo);
    sb_out(&s, argv[1], "");
}

/* GITCAT(repo, path, out) — the committed content of blob `path` with attribute
   marks restored (newlines -> @AM), ready for the checkout side to WRITE. */
void mvx_sub_GITCAT(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], path[700];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], path, sizeof path);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }
    git_tree *t = head_tree(repo);
    git_tree_entry *te = NULL;
    git_blob *blob = NULL;
    if (t && git_tree_entry_bypath(&te, t, path) == 0 &&
        git_blob_lookup(&blob, repo, git_tree_entry_id(te)) == 0) {
        const char *cp = git_blob_rawcontent(blob);
        int64_t clen = (int64_t)git_blob_rawsize(blob), rl;
        char *r = xlate(cp, clen, '\n', (char)0xFE, &rl);
        mv_set_str(argv[2], r, rl);
        free(r);
        git_blob_free(blob);
    } else {
        mv_set_str(argv[2], "", 0);
    }
    if (te) git_tree_entry_free(te);
    if (t) git_tree_free(t);
    git_repository_free(repo);
}

/* GITIXCAT(repo, path, out) — the STAGED content of blob `path`: the index, not
   HEAD, with attribute marks restored the way GITCAT does.  "" when the index
   has no such path.

   The attribute editor has to read what it is about to edit, and for an
   attribute set that is the INDEX.  HEAD is only what was last committed, so a
   second `GIT ATTR --set` before a commit read from HEAD would build on the
   commit rather than on the first edit — and every edit but the last would
   disappear with no sign that it had. */
void mvx_sub_GITIXCAT(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], path[700];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], path, sizeof path);
    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[2], "open"); return; }
    const git_index_entry *e = git_index_get_bypath(index, path, 0);
    git_blob *blob = NULL;
    if (e && git_blob_lookup(&blob, repo, &e->id) == 0) {
        const char *cp = git_blob_rawcontent(blob);
        int64_t clen = (int64_t)git_blob_rawsize(blob), rl;
        char *r = xlate(cp, clen, '\n', (char)0xFE, &rl);
        mv_set_str(argv[2], r, rl);
        free(r);
        git_blob_free(blob);
    } else {
        mv_set_str(argv[2], "", 0);
    }
    git_index_free(index);
    git_repository_free(repo);
}

/* GITSTAGED(repo, out) — what the index holds that HEAD does not, one
   "<status>  <path>" line per delta: the same first-column form the C status
   already prints for its staged section.

   The session platforms' status walks HEAD's paths and compares each with the
   live record, which can see a record change or a record vanish but can never
   see the INDEX.  Anything staged with no backing record to compare against — a
   %FILE% control, the account descriptor — was therefore invisible there and
   visible on MVX, for the same repository.  This is the arm that closes that. */
void mvx_sub_GITSTAGED(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[1], "open"); return; }
    git_tree *ht = head_tree(repo);
    sbuf s = {0, 0, 0};
    git_diff *sd = NULL;
    if (git_diff_tree_to_index(&sd, repo, ht, index, NULL) == 0) {
        size_t n = git_diff_num_deltas(sd);
        for (size_t i = 0; i < n; i++) {
            const git_diff_delta *d = git_diff_get_delta(sd, i);
            char line[700];
            snprintf(line, sizeof line, "%c  %s",
                     git_diff_status_char(d->status), d->new_file.path);
            sb_line(&s, line);
        }
        git_diff_free(sd);
    }
    if (ht) git_tree_free(ht);
    git_index_free(index);
    git_repository_free(repo);
    sb_out(&s, argv[1], "");
}

/* Write one tracked subtree's records back into its MVX file, deleting
   records absent from the tree. */
/* Map a %FILE% control's content — the open form DIR/hash, or a native
   FILE<VM>type — to a CREATE-FILE type spec: "DIR" for a directory file, "" for
   the account's default hash backend, or the extended hash form
   "hash <modulo> DYNAMIC|STATIC" when the open control records a modulo.  The
   modulo spec lets a materialise recreate a hash file at its real size (and keep
   a static file static) instead of always creating a default dynamic file; a
   backend that has no notion of modulo (MVX/LMDB) sees a leading "hash" and
   treats it as the default hash, ignoring the size hint. */
static void control_type(const char *c, int64_t cl, char *out, size_t cap) {
    out[0] = '\0';
    if (!c) return;
    /* THE CLASS IS LINE ONE.  Everything after the first newline is a
       `key = value` registry parameter (MV_GIT_CTL_MAX), not part of the class,
       so stop there before trimming — reading the whole blob turned an extended
       control into an unrecognised class and the file came back as the wrong
       kind entirely. */
    {
        const char *nl = memchr(c, '\n', (size_t)cl);
        if (nl) cl = nl - c;
    }
    while (cl > 0 && (c[cl - 1] == '\n' || c[cl - 1] == '\r' ||
                      c[cl - 1] == ' ' || c[cl - 1] == '\t'))
        cl--;
    if (cl == 3 && strncasecmp(c, "DIR", 3) == 0) { snprintf(out, cap, "DIR"); return; }
    if (cl >= 4 && strncasecmp(c, "hash", 4) == 0) {
        /* "hash" alone -> default hash (""); "hash <modulo> <DYNAMIC|STATIC>"
           -> pass the validated modulo spec through for the backend to honour. */
        const char *p = c + 4, *end = c + cl;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) return;                            /* bare hash */
        long mod = 0; const char *mp = p;
        while (mp < end && *mp >= '0' && *mp <= '9') { mod = mod * 10 + (*mp - '0'); mp++; }
        if (mp == p || mod <= 0) return;                 /* no valid modulo */
        while (mp < end && (*mp == ' ' || *mp == '\t')) mp++;
        const char *dyn = "DYNAMIC";
        if (end - mp >= 6 && strncasecmp(mp, "STATIC", 6) == 0) dyn = "STATIC";
        snprintf(out, cap, "hash %ld %s", mod, dyn);
        return;
    }
    if (cl >= 5 && memcmp(c, "FILE", 4) == 0 && (unsigned char)c[4] == 0xFD) {
        const char *t = c + 5, *end = c + cl;
        const char *m2 = memchr(t, 0xFD, (size_t)(end - t));
        const char *te = m2 ? m2 : end;
        if (te - t == 3 && strncasecmp(t, "dir", 3) == 0) snprintf(out, cap, "DIR");
    }
}

/* The CREATE-FILE type of the file whose dictionary control lives at
   `<base>.DICT/%FILE%` in `head` (DIR, or "" for the default hash backend). */
static void file_type_of(git_repository *repo, git_tree *head, const char *base,
                         char *out, size_t cap) {
    out[0] = '\0';
    char path[600];
    snprintf(path, sizeof path, "%s.DICT/%%FILE%%", base);
    git_tree_entry *te = NULL;
    if (git_tree_entry_bypath(&te, head, path) != 0) return;
    git_blob *b = NULL;
    if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) == 0) {
        control_type(git_blob_rawcontent(b), (int64_t)git_blob_rawsize(b),
                     out, cap);
        git_blob_free(b);
    }
    git_tree_entry_free(te);
}

static void materialize_file(mv_ctx *ctx, git_repository *repo, git_tree *head,
                             git_tree *subtree, const char *fn,
                             int keep_extra, int64_t *nw, int64_t *nd) {
    mv_value fvar, id, rec;
    mv_init(&fvar); mv_init(&id); mv_init(&rec);
    size_t ln = strlen(fn);
    int is_dict = ln > 5 && strcmp(fn + ln - 5, ".DICT") == 0;
    char base[300], type[64] = "";
    if (is_dict) snprintf(base, sizeof base, "%.*s", (int)(ln - 5), fn);
    else         snprintf(base, sizeof base, "%s", fn);
    file_type_of(repo, head, base, type, sizeof type);
    /* A directory-backed file terminates each record with a newline on disk and
       strips a trailing empty attribute on read, so its git blob carries a
       trailing terminator mark.  Writing that mark back verbatim would let the
       backend add a second terminator and the record would grow by one empty
       attribute on every clone — drop the one trailing mark so the write/read
       round-trip is stable.  Hash backends store records verbatim, so leave
       theirs untouched (a genuine trailing empty attribute is preserved). */
    int is_dir = strcasecmp(type, "DIR") == 0;
    if (fn[0]) {
        /* Create the file with the backend its %FILE% names (a fresh clone);
           a no-op when it already exists (a branch switch).  The dictionary
           tree names the same base file, so an empty file that has only a
           dictionary in git still gets created. */
        mv_value spec;
        mv_init(&spec);
        mv_set_str(&spec, base, (int64_t)strlen(base));
        if (type[0]) {
            mv_value tv;
            mv_init(&tv);
            mv_set_str(&tv, type, (int64_t)strlen(type));
            mv_createfile(ctx, &spec, &tv);
            mv_clear(&tv);
        } else {
            mv_createfile(ctx, &spec, NULL);
        }
        mv_clear(&spec);
    }
    if (!open_named(ctx, fn, &fvar)) {
        mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
        return;
    }
    char (*seen)[256] = NULL;
    size_t ns = 0, cap = 0;
    size_t cnt = git_tree_entrycount(subtree);
    for (size_t i = 0; i < cnt; i++) {
        const git_tree_entry *te = git_tree_entry_byindex(subtree, i);
        const char *name = git_tree_entry_name(te);
        /* The %FILE% control was written natively by createfile; keep it (and
           mark it seen so the reconcile pass below does not delete it) rather
           than overwrite with the open DIR/hash form. */
        if (is_dict && strcmp(name, "%FILE%") == 0) {
            if (ns == cap) { cap = cap ? cap * 2 : 64;
                seen = realloc(seen, cap * sizeof *seen);
                if (!seen) mv_fatal("out of memory in checkout"); }
            snprintf(seen[ns++], 256, "%s", name);
            continue;
        }
        git_blob *blob = NULL;
        if (git_blob_lookup(&blob, repo, git_tree_entry_id(te)) != 0)
            continue;
        const char *cp = git_blob_rawcontent(blob);
        int64_t clen = (int64_t)git_blob_rawsize(blob), rl;
        char *r = xlate(cp, clen, '\n', (char)0xFE, &rl);
        if (is_dir && rl > 0 && (unsigned char)r[rl - 1] == 0xFE) rl--;
        mv_set_str(&rec, r, rl);
        free(r);
#ifdef MVXGIT_UDT
        /* open-form dictionary D/I item -> native UniData order (SM/assoc). */
        if (is_dict && mv_openaccount()) {
            char rnb[40];
            const char *rp;
            int64_t rlen = mv_val_chars(&rec, rnb, sizeof rnb, &rp);
            int64_t dl;
            char *dn = dict_item_swap(rp, rlen, &dl);
            if (dn) { mv_set_str(&rec, dn, dl); free(dn); }
        }
#endif
        mv_set_str(&id, name, (int64_t)strlen(name));
        mv_write(ctx, &rec, &fvar, &id, 0, 0);
        (*nw)++;
        if (ns == cap) { cap = cap ? cap * 2 : 64;
            seen = realloc(seen, cap * sizeof *seen);
            if (!seen) mv_fatal("out of memory in checkout"); }
        snprintf(seen[ns++], 256, "%s", name);
        git_blob_free(blob);
    }
    /* Reconcile: drop records the commit no longer has — but only on a branch
       switch.  On a fresh clone (keep_extra) the destination is a just-created
       account whose files carry system records the open-account commit
       deliberately omits (VOC verbs/keywords, catalog pointers such as CTLGTB);
       deleting those would break the account.  Clone only adds.

       THE SAME REASONING NOW APPLIES TO A BRANCH SWITCH.  Since mv_git#46 a
       native commit deliberately omits the flavour's stock VOC — 847 records for
       PICK — so "absent from the commit" stopped meaning "deleted by the user".
       Reconciling without that knowledge gutted the account: a pull reported
       "847 removed" and left a VOC with no verbs in it.  A record the baseline
       calls stock is not the commit's to remove. */
    if (!keep_extra) {
        mv_select(ctx, &fvar);
        mv_value dl;
        mv_init(&dl);
        while (mv_readnext(ctx, &dl)) {
            char idb[256];
            arg_str(&dl, idb, sizeof idb);
            int found = 0;
            for (size_t i = 0; i < ns; i++)
                if (strcmp(seen[i], idb) == 0) { found = 1; break; }
            if (!found && !is_stock_id(idb)) {
                mv_delete_rec(ctx, &fvar, &dl);
                (*nd)++;
            }
        }
        mv_clear(&dl);
    }
    free(seen);
    mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
}

/* Whether a top-level tree name is an MV file (its records go to the backend)
   rather than a plain directory git tracks verbatim: a data file `<name>` has a
   `<name>.DICT/%FILE%` control; a dictionary `<name>.DICT` has its own
   `%FILE%`. */
/* What the COMMIT'S OWN VOC calls a file.
 *
 * The %FILE% test below only works on a commit in the OPEN interchange form,
 * which is the only form that carries those controls.  A NATIVE commit — a
 * UniVerse or UniData account committed as itself — has none, so nothing was
 * recognised as an MV file and a clone materialised exactly nothing while
 * cheerfully reporting success.
 *
 * A native commit does carry its VOC, though, and a VOC record of type F or DIR
 * is the account's own statement that a file exists.  That is the same authority
 * the live side consults (mv_filelist reads the VOC); here it is read out of the
 * tree, because at clone time there is no account yet to ask. */
static nameset g_treefiles;
static int g_treefiles_done;

static void tree_files_reset(void) {
    free(g_treefiles.n);
    g_treefiles.n = NULL;
    g_treefiles.c = g_treefiles.cap = 0;
    g_treefiles_done = 0;
}

static void tree_files_load(git_repository *repo, git_tree *head) {
    if (g_treefiles_done) return;
    g_treefiles_done = 1;
    git_tree_entry *ve = NULL;
    if (git_tree_entry_bypath(&ve, head, "VOC") != 0) return;
    git_tree *voc = NULL;
    if (git_tree_lookup(&voc, repo, git_tree_entry_id(ve)) == 0) {
        size_t n = git_tree_entrycount(voc);
        for (size_t i = 0; i < n; i++) {
            const git_tree_entry *te = git_tree_entry_byindex(voc, i);
            if (git_tree_entry_type(te) != GIT_OBJECT_BLOB) continue;
            git_blob *b = NULL;
            if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) != 0) continue;
            const char *c = git_blob_rawcontent(b);
            size_t bl = (size_t)git_blob_rawsize(b);
            /* A committed record is stored TRANSLATED — attribute marks are
               newlines — so attribute 1 is the first line, and its first token
               is the type (UniVerse may append a description). */
            size_t e = 0;
            while (e < bl && c[e] != '\n') e++;
            size_t t = 0;
            while (t < e && c[t] != ' ' && c[t] != '\t') t++;
            if ((t == 1 && c[0] == 'F') || (t == 3 && strncmp(c, "DIR", 3) == 0))
                ns_add(&g_treefiles, git_tree_entry_name(te));
            git_blob_free(b);
        }
        git_tree_free(voc);
    }
    git_tree_entry_free(ve);
}

static int tree_voc_says_file(const char *name) {
    /* A dictionary subtree belongs to its base file. */
    char base[300];
    size_t ln = strlen(name);
    if (ln > 5 && strcmp(name + ln - 5, ".DICT") == 0)
        snprintf(base, sizeof base, "%.*s", (int)(ln - 5), name);
    else
        snprintf(base, sizeof base, "%s", name);
    for (size_t i = 0; i < g_treefiles.c; i++)
        if (!strcmp(g_treefiles.n[i], base)) return 1;
    return 0;
}

static int tree_is_mv_file(git_tree *head, const char *name) {
    char path[600];
    size_t ln = strlen(name);
    if (ln > 5 && strcmp(name + ln - 5, ".DICT") == 0)
        snprintf(path, sizeof path, "%s/%%FILE%%", name);
    else
        snprintf(path, sizeof path, "%s.DICT/%%FILE%%", name);
    git_tree_entry *te = NULL;
    if (git_tree_entry_bypath(&te, head, path) == 0) {
        git_tree_entry_free(te);
        return 1;                       /* open form: the control says so */
    }
    return tree_voc_says_file(name);    /* native form: the VOC says so */
}

/* Materialize the tracked MV files in a commit tree into their backends.  With
   `strict` (a fresh clone, where the files cannot be opened to tell an MV file
   from a plain directory) only trees that carry a dictionary control are
   materialised, so a plain directory (server/, test/, a submodule) is left to
   git.  Without it (a branch switch on an existing account) every top-level tree
   is materialised, as before — a file added without its dictionary still
   round-trips. */
/* HEAD's tree AS THIS ACCOUNT SEES IT.
 *
 * A commit's top level is the repository's, not the account's: with several
 * accounts it holds `acctA`, `acctB` and whatever ordinary files sit beside
 * them.  The walk below treats every subtree as an MV FILE, so run against the
 * repository root it would take `acctA` for a file and `acctA/CUST` for one of
 * its record ids — restoring nonsense, or more likely nothing.
 *
 * Descending to the account's own subtree first puts us back in the world the
 * rest of this code assumes, where a subtree IS a file and its entries ARE
 * records.  It is the exact mirror of the prefix that staging applies, and it
 * is what makes a multi-account repository restorable rather than merely
 * committable (mv_git#44).
 *
 * Returns NULL when the account has nothing committed yet, which is not an
 * error — a new account in an existing repository is exactly that. */
static git_tree *account_subtree(git_repository *repo, git_tree *root) {
    if (!g_prefix[0]) return NULL;              /* the root IS the account */
    char p[256];
    size_t n = strlen(g_prefix);
    snprintf(p, sizeof p, "%.*s", (int)(n - 1), g_prefix);   /* drop the '/' */
    git_tree_entry *te = NULL;
    git_tree *sub = NULL;
    if (git_tree_entry_bypath(&te, root, p) == 0) {
        if (git_tree_entry_type(te) == GIT_OBJECT_TREE)
            git_tree_lookup(&sub, repo, git_tree_entry_id(te));
        git_tree_entry_free(te);
    }
    return sub;
}

static void materialize_tree_x(mv_ctx *ctx, git_repository *repo,
                               git_tree *tree, int strict,
                               int64_t *nw, int64_t *nd) {
    /* Every materialisation needs the stock baseline, not just `checkout`:
       a clone and a pull remove records the tree does not carry, and the stock
       ones it deliberately never carried must survive that.  Idempotent and
       once per process, so calling it on the common path is free. */
    stock_ensure_udt(ctx, git_repository_workdir(repo));
    /* Below a repository root, this account's files live in its own subtree. */
    git_tree *owned = NULL;
    if (g_prefix[0]) {
        owned = account_subtree(repo, tree);
        if (!owned) return;                 /* nothing committed for us yet */
        tree = owned;
    }
    /* Learn what this commit calls a file before deciding which subtrees are
       ones.  Reset first: a different tree may say something different. */
    tree_files_reset();
    tree_files_load(repo, tree);
    size_t n = git_tree_entrycount(tree);
    for (size_t i = 0; i < n; i++) {
        const git_tree_entry *te = git_tree_entry_byindex(tree, i);
        if (git_tree_entry_type(te) != GIT_OBJECT_TREE) continue;
        const char *name = git_tree_entry_name(te);
        if (strict && !tree_is_mv_file(tree, name)) continue;
        git_tree *sub = NULL;
        if (git_tree_lookup(&sub, repo, git_tree_entry_id(te)) != 0)
            continue;
        /* strict == a fresh clone: keep the account's own system records
           (see materialize_file) rather than reconcile-deleting them. */
        materialize_file(ctx, repo, tree, sub, name, strict, nw, nd);
        git_tree_free(sub);
    }
    /* FILES THIS COMMIT NO LONGER HAS.  The loop above walks what the TREE
       holds, so a file dropped between commits is simply never visited and
       survives the checkout — the account keeps a file the branch does not have.
       %FILE% is the file's existence in git, so a commit without one is a commit
       without the file: delete it, dictionary and all.
       Not on a fresh clone (strict): there is nothing to reconcile against, and
       the destination's own files are its own.  Only files GIT TRACKED are
       touched — a file the account has that git never knew about is not ours to
       remove — and never the VOC, which is the account itself. */
    if (!strict) {
        git_index *idx = NULL;
        if (git_repository_index(&idx, repo) == 0) {
            mv_value fl;
            mv_init(&fl);
            mv_filelist(ctx, &fl);
            char nb[40];
            const char *p;
            int64_t len = mv_val_chars(&fl, nb, sizeof nb, &p), i = 0;
            while (i < len) {
                int64_t st = i;
                while (i < len && (unsigned char)p[i] != 0xFE &&
                       (unsigned char)p[i] != 0xFD) i++;
                int64_t nl = i - st;
                if (nl > 0 && nl < 256) {
                    char nm[256];
                    memcpy(nm, p + st, (size_t)nl);
                    nm[nl] = '\0';
                    size_t l = strlen(nm);
                    int is_dict = l > 5 && strcmp(nm + l - 5, ".DICT") == 0;
                    if (!is_dict && strcasecmp(nm, "VOC") != 0 &&
                        strcasecmp(nm, "MD") != 0 && !tree_is_mv_file(tree, nm)) {
                        char ctl[600], path[700];
                        snprintf(ctl, sizeof ctl, "%s.DICT", nm);
                        record_path(path, sizeof path, ctl, "%FILE%");
                        if (git_index_get_bypath(idx, path, 0)) {
                            mv_value spec;
                            mv_init(&spec);
                            mv_set_str(&spec, nm, (int64_t)strlen(nm));
                            if (mv_deletefile(ctx, &spec) && nd) (*nd)++;
                            mv_clear(&spec);
                        }
                    }
                }
                while (i < len && (unsigned char)p[i] != 0xFE) i++;
                if (i < len) i++;
            }
            mv_clear(&fl);
            git_index_free(idx);
        }
    }
    if (owned) git_tree_free(owned);
}

static void materialize_tree(mv_ctx *ctx, git_repository *repo,
                             git_tree *tree, int64_t *nw, int64_t *nd) {
    materialize_tree_x(ctx, repo, tree, 0, nw, nd);   /* branch ops: all trees */
}

/* Reset the persistent index to a commit tree (so status is clean). */
static void sync_index(git_repository *repo, const char *rp,
                       git_tree *tree) {
    char ipath[4200];
    const char *gd = git_repository_path(repo);   /* submodule-safe git dir */
    snprintf(ipath, sizeof ipath, "%sindex", gd ? gd : "");
    (void)rp;
    git_index *idx = NULL;
    if (git_index_open(&idx, ipath) == 0) {
        git_index_read_tree(idx, tree);
        git_index_write(idx);
        git_index_free(idx);
    }
}

/* Write a tree's blobs to disk under `prefix` (recursively): the plain files a
   checkout leaves on disk — README, scripts, a plain subdirectory, a submodule
   is skipped.  MV files never come here (they go to the backend). */
static void checkout_plain_tree(git_repository *repo, git_tree *tree,
                                const char *prefix) {
    size_t n = git_tree_entrycount(tree);
    for (size_t i = 0; i < n; i++) {
        const git_tree_entry *te = git_tree_entry_byindex(tree, i);
        const char *name = git_tree_entry_name(te);
        char path[4096];
        snprintf(path, sizeof path, "%s%s%s", prefix, prefix[0] ? "/" : "", name);
        git_object_t ty = git_tree_entry_type(te);
        if (ty == GIT_OBJECT_TREE) {
            git_tree *sub = NULL;
            if (git_tree_lookup(&sub, repo, git_tree_entry_id(te)) == 0) {
                mkdir(path, 0755);
                checkout_plain_tree(repo, sub, path);
                git_tree_free(sub);
            }
        } else if (ty == GIT_OBJECT_BLOB) {
            git_blob *b = NULL;
            if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) == 0) {
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(git_blob_rawcontent(b), 1,
                           (size_t)git_blob_rawsize(b), f);
                    fclose(f);
                    if (git_tree_entry_filemode(te) ==
                        GIT_FILEMODE_BLOB_EXECUTABLE)
                        chmod(path, 0755);
                }
                git_blob_free(b);
            }
        }
    }
}

/* Materialise a native account directly from the repo's HEAD tree — the clone
   path.  MV files (records + dictionaries) go straight to the backend in the
   native form; the account descriptor `.mv-account`/`.mvx` becomes `.mvx`; plain
   files land on disk.  The open form never touches disk, and no external adopt
   is run.  GITMATERIALIZE(repo, out) */
void mvx_sub_GITMATERIALIZE(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 2) return;
    ensure_init();
    char rp[4096];
    arg_str(argv[0], rp, sizeof rp);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[1], "open"); return; }
    git_tree *tree = head_tree(repo);
    if (!tree) { git_repository_free(repo); mv_set_str(argv[1], "empty", 5); return; }

    int64_t nw = 0, nd = 0;
    materialize_tree_x(ctx, repo, tree, 1, &nw, &nd);   /* strict: MV files only */

    /* Everything that is not an MV file: the descriptor, and plain files. */
    size_t n = git_tree_entrycount(tree);
    for (size_t i = 0; i < n; i++) {
        const git_tree_entry *te = git_tree_entry_byindex(tree, i);
        const char *name = git_tree_entry_name(te);
        git_object_t ty = git_tree_entry_type(te);
        if (ty == GIT_OBJECT_TREE && tree_is_mv_file(tree, name)) continue;
        if (ty == GIT_OBJECT_BLOB && strcmp(name, ".mv-account") == 0) {
            /* portable descriptor -> minimal native `.mvx` (convert; local
               security policy is re-seeded locally, never shipped in git) */
            git_blob *b = NULL;
            if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) == 0) {
                acct_desc d;
                desc_parse(git_blob_rawcontent(b),
                           (size_t)git_blob_rawsize(b), &d);
                char nat[1024];
                int nl = desc_render_native(&d, nat, sizeof nat);
                FILE *f = fopen(".mvx", "wb");
                if (f) { if (nl > 0) fwrite(nat, 1, (size_t)nl, f); fclose(f); }
                git_blob_free(b);
            }
            continue;
        }
        if (ty == GIT_OBJECT_BLOB && strcmp(name, ".mvx") == 0) {
            /* native descriptor committed verbatim (a native, non-open repo):
               restore as-is so its local `permit`/`deny` policy round-trips */
            git_blob *b = NULL;
            if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) == 0) {
                FILE *f = fopen(".mvx", "wb");
                if (f) { fwrite(git_blob_rawcontent(b), 1,
                                (size_t)git_blob_rawsize(b), f); fclose(f); }
                git_blob_free(b);
            }
            continue;
        }
        if (ty == GIT_OBJECT_TREE) {
            git_tree *sub = NULL;
            if (git_tree_lookup(&sub, repo, git_tree_entry_id(te)) == 0) {
                mkdir(name, 0755);
                checkout_plain_tree(repo, sub, name);
                git_tree_free(sub);
            }
        } else if (ty == GIT_OBJECT_BLOB) {
            git_blob *b = NULL;
            if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) == 0) {
                FILE *f = fopen(name, "wb");
                if (f) {
                    fwrite(git_blob_rawcontent(b), 1,
                           (size_t)git_blob_rawsize(b), f);
                    fclose(f);
                    if (git_tree_entry_filemode(te) ==
                        GIT_FILEMODE_BLOB_EXECUTABLE)
                        chmod(name, 0755);
                }
                git_blob_free(b);
            }
        }
    }

    sync_index(repo, rp, tree);
    git_tree_free(tree);
    git_repository_free(repo);
    char out[80];
    snprintf(out, sizeof out, "materialised %lld record(s)", (long long)nw);
    mv_set_str(argv[1], out, (int64_t)strlen(out));
}

/* GITBRANCH(repo, name, out) — list branches, or create one at HEAD. */
void mvx_sub_GITBRANCH(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], name[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], name, sizeof name);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }

    if (!name[0]) {                     /* list */
        sbuf s = {0, 0, 0};
        git_branch_iterator *it = NULL;
        git_reference *cur = NULL;
        const char *curname = NULL;
        if (git_repository_head(&cur, repo) == 0)
            git_branch_name(&curname, cur);
        if (git_branch_iterator_new(&it, repo, GIT_BRANCH_LOCAL) == 0) {
            git_reference *ref = NULL;
            git_branch_t bt;
            while (git_branch_next(&ref, &bt, it) == 0) {
                const char *bn = NULL;
                git_branch_name(&bn, ref);
                char line[300];
                snprintf(line, sizeof line, "%s %s",
                         (curname && bn && strcmp(curname, bn) == 0)
                             ? "*" : " ",
                         bn ? bn : "?");
                sb_line(&s, line);
                git_reference_free(ref);
            }
            git_branch_iterator_free(it);
        }
        if (cur) git_reference_free(cur);
        git_repository_free(repo);
        sb_out(&s, argv[2], "no branches");
        return;
    }

    git_object *head = NULL;
    git_commit *c = NULL;
    git_reference *br = NULL;
    int rc = git_revparse_single(&head, repo, "HEAD");
    if (rc == 0) rc = git_commit_lookup(&c, repo, git_object_id(head));
    if (rc == 0) rc = git_branch_create(&br, repo, name, c, 0);
    if (br) git_reference_free(br);
    if (c) git_commit_free(c);
    if (head) git_object_free(head);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[2], "branch"); return; }
    char out[300];
    snprintf(out, sizeof out, "created branch %s", name);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* GITCHECKOUT(repo, name, out) — switch to a branch and write its
   records into the hash files (our working tree). */
void mvx_sub_GITCHECKOUT(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 3) return;
    ensure_init();
    char rp[4096], name[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], name, sizeof name);
    /* The baseline is needed to CHECK OUT as much as to add: records the commit
       does not carry because they are stock must not then be deleted as
       "extra".  Without it a clone strips the destination's own VOC — the
       symptom is a stock verb like CT vanishing from a freshly cloned
       account. */
    stock_ensure_udt(ctx, rp);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }
    git_reference *br = NULL;
    if (git_branch_lookup(&br, repo, name, GIT_BRANCH_LOCAL) != 0) {
        git_repository_free(repo);
        fail(argv[2], "no such branch");
        return;
    }
    git_repository_set_head(repo, git_reference_name(br));
    git_reference_free(br);
    git_tree *t = head_tree(repo);
    int64_t nw = 0, nd = 0;
    if (t) {
        materialize_tree(ctx, repo, t, &nw, &nd);
        sync_index(repo, rp, t);
        git_tree_free(t);
    }
    git_repository_free(repo);
    char out[300];
    snprintf(out, sizeof out,
             "switched to %s (%lld record(s) updated, %lld removed)",
             name, (long long)nw, (long long)nd);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* GITSWITCH(repo, name, out) — move HEAD to local branch `name` and reset the
   index to its tree, but do NOT materialize records.  The UniData CHECKOUT uses
   this and then re-materializes the new HEAD natively (GITUDT.CHECKOUT), because
   mv_write cannot reach a UniData hash file — only the native WRITE loop can.  On
   MVX the ordinary GITCHECKOUT (switch + materialize in one) is used instead. */
void mvx_sub_GITSWITCH(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], name[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], name, sizeof name);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }
    git_reference *br = NULL;
    if (git_branch_lookup(&br, repo, name, GIT_BRANCH_LOCAL) != 0) {
        git_repository_free(repo);
        fail(argv[2], "no such branch");
        return;
    }
    git_repository_set_head(repo, git_reference_name(br));
    git_reference_free(br);
    git_tree *t = head_tree(repo);
    if (t) { sync_index(repo, rp, t); git_tree_free(t); }
    git_repository_free(repo);
    char out[300];
    snprintf(out, sizeof out, "switched to %s", name);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* Commit a merged/cherry-picked index and materialize it. */
static void finish_merge(mv_ctx *ctx, git_repository *repo,
                         const char *rp, git_index *mindex,
                         git_commit *ours, git_commit *theirs,
                         const char *msg, mv_value *out) {
    if (git_index_has_conflicts(mindex)) {
        sbuf s = {0, 0, 0};
        sb_line(&s, "CONFLICT — resolve these records, then GIT ADD "
                    "and GIT COMMIT:");
        git_index_conflict_iterator *ci = NULL;
        if (git_index_conflict_iterator_new(&ci, mindex) == 0) {
            const git_index_entry *a, *o, *th;
            while (git_index_conflict_next(&a, &o, &th, ci) == 0) {
                const char *p = th ? th->path : (o ? o->path :
                                (a ? a->path : "?"));
                char line[700];
                snprintf(line, sizeof line, "  %s", p);
                sb_line(&s, line);
            }
            git_index_conflict_iterator_free(ci);
        }
        sb_out(&s, out, "conflict");
        return;
    }
    git_oid tree_oid, commit_oid;
    git_tree *tree = NULL;
    git_signature *sig = NULL;
    int rc = git_index_write_tree_to(&tree_oid, mindex, repo);
    if (rc == 0) rc = git_tree_lookup(&tree, repo, &tree_oid);
    if (rc == 0 && git_signature_default(&sig, repo) != 0)
        rc = git_signature_now(&sig, "MVX", "mvx@localhost");
    if (rc == 0) {
        const git_commit *parents[2];
        int np = 1;
        parents[0] = ours;
        if (theirs) { parents[1] = theirs; np = 2; }
        rc = git_commit_create(&commit_oid, repo, "HEAD", sig, sig,
                               NULL, msg, tree, np, parents);
    }
    int64_t nw = 0, nd = 0;
    if (rc == 0 && tree) {
        materialize_tree(ctx, repo, tree, &nw, &nd);
        sync_index(repo, rp, tree);
    }
    if (sig) git_signature_free(sig);
    if (tree) git_tree_free(tree);
    if (rc != 0) { fail(out, "merge commit"); return; }
    char o[300];
    snprintf(o, sizeof o, "%s (%lld record(s) updated, %lld removed)",
             msg, (long long)nw, (long long)nd);
    mv_set_str(out, o, (int64_t)strlen(o));
}

/* GITMERGE(repo, branch, out) — 3-way merge a branch into HEAD. */
/* Fast-forward HEAD to `target` and materialise its tree — no merge commit.
   Returns 0 ok. */
static int merge_fast_forward(mv_ctx *ctx, git_repository *repo, const char *rp,
                              git_commit *target, int64_t *nw, int64_t *nd,
                              int materialise) {
    git_reference *head = NULL, *moved = NULL;
    git_tree *tree = NULL;
    int rc = git_repository_head(&head, repo);
    if (rc == 0)
        rc = git_reference_set_target(&moved, head, git_commit_id(target),
                                      "mvx-git: fast-forward");
    if (rc == 0) rc = git_commit_tree(&tree, target);
    if (rc == 0) {
        if (materialise) materialize_tree(ctx, repo, tree, nw, nd);
        sync_index(repo, rp, tree);      /* the git index, not records — safe */
    }
    if (tree) git_tree_free(tree);
    if (moved) git_reference_free(moved);
    if (head) git_reference_free(head);
    return rc;
}

/* Merge commit-ish NAME into HEAD of an already-open REPO (caller frees repo).
   NAME is anything git_revparse resolves — a local branch, a remote-tracking ref
   (origin/main), FETCH_HEAD (so pull reuses this), or a SHA.  Fast-forwards when
   HEAD is an ancestor, reports up-to-date, else makes a real merge commit; record
   conflicts surfaced by finish_merge.  One path for both `merge` and `pull`. */
/* `materialise` says whether THIS process writes the records back.
 *
 * It cannot always be the one to do it.  mvgitd is built MVXGIT_NORECORDS — the
 * BASIC session owns the records (Model B) — so a record primitive there calls
 * mv_fatal and the daemon DIES mid-request, leaving the caller waiting for a
 * reply that can never come (mv_git#53: over an hour asleep in pipe_read).
 * With the flag off, the git-object work still happens here and the caller
 * re-materialises natively afterwards, which is exactly what GIT MERGE and GIT
 * CHERRY-PICK have always done via GITUDT.CHECKOUT. */
static void merge_into_head(mv_ctx *ctx, git_repository *repo, const char *rp,
                            const char *name, mv_value *out, int materialise) {
    git_object *ho = NULL, *to = NULL;
    git_commit *ours = NULL, *theirs = NULL;
    git_annotated_commit *ann = NULL;
    git_index *mindex = NULL;
    int64_t nw = 0, nd = 0;
    int rc = git_revparse_single(&ho, repo, "HEAD");
    if (rc == 0) rc = git_commit_lookup(&ours, repo, git_object_id(ho));
    if (rc == 0) rc = git_revparse_single(&to, repo, name);
    if (rc == 0) rc = git_commit_lookup(&theirs, repo, git_object_id(to));
    if (rc != 0) { fail(out, "no such revision"); goto done; }

    git_merge_analysis_t an = GIT_MERGE_ANALYSIS_NONE;
    git_merge_preference_t pref = GIT_MERGE_PREFERENCE_NONE;
    if (git_annotated_commit_lookup(&ann, repo, git_object_id(to)) == 0) {
        const git_annotated_commit *heads[1] = {ann};
        git_merge_analysis(&an, &pref, repo, heads, 1);
    }

    if (an & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
        mv_set_str(out, "already up to date", 18);
    } else if (an & GIT_MERGE_ANALYSIS_FASTFORWARD) {
        if (merge_fast_forward(ctx, repo, rp, theirs, &nw, &nd, materialise) != 0) {
            fail(out, "fast-forward");
        } else {
            char msg[300];
            snprintf(msg, sizeof msg,
                     "fast-forward (%lld record(s) updated, %lld removed)",
                     (long long)nw, (long long)nd);
            mv_set_str(out, msg, (int64_t)strlen(msg));
        }
    } else if (git_merge_commits(&mindex, repo, ours, theirs, NULL) == 0) {
        char msg[300];
        snprintf(msg, sizeof msg, "Merge '%s'", name);
        finish_merge(ctx, repo, rp, mindex, ours, theirs, msg, out);
    } else {
        fail(out, "merge");
    }
done:
    if (mindex) git_index_free(mindex);
    if (ann) git_annotated_commit_free(ann);
    if (theirs) git_commit_free(theirs);
    if (ours) git_commit_free(ours);
    if (to) git_object_free(to);
    if (ho) git_object_free(ho);
}

/* GITMERGE(repo, revspec, out) — merge revspec into HEAD. */
void mvx_sub_GITMERGE(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 3) return;
    ensure_init();
    char rp[4096], name[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], name, sizeof name);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }
    merge_into_head(ctx, repo, rp, name, argv[2], 1);
    git_repository_free(repo);
}

/* GITFETCH(repo, remote, out) — fetch REMOTE (default origin), updating the
   remote-tracking refs; no working-tree change (records untouched). */
void mvx_sub_GITFETCH(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 3) return;
    ensure_init();
    char rp[4096], rn[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], rn, sizeof rn);
    if (!rn[0]) snprintf(rn, sizeof rn, "origin");
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }
    git_remote *rem = NULL;
    int rc = git_remote_lookup(&rem, repo, rn);
    if (rc == 0) {
        git_fetch_options fo = GIT_FETCH_OPTIONS_INIT;
        rc = git_remote_fetch(rem, NULL, &fo, NULL);
    }
    if (rem) git_remote_free(rem);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[2], "fetch"); return; }
    char out[300];
    snprintf(out, sizeof out, "fetched %s", rn);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* GITPUSH(repo, remote, refspec, out) — push REFSPEC (default: the current
   branch) to REMOTE (default origin).  Needs a writable remote; public transport
   only for now (add a credential callback for authenticated pushes). */
void mvx_sub_GITPUSH(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 4) return;
    ensure_init();
    char rp[4096], rn[256], spec[512];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], rn, sizeof rn);
    arg_str(argv[2], spec, sizeof spec);
    if (!rn[0]) snprintf(rn, sizeof rn, "origin");
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[3], "open"); return; }
    char specbuf[700];
    specbuf[0] = '\0';
    if (!spec[0]) {                                    /* current branch */
        git_reference *head = NULL;
        if (git_repository_head(&head, repo) == 0) {
            const char *hn = git_reference_name(head);
            snprintf(specbuf, sizeof specbuf, "%s:%s", hn, hn);
            git_reference_free(head);
        }
    } else if (strchr(spec, ':') || strncmp(spec, "refs/", 5) == 0) {
        snprintf(specbuf, sizeof specbuf, "%s", spec);
    } else {                                           /* bare branch name */
        snprintf(specbuf, sizeof specbuf, "refs/heads/%s:refs/heads/%s", spec, spec);
    }
    git_remote *rem = NULL;
    int rc = specbuf[0] ? git_remote_lookup(&rem, repo, rn) : -1;
    if (rc == 0) {
        char *specs[1] = {specbuf};
        git_strarray arr = {specs, 1};
        git_push_options po = GIT_PUSH_OPTIONS_INIT;
        rc = git_remote_push(rem, &arr, &po);
    }
    if (rem) git_remote_free(rem);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[3], "push"); return; }
    char out[800];
    snprintf(out, sizeof out, "pushed %s to %s", specbuf, rn);
    mv_set_str(argv[3], out, (int64_t)strlen(out));
}

/* GITPULL(repo, remote, branch, out) — fetch then merge into HEAD, re-materialising
   records (a plain `git pull` refuses: our working tree is the native form).
   Merges refs/remotes/<remote>/<branch>, or FETCH_HEAD when no branch is named. */
/* GITPULLREF is GITPULL without the record write — see merge_into_head. */
static int g_pull_materialise = 1;
void mvx_sub_GITPULL(mv_ctx *ctx, int32_t argc, mv_value **argv);

void mvx_sub_GITPULLREF(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    g_pull_materialise = 0;
    mvx_sub_GITPULL(ctx, argc, argv);
    g_pull_materialise = 1;
}

void mvx_sub_GITPULL(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 4) return;
    ensure_init();
    char rp[4096], rn[256], br[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], rn, sizeof rn);
    arg_str(argv[2], br, sizeof br);
    if (!rn[0]) snprintf(rn, sizeof rn, "origin");
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[3], "open"); return; }
    git_remote *rem = NULL;
    int rc = git_remote_lookup(&rem, repo, rn);
    if (rc == 0) {
        git_fetch_options fo = GIT_FETCH_OPTIONS_INIT;
        rc = git_remote_fetch(rem, NULL, &fo, NULL);
    }
    if (rem) git_remote_free(rem);
    if (rc != 0) { git_repository_free(repo); fail(argv[3], "fetch"); return; }
    char mref[320];
    if (br[0]) snprintf(mref, sizeof mref, "refs/remotes/%s/%s", rn, br);
    else       snprintf(mref, sizeof mref, "FETCH_HEAD");
    merge_into_head(ctx, repo, rp, mref, argv[3], g_pull_materialise);
    git_repository_free(repo);
}

/* GITREMOTE(repo, action, name, url, out) — manage remotes without the OS git:
   no action (or "list") prints "<name>\t<url>" lines; "add"/"set-url"/"remove"
   configure one.  Needed to point a GIT INIT'd repo at a remote before push/pull
   (a clone sets origin already) — and the only way to do it on a backend with no
   OS git to pass `git remote` through to (D3). */
void mvx_sub_GITREMOTE(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 5) return;
    ensure_init();
    char rp[4096], act[64], name[256], url[4096];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], act, sizeof act);
    arg_str(argv[2], name, sizeof name);
    arg_str(argv[3], url, sizeof url);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[4], "open"); return; }

    if (!act[0] || strcasecmp(act, "list") == 0 || strcmp(act, "-v") == 0) {
        sbuf s = {0, 0, 0};
        git_strarray rs = {0, 0};
        if (git_remote_list(&rs, repo) == 0) {
            for (size_t i = 0; i < rs.count; i++) {
                git_remote *rem = NULL;
                const char *u = "";
                if (git_remote_lookup(&rem, repo, rs.strings[i]) == 0)
                    u = git_remote_url(rem) ? git_remote_url(rem) : "";
                char line[4400];
                snprintf(line, sizeof line, "%s\t%s", rs.strings[i], u);
                sb_line(&s, line);
                if (rem) git_remote_free(rem);
            }
            git_strarray_dispose(&rs);
        }
        git_repository_free(repo);
        sb_out(&s, argv[4], "no remotes");
        return;
    }

    int rc;
    if (strcasecmp(act, "add") == 0) {
        git_remote *rem = NULL;
        rc = git_remote_create(&rem, repo, name, url);
        if (rem) git_remote_free(rem);
    } else if (strcasecmp(act, "set-url") == 0) {
        rc = git_remote_set_url(repo, name, url);
    } else if (strcasecmp(act, "remove") == 0 || strcasecmp(act, "rm") == 0) {
        rc = git_remote_delete(repo, name);
    } else {
        git_repository_free(repo);
        fail(argv[4], "usage: remote [add|set-url|remove] <name> [url]");
        return;
    }
    git_repository_free(repo);
    if (rc != 0) { fail(argv[4], "remote"); return; }
    char out[400];
    snprintf(out, sizeof out, "remote %s %s", act, name);
    mv_set_str(argv[4], out, (int64_t)strlen(out));
}

/* GITCONFIG(repo, key, value, out) — get (empty value) or set repo config.  The
   only way to set commit identity (user.name/user.email) or mvx.openaccount on a
   backend with no OS git to run `git config` (D3).  Get of an unset key is "",
   not an error. */
void mvx_sub_GITCONFIG(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 4) return;
    ensure_init();
    char rp[4096], key[256], val[4096];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], key, sizeof key);
    arg_str(argv[2], val, sizeof val);
    if (!key[0]) { fail(argv[3], "usage: config <key> [value]"); return; }
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[3], "open"); return; }
    git_config *cfg = NULL;
    if (git_repository_config(&cfg, repo) != 0) {
        git_repository_free(repo); fail(argv[3], "config"); return;
    }
    if (val[0]) {                                   /* set */
        if (git_config_set_string(cfg, key, val) == 0) {
            char out[4400];
            snprintf(out, sizeof out, "%s = %s", key, val);
            mv_set_str(argv[3], out, (int64_t)strlen(out));
        } else {
            fail(argv[3], "set");
        }
    } else {                                        /* get (unset -> "") */
        git_buf b = GIT_BUF_INIT;
        if (git_config_get_string_buf(&b, cfg, key) == 0 && b.ptr)
            mv_set_str(argv[3], b.ptr, (int64_t)strlen(b.ptr));
        else
            mv_set_str(argv[3], "", 0);
        git_buf_dispose(&b);
    }
    git_config_free(cfg);
    git_repository_free(repo);
}

/* GITTAG(repo, op, name, target, message, out) — no-shell tags for releases (a
   version resolves via its tag).  op "" / "list" prints the tag names; "add"
   creates tag NAME at TARGET (HEAD if empty) — annotated when MESSAGE is given,
   else lightweight; "delete" removes NAME. */
void mvx_sub_GITTAG(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 6) return;
    ensure_init();
    char rp[4096], op[32], name[256], target[256], msg[4096];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], op, sizeof op);
    arg_str(argv[2], name, sizeof name);
    arg_str(argv[3], target, sizeof target);
    arg_str(argv[4], msg, sizeof msg);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[5], "open"); return; }

    if (!op[0] || strcasecmp(op, "list") == 0) {
        sbuf s = {0, 0, 0};
        git_strarray ts = {0, 0};
        if (git_tag_list(&ts, repo) == 0) {
            for (size_t i = 0; i < ts.count; i++) sb_line(&s, ts.strings[i]);
            git_strarray_dispose(&ts);
        }
        git_repository_free(repo);
        sb_out(&s, argv[5], "no tags");
        return;
    }

    if (strcasecmp(op, "delete") == 0) {
        int rc = git_tag_delete(repo, name);
        git_repository_free(repo);
        if (rc != 0) { fail(argv[5], "no such tag"); return; }
        char out[300];
        snprintf(out, sizeof out, "deleted tag %s", name);
        mv_set_str(argv[5], out, (int64_t)strlen(out));
        return;
    }

    /* add: resolve the target commit-ish (HEAD by default) */
    const char *tgt = target[0] ? target : "HEAD";
    git_object *obj = NULL;
    git_oid oid;
    int rc = git_revparse_single(&obj, repo, tgt);
    if (rc == 0) {
        if (msg[0]) {
            git_signature *sig = NULL;
            if (git_signature_default(&sig, repo) != 0)
                git_signature_now(&sig, "MVX", "mvx@localhost");
            rc = git_tag_create(&oid, repo, name, obj, sig, msg, 0);
            if (sig) git_signature_free(sig);
        } else {
            rc = git_tag_create_lightweight(&oid, repo, name, obj, 0);
        }
    }
    if (obj) git_object_free(obj);
    git_repository_free(repo);
    if (rc != 0) { fail(argv[5], "tag"); return; }
    char out[600];
    snprintf(out, sizeof out, "tagged %s at %s", name, tgt);
    mv_set_str(argv[5], out, (int64_t)strlen(out));
}

/* --- remotes & clone (libgit2, no OS git) — mvx#94 / mvpkg#23 -------------
   Pure-engine transport so the GIT verb OWNS clone/fetch/pull/push: each runs at
   any tier (no shell, no `!` gate), and a backend-swapped build reuses the same
   code — the path a D3/UniVerse `.git`-in-an-MV-file store needs.  Public
   transport for now (no credential callback yet; the udt libgit2 must be built
   with USE_HTTPS=ON for https remotes). */

/* GITCLONE(url, dir, ref, out) — clone URL into DIR as a plain working-tree clone
   (what a source package needs), then check REF out if given: a branch becomes a
   local tracking branch, a tag or raw SHA detaches HEAD. */
void mvx_sub_GITCLONE(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 4) return;
    ensure_init();
    char url[4096], dir[4096], ref[256];
    arg_str(argv[0], url, sizeof url);
    arg_str(argv[1], dir, sizeof dir);
    arg_str(argv[2], ref, sizeof ref);
    if (!url[0] || !dir[0]) { fail(argv[3], "usage: clone <url> <dir> [ref]"); return; }

    git_repository *repo = NULL;
    git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
    if (git_clone(&repo, url, dir, &opts) != 0) { fail(argv[3], "clone"); return; }

    if (ref[0]) {
        int rc = -1;
        char heads[300], remote[300];
        snprintf(heads, sizeof heads, "refs/heads/%s", ref);
        snprintf(remote, sizeof remote, "refs/remotes/origin/%s", ref);
        git_reference *lr = NULL, *rr = NULL;
        git_object *obj = NULL;
        if (git_reference_lookup(&lr, repo, heads) == 0) {   /* already local (default) */
            rc = git_repository_set_head(repo, heads);
            git_reference_free(lr);
        } else if (git_reference_lookup(&rr, repo, remote) == 0) {  /* remote branch */
            git_commit *c = NULL;
            git_reference *nb = NULL;
            if (git_commit_lookup(&c, repo, git_reference_target(rr)) == 0) {
                if (git_branch_create(&nb, repo, ref, c, 0) == 0) {
                    git_reference_free(nb);
                    rc = git_repository_set_head(repo, heads);
                }
                git_commit_free(c);
            }
            git_reference_free(rr);
        } else if (git_revparse_single(&obj, repo, ref) == 0) {   /* tag or SHA */
            rc = git_repository_set_head_detached(repo, git_object_id(obj));
        }
        if (obj) git_object_free(obj);
        if (rc == 0) {
            git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
            co.checkout_strategy = GIT_CHECKOUT_FORCE;
            rc = git_checkout_head(repo, &co);
        }
        if (rc != 0) { git_repository_free(repo); fail(argv[3], "no such ref"); return; }
    }
    git_repository_free(repo);
    char out[4500];
    if (ref[0]) snprintf(out, sizeof out, "cloned %s -> %s @ %s", url, dir, ref);
    else        snprintf(out, sizeof out, "cloned %s -> %s", url, dir);
    mv_set_str(argv[3], out, (int64_t)strlen(out));
}

/* GITCHERRYPICK(repo, commitish, out) — apply one commit onto HEAD. */
void mvx_sub_GITCHERRYPICK(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 3) return;
    ensure_init();
    char rp[4096], rev[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], rev, sizeof rev);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }
    git_object *ho = NULL, *co = NULL;
    git_commit *ours = NULL, *pick = NULL;
    git_index *mindex = NULL;
    int rc = git_revparse_single(&ho, repo, "HEAD");
    if (rc == 0) rc = git_commit_lookup(&ours, repo, git_object_id(ho));
    if (rc == 0) rc = git_revparse_single(&co, repo, rev);
    if (rc == 0) rc = git_commit_lookup(&pick, repo, git_object_id(co));
    if (rc == 0)
        rc = git_cherrypick_commit(&mindex, repo, pick, ours, 0, NULL);
    if (rc == 0) {
        const char *sum = git_commit_summary(pick);
        char msg[400];
        snprintf(msg, sizeof msg, "%s", sum ? sum : "cherry-pick");
        finish_merge(ctx, repo, rp, mindex, ours, NULL, msg, argv[2]);
    } else {
        fail(argv[2], "cherry-pick");
    }
    if (mindex) git_index_free(mindex);
    if (pick) git_commit_free(pick);
    if (ours) git_commit_free(ours);
    if (co) git_object_free(co);
    if (ho) git_object_free(ho);
    git_repository_free(repo);
}

/* GITRESTORE(repo, file, out) — write records back from HEAD, deleting
   records absent from the commit (git restore / checkout of the file). */
void mvx_sub_GITRESTORE(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    if (argc < 3) return;
    ensure_init();
    char rp[4096], fn[256];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], fn, sizeof fn);
    git_repository *repo = NULL;
    if (git_repository_open(&repo, rp) != 0) { fail(argv[2], "open"); return; }
    git_tree *t = head_tree(repo);
    git_tree_entry *sub = NULL;
    git_tree *subtree = NULL;
    /* The file is named account-relative but lives in the commit under this
       account's prefix, so look it up there (mv_git#44). */
    char tpath[512];
    snprintf(tpath, sizeof tpath, "%s%s", g_prefix, fn);
    if (!t || git_tree_entry_bypath(&sub, t, tpath) != 0 ||
        git_tree_lookup(&subtree, repo, git_tree_entry_id(sub)) != 0) {
        if (sub) git_tree_entry_free(sub);
        if (t) git_tree_free(t);
        git_repository_free(repo);
        fail(argv[2], "no committed history for file");
        return;
    }
    mv_value fvar, id, rec;
    mv_init(&fvar); mv_init(&id); mv_init(&rec);
    int64_t nw = 0, nd = 0;
    char (*seen)[256] = NULL;
    size_t ns = 0, cap = 0;
    if (open_named(ctx, fn, &fvar)) {
        size_t cnt = git_tree_entrycount(subtree);
        for (size_t i = 0; i < cnt; i++) {
            const git_tree_entry *te = git_tree_entry_byindex(subtree, i);
            const char *name = git_tree_entry_name(te);
            git_blob *blob = NULL;
            if (git_blob_lookup(&blob, repo, git_tree_entry_id(te)) != 0)
                continue;
            const char *cp = git_blob_rawcontent(blob);
            int64_t clen = (int64_t)git_blob_rawsize(blob), rl;
            char *r = xlate(cp, clen, '\n', (char)0xFE, &rl);
            mv_set_str(&rec, r, rl);
            free(r);
            mv_set_str(&id, name, (int64_t)strlen(name));
            mv_write(ctx, &rec, &fvar, &id, 0, 0);
            nw++;
            if (ns == cap) { cap = cap ? cap * 2 : 64;
                seen = realloc(seen, cap * sizeof *seen);
                if (!seen) mv_fatal("out of memory in restore"); }
            snprintf(seen[ns++], 256, "%s", name);
            git_blob_free(blob);
        }
        mv_select(ctx, &fvar);
        mv_value dl;
        mv_init(&dl);
        while (mv_readnext(ctx, &dl)) {
            char idb[256];
            arg_str(&dl, idb, sizeof idb);
            int found = 0;
            for (size_t i = 0; i < ns; i++)
                if (strcmp(seen[i], idb) == 0) { found = 1; break; }
            if (!found) { mv_delete_rec(ctx, &fvar, &dl); nd++; }
        }
        mv_clear(&dl);
    }
    free(seen);
    mv_clear(&fvar); mv_clear(&id); mv_clear(&rec);
    git_tree_entry_free(sub);
    git_tree_free(subtree);
    git_tree_free(t);
    git_repository_free(repo);
    char out[80];
    snprintf(out, sizeof out, "restored %lld record(s), %lld removed",
             (long long)nw, (long long)nd);
    mv_set_str(argv[2], out, (int64_t)strlen(out));
}

/* --- plain-C API (#58) -------------------------------------------------
   Thin wrappers the mvx-git executable calls: build the mv_value argv the
   subroutine ABI expects, invoke the very same engine the GIT verb uses, and
   return the last (output) arg as a malloc'd @AM-separated string.  So the
   executable and the verb drive one mechanism — records straight to/from git
   objects, no export copy. */
#include "mvxgit.h"

typedef void (*sub_fn)(mv_ctx *, int32_t, mv_value **);

/* GITSTAGEBLOB(repo, path, content, out) — stage a raw blob at git `path` with
   `content`.  Used to synthesise open-account controls that have no backing
   record: on UniData there is no on-disk %FILE% descriptor to reduce, so
   udt-git writes the open `<file>.DICT/%FILE%` (DIR/hash) and `.mv-account`
   directly. */
void mvx_sub_GITSTAGEBLOB(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 4) return;
    char rp[4096], path[600];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], path, sizeof path);
    const char *content;
    char nb[40];
    int64_t clen = mv_val_chars(argv[2], nb, sizeof nb, &content);
    char keep[MV_GIT_CTL_MAX];
    content = mv_git_sticky_control(rp, path, content, &clen, keep, sizeof keep);
    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[3], "open"); return; }
    git_oid boid;
    if (git_blob_create_from_buffer(&boid, repo, content, (size_t)clen) == 0) {
        git_index_entry e;
        memset(&e, 0, sizeof e);
        e.path = path;
        e.mode = GIT_FILEMODE_BLOB;
        e.id = boid;
        git_index_add(index, &e);
        git_index_write(index);
    }
    git_index_free(index);
    git_repository_free(repo);
    char out[700];
    snprintf(out, sizeof out, "staged %s", path);
    mv_set_str(argv[3], out, (int64_t)strlen(out));
}

/* GITSTAGECTL(repo, path, content, out) — stage a control blob VERBATIM, with
   no stickiness.

   Staging a %FILE% normally goes through mv_git_sticky_control, which puts the
   recorded geometry back: a resize is local operational tuning and must not
   become a commit, and must not ride out to other clones as a new default.  The
   attribute editor is the one place that rule is meant to yield — it exists
   precisely so that changing a shipped default is a deliberate act rather than
   a side effect — so it needs a way in that sticky does not undo.  That is this
   op, and it is the ONLY caller: everything else still stages through
   GITSTAGEBLOB and stays sticky.

   Straight to the index rather than into the batch, so the edit is on disk when
   the command returns and the next GITIXCAT reads it back. */
void mvx_sub_GITSTAGECTL(mv_ctx *ctx, int32_t argc, mv_value **argv) {
    (void)ctx;
    if (argc < 4) return;
    ensure_init();
    char rp[4096], path[700];
    arg_str(argv[0], rp, sizeof rp);
    arg_str(argv[1], path, sizeof path);
    const char *content;
    char nb[40];
    int64_t clen = mv_val_chars(argv[2], nb, sizeof nb, &content);
    git_repository *repo = NULL;
    git_index *index = NULL;
    if (repo_open(rp, &repo, &index) != 0) { fail(argv[3], "open"); return; }
    git_oid boid;
    if (git_blob_create_from_buffer(&boid, repo, content, (size_t)clen) == 0) {
        git_index_entry e;
        memset(&e, 0, sizeof e);
        e.path = path;
        e.mode = GIT_FILEMODE_BLOB;
        e.id = boid;
        git_index_add(index, &e);
        git_index_write(index);
    }
    git_index_free(index);
    git_repository_free(repo);
    char out[700];
    snprintf(out, sizeof out, "staged %s", path);
    mv_set_str(argv[3], out, (int64_t)strlen(out));
}

/* Batched staging for bulk in-session use (the UniData GIT verb via CallC):
   open the repository and index once, accumulate blobs in memory across many
   mv_git_batch_add calls, and write the index once at mv_git_batch_end — O(n),
   avoiding the per-record repo/index re-open + re-write (and the resulting
   resource churn / crash) of calling mv_git_stageblob thousands of times.
   Reuses the same repo_open / xlate as the rest of the engine, so the layout
   and blob form match a normal commit. */
static git_repository *g_brepo;
static git_index *g_bindex;

/* How many records the current batch had to skip because their id cannot be a
   git path.  Reported to the user rather than swallowed — a record that is not
   versioned is something they need to know about. */
static long g_batch_skipped = 0;

int mv_git_batch_begin(const char *repo) {
    ensure_init();
    if (g_bindex) return 1;                     /* already open in this session */
    g_batch_skipped = 0;
    if (repo_open(repo, &g_brepo, &g_bindex) != 0) return 0;
    return 1;
}

/* Stage one blob at git `path`.  translate != 0 turns attribute marks
   (@AM = 0xFE) into newlines (records); 0 stores `content` verbatim (the
   open-account controls, .mv-account). */
/* Can `path` be stored by git?  Record ids are arbitrary MV strings and git
   paths are not: a component may not be empty, "." or "..".  An id containing
   "/" splits into components, and an id that IS "/" produces an empty one.
   That matters far beyond the one record — git cannot build a tree containing
   it, so a single bad id fails the ENTIRE commit ("index cache-tree records
   empty sub-tree") and everything staged alongside it is lost. */
static int path_storable(const char *path) {
    if (!path || !*path) return 0;
    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n == 0) return 0;                                   /* empty component */
        if (n == 1 && p[0] == '.') return 0;                    /* "." */
        if (n == 2 && p[0] == '.' && p[1] == '.') return 0;     /* ".." */
        if (!slash) break;
        p = slash + 1;
    }
    return 1;
}

long mv_git_batch_skipped(void) { return g_batch_skipped; }

void mv_git_batch_add(const char *path, const char *content, int64_t len,
                      int translate) {
    if (!g_bindex) return;
    if (!path_storable(path)) { g_batch_skipped++; return; }
    const char *buf = content;
    char *tmp = NULL;
    int64_t bl = len;
    if (translate) { tmp = xlate(content, len, (char)0xFE, '\n', &bl); buf = tmp; }
    git_oid boid;
    if (git_blob_create_from_buffer(&boid, g_brepo, buf, (size_t)bl) == 0) {
        git_index_entry e;
        memset(&e, 0, sizeof e);
        e.path = path;
        e.mode = GIT_FILEMODE_BLOB;
        e.id = boid;
        git_index_add(g_bindex, &e);
    }
    free(tmp);
}

/* Write the accumulated index to disk and close the batch. */
void mv_git_batch_end(void) {
    if (g_bindex) {
        git_index_write(g_bindex);
        git_index_free(g_bindex);
        g_bindex = NULL;
    }
    if (g_brepo) { git_repository_free(g_brepo); g_brepo = NULL; }
}

/* Run an engine subroutine and return its output.  When `outlen` is non-NULL the
   true byte length comes back through it, which matters for anything that can
   carry binary: a committed record may contain NULs, so a caller that measures
   the result with strlen would truncate it and lose data silently.  Text-output
   callers pass NULL and use the NUL terminator as before. */
static char *run_sub_len(sub_fn fn, mv_ctx *ctx, const char **args, int n,
                         int64_t *outlen) {
    mv_value vals[8];
    mv_value *argv[8];
    for (int i = 0; i < n; i++) {
        mv_init(&vals[i]);
        mv_set_str(&vals[i], args[i], (int64_t)strlen(args[i]));
        argv[i] = &vals[i];
    }
    mv_init(&vals[n]);                   /* output slot */
    argv[n] = &vals[n];
    fn(ctx, n + 1, argv);
    char nb[40];
    const char *p;
    int64_t len = mv_val_chars(&vals[n], nb, sizeof nb, &p);
    char *r = malloc((size_t)len + 1);
    if (!r) mv_fatal("out of memory in git");
    memcpy(r, p, (size_t)len);
    r[len] = '\0';
    if (outlen) *outlen = len;
    for (int i = 0; i <= n; i++) mv_clear(&vals[i]);
    return r;
}

static char *run_sub(sub_fn fn, mv_ctx *ctx, const char **args, int n) {
    return run_sub_len(fn, ctx, args, n, NULL);
}

char *mv_git_init(mv_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITINIT, ctx, a, 1);
}

char *mv_git_index_ids(mv_ctx *ctx, const char *repo, const char *file) {
    const char *a[] = {repo, file};
    return run_sub(mvx_sub_GITINDEXIDS, ctx, a, 2);
}

char *mv_git_prune_gone(mv_ctx *ctx, const char *repo, const char *live) {
    const char *a[] = {repo, live ? live : ""};
    return run_sub(mvx_sub_GITPRUNE, ctx, a, 2);
}

/* textconv (mvx#25 tidy diffs) — a git textconv filter: read the record blob at
   `path` (git hands the content as a temp file) and write a legible rendering to
   stdout.  DISPLAY ONLY — the stored blob is never altered.  The blob already
   carries @AM as newlines; put each sub-attribute value on its own indented line
   so a diff reads cleanly instead of showing raw mark bytes:
     @VM (0xFD) -> "\n]"   @SVM (0xFC) -> "\n]]"   @TM (0xFB) -> "\n]]]"
   (a bare @AM 0xFE, should one ever appear, -> newline).  Plain text with no
   marks passes through unchanged, so it is safe to apply to every path. */
int mv_git_textconv(const char *path) {
    FILE *f = (path && strcmp(path, "-") != 0) ? fopen(path, "rb") : stdin;
    if (!f) { fprintf(stderr, "mvx-git: textconv: cannot open %s\n", path ? path : "-"); return 1; }
    int c;
    while ((c = fgetc(f)) != EOF) {
        switch ((unsigned char)c) {
            case 0xFE: fputc('\n', stdout); break;
            case 0xFD: fputs("\n]", stdout); break;
            case 0xFC: fputs("\n]]", stdout); break;
            case 0xFB: fputs("\n]]]", stdout); break;
            default:   fputc(c, stdout);
        }
    }
    if (f != stdin) fclose(f);
    return 0;
}
char *mv_git_add(mv_ctx *ctx, const char *repo, const char *file,
                  const char *id) {
    const char *a[] = {repo, file, id};
    return run_sub(mvx_sub_GITADD, ctx, a, 3);
}
char *mv_git_addsub(mv_ctx *ctx, const char *repo, const char *name) {
    const char *a[] = {repo, name};
    return run_sub(mvx_sub_GITADDSUB, ctx, a, 2);
}
char *mv_git_ixcat(mv_ctx *ctx, const char *repo, const char *path) {
    const char *a[] = {repo, path};
    return run_sub(mvx_sub_GITIXCAT, ctx, a, 2);
}
char *mv_git_ixcat_len(mv_ctx *ctx, const char *repo, const char *path,
                       int64_t *outlen) {
    const char *a[] = {repo, path};
    return run_sub_len(mvx_sub_GITIXCAT, ctx, a, 2, outlen);
}
char *mv_git_staged(mv_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITSTAGED, ctx, a, 1);
}
char *mv_git_stagectl(mv_ctx *ctx, const char *repo, const char *path,
                      const char *content) {
    const char *a[] = {repo, path, content};
    return run_sub(mvx_sub_GITSTAGECTL, ctx, a, 3);
}
char *mv_git_stageblob(mv_ctx *ctx, const char *repo, const char *path,
                       const char *content) {
    const char *a[] = {repo, path, content};
    return run_sub(mvx_sub_GITSTAGEBLOB, ctx, a, 3);
}

/* See mvxgit.h: keep an already-committed %FILE% control instead of the live
   file's current geometry. */
/* The control STAGED for `base` (the index's <base>.DICT/%FILE%) into out[cap];
   its length, or -1 when the index has no entry for it. */
static int staged_control(const char *repo, const char *base,
                          char *out, size_t cap) {
    if (cap) out[0] = '\0';
    ensure_init();
    git_repository *r = NULL;
    if (git_repository_open(&r, repo) != 0) return -1;   /* never create here */
    git_index *ix = NULL;
    int n = -1;
    if (git_repository_index(&ix, r) == 0) {
        char path[600];
        snprintf(path, sizeof path, "%s.DICT/%%FILE%%", base);
        const git_index_entry *e = git_index_get_bypath(ix, path, 0);
        git_blob *b = NULL;
        if (e && git_blob_lookup(&b, r, &e->id) == 0) {
            const char *c = git_blob_rawcontent(b);
            int64_t cl = (int64_t)git_blob_rawsize(b);
            if (cl >= 0 && (size_t)cl < cap) {
                memcpy(out, c, (size_t)cl);
                out[cl] = '\0';
                n = (int)cl;
            }
            git_blob_free(b);
        }
        git_index_free(ix);
    }
    git_repository_free(r);
    return n;
}

const char *mv_git_sticky_control(const char *repo, const char *path,
                                  const char *content, int64_t *len,
                                  char *keep, size_t cap) {
    if (!path || !content) return content;
    size_t plen = strlen(path);
    const char *suf = ".DICT/%FILE%";
    size_t sl = strlen(suf);
    if (plen <= sl || strcmp(path + plen - sl, suf) != 0) return content;
    if (strncmp(content, "hash", 4) != 0) return content;   /* DIR has no size */
    char base[512];
    snprintf(base, sizeof base, "%.*s", (int)(plen - sl), path);
    /* THE INDEX FIRST, THEN HEAD.  What git knows about this file's geometry is
       what is staged; HEAD is only the last thing committed.  Consulting HEAD
       alone let a plain `add -A` between an attribute edit and its commit put
       the committed geometry back over the edit — the edit would vanish before
       it was ever committed, and status would go clean as if it had landed.
       It also settles a file whose geometry is staged but not yet committed:
       once git has learned it, it stops moving. */
    if (staged_control(repo, base, keep, cap) < 0 &&
        mv_git_committed_control(repo, base, keep, cap) < 0) return content;
    if (strncmp(keep, "hash", 4) != 0) return content;
    if (len) *len = (int64_t)strlen(keep);
    return keep;
}

int mv_git_committed_control(const char *repo, const char *base,
                             char *out, size_t cap) {
    if (cap) out[0] = '\0';
    ensure_init();
    git_repository *r = NULL;
    if (git_repository_open(&r, repo) != 0) return -1;   /* never create here */
    git_tree *t = head_tree(r);
    int n = head_control(r, t, base, out, cap);
    if (t) git_tree_free(t);
    git_repository_free(r);
    return n;
}
char *mv_git_headfiles(mv_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITFILES, ctx, a, 1);
}
char *mv_git_catpath(mv_ctx *ctx, const char *repo, const char *path) {
    const char *a[] = {repo, path};
    return run_sub(mvx_sub_GITCAT, ctx, a, 2);
}

/* As mv_git_catpath, but reporting the content's true length.  A committed
   record is arbitrary bytes and may contain NULs, so a caller that has a way to
   carry an explicit length (the background process's framed pipe) must use this
   rather than measure the result with strlen and silently truncate. */
char *mv_git_catpath_len(mv_ctx *ctx, const char *repo, const char *path,
                         int64_t *outlen) {
    const char *a[] = {repo, path};
    return run_sub_len(mvx_sub_GITCAT, ctx, a, 2, outlen);
}
char *mv_git_adddisk(mv_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITADDDISK, ctx, a, 1);
}
char *mv_git_openform(mv_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITOPENFORM, ctx, a, 1);
}
char *mv_git_materialize(mv_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITMATERIALIZE, ctx, a, 1);
}
char *mv_git_rm(mv_ctx *ctx, const char *repo, const char *file,
                 const char *id) {
    const char *a[] = {repo, file, id};
    return run_sub(mvx_sub_GITRM, ctx, a, 3);
}
char *mv_git_commit(mv_ctx *ctx, const char *repo, const char *msg) {
    const char *a[] = {repo, msg};
    return run_sub(mvx_sub_GITCOMMIT, ctx, a, 2);
}
char *mv_git_status(mv_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITSTATUS, ctx, a, 1);
}
char *mv_git_log(mv_ctx *ctx, const char *repo, const char *count) {
    const char *a[] = {repo, count};
    return run_sub(mvx_sub_GITLOG, ctx, a, 2);
}
char *mv_git_diff(mv_ctx *ctx, const char *repo, const char *file) {
    const char *a[] = {repo, file};
    return run_sub(mvx_sub_GITDIFF, ctx, a, 2);
}
char *mv_git_diff_u(mv_ctx *ctx, const char *repo, const char *file) {
    const char *a[] = {repo, file};
    return run_sub(mvx_sub_GITDIFFU, ctx, a, 2);
}
char *mv_git_udiff(mv_ctx *ctx, const char *oldtext, const char *newtext,
                   const char *path) {
    const char *a[] = {oldtext, newtext, path};
    return run_sub(mvx_sub_GITUDIFF, ctx, a, 3);
}
char *mv_git_show(mv_ctx *ctx, const char *repo, const char *file,
                   const char *id) {
    const char *a[] = {repo, file, id};
    return run_sub(mvx_sub_GITSHOW, ctx, a, 3);
}
char *mv_git_branch(mv_ctx *ctx, const char *repo, const char *name) {
    const char *a[] = {repo, name};
    return run_sub(mvx_sub_GITBRANCH, ctx, a, 2);
}
char *mv_git_checkout(mv_ctx *ctx, const char *repo, const char *name) {
    const char *a[] = {repo, name};
    return run_sub(mvx_sub_GITCHECKOUT, ctx, a, 2);
}
char *mv_git_switch(mv_ctx *ctx, const char *repo, const char *name) {
    const char *a[] = {repo, name};
    return run_sub(mvx_sub_GITSWITCH, ctx, a, 2);
}
char *mv_git_merge(mv_ctx *ctx, const char *repo, const char *branch) {
    const char *a[] = {repo, branch};
    return run_sub(mvx_sub_GITMERGE, ctx, a, 2);
}
char *mv_git_cherrypick(mv_ctx *ctx, const char *repo, const char *commit) {
    const char *a[] = {repo, commit};
    return run_sub(mvx_sub_GITCHERRYPICK, ctx, a, 2);
}
char *mv_git_restore(mv_ctx *ctx, const char *repo, const char *file) {
    const char *a[] = {repo, file};
    return run_sub(mvx_sub_GITRESTORE, ctx, a, 2);
}
char *mv_git_clone(mv_ctx *ctx, const char *url, const char *dir, const char *ref) {
    const char *a[] = {url, dir, ref ? ref : ""};
    return run_sub(mvx_sub_GITCLONE, ctx, a, 3);
}
char *mv_git_fetch(mv_ctx *ctx, const char *repo, const char *remote) {
    const char *a[] = {repo, remote ? remote : ""};
    return run_sub(mvx_sub_GITFETCH, ctx, a, 2);
}
char *mv_git_push(mv_ctx *ctx, const char *repo, const char *remote, const char *refspec) {
    const char *a[] = {repo, remote ? remote : "", refspec ? refspec : ""};
    return run_sub(mvx_sub_GITPUSH, ctx, a, 3);
}
char *mv_git_pull(mv_ctx *ctx, const char *repo, const char *remote, const char *branch) {
    const char *a[] = {repo, remote ? remote : "", branch ? branch : ""};
    return run_sub(mvx_sub_GITPULL, ctx, a, 3);
}
char *mv_git_pullref(mv_ctx *ctx, const char *repo, const char *remote,
                     const char *branch) {
    const char *a[] = {repo, remote ? remote : "", branch ? branch : ""};
    return run_sub(mvx_sub_GITPULLREF, ctx, a, 3);
}
char *mv_git_remote(mv_ctx *ctx, const char *repo, const char *action,
                    const char *name, const char *url) {
    const char *a[] = {repo, action ? action : "", name ? name : "", url ? url : ""};
    return run_sub(mvx_sub_GITREMOTE, ctx, a, 4);
}
char *mv_git_config(mv_ctx *ctx, const char *repo, const char *key, const char *value) {
    const char *a[] = {repo, key ? key : "", value ? value : ""};
    return run_sub(mvx_sub_GITCONFIG, ctx, a, 3);
}
char *mv_git_addall(mv_ctx *ctx, const char *repo) {
    const char *a[] = {repo};
    return run_sub(mvx_sub_GITADDALL, ctx, a, 1);
}
char *mv_git_tag(mv_ctx *ctx, const char *repo, const char *op, const char *name,
                 const char *target, const char *message) {
    const char *a[] = {repo, op ? op : "", name ? name : "",
                       target ? target : "", message ? message : ""};
    return run_sub(mvx_sub_GITTAG, ctx, a, 5);
}
