/*
 * mv_git — record-git for MultiValue.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* uv-git — git for a UniVerse account, from the shell.
 *
 * The counterpart of mvx-git and udt-git.  All three do the same thing: keep the
 * git objects in process with libgit2, and reach records through whatever this
 * platform allows.  mvx-git goes through the MVX runtime, udt-git through
 * UniData's InterCall, and uv-git through a SESSION running BP/GIT.AGENT
 * (mv_git#47) — because on UniVerse C cannot touch records directly:
 *
 *   - GCI, the in-process C bridge, is licensed and non-functional in the
 *     UniVerse Trial Edition (mv_git#37) — it refuses even Rocket's own shipped
 *     example.
 *   - InterCall, the documented record API, is a CLIENT SDK shipped in the
 *     Rocket U2 Clients bundle, not with the server, and is not available for
 *     Linux to us (mv_git#42).  The library that IS installed, libvm_ici.a, is
 *     the superseded ICI: undocumented in both shipped headers, and it segfaults
 *     rather than erroring when called.
 *
 * A session is the only way in — but only the RECORD half needs one.  An earlier
 * design drove the in-session GIT verb and shipped git objects back out to a
 * second process; this one keeps the git work here and asks the session for
 * records alone, which is both simpler and the correct division of labour.  See
 * uvgit_rt.c for the record contract and uvsession.c for the session lifecycle,
 * where the important constraint lives: a session is a LICENCE, so it is opened
 * lazily, held only while there is work, and released cleanly.
 *
 * TWO PATHS, chosen by what the directory actually is:
 *
 *   an ACCOUNT (it has a VOC)   the full engine, records included
 *   an ORDINARY DIRECTORY       the engine's disk-side operations only — no
 *                               session, because there is nothing a session
 *                               could contribute to a directory of plain files
 *
 * The second is not a courtesy: a repository may hold ordinary files beside its
 * accounts, and refusing to work on them would make uv-git useless for exactly
 * the repositories that need it most.
 *
 *   uv-git [-a account] [-j jobs] [--idle-timeout secs] <command> [args...]
 */

#define _POSIX_C_SOURCE 200809L

#include "mvxgit.h"
#include "uvsession.h"
#include <git2.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define UV_BIN "uv"

/* The repository this directory belongs to, found the way git finds it: by
 * searching upward.  An account may sit at a repository root, or beneath one —
 * a repo holding several accounts, or an account inside a larger project — and
 * both must work.
 *
 * Returns 1 and fills `out` with the git directory when the repository is above
 * us; 0 when there is none, or when the repository found is our own `.git`
 * (in which case the default ".git" is already right). */
static int enclosing_repo(char *out, size_t cap) {
    if (access(".git", F_OK) == 0) return 0;      /* our own — not enclosing */
    git_libgit2_init();
    git_buf found = {0};
    /* across_fs = 0: do not wander over a mount boundary looking for a repo */
    if (git_repository_discover(&found, ".", 0, NULL) != 0) return 0;
    snprintf(out, cap, "%s", found.ptr ? found.ptr : "");
    git_buf_dispose(&found);
    return out[0] != '\0';
}

/* --- adopt: turn a checked-out account tree into an account HERE ----------
 *
 * A clone writes files; it does not create accounts.  So a repository holding
 * an account arrives on UniVerse as a directory of records with a descriptor
 * naming a platform that may not be this one — `.mvx`, `.udt`.  Adoption is the
 * step that makes it real here, and it is deliberately a separate command
 * rather than something clone does on its own: it CONVERTS the account, and a
 * conversion the user did not ask for is a bad surprise.
 *
 * Two things have to be settled that the repository cannot answer.
 *
 * THE FLAVOUR.  A UniVerse account is created with a VOC flavour — Ideal, IN2,
 * Prime Information, PICK, PI/open, Microdata Reality — chosen once, when the
 * account is born, and it governs how the account's own VOC behaves.  Verified
 * on UniVerse 14.2.1: the flavour is baked into the VOC's CONTENTS at creation
 * (the templates differ — 847 records for PICK, 840 for Ideal, 851 for Reality)
 * and is recorded NOWHERE that can be read back.  There is no $OPTIONS record,
 * no SYSTEM() code, no dotfile: two accounts of different flavours are
 * indistinguishable on disk by name.  So a descriptor that does not carry the
 * flavour has lost it permanently, and the only honest recovery is to ask.
 *
 * Worse, the prompt UniVerse itself shows accepts a BLANK answer and silently
 * takes Ideal.  An account built by feeding it defaults looks correct and
 * behaves differently, which is exactly the failure this command exists to
 * prevent.
 *
 * THE FORM.  Once adopted, does the repository describe this account natively
 * (`.uv`) or portably (`.mv-account`)?  The portable form is what lets it
 * travel to another MV platform; the native form describes this system alone.
 * Either way the descriptor now says something different from what was cloned,
 * so the change is left in the working tree — `git status` shows the rename and
 * the open-form edits, and the user commits them like any other change.
 */

/* The flavours UniVerse offers, and the menu answer each corresponds to.
   Read off the live prompt on 14.2.1 rather than from documentation, since this
   is fed to that prompt verbatim and an off-by-one builds the wrong account. */
static const struct { const char *name; const char *code; const char *shown; }
uv_flavours[] = {
    { "IDEAL",   "0", "Ideal UniVerse compatibility" },
    { "IN2",     "1", "IN2 compatibility"            },
    { "PRIME",   "2", "Prime Information compatibility" },
    { "PICK",    "3", "PICK compatibility"           },
    { "PIOPEN",  "4", "PI/open compatibility"        },
    { "REALITY", "5", "Microdata Reality compatibility" },
};
#define NFLAVOURS ((int)(sizeof uv_flavours / sizeof uv_flavours[0]))

/* Resolve a flavour the user typed — by name, by a common alias, or by the menu
   number — to its table entry.  Returns the index, or -1. */
static int flavour_index(const char *s) {
    if (!s || !s[0]) return -1;
    /* Fold to upper case and drop the separators people vary on, so "PI/open",
       "pi-open" and "PIOPEN" are one answer. */
    char up[32];
    size_t o = 0;
    for (size_t k = 0; s[k] && o < sizeof up - 1; k++) {
        char c = s[k];
        if (c == '/' || c == '-' || c == ' ' || c == '_') continue;
        up[o++] = (char)toupper((unsigned char)c);
    }
    up[o] = '\0';
    for (int k = 0; k < NFLAVOURS; k++) {
        if (!strcasecmp(up, uv_flavours[k].name)) return k;
        if (!strcmp(up, uv_flavours[k].code))     return k;
    }
    /* spellings a user reasonably types for the same thing */
    if (!strcasecmp(up, "PI"))            return 2;  /* Prime Information */
    if (!strcasecmp(up, "INFORMATION"))   return 2;
    if (!strcasecmp(up, "PIOPEN"))        return 4;
    if (!strcasecmp(up, "MICRODATA"))     return 5;
    if (!strcasecmp(up, "UNIVERSE"))      return 0;
    return -1;
}

/* Read a line from the terminal into buf; returns 0 if there is no terminal. */
static int ask(const char *prompt, char *buf, size_t cap) {
    if (!isatty(STDIN_FILENO)) return 0;
    fputs(prompt, stderr);
    fflush(stderr);
    if (!fgets(buf, (int)cap, stdin)) return 0;
    size_t n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    return 1;
}

/* The descriptor present in this directory, if any: the portable form first,
   then the native ones.  `platform_out` receives the bare marker name. */
static const char *find_descriptor(void) {
    static const char *names[] = { ".mv-account", ".mvx", ".udt", ".uv", NULL };
    for (int i = 0; names[i]; i++)
        if (access(names[i], F_OK) == 0) return names[i];
    return NULL;
}

/* --- what account creation added, and keeping it out of git ---------------
 *
 * Making a directory an account is not free of side effects: UniVerse writes its
 * own files there — VOC, D_VOC, VOCLIB, D_VOCLIB, &SAVEDLISTS&, D_&SAVEDLISTS&
 * on 14.2.1 — and they are hash-file BINARIES, not content anyone means to
 * version.  Left alone they turn up as untracked files, so adopting a repository
 * would dirty it just by being run.
 *
 * The invariant that matters: checking out a repo whose account is already
 * native here (`.uv`), then adopting it, must leave `git status` CLEAN.  Adopt
 * is the step that reconciles a checkout with this system; if it reports work to
 * do, it has done its job badly.
 *
 * Two decisions follow from that.
 *
 * The list is DERIVED, not hardcoded: the directory is read before and after
 * creation, and whatever appeared is what creation added.  That is exact for the
 * UniVerse actually running, needs no table to drift out of date, and says
 * precisely what it means — the difference from the default account.
 *
 * The exclusions go in `.git/info/exclude`, NOT `.gitignore`.  A tracked
 * .gitignore would itself be a change, which is the very thing we are trying not
 * to produce; and these names are a property of THIS checkout on THIS system,
 * not of the project.  Adopt runs once per clone, which is exactly the scope
 * info/exclude has.
 */

/* Directory entries, sorted, NUL-separated in a single buffer. */
typedef struct { char *d; size_t len, cap; int n; } names;

static void names_add(names *v, const char *s) {
    size_t need = strlen(s) + 1;
    if (v->len + need > v->cap) {
        v->cap = (v->len + need) * 2 + 256;
        v->d = realloc(v->d, v->cap);
        if (!v->d) { perror("uv-git"); exit(1); }
    }
    memcpy(v->d + v->len, s, need);
    v->len += need;
    v->n++;
}

static int names_has(const names *v, const char *s) {
    for (size_t i = 0; i < v->len; i += strlen(v->d + i) + 1)
        if (!strcmp(v->d + i, s)) return 1;
    return 0;
}

/* Snapshot the current directory's entries, skipping dotfiles — the descriptor
   and .git are ours, not UniVerse's, and must never be excluded. */
static void snapshot(names *v) {
    memset(v, 0, sizeof *v);
    DIR *d = opendir(".");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        names_add(v, e->d_name);
    }
    if (d) closedir(d);
}

/* Append `path`-prefixed exclusions for everything in `now` that was not in
   `before`.  `prefix` is the account's position under the repository root
   ("" at the root, "acctA/" below it), since info/exclude is repo-wide.

   A hashed file needs BOTH forms: `/VOC` hides the on-disk binary, and
   `!/VOC/` puts the record PATHS back, because a record stages at `VOC/<id>`
   and a bare `/VOC` rule would hide those too — silently, in status.  Verified
   against git: with `/VOC` alone, `VOC/BASIC` checks as ignored; with the
   negation after it, the file is ignored and the path is not. */
static void exclude_new(const names *before, const names *now,
                        const char *gitdir, const char *prefix) {
    char path[4096];
    snprintf(path, sizeof path, "%s/info/exclude", gitdir);
    /* .git/info may not exist in a repo created by something terse */
    {
        char dir[4096];
        snprintf(dir, sizeof dir, "%s/info", gitdir);
        mkdir(dir, 0777);
    }
    /* Read what is already there so a second adopt does not duplicate lines. */
    char have[65536] = "";
    {
        FILE *f = fopen(path, "rb");
        if (f) { size_t n = fread(have, 1, sizeof have - 1, f); have[n] = '\0'; fclose(f); }
    }
    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "uv-git adopt: warning: cannot write %s: %s\n",
                path, strerror(errno));
        return;
    }
    int wrote_header = 0;
    for (size_t i = 0; i < now->len; i += strlen(now->d + i) + 1) {
        const char *n = now->d + i;
        if (names_has(before, n)) continue;
        struct stat sb;
        int isdir = (stat(n, &sb) == 0 && S_ISDIR(sb.st_mode));
        char line[4200];
        snprintf(line, sizeof line, "/%s%s%s\n", prefix, n, isdir ? "/" : "");
        if (strstr(have, line)) continue;
        if (!wrote_header) {
            fputs("\n# uv-git adopt: files UniVerse created when this directory\n"
                  "# became an account.  Hash-file binaries, not content.\n", f);
            wrote_header = 1;
        }
        fputs(line, f);
        /* Put record paths back for a hashed file: the binary is ignored, the
           records it holds are not. */
        if (!isdir) fprintf(f, "!/%s%s/\n", prefix, n);
    }
    fclose(f);
}

/* Create the UniVerse account in the current directory with `code` as the
   flavour answer.  A fresh directory becomes an account on its first `uv`,
   which asks whether to set it up and then which flavour to use. */
static int make_account(const char *code) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "printf 'Y\\n%s\\nQUIT\\n' | %s >/dev/null 2>&1",
             code, UV_BIN);
    int rc = system(cmd);
    (void)rc;
    return access("VOC", F_OK) == 0 ? 0 : -1;
}

/* One agent call, rendered for a person: the reply as lines, or the status. */
static int agent_one(uv_session *ses, const char *op, int na, const char *const *a) {
    char *body = NULL;
    long blen = 0;
    int st = uvs_calls(ses, op, na, a, &body, &blen);
    if (st != 0) {
        fprintf(stderr, "status %d%s%.*s\n", st, blen ? ": " : "",
                (int)blen, body ? body : "");
        free(body);
        return 1;
    }
    if (body) {
        /* record and list text is @AM/@VM separated; show the structure */
        for (long k = 0; k < blen; k++) {
            unsigned char c = (unsigned char)body[k];
            if (c == 0xFE) body[k] = '\n';
            else if (c == 0xFD) body[k] = '|';
        }
        fwrite(body, 1, (size_t)blen, stdout);
        free(body);
    }
    fputc('\n', stdout);
    return 0;
}

/* Where we are in the repository: `gitdir` receives the real git directory
   (which is where info/exclude lives) and `prefix` the account's path beneath
   the working tree, "" at the root and "acctA/" below it.  Exclusion patterns
   are repo-wide, so a nested account's must carry its own prefix or they would
   match the wrong directory.  Returns 0 on success. */
static int repo_place(char *gitdir, size_t gcap, char *prefix, size_t pcap) {
    git_libgit2_init();
    git_repository *repo = NULL;
    if (git_repository_open_ext(&repo, ".", 0, NULL) != 0) return -1;
    const char *gd = git_repository_path(repo);
    const char *wd = git_repository_workdir(repo);
    snprintf(gitdir, gcap, "%s", gd ? gd : ".git/");
    /* strip the trailing slash libgit2 always appends */
    size_t gl = strlen(gitdir);
    if (gl && gitdir[gl - 1] == '/') gitdir[gl - 1] = '\0';
    prefix[0] = '\0';
    char cwd[4096];
    if (wd && getcwd(cwd, sizeof cwd)) {
        size_t wl = strlen(wd);
        /* wd ends in '/', so cwd == wd minus that slash means we are at the root */
        if (wl && !strncmp(cwd, wd, wl - 1)) {
            const char *rest = cwd + (wl - 1);
            while (*rest == '/') rest++;
            if (*rest) snprintf(prefix, pcap, "%s/", rest);
        }
    }
    git_repository_free(repo);
    return 0;
}

static int adopt(int argc, char **argv, int i) {
    const char *dir = NULL, *want_flavour = NULL;
    int open_form = -1;                       /* -1 = ask */

    for (i++; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--flavour=", 10))      want_flavour = a + 10;
        else if (!strncmp(a, "--flavor=", 9))   want_flavour = a + 9;
        else if ((!strcmp(a, "--flavour") || !strcmp(a, "--flavor")) && i + 1 < argc)
            want_flavour = argv[++i];
        else if (!strcmp(a, "--open-account") || !strcmp(a, "--open"))
            open_form = 1;
        else if (!strcmp(a, "--no-open-account") || !strcmp(a, "--no-open"))
            open_form = 0;
        else if (a[0] == '-') {
            fprintf(stderr, "uv-git adopt: unknown option %s\n", a);
            return 2;
        } else dir = a;
    }
    if (dir && chdir(dir) != 0) {
        fprintf(stderr, "uv-git adopt: cannot enter %s: %s\n", dir, strerror(errno));
        return 1;
    }

    const char *desc_name = find_descriptor();
    if (!desc_name) {
        fprintf(stderr,
            "uv-git adopt: this directory carries no MV account descriptor\n"
            "        (.mv-account, .mvx, .udt or .uv), so there is no account "
            "here to adopt.\n");
        return 1;
    }

    char src[65536];
    size_t srclen = 0;
    {
        FILE *f = fopen(desc_name, "rb");
        if (!f) {
            fprintf(stderr, "uv-git adopt: cannot read %s: %s\n",
                    desc_name, strerror(errno));
            return 1;
        }
        srclen = fread(src, 1, sizeof src, f);
        fclose(f);
    }

    int already = (access("VOC", F_OK) == 0);

    /* --- the flavour ---------------------------------------------------- */
    char have[64] = "";
    int fi = -1;
    if (want_flavour) {
        fi = flavour_index(want_flavour);
        if (fi < 0) {
            fprintf(stderr, "uv-git adopt: '%s' is not a UniVerse flavour\n",
                    want_flavour);
            for (int k = 0; k < NFLAVOURS; k++)
                fprintf(stderr, "        %-8s %s\n",
                        uv_flavours[k].name, uv_flavours[k].shown);
            return 2;
        }
    } else if (mv_git_desc_field(src, srclen, "flavour", have, sizeof have)) {
        fi = flavour_index(have);
        if (fi < 0)
            fprintf(stderr, "uv-git adopt: the descriptor names an unknown "
                            "flavour '%s'; asking instead\n", have);
    }

    if (fi < 0 && already) {
        /* The account exists, so nothing is about to be created with the wrong
           flavour — and UniVerse cannot be asked what it was.  Say so rather
           than record a guess in the descriptor. */
        fprintf(stderr, "uv-git adopt: this is already a UniVerse account and "
                        "its flavour cannot be read back;\n"
                        "        pass --flavour=<name> to record it in the "
                        "descriptor.\n");
    } else if (fi < 0) {
        char line[64];
        fprintf(stderr,
            "This repository does not say which VOC flavour the account needs,\n"
            "and UniVerse cannot be asked afterwards: the flavour is fixed when\n"
            "the account is created and is not recorded anywhere readable.\n\n");
        for (int k = 0; k < NFLAVOURS; k++)
            fprintf(stderr, "    %-8s %s\n",
                    uv_flavours[k].name, uv_flavours[k].shown);
        fprintf(stderr, "\n");
        while (fi < 0) {
            if (!ask("Flavour for this account [PICK]: ", line, sizeof line)) {
                fprintf(stderr,
                    "uv-git adopt: no terminal to ask on — pass "
                    "--flavour=<name> (e.g. --flavour=PICK).\n");
                return 1;
            }
            if (!line[0]) { fi = flavour_index("PICK"); break; }
            fi = flavour_index(line);
            if (fi < 0) fprintf(stderr, "  not a flavour: %s\n", line);
        }
    }

    /* --- the form -------------------------------------------------------- */
    /* An account already committed in the portable form stays portable — there
       is nothing to decide, and asking would invite an answer that silently
       demotes it. */
    if (open_form < 0 && !strcmp(desc_name, ".mv-account"))
        open_form = 1;
    if (open_form < 0) {
        char line[16];
        fprintf(stderr,
            "\nAlso make this an open account?\n"
            "  The portable descriptor (.mv-account) is what lets the account "
            "travel to\n"
            "  another MV platform; the native one (.uv) describes this system "
            "alone.\n");
        if (!ask("Open account? [Y/n]: ", line, sizeof line)) {
            fprintf(stderr,
                "uv-git adopt: no terminal to ask on — pass --open-account or "
                "--no-open-account.\n");
            return 1;
        }
        open_form = !(line[0] == 'n' || line[0] == 'N');
    }

    /* --- create the account ---------------------------------------------- */
    if (!already) {
        if (fi < 0) fi = flavour_index("PICK");
        fprintf(stderr, "uv-git adopt: creating a UniVerse account (%s)\n",
                uv_flavours[fi].name);
        names before, after;
        snapshot(&before);
        if (make_account(uv_flavours[fi].code) != 0) {
            fprintf(stderr, "uv-git adopt: the account was not created "
                            "(no VOC); is `%s` on PATH?\n", UV_BIN);
            free(before.d);
            return 1;
        }
        /* Whatever appeared is the default account's own furniture — keep it out
           of git, or adopting a repository would dirty it just by running. */
        snapshot(&after);
        {
            char gitdir[4096] = "", prefix[4096] = "";
            if (repo_place(gitdir, sizeof gitdir, prefix, sizeof prefix) == 0)
                exclude_new(&before, &after, gitdir, prefix);
        }
        free(before.d);
        free(after.d);
    }

    /* --- rewrite the descriptor ------------------------------------------ */
    char out[65536], newname[64];
    int n = mv_git_desc_adopt(src, srclen, "uv",
                              fi >= 0 ? uv_flavours[fi].name : "",
                              open_form, newname, sizeof newname,
                              out, sizeof out);
    if (n <= 0) {
        fprintf(stderr, "uv-git adopt: could not render the descriptor\n");
        return 1;
    }
    /* Did the descriptor actually change?  Adopting a repository whose account is
       already native here should be a no-op in git, and saying otherwise would
       send the user looking for a change that is not there. */
    int desc_changed = strcmp(newname, desc_name) != 0 ||
                       (size_t)n != srclen || memcmp(out, src, (size_t)n) != 0;
    {
        FILE *f = fopen(newname, "wb");
        if (!f || fwrite(out, 1, (size_t)n, f) != (size_t)n) {
            fprintf(stderr, "uv-git adopt: cannot write %s: %s\n",
                    newname, strerror(errno));
            if (f) fclose(f);
            return 1;
        }
        fclose(f);
    }
    if (strcmp(newname, desc_name) != 0 && unlink(desc_name) != 0)
        fprintf(stderr, "uv-git adopt: warning: could not remove %s: %s\n",
                desc_name, strerror(errno));

    /* The open form is an opt-in the engine reads from the repo config, so a
       later add/commit here emits open-form blobs to match the descriptor we
       just wrote.  Without this the descriptor would claim one thing and the
       commits do another. */
    if (open_form) {
        git_libgit2_init();
        git_repository *repo = NULL;
        if (git_repository_open_ext(&repo, ".", 0, NULL) == 0) {
            git_config *cfg = NULL;
            if (git_repository_config(&cfg, repo) == 0) {
                git_config_set_bool(cfg, "mvx.openaccount", 1);
                git_config_free(cfg);
            }
            git_repository_free(repo);
        }
    }

    printf("adopted as a UniVerse account%s%s%s\n",
           fi >= 0 ? " (" : "",
           fi >= 0 ? uv_flavours[fi].name : "",
           fi >= 0 ? " flavour)" : "");
    if (strcmp(newname, desc_name) != 0)
        printf("  %s -> %s\n", desc_name, newname);
    else if (desc_changed)
        printf("  %s updated\n", newname);
    else
        printf("  %s unchanged — this repository already describes a "
               "UniVerse account\n", newname);
    if (open_form) printf("  mvx.openaccount = true\n");
    if (desc_changed)
        printf("\nReview it with `git status`: the descriptor change is an "
               "ordinary change\nin the working tree, to commit like any "
               "other.\n");
    else
        printf("\nNothing for git to show: the account is now real here, and "
               "the files\nUniVerse created for it are excluded locally "
               "(.git/info/exclude).\n");
    if (access("BP", F_OK) == 0 && access("VOC", F_OK) == 0)
        printf("\nThe records are on disk but not yet in the account's files.\n"
               "Install git here (./install.sh), then `uv-git checkout` to load "
               "them.\n");
    return 0;
}

/* status for a plain directory, straight from libgit2.
 *
 * The engine's mv_git_status cannot serve this: it compares LIVE RECORDS against
 * HEAD, so it calls mv_open and — in this binary, built against the recordless
 * backend — aborts.  That abort is the backend working as intended (it announces
 * a wrong assumption rather than misbehaving quietly), and it caught this very
 * mistake.  A directory of files has no records to compare, so the right answer
 * is git's own status, which is what a user typing `uv-git status` there means.
 *
 * mv_git_diff is record-based for the same reason and has no plain equivalent
 * here, so it is simply not offered — `git diff` is the tool for that. */
static int plain_status(void) {
    git_libgit2_init();
    git_repository *repo = NULL;
    if (git_repository_open(&repo, ".") != 0) {
        fprintf(stderr, "uv-git: not a git repository\n");
        return 1;
    }
    git_status_list *sl = NULL;
    git_status_options o = GIT_STATUS_OPTIONS_INIT;
    o.show  = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    o.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED;
    int rc = git_status_list_new(&sl, repo, &o);
    if (rc != 0) {
        const git_error *e = git_error_last();
        fprintf(stderr, "uv-git: status: %s\n",
                (e && e->message) ? e->message : "failed");
        git_repository_free(repo);
        return 1;
    }
    size_t n = git_status_list_entrycount(sl), shown = 0;
    for (size_t k = 0; k < n; k++) {
        const git_status_entry *se = git_status_byindex(sl, k);
        const char *path = se->index_to_workdir ? se->index_to_workdir->old_file.path
                                                : se->head_to_index->old_file.path;
        const char *how = "?";
        unsigned st = se->status;
        if      (st & (GIT_STATUS_INDEX_NEW))                       how = "A";
        else if (st & (GIT_STATUS_INDEX_MODIFIED))                  how = "M";
        else if (st & (GIT_STATUS_INDEX_DELETED))                   how = "D";
        else if (st & (GIT_STATUS_WT_MODIFIED))                     how = " M";
        else if (st & (GIT_STATUS_WT_DELETED))                      how = " D";
        else if (st & (GIT_STATUS_WT_NEW))                          how = "??";
        printf(" %s %s\n", how, path);
        shown++;
    }
    if (!shown) printf("nothing to commit, working tree clean\n");
    git_status_list_free(sl);
    git_repository_free(repo);
    return 0;
}

/* Serve an ordinary directory: no session, no records, just git.
 *
 * The engine ops used here are disk-side ones — init, adddisk, commit, log —
 * which touch no record primitives.  That set was established by RUNNING them
 * against the recordless backend, not by reading: `status` was in the list
 * until it aborted with "mv_open was called", because it compares live records
 * against HEAD.  It is served by plain_status() instead, and `diff` is left out
 * for the same reason.  Anything else is refused by name rather than attempted. */
static int run_plain(int argc, char **argv, int i) {
    /* Fold the subcommand to lower case.  A UniVerse sentence is
       case-insensitive and MV users type verbs in upper case — `GIT ADD` must
       reach the same place as `git add`, which a plain strcmp does not. */
    char subbuf[64];
    {
        const char *raw = argv[i++];
        size_t k = 0;
        for (; raw[k] && k < sizeof subbuf - 1; k++)
            subbuf[k] = (char)tolower((unsigned char)raw[k]);
        subbuf[k] = '\0';
    }
    const char *sub = subbuf;
    const char *a0 = (i     < argc) ? argv[i]     : "";
    const char *a1 = (i + 1 < argc) ? argv[i + 1] : "";
    mv_ctx *ctx = mv_ctx_create();
    char *out = NULL;

    /* Find the repository the way git does: search upward, so running in a
       subdirectory works on the enclosing repo rather than failing to find one.
       Falls back to ".git" here, which is what `init` needs — there is nothing
       to discover before it exists. */
    char found[4096], repobuf[4096];
    const char *repo = ".git";
    if (enclosing_repo(found, sizeof found)) {
        snprintf(repobuf, sizeof repobuf, "%s", found);
        repo = repobuf;
    }

    if      (!strcmp(sub, "init"))    out = mv_git_init(ctx, repo);
    else if (!strcmp(sub, "status"))  { mv_ctx_destroy(ctx); return plain_status(); }
    else if (!strcmp(sub, "log"))     out = mv_git_log(ctx, repo, *a0 ? a0 : "20");
    else if (!strcmp(sub, "branch"))  out = mv_git_branch(ctx, repo, a0);
    else if (!strcmp(sub, "config"))  out = mv_git_config(ctx, repo, a0, a1);
    else if (!strcmp(sub, "add"))     out = mv_git_adddisk(ctx, repo);
    else if (!strcmp(sub, "commit")) {
        /* -m <msg>, as git spells it */
        const char *msg = a0;
        if (!strcmp(a0, "-m")) msg = a1;
        out = mv_git_commit(ctx, repo, msg);
    } else {
        fprintf(stderr,
            "uv-git: '%s' needs a UniVerse account, and this is an ordinary "
            "directory.\n"
            "        Available here: init, add, commit, status, log, branch, "
            "config.\n", sub);
        mv_ctx_destroy(ctx);
        return 2;
    }

    if (out) {
        /* engine output is @AM-separated; render it as lines */
        for (char *p = out; *p; p++) if ((unsigned char)*p == 0xFE) *p = '\n';
        printf("%s\n", out);
        free(out);
    }
    mv_ctx_destroy(ctx);
    return 0;
}

/* Serve a UniVerse ACCOUNT: the engine, in process, with records reached through
 * the session agent (mv_git#47).
 *
 * This is the same dispatch run_plain does, with the record-based operations
 * added — because with uvgit_rt.c linked they now work.  There is no verb here
 * and no BASIC git code: uv-git owns the git objects, and the session is asked
 * only for records.
 *
 * The session is opened lazily by the backend on first record use, so an
 * operation that touches no records costs no licence.
 */
static int run_account(int argc, char **argv, int i) {
    /* Fold the subcommand to lower case.  A UniVerse sentence is
       case-insensitive and MV users type verbs in upper case — `GIT ADD` must
       reach the same place as `git add`, which a plain strcmp does not. */
    char subbuf[64];
    {
        const char *raw = argv[i++];
        size_t k = 0;
        for (; raw[k] && k < sizeof subbuf - 1; k++)
            subbuf[k] = (char)tolower((unsigned char)raw[k]);
        subbuf[k] = '\0';
    }
    const char *sub = subbuf;
    const char *a0 = (i     < argc) ? argv[i]     : "";
    const char *a1 = (i + 1 < argc) ? argv[i + 1] : "";
    const char *a2 = (i + 2 < argc) ? argv[i + 2] : "";
    mv_ctx *ctx = mv_ctx_create();
    char *out = NULL;

    /* Find the repository the way git does — search upward — so an account that
       sits inside a larger repository works on the enclosing one (mv_git#39). */
    char found[4096], repobuf[4096];
    const char *repo = ".git";
    if (enclosing_repo(found, sizeof found)) {
        snprintf(repobuf, sizeof repobuf, "%s", found);
        repo = repobuf;
    }
    /* The open account format is an opt-in the engine reads from the environment;
       seed it from the repository's config so add and status agree with what the
       descriptor claims. */
    {
        git_libgit2_init();
        git_repository *gr = NULL;
        if (git_repository_open_ext(&gr, ".", 0, NULL) == 0) {
            git_config *cfg = NULL;
            int on = 0;
            if (git_repository_config(&cfg, gr) == 0) {
                if (git_config_get_bool(&on, cfg, "mvx.openaccount") == 0 && on)
                    setenv("MVX_OPENACCOUNT", "1", 1);
                git_config_free(cfg);
            }
            git_repository_free(gr);
        }
    }

    if      (!strcmp(sub, "init"))     out = mv_git_init(ctx, repo);
    else if (!strcmp(sub, "status"))   out = mv_git_status(ctx, repo);
    else if (!strcmp(sub, "log"))      out = mv_git_log(ctx, repo, *a0 ? a0 : "20");
    else if (!strcmp(sub, "branch"))   out = mv_git_branch(ctx, repo, a0);
    else if (!strcmp(sub, "config"))   out = mv_git_config(ctx, repo, a0, a1);
    else if (!strcmp(sub, "diff"))     out = mv_git_diff(ctx, repo, a0);
    else if (!strcmp(sub, "checkout")) out = mv_git_checkout(ctx, repo, a0);
    else if (!strcmp(sub, "switch"))   out = mv_git_switch(ctx, repo, a0);
    else if (!strcmp(sub, "merge"))    out = mv_git_merge(ctx, repo, a0);
    else if (!strcmp(sub, "restore"))  out = mv_git_restore(ctx, repo, a0);
    else if (!strcmp(sub, "rm"))       out = mv_git_rm(ctx, repo, a0, a1);
    else if (!strcmp(sub, "show"))     out = mv_git_show(ctx, repo, a0, a1);
    else if (!strcmp(sub, "tag"))
        out = mv_git_tag(ctx, repo, a0, a1, a2,
                         (i + 3 < argc) ? argv[i + 3] : "");
    else if (!strcmp(sub, "remote"))   out = mv_git_remote(ctx, repo, a0, a1, a2);
    else if (!strcmp(sub, "fetch"))    out = mv_git_fetch(ctx, repo, a0);
    else if (!strcmp(sub, "push"))     out = mv_git_push(ctx, repo, a0, a1);
    else if (!strcmp(sub, "pull"))     out = mv_git_pull(ctx, repo, a0, a1);
    else if (!strcmp(sub, "cherry-pick"))
        out = mv_git_cherrypick(ctx, repo, a0);
    else if (!strcmp(sub, "add")) {
        /* `add -A` (or . / --all) stages the whole account: every local file's
           records and dictionary.  `add <file>` stages one file. */
        if (!*a0 || !strcmp(a0, "-A") || !strcmp(a0, ".") || !strcmp(a0, "--all"))
            out = mv_git_addall(ctx, repo);
        else
            out = mv_git_add(ctx, repo, a0, a1);
    } else if (!strcmp(sub, "commit")) {
        const char *msg = a0;
        if (!strcmp(a0, "-m")) msg = a1;
        out = mv_git_commit(ctx, repo, msg);
    } else {
        fprintf(stderr, "uv-git: unknown command '%s'\n", sub);
        mv_ctx_destroy(ctx);
        return 2;
    }

    if (out) {
        /* engine output is @AM-separated; render it as lines */
        for (char *p = out; *p; p++) if ((unsigned char)*p == 0xFE) *p = '\n';
        printf("%s\n", out);
        free(out);
    }
    mv_ctx_destroy(ctx);
    /* Hand the licence back now rather than at exit, so a long-running caller
       does not keep one for work it has finished. */
    uvs_close_all();
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    const char *account = ".";

    /* Global switches, before the subcommand.  Both are about licences: how many
       sessions this run may hold at once, and how long an unused one waits before
       giving its licence back. */
    while (i < argc) {
        if (!strcmp(argv[i], "-a") && i + 1 < argc) {
            account = argv[i + 1];
            i += 2;
        } else if ((!strcmp(argv[i], "-j") || !strcmp(argv[i], "--jobs")) &&
                   i + 1 < argc) {
            uvs_set_jobs(atoi(argv[i + 1]));
            i += 2;
        } else if (!strcmp(argv[i], "--idle-timeout") && i + 1 < argc) {
            uvs_set_idle(atoi(argv[i + 1]));
            i += 2;
        } else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: uv-git [-a account] [-j jobs] "
                        "[--idle-timeout secs] <command> [args]\n");
        return 2;
    }

    /* textconv is a git diff filter — it renders a record blob legibly for the
       diff view and touches no records at all, so it runs here rather than
       paying for a session. */
    if (strcmp(argv[i], "textconv") == 0)
        return mv_git_textconv(i + 1 < argc ? argv[i + 1] : "-");

    /* `agent` speaks to the account I/O agent directly (mv_git#47): one opcode,
       its arguments, the reply on stdout.  It is the diagnostic for the session
       layer — when a record operation misbehaves, this says whether the session,
       the protocol or the caller is at fault, without a repository in the way.
         uv-git [-a acct] agent <OPCODE> [arg...] */
    if (strcmp(argv[i], "agent") == 0) {
        char err[512] = "";
        uv_session *ses = uvs_open(account, err, sizeof err);
        if (!ses) {
            fprintf(stderr, "uv-git agent: %s\n", err);
            return 1;
        }
        int rc = 0;
        if (i + 1 < argc) {
            const char *a[8];
            int na = 0;
            for (int k = i + 2; k < argc && na < 8; k++) a[na++] = argv[k];
            rc = agent_one(ses, argv[i + 1], na, a);
        } else {
            /* No opcode: read a sequence from stdin, one "OP arg arg…" per line,
               all on ONE session.  State lives in the session — a file handle
               from OPEN is only meaningful to later calls on the same one — so a
               probe that could not hold a session could never exercise the part
               that matters. */
            char line[8192];
            while (fgets(line, sizeof line, stdin)) {
                size_t n = strlen(line);
                while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
                if (!line[0] || line[0] == '#') continue;
                const char *a[8];
                int na = 0;
                char *save = NULL;
                char *op = strtok_r(line, " \t", &save);
                if (!op) continue;
                for (char *t; na < 8 && (t = strtok_r(NULL, " \t", &save)); )
                    a[na++] = t;
                printf("%s: ", op);
                fflush(stdout);
                if (agent_one(ses, op, na, a) != 0) rc = 1;
            }
        }
        uvs_close(ses);
        return rc;
    }

    /* adopt runs BEFORE the account test below, because the whole point is that
       this is not an account yet — a cloned tree has records on disk and no VOC. */
    if (strcmp(argv[i], "adopt") == 0) {
        if (chdir(account) != 0) {
            fprintf(stderr, "uv-git: cannot enter %s: %s\n",
                    account, strerror(errno));
            return 1;
        }
        return adopt(argc, argv, i);
    }

    if (chdir(account) != 0) {
        fprintf(stderr, "uv-git: cannot enter account %s: %s\n",
                account, strerror(errno));
        return 1;
    }

    /* A UniVerse account is identified by its VOC.  Without one this is an
       ordinary directory, and there is nothing a session could do for it — but
       there is plenty git can, and we are already linked against libgit2 for
       textconv.  So serve it directly rather than refusing: one binary, two
       paths, chosen by what the directory actually is.  That mirrors how
       mvx-git treats record files and plain files.
       Ops that would need records are simply not offered here; the recordless
       backend aborts loudly if one is ever reached, so a wrong assumption
       announces itself instead of quietly doing the wrong thing. */
    if (access("VOC", F_OK) != 0)
        return run_plain(argc, argv, i);

    /* An account inside a larger repository is fine: the engine finds the
       enclosing repository by walking up, exactly as git does, and records stage
       under the account's own prefix.  init is the one case worth stopping —
       it would create a SECOND repository nested in the first and split the
       history — so it is refused by name rather than by refusing the account. */
    if (!strcmp(argv[i], "init")) {
        char up[4096];
        if (enclosing_repo(up, sizeof up)) {
            fprintf(stderr,
                "uv-git: this account is inside the repository at\n"
                "          %s\n"
                "        Running init here would create a SECOND repository "
                "nested in that one,\n"
                "        splitting the history.  Run it at the repository root "
                "instead.\n", up);
            return 1;
        }
    }

    return run_account(argc, argv, i);
}
