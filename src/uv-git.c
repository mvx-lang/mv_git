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
 * agent_rt.c for the record contract and mvsession.c for the session lifecycle,
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
#define _DEFAULT_SOURCE          /* realpath */

#include "mvxgit.h"
#include "mvsession.h"
#include "agent_src.h"   /* generated from BP/GIT.AGENT by build-gitd.sh */

/* agent_rt.c: finish with the current account and give its licence back. */
void mv_agent_release(void);
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
typedef struct { char *d; size_t len, cap; int n; } names_t;

static void names_add(names_t *v, const char *s) {
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

static int names_has(const names_t *v, const char *s) {
    for (size_t i = 0; i < v->len; i += strlen(v->d + i) + 1)
        if (!strcmp(v->d + i, s)) return 1;
    return 0;
}

/* Snapshot the current directory's entries, skipping dotfiles — the descriptor
   and .git are ours, not UniVerse's, and must never be excluded. */
static void snapshot(names_t *v) {
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
static void exclude_new(const names_t *before, const names_t *now,
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

/* Defined below: a LIVE account, as distinct from a checkout of one. */
static int is_live_account(const char *dir);

/* Create the UniVerse account in the current directory with `code` as the
   flavour answer.  A fresh directory becomes an account on its first `uv`,
   which asks whether to set it up and then which flavour to use. */
static int make_account(const char *code) {
    /* KEEP WHAT uv SAID.  This discarded its output, and then reported only
       "the account was not created (no VOC); is `uv` on PATH?" — a guess, and
       a wrong one when uv is plainly on PATH.  It fails intermittently (roughly
       one clone in three observed), and with the output thrown away there was
       nothing to tell one cause from another, so it got a retry instead of a
       diagnosis.  The reason is in what uv prints. */
    char out[512];
    snprintf(out, sizeof out, "%s", ".uvsetup.log");
    char cmd[512];
    snprintf(cmd, sizeof cmd, "printf 'Y\\n%s\\nQUIT\\n' | %s > %s 2>&1",
             code, UV_BIN, out);
    int rc = system(cmd);
    (void)rc;
    if (is_live_account(".")) { unlink(out); return 0; }
    /* Failed — say what happened, in uv's own words. */
    FILE *f = fopen(out, "r");
    if (f) {
        char line[512], last[512] = "";
        int shown = 0;
        fprintf(stderr, "uv-git: uv did not create the account here. It said:\n");
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
            if (!n) continue;
            snprintf(last, sizeof last, "%s", line);
            if (shown++ < 12) fprintf(stderr, "        %s\n", line);
        }
        if (shown > 12) fprintf(stderr, "        ... (%d more) last: %s\n",
                                shown - 12, last);
        fclose(f);
    } else {
        fprintf(stderr, "uv-git: uv produced no output at all while creating "
                        "the account — it may not have run.\n");
    }
    return -1;
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

    int already = is_live_account(".");

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
        names_t before, after;
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
 * added — because with agent_rt.c linked they now work.  There is no verb here
 * and no BASIC git code: uv-git owns the git objects, and the session is asked
 * only for records.
 *
 * The session is opened lazily by the backend on first record use, so an
 * operation that touches no records costs no licence.
 */
/* Defined with the stock-baseline helpers below. */
static void apply_stock(const char *gitdir);

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

    /* Subtract this flavour's stock VOC, so a commit carries what the account
       added rather than what UniVerse supplied (mv_git#46).  Resolved per
       account: accounts in one repository may differ in flavour. */
    {
        char gd[4096], pfx[4096];
        if (repo_place(gd, sizeof gd, pfx, sizeof pfx) == 0) apply_stock(gd);
    }

    if      (!strcmp(sub, "init")) {
        out = mv_git_init(ctx, repo);
        /* An account that is going to be version-controlled should SAY it is an
           account.  Without a descriptor the commit carries no statement of what
           it is, and a clone has to infer it from the presence of a VOC and then
           ask for a flavour it could have been told (mv_git#52).
           The flavour is left out when nothing supplies one rather than guessed:
           an empty field is a question a clone can ask, a wrong one is an account
           that looks right and behaves differently. */
        if (!find_descriptor()) {
            char nm[256] = "account", cwd[4096];
            if (getcwd(cwd, sizeof cwd)) {
                const char *b = strrchr(cwd, '/');
                snprintf(nm, sizeof nm, "%s", b ? b + 1 : cwd);
            }
            const char *fl = NULL;
            for (int k = i; k < argc; k++) {
                if (!strncmp(argv[k], "--flavour=", 10))    fl = argv[k] + 10;
                else if (!strncmp(argv[k], "--flavor=", 9)) fl = argv[k] + 9;
            }
            int fi = fl ? flavour_index(fl) : -1;
            FILE *f = fopen(".uv", "wb");
            if (f) {
                fprintf(f, "# UV account descriptor\nname = %s\nversion = 1\n", nm);
                if (fi >= 0) fprintf(f, "flavour = %s\n", uv_flavours[fi].name);
                fclose(f);
                fprintf(stderr, "uv-git: wrote .uv%s\n", fi >= 0 ? "" :
                        " — it names no flavour; add `flavour = <name>` or pass "
                        "--flavour so a clone need not ask");
            }
        }
    }
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
    else if (!strcmp(sub, "tag")) {
        /* git's own spelling, which is what a user types and what the BASIC
           handler used to decode before handing the engine an op:
             GIT TAG                      list
             GIT TAG name {commit}        lightweight tag, HEAD if no commit
             GIT TAG -a name -m message   annotated
             GIT TAG -d name              delete                                */
        const char *op = "list", *name = "", *target = "", *msg = "";
        for (int k = i; k < argc; k++) {
            if (!strcmp(argv[k], "-d") && k + 1 < argc) {
                op = "delete"; name = argv[++k];
            } else if (!strcmp(argv[k], "-a") && k + 1 < argc) {
                op = "add";    name = argv[++k];
            } else if (!strcmp(argv[k], "-m") && k + 1 < argc) {
                msg = argv[++k];
            } else if (argv[k][0] != '-') {
                if (!*name)        { op = "add"; name = argv[k]; }
                else if (!*target) target = argv[k];
            }
        }
        if (strcmp(op, "list") != 0 && !*name) {
            fprintf(stderr, "usage: uv-git tag {name {commit} | -a name "
                            "-m msg | -d name}\n");
            mv_ctx_destroy(ctx);
            return 2;
        }
        out = mv_git_tag(ctx, repo, op, name, target, msg);
    }
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
    } else if (!strcmp(sub, "attr")) {
        /* ATTR IS AN IN-SESSION VERB, and deliberately has no twin here.  Every
           other command in this file is the engine reached directly from C;
           GIT ATTR is BASIC — registry, validation, staging and a full-screen
           editor — and verbs are BASIC rather than C on purpose, so writing a
           second implementation to reach it from the shell is the one thing
           that must not happen.  Say where it lives instead of "unknown
           command", which reads as "this does not exist". */
        fprintf(stderr,
            "uv-git: 'attr' is an in-session verb, not a shell command.\n"
            "        Run it inside a UniVerse session in this account:\n"
            "            GIT ATTR                       account attributes\n"
            "            GIT ATTR <file>                a file's parameters\n"
            "            GIT ATTR <file> --set k=v      change one\n");
        mv_ctx_destroy(ctx);
        return 2;
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
    /* Hand the licence back now rather than at exit, so a caller does not keep
       one for work it has finished.  Goes through the backend so its cached
       session pointer is cleared with it — mvs_close_all() alone would free the
       session and leave the backend holding a dangling pointer. */
    mv_agent_release();
    return 0;
}

/* --- the stock account baseline (mv_git#46) -------------------------------
 *
 * A UniVerse account is born with a VOC full of records nobody wrote — 847 of
 * them for PICK — and committing them buries the account's real content in
 * furniture.  The engine subtracts them if it is handed a baseline; building
 * that baseline is this side's job, because it needs a session.
 *
 * The method is the obvious one and it is exact: create a throwaway account of
 * the same flavour, read its VOC, and record each record's id and blob oid.
 * Whatever a fresh account of that flavour holds is by definition stock.
 *
 * Cached per clone under .git/mvgit/stock-<FLAVOUR>, and per FLAVOUR rather
 * than per account because that is what actually determines the answer — two
 * PICK accounts in one repository share a baseline.  Not committed: a stock VOC
 * belongs to a UniVerse release, and a committed one would subtract 14.2's
 * furniture from a 14.3 account.
 */

/* Write out the VOC of a fresh account of `flavour` as `<oid> <id>` lines.
   Returns 0 on success. */
/* Put a working agent into the account we are standing in: a BP file to hold it,
   the source copied from the account that HAS one, and a compile.  A stock
   account is stock precisely because nothing has been installed into it, so it
   cannot answer until this is done — and seeding it is cheaper and far more
   honest than trying to read a VOC without a session. */
/* Put the in-session GIT verb into the account we are standing in.
 *
 * UniVerse has NO GLOBAL CATALOG, so every account compiles and catalogs its
 * own copy of the verb and each handler it dispatches to.  A cloned account had
 * none, so the first thing a developer typed there failed with "Unable to open
 * the operating system file BP.O/GIT" — and the only remedy was running the
 * installer by hand, at a shell, which is the one place the MV developer is not
 * (mv_git#56).
 *
 * The recipe is NOT duplicated here.  Standing an account's verb up has real
 * subtleties — CREATE.FILE's seven prompts, the empty file DESCRIPTION that
 * keeps the VOC type readable as plain "F", the trailing newline UniVerse's
 * compiler insists on — all of which install.sh already gets right and
 * documents.  A second copy in C would drift from it.  So install.sh is staged
 * at install time and re-run here with --verb-only.
 */
static void deploy_git_verb(void) {
    const char *share = getenv("MVGIT_SHARE");
    char base[4096], script[4200];
    snprintf(base, sizeof base, "%s", share && share[0] ? share
                                                        : "/usr/local/share/mvgit");
    snprintf(script, sizeof script, "%s/install.sh", base);
    if (access(script, R_OK) != 0) {
        fprintf(stderr,
                "uv-git: no staged verb sources at %s, so this account cannot run\n"
                "        the GIT verb yet — run install.sh inside it, or set\n"
                "        MVGIT_SHARE to the staged package.\n", base);
        return;
    }
    char cmd[8500];
    snprintf(cmd, sizeof cmd, "sh '%s' --verb-only >/dev/null 2>&1", script);
    if (system(cmd) == 0 && access("BP.O/GIT", F_OK) == 0)
        printf("uv-git: in-session GIT verb set up in this account "
               "(try: GIT STATUS)\n");
    else
        fprintf(stderr, "uv-git: could not set up the in-session GIT verb here; "
                        "run install.sh in this account to see why\n");
}

static int build_stock(const char *flavour, const char *code, const char *out) {
    char tmpl[] = "/tmp/uvstockXXXXXX";
    if (!mkdtemp(tmpl)) return -1;
    int rc = -1;
    char here[4096];
    if (!getcwd(here, sizeof here)) { rmdir(tmpl); return -1; }
    fprintf(stderr, "uv-git: learning what a stock %s account holds "
                    "(once per clone)\n", flavour);
    int seeded = chdir(tmpl) == 0 && make_account(code) == 0 &&
                 mv_agent_seed() == 0;
    if (!seeded)
        fprintf(stderr, "uv-git: could not stand up a stock %s account to learn "
                        "from; commits will carry the system's own VOC records "
                        "(mv_git#46)\n", flavour);
    if (seeded) {
        char err[512] = "";
        mv_session *s = mvs_open(".", err, sizeof err);
        if (s) {
            const char *a[1] = { "VOC" };
            char *body = NULL;
            long blen = 0;
            if (mvs_calls(s, "OPEN", 1, a, &body, &blen) == 0) {
                free(body);
                const char *h[1] = { "1" };
                mvs_calls(s, "SELECT", 1, h, NULL, NULL);
                FILE *f = fopen(out, "w");
                if (f) {
                    fprintf(f, "# stock VOC for a %s account — generated here, "
                               "not committed:\n"
                               "# the contents belong to this UniVerse release "
                               "(mv_git#46).\n", flavour);
                    long n = 0;
                    for (;;) {
                        char *idb = NULL;
                        long idl = 0;
                        if (mvs_calls(s, "READNEXT", 0, NULL, &idb, &idl) != 0 ||
                            idl <= 0) { free(idb); break; }
                        /* Seeding had to CREATE.FILE BP and compile into
                           BP.O, so those two VOC entries are OURS, not the
                           stock account's.  Recording them would subtract a
                           real account's own BP pointer, and a clone would then
                           not recreate the file.  The account is stock in every
                           other respect; these are the only two we added. */
                        if ((idl == 2 && memcmp(idb, "BP", 2) == 0) ||
                            (idl == 4 && memcmp(idb, "BP.O", 4) == 0)) {
                            free(idb);
                            continue;
                        }
                        const char *r[2] = { "1", idb };
                        char *rec = NULL;
                        long rl = 0;
                        if (mvs_calls(s, "READ", 2, r, &rec, &rl) == 0) {
                            git_oid oid;
                            char hex[41];
                            /* the engine hashes a record's TRANSLATED content;
                               match that exactly or nothing ever compares equal */
                            char *t = malloc((size_t)rl + 1);
                            if (t) {
                                for (long k = 0; k < rl; k++)
                                    t[k] = ((unsigned char)rec[k] == 0xFE) ? '\n' : rec[k];
                                if (git_odb_hash(&oid, t, (size_t)rl,
                                                 GIT_OBJECT_BLOB) == 0) {
                                    git_oid_fmt(hex, &oid);
                                    hex[40] = '\0';
                                    fprintf(f, "%s %.*s\n", hex, (int)idl, idb);
                                    n++;
                                }
                                free(t);
                            }
                        }
                        free(rec);
                        free(idb);
                    }
                    fclose(f);
                    fprintf(stderr, "uv-git: %ld stock record(s) recorded\n", n);
                    rc = 0;
                }
            }
            mvs_close(s);
        } else {
            fprintf(stderr, "uv-git: could not read a stock account: %s\n", err);
        }
    }
    if (chdir(here) != 0) { /* best effort */ }
    /* the throwaway account is a directory of files; leave no litter */
    char rmcmd[4200];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf '%s'", tmpl);
    if (system(rmcmd) != 0) { /* best effort */ }
    return rc;
}

/* Point the engine at this account's stock baseline, building it if this clone
   has not seen this flavour yet.  Silently does nothing when the descriptor
   names no flavour — there is then nothing to be sure of, and guessing one
   would subtract the wrong furniture. */
static void apply_stock(const char *gitdir) {
    const char *desc = find_descriptor();
    if (!desc) return;
    char src[65536];
    size_t srclen = 0;
    FILE *df = fopen(desc, "rb");
    if (!df) return;
    srclen = fread(src, 1, sizeof src, df);
    fclose(df);
    char flav[64];
    if (!mv_git_desc_field(src, srclen, "flavour", flav, sizeof flav)) return;
    int fi = flavour_index(flav);
    if (fi < 0) return;

    char dir[4096], path[4200];
    snprintf(dir, sizeof dir, "%s/mvgit", gitdir);
    mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/stock-%s", dir, uv_flavours[fi].name);
    if (access(path, R_OK) != 0 &&
        build_stock(uv_flavours[fi].name, uv_flavours[fi].code, path) != 0)
        return;
    mv_git_set_stock(path);
}

/* --- several accounts in one repository (mv_git#44, #47) -------------------
 *
 * A repository may hold more than one account — the mvx repository already does
 * — and then a record cannot commit at `<file>/<id>`, because two accounts would
 * both claim `CUST/C1`.  Each account's records live under its own directory,
 * and uv-git visits the accounts in turn.
 *
 * ONE AT A TIME IS THE DEFAULT, and that is about licences rather than tidiness.
 * A session is a licence; visiting four accounts must not mean holding four.  So
 * each account is entered, worked, and its session released before the next is
 * opened — log in, do the work, log out.  -j raises the cap for someone who has
 * the licences and wants the wall clock; the cap is enforced in mvsession.c by
 * retiring the oldest session rather than exceeding it.
 */

/* Is this directory a LIVE UniVerse account?
 *
 * The VOC is what makes it one — but "a VOC exists" is not the test, and getting
 * that wrong is worse than it sounds.  A native account's records are committed
 * at `VOC/<id>`, so a plain `git clone` of one writes a DIRECTORY called VOC
 * full of record files.  That checkout is precisely what adopt exists to turn
 * into an account, and a bare existence check reads it as one already — so
 * adopt does nothing, and the user is left with a directory of files that no
 * session can open.
 *
 * A live account's VOC is a hash file: a regular file, with a D_VOC dictionary
 * beside it.  A checkout's is a directory.  That is the distinction. */
static int is_live_account(const char *dir) {
    char p[4096];
    struct stat sb;
    snprintf(p, sizeof p, "%s/VOC", dir);
    return stat(p, &sb) == 0 && S_ISREG(sb.st_mode);
}

static int dir_is_account(const char *dir) { return is_live_account(dir); }

/* The accounts directly beneath `root`, sorted so a run is reproducible.
   Returns the count; names are malloc'd into `out` (caller frees). */
static int find_accounts(const char *root, char ***out) {
    char **v = NULL;
    int n = 0, cap = 0;
    DIR *d = opendir(root);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[4096];
        snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        struct stat sb;
        if (stat(p, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
        if (!dir_is_account(p)) continue;
        if (n >= cap) {
            cap = cap ? cap * 2 : 8;
            v = realloc(v, (size_t)cap * sizeof *v);
            if (!v) { perror("uv-git"); exit(1); }
        }
        v[n++] = strdup(e->d_name);
    }
    if (d) closedir(d);
    for (int i = 1; i < n; i++)            /* insertion sort: a handful of names */
        for (int k = i; k > 0 && strcmp(v[k - 1], v[k]) > 0; k--) {
            char *t = v[k - 1]; v[k - 1] = v[k]; v[k] = t;
        }
    *out = v;
    return n;
}

/* Does this operation need to look at records?  Only those have to visit the
   accounts; the rest are repository-level and run once, where they are. */
static int needs_records(const char *sub) {
    static const char *rec[] = { "add", "status", "checkout", "switch",
                                 "restore", "merge", "cherry-pick", "rm",
                                 "diff", NULL };
    for (int i = 0; rec[i]; i++) if (!strcmp(sub, rec[i])) return 1;
    return 0;
}

/* Run a record operation across every account in the repository, one session at
   a time.  `root` is the repository working tree; we are standing in it. */
static int run_accounts(const char *root, int argc, char **argv, int i) {
    char **acct = NULL;
    int n = find_accounts(root, &acct);
    if (n == 0) {
        free(acct);
        return -1;                          /* nothing to walk; caller decides */
    }
    char here[4096];
    if (!getcwd(here, sizeof here)) { perror("uv-git"); return 1; }
    int rc = 0;

    /* The repository's own top-level files — README, docs, whatever sits beside
       the accounts — belong to the repository, not to any account, so they are
       staged once from here.  Each account then stages only what is under its
       own directory. */
    if (!strcmp(argv[i], "add")) {
        mv_git_set_prefix("");
        mv_ctx *rctx = mv_ctx_create();
        char *o = mv_git_adddisk(rctx, ".git");
        free(o);
        mv_ctx_destroy(rctx);
    }
    for (int k = 0; k < n; k++) {
        if (n > 1) printf("== %s\n", acct[k]);
        if (chdir(acct[k]) != 0) {
            fprintf(stderr, "uv-git: cannot enter %s: %s\n",
                    acct[k], strerror(errno));
            rc = 1;
            continue;
        }
        /* The prefix is what keeps two accounts' records apart in one
           repository; setting it also forgets what was cached about the last
           account. */
        mv_git_set_prefix(acct[k]);
        if (run_account(argc, argv, i) != 0) rc = 1;
        /* Hand the licence back before entering the next account. */
        mv_agent_release();
        if (chdir(here) != 0) { perror("uv-git"); return 1; }
    }
    mv_git_set_prefix("");
    for (int k = 0; k < n; k++) free(acct[k]);
    free(acct);
    return rc;
}

/* --- clone (mv_git#52) -----------------------------------------------------
 *
 * Cloning a UniVerse account is not "git clone, then fix it up", and trying it
 * that way does not merely look untidy — it cannot work.  A native account's
 * records commit at `VOC/<id>`, so an ordinary checkout writes a DIRECTORY
 * called VOC full of record files, and UniVerse then cannot create its own VOC
 * hash file because that name is taken.  The account can never be created, so
 * the records can never be loaded.  The intermediate form blocks the thing it
 * was supposed to become.
 *
 * So the checkout never happens.  Clone with --no-checkout, read what is needed
 * out of the commit, build the account, and materialise the records straight
 * from the git objects into it.  mvx-git already does exactly this, for the same
 * reason: the open form never lands on disk.
 *
 * The flavour comes from the committed descriptor, so unlike `adopt` this
 * usually needs to ask nothing — the repository already says what kind of
 * account it is.  When it does not say, we ask exactly as adopt does, because
 * guessing produces an account that looks right and behaves differently. */

/* A blob from HEAD, malloc'd, or NULL.  The working tree is empty at this point
   — that is the whole point — so anything we need must come from the objects. */
static char *head_blob(const char *gitdir, const char *path, size_t *len) {
    git_libgit2_init();
    git_repository *repo = NULL;
    char *out = NULL;
    if (git_repository_open(&repo, gitdir) != 0) return NULL;
    git_object *tree = NULL;
    if (git_revparse_single(&tree, repo, "HEAD^{tree}") == 0) {
        git_tree_entry *te = NULL;
        if (git_tree_entry_bypath(&te, (git_tree *)tree, path) == 0) {
            git_blob *b = NULL;
            if (git_blob_lookup(&b, repo, git_tree_entry_id(te)) == 0) {
                size_t n = (size_t)git_blob_rawsize(b);
                out = malloc(n + 1);
                if (out) {
                    memcpy(out, git_blob_rawcontent(b), n);
                    out[n] = '\0';
                    if (len) *len = n;
                }
                git_blob_free(b);
            }
            git_tree_entry_free(te);
        }
        git_object_free(tree);
    }
    git_repository_free(repo);
    return out;
}

static int clone_cmd(int argc, char **argv, int i) {
    const char *url = (i + 1 < argc) ? argv[i + 1] : NULL;
    const char *dir = (i + 2 < argc) ? argv[i + 2] : NULL;
    const char *want_flavour = NULL;
    for (int k = i + 1; k < argc; k++) {
        if (!strncmp(argv[k], "--flavour=", 10))    want_flavour = argv[k] + 10;
        else if (!strncmp(argv[k], "--flavor=", 9)) want_flavour = argv[k] + 9;
    }
    if (!url) {
        fprintf(stderr, "usage: uv-git clone <url> [directory]\n");
        return 2;
    }
    char dbuf[4096];
    if (!dir) {
        /* git's own rule: the last path component, without a trailing .git */
        const char *base = strrchr(url, '/');
        base = base ? base + 1 : url;
        snprintf(dbuf, sizeof dbuf, "%s", base);
        size_t n = strlen(dbuf);
        if (n > 4 && !strcmp(dbuf + n - 4, ".git")) dbuf[n - 4] = '\0';
        dir = dbuf;
    }

    /* --no-checkout: the records go into the account, never onto the disk. */
    {
        char cmd[8500];
        snprintf(cmd, sizeof cmd, "git clone --no-checkout '%s' '%s'", url, dir);
        if (system(cmd) != 0) {
            fprintf(stderr, "uv-git clone: git clone failed\n");
            return 1;
        }
    }
    if (chdir(dir) != 0) {
        fprintf(stderr, "uv-git clone: cannot enter %s: %s\n", dir, strerror(errno));
        return 1;
    }

    /* An UNBORN HEAD is not an error to give up on.  A remote whose default
       branch differs from the one that actually carries the history — a bare
       repo created with `master` as its default and then pushed `main` — leaves
       the clone pointing at a branch that does not exist, and everything after
       this reads as "there is nothing here".  git itself only warns.  Adopt a
       branch that DOES exist, preferring the conventional names, and move HEAD
       to it WITHOUT checking anything out: the records go into the account, and
       the working tree must stay empty for the account to be creatable at all. */
    if (system("git rev-parse --verify -q HEAD >/dev/null 2>&1") != 0) {
        static const char *pref[] = { "main", "master", NULL };
        char pick[256] = "";
        for (int k = 0; pref[k] && !pick[0]; k++) {
            char c[256];
            snprintf(c, sizeof c,
                     "git show-ref --verify -q refs/remotes/origin/%s", pref[k]);
            if (system(c) == 0) snprintf(pick, sizeof pick, "%s", pref[k]);
        }
        if (!pick[0]) {
            /* whatever the remote does have */
            FILE *pp = popen("git for-each-ref --count=1 "
                             "--format='%(refname:strip=3)' refs/remotes/origin/", "r");
            if (pp) {
                if (fgets(pick, sizeof pick, pp)) {
                    size_t n = strlen(pick);
                    while (n && (pick[n-1] == '\n' || pick[n-1] == '\r')) pick[--n] = '\0';
                }
                pclose(pp);
            }
        }
        if (pick[0] && strcmp(pick, "HEAD") != 0) {
            char c[600];
            fprintf(stderr, "uv-git clone: the remote's HEAD names a branch that "
                            "does not exist; using '%s'\n", pick);
            snprintf(c, sizeof c, "git branch -q -f '%s' 'origin/%s' 2>/dev/null",
                     pick, pick);
            if (system(c) != 0) { /* reported below if HEAD stays unborn */ }
            snprintf(c, sizeof c, "git symbolic-ref HEAD 'refs/heads/%s'", pick);
            if (system(c) != 0) { /* likewise */ }
        }
    }

    /* What does the commit say this account is? */
    static const char *names[] = { ".mv-account", ".uv", ".mvx", ".udt", NULL };
    char *desc = NULL;
    size_t dlen = 0;
    const char *dname = NULL;
    for (int k = 0; names[k] && !desc; k++) {
        desc = head_blob(".git", names[k], &dlen);
        if (desc) dname = names[k];
    }
    /* No descriptor does not settle it.  An account committed before anything
       wrote one still HAS a VOC in its history, and a VOC is the account's own
       statement that it is one — the same authority materialise consults.  Only
       the flavour is then unknown, and that must be supplied rather than
       guessed: an account built at the wrong flavour looks right and behaves
       differently. */
    if (!desc && head_blob(".git", "VOC", NULL) == NULL) {
        git_libgit2_init();
        git_repository *r = NULL;
        int has_voc = 0;
        if (git_repository_open(&r, ".git") == 0) {
            git_object *t = NULL;
            if (git_revparse_single(&t, r, "HEAD^{tree}") == 0) {
                git_tree_entry *te = NULL;
                if (git_tree_entry_bypath(&te, (git_tree *)t, "VOC") == 0) {
                    has_voc = git_tree_entry_type(te) == GIT_OBJECT_TREE;
                    git_tree_entry_free(te);
                }
                git_object_free(t);
            }
            git_repository_free(r);
        }
        if (has_voc && !want_flavour) {
            fprintf(stderr,
                "uv-git clone: this repository holds an account (its history has "
                "a VOC) but\n"
                "              does not say which VOC flavour it needs, and "
                "UniVerse cannot be\n"
                "              asked afterwards.  Re-run with --flavour=<name>, "
                "e.g. --flavour=PICK.\n");
            return 1;
        }
        if (has_voc) {
            /* A flavour was given, so proceed without a descriptor. */
            desc = strdup("");
            dlen = 0;
            dname = ".uv";
        }
    }

    if (!desc) {
        /* Not an MV account at all — an ordinary repository.  Check it out the
           ordinary way and leave it alone; refusing would make uv-git useless
           for the plain repositories that sit beside accounts. */
        char co[64];
        snprintf(co, sizeof co, "git checkout");
        if (system(co) != 0) { /* git reports its own failure */ }
        printf("cloned an ordinary repository (no MV account descriptor)\n");
        return 0;
    }

    int fi = -1;
    char have[64] = "";
    if (want_flavour) fi = flavour_index(want_flavour);
    else if (mv_git_desc_field(desc, dlen, "flavour", have, sizeof have))
        fi = flavour_index(have);
    if (fi < 0) {
        char line[64];
        fprintf(stderr,
            "This repository does not say which VOC flavour its account needs,\n"
            "and UniVerse cannot be asked afterwards: the flavour is fixed when\n"
            "the account is created and is not recorded anywhere readable.\n\n");
        for (int k = 0; k < NFLAVOURS; k++)
            fprintf(stderr, "    %-8s %s\n", uv_flavours[k].name,
                    uv_flavours[k].shown);
        fprintf(stderr, "\n");
        while (fi < 0) {
            if (!ask("Flavour for this account [PICK]: ", line, sizeof line)) {
                fprintf(stderr, "uv-git clone: no terminal to ask on — pass "
                                "--flavour=<name>.\n");
                free(desc);
                return 1;
            }
            if (!line[0]) { fi = flavour_index("PICK"); break; }
            fi = flavour_index(line);
            if (fi < 0) fprintf(stderr, "  not a flavour: %s\n", line);
        }
    }

    /* Build the account, and keep what UniVerse creates for it out of git. */
    fprintf(stderr, "uv-git clone: creating a UniVerse account (%s)\n",
            uv_flavours[fi].name);
    names_t before, after;
    snapshot(&before);
    if (make_account(uv_flavours[fi].code) != 0) {
        fprintf(stderr, "uv-git clone: the account was not created (no VOC); "
                        "is `%s` on PATH?\n", mvs_shell());
        free(desc);
        return 1;
    }
    snapshot(&after);
    {
        char gd[4096], pfx[4096];
        if (repo_place(gd, sizeof gd, pfx, sizeof pfx) == 0)
            exclude_new(&before, &after, gd, pfx);
    }
    free(before.d);
    free(after.d);

    /* The descriptor itself is content: write it out as the commit has it, so
       the working tree matches HEAD and status is clean. */
    {
        FILE *f = fopen(dname, "wb");
        if (f) { fwrite(desc, 1, dlen, f); fclose(f); }
    }
    free(desc);

    /* Nothing is installed yet, so nothing can answer — seed an agent first. */
    if (mv_agent_seed() != 0) {
        fprintf(stderr, "uv-git clone: could not put an agent into the new "
                        "account; the account exists but holds no records yet\n");
        return 1;
    }

    /* Records, straight from the git objects into the account's files. */
    mv_ctx *ctx = mv_ctx_create();
    char *out = mv_git_materialize(ctx, ".git");
    if (out) {
        for (char *p = out; *p; p++) if ((unsigned char)*p == 0xFE) *p = '\n';
        printf("%s\n", out);
        free(out);
    }
    mv_ctx_destroy(ctx);
    mv_agent_release();      /* the seeding session goes before the verb build */

    /* An account that cannot run GIT is not finished being cloned. */
    deploy_git_verb();

    /* RECORD THE FLAVOUR WE WERE TOLD, so the next clone need not be told again.
     *
     * A UniVerse account is created WITH a flavour and cannot be asked for it
     * afterwards (mv_git#15), so clone reads it from the committed descriptor
     * and, finding none, refuses to guess.  We were just told it — but the
     * descriptor materialised out of the commit still says nothing, so an
     * account cloned with --flavour committed a descriptor that would make the
     * NEXT clone ask all over again.  The knowledge died with the command that
     * had it.
     *
     * Writing it here means the next `add` stages a descriptor that carries the
     * flavour forward, and --flavour becomes a thing you supply once for a
     * lineage rather than every time.  adopt already does this; clone did not. */
    if (fi >= 0) {
        char have[64];
        char cur[4096];
        size_t curlen = 0;
        FILE *r = fopen(".uv", "rb");
        if (r) { curlen = fread(cur, 1, sizeof cur - 1, r); fclose(r); }
        cur[curlen] = '\0';
        if (!mv_git_desc_field(cur, curlen, "flavour", have, sizeof have)) {
            FILE *f = fopen(".uv", "ab");
            if (f) {
                if (curlen && cur[curlen - 1] != '\n') fputc('\n', f);
                fprintf(f, "flavour = %s\n", uv_flavours[fi].name);
                fclose(f);
                printf("uv-git: recorded flavour %s in .uv — commit it and no "
                       "later clone need be told\n", uv_flavours[fi].name);
            }
        }
    }

    printf("cloned into %s as a UniVerse account (%s flavour)\n",
           dir, uv_flavours[fi].name);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    const char *account = ".";

    /* This driver's shell.  The session layer is shared with UniData and has no
       default, so naming it here is what makes these sessions UniVerse ones. */
    mvs_set_shell("uv");

    /* Global switches, before the subcommand.  Both are about licences: how many
       sessions this run may hold at once, and how long an unused one waits before
       giving its licence back. */
    while (i < argc) {
        if (!strcmp(argv[i], "-a") && i + 1 < argc) {
            account = argv[i + 1];
            i += 2;
        } else if ((!strcmp(argv[i], "-j") || !strcmp(argv[i], "--jobs")) &&
                   i + 1 < argc) {
            mvs_set_jobs(atoi(argv[i + 1]));
            i += 2;
        } else if (!strcmp(argv[i], "--idle-timeout") && i + 1 < argc) {
            mvs_set_idle(atoi(argv[i + 1]));
            i += 2;
        } else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: uv-git [-a account] [-j jobs] "
                        "[--idle-timeout secs] <command> [args]\n");
        return 2;
    }

    /* Fold the subcommand ONCE, here, so every dispatch below agrees about it.
       A UniVerse sentence is case-insensitive and MV users type `GIT CLONE`;
       run_account folds its own copy, so leaving these checks case-sensitive
       meant an upper-case verb sailed past every one of them and arrived at
       run_account as an "unknown command" it had just lowercased itself. */
    {
        static char subfold[64];
        size_t k = 0;
        for (; argv[i][k] && k < sizeof subfold - 1; k++)
            subfold[k] = (char)tolower((unsigned char)argv[i][k]);
        subfold[k] = '\0';
        argv[i] = subfold;
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
    if (strcmp(argv[i], "agent") == 0)
        return mv_agent_cmd(argc, argv, i);

    /* adopt runs BEFORE the account test below, because the whole point is that
       this is not an account yet — a cloned tree has records on disk and no VOC. */
    /* clone runs before every account test: there is no account yet, and after
       this there will be one. */
    if (strcmp(argv[i], "clone") == 0) {
        if (chdir(account) != 0) {
            fprintf(stderr, "uv-git: cannot enter %s: %s\n",
                    account, strerror(errno));
            return 1;
        }
        return clone_cmd(argc, argv, i);
    }

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
    if (!is_live_account(".")) {
        /* Not an account.  It may still be a repository HOLDING accounts, and a
           record operation run at the root should visit each of them — which is
           how a multi-account repository is worked (mv_git#44). */
        char gd[4096], pfx[4096];
        if (needs_records(argv[i]) && repo_place(gd, sizeof gd, pfx, sizeof pfx) == 0) {
            char root[4096];
            if (getcwd(root, sizeof root)) {
                int rc = run_accounts(root, argc, argv, i);
                if (rc >= 0) return rc;      /* -1 = no accounts here */
            }
        }
        return run_plain(argc, argv, i);
    }

    /* An account.  Where it sits beneath the repository root decides the prefix
       its records commit under: "" at the root (every single-account package,
       unchanged), "acctA/" below one. */
    {
        char gd[4096], pfx[4096];
        if (repo_place(gd, sizeof gd, pfx, sizeof pfx) == 0)
            mv_git_set_prefix(pfx);
    }

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
