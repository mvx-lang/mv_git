#!/bin/sh
# mv_git for Rocket UniVerse — host installer.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
#
# Run from inside the unpacked package directory, which IS the account:
#
#   ./install.sh [-p prefix]      # prefix defaults to /usr/local
#
# It installs the two binaries, makes this directory a UniVerse account if it is
# not one already, writes the platform header, then compiles and catalogs the
# GIT verb here.
#
# Nothing is installed as a service.  mvgitd — the background process that runs
# libgit2 for a session that cannot (see INSTALL.txt) — is started on demand by
# the verb, runs as whoever started it, and exits when their session goes.
set -eu

PREFIX=/usr/local
VERBONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        -p|--prefix) PREFIX="$2"; shift 2 ;;
        # --verb-only: set up the in-session verb in the CURRENT account and
        # nothing else — no binaries, no account creation.  It exists so a
        # freshly cloned account can be made usable without a second copy of
        # this recipe living in C (mv_git#56).  The sources come from the
        # staged tree beside this script; the work happens in the caller's cwd.
        --verb-only) VERBONLY=1; shift ;;
        *) echo "usage: ./install.sh [-p prefix] [--verb-only]" >&2; exit 2 ;;
    esac
done

SRC="$(cd "$(dirname "$0")" && pwd)"
HERE="$SRC"
[ "$VERBONLY" = 1 ] && HERE="$PWD"
cd "$HERE"
say() { printf '  %s\n' "$*"; }

# ---- preflight --------------------------------------------------------------
echo "mv_git (UniVerse) installer"

command -v uv >/dev/null 2>&1 || {
    echo "install.sh: 'uv' is not on PATH — set up the UniVerse environment first" >&2
    exit 1; }

# The binaries link libgit2.  A missing one fails at run time with a loader
# error that says nothing about git, so it is worth catching here where the
# remedy can be named.
if ldd ./bin/mvgitd 2>/dev/null | grep -q 'not found'; then
    echo "install.sh: a shared library the binaries need is missing:" >&2
    ldd ./bin/mvgitd 2>/dev/null | grep 'not found' | sed 's/^/    /' >&2
    echo "    on EL8:  sudo dnf install epel-release && sudo dnf install libgit2_1.7" >&2
    exit 1
fi

# ---- binaries ---------------------------------------------------------------
if [ "$VERBONLY" = 0 ]; then
say "installing mvgitd + uv-git to $PREFIX/bin"
mkdir -p "$PREFIX/bin"
install -m 0755 bin/mvgitd  "$PREFIX/bin/mvgitd"
install -m 0755 bin/uv-git  "$PREFIX/bin/uv-git"

# Stage what a LATER clone will need to set itself up: the verb sources, the
# platform header and this script.  Without it a cloned account has no route to
# the verb at all, since UniVerse has no global catalog and every account must
# compile its own (mv_git#56).
say "staging the verb sources to $PREFIX/share/mvgit"
mkdir -p "$PREFIX/share/mvgit"
cp -r "$SRC/BP" "$PREFIX/share/mvgit/BP.staged" 2>/dev/null || \
  cp -r "$SRC/BP.staged" "$PREFIX/share/mvgit/BP.staged" 2>/dev/null || true
cp "$SRC/PLATFORM.H" "$PREFIX/share/mvgit/PLATFORM.H" 2>/dev/null || true
install -m 0755 "$SRC/install.sh" "$PREFIX/share/mvgit/install.sh"
fi

# ---- make this directory an account ----------------------------------------
# A fresh UniVerse directory becomes an account on its first `uv`, which asks to
# update the VOC's RELLEVEL and then for a flavour.  The packages target classic
# Pick, so the flavour is 3.  An existing account skips both prompts and the
# extra answers are harmless.
if [ "$VERBONLY" = 0 ] && [ ! -e VOC ]; then
    say "making this directory a UniVerse account (Pick flavour)"
    printf 'Y\n3\nQUIT\n' | uv >/dev/null 2>&1 || true
fi
[ -e VOC ] || { echo "install.sh: could not create the account (no VOC)" >&2; exit 1; }

# ---- the BASIC source and include files ------------------------------------
# CREATE.FILE asks SEVEN questions, not six: modulo, separation and file type
# for the DICTionary, the same three for the DATA part, and then a FILE
# DESCRIPTION.  That last one is the trap — UniVerse stores it in VOC attribute
# 1 as "F <description>", and code that compares attribute 1 to "F" (as the
# account scan does) then cannot see the file at all.  So the description is
# answered with an empty line, leaving a clean "F".
#   dict: modulo 1, separation 2, type 3 (hashed)
#   data: modulo 1, separation 2, type 19 (directory — it holds source items)
# CREATE.FILE'S REFUSAL IS THE ONLY THING THAT SAYS WHY, and this threw it away.
# When it declines -- "\"BP.O\" is already in your VOC file as a file definition
# record.  File not created." -- the caller saw nothing but a missing directory
# and reported "CREATE.FILE did not create BP.O", which names neither the reason
# nor the remedy.  Four CI runs were spent on that message (mv_git#226).
MKFILE_OUT=
mkfile() {
    MKFILE_OUT=$(printf 'CREATE.FILE %s\n1\n2\n3\n1\n2\n19\n\nQUIT\n' "$1" | uv 2>&1) || true
}

# BP arrives as a plain directory of sources, which is NOT yet a UniVerse file:
# it needs a VOC pointer and a dictionary, and `BASIC BP *` cannot see it
# without them — the failure is a silent "compiled 0 programs", not an error.
#
# Rather than hand-build the VOC record, let CREATE.FILE do it properly and move
# the sources in afterwards: it exists to create the pointer, the dictionary and
# the directory together, and refuses when the directory is already there.
# WHAT MAKES BP USABLE IS ITS VOC POINTER, not a dictionary file on disk — and
# those two come apart more often than they look like they should.  An account
# that received a copy of the package, or one materialised by a clone, has D_BP
# sitting right there with no VOC record naming it; this test used to be
# `[ ! -e D_BP ]`, so in exactly those accounts it decided BP was already
# registered, skipped this, and `BASIC BP *` then found nothing.  The failure is
# "compiled 0 program(s)" — no error, just a verb that never appears.
#
# So ask the VOC.  It is the thing the compiler consults.
#
# AND A QUESTION THAT COULD NOT BE ASKED IS NOT AN ANSWER OF "NO".  This piped a
# session and grepped what came back, so when no session could be had -- a
# two-seat licence and a build that opens a dozen in a row -- the empty output
# read as "not registered", and ensure_file went on to delete the directories of
# a file that WAS registered.  CREATE.FILE then refused to re-make it, because
# the VOC record it had never looked at was still there, and every retry took
# the same path: the account was destroyed by the recovery, not by the fault
# (mv_git#226).
#
# The echoed command is the proof the session ran.  Without it, return 2 -- not
# 0, and not 1 -- and let the caller refuse rather than guess.
voc_has() {
    _vh=$(printf 'CT VOC %s\nQUIT\n' "$1" | uv 2>&1)
    case "$_vh" in
        *"CT VOC $1"*) ;;
        *) return 2 ;;
    esac
    printf '%s\n' "$_vh" | grep -q '^0001[[:space:]]*F'
}
# Make $1 a real UniVerse file, whatever state the directory is in.  CREATE.FILE
# builds the pointer, the dictionary and the directory TOGETHER and refuses when
# any of the three is already there — so OS files with no pointer are debris in
# its way, and they go first.  $2, when given, is content to put back afterwards.
ensure_file() {
    # `|| _vr=$?`, not a bare call: this script runs under `set -e`, where a
    # command that is not part of an AND-OR list and returns non-zero ends the
    # script.  A bare `voc_has "$1"` therefore exited the installer the moment
    # the answer was "not registered" -- the ordinary case on a fresh account.
    _vr=0; voc_has "$1" || _vr=$?
    # REGISTERED IS NOT THE SAME AS USABLE, and this asked only the first half.
    # A clone carries the VOC record across without the directory behind it, and
    # a failed earlier attempt leaves exactly the same shape -- so the answer was
    # "already a file, nothing to do" for a name that names nothing.  The
    # compiler then had nowhere to write and said "compiled 0 program(s)", which
    # is not an error and mentions neither the file nor the VOC (mv_git#226).
    # Ask both, and treat a record with nothing behind it as work to do.
    if [ "$_vr" -eq 0 ] && [ -e "$1" ]; then return 0; fi
    # 2 = the session never ran the command.  Guessing "not registered" here is
    # what deletes a registered file's directories; stop instead.
    if [ "$_vr" -eq 2 ]; then
        echo "install.sh: could not ask VOC about $1 — no UniVerse session answered." >&2
        echo "            Refusing to guess: guessing 'not registered' deletes the file." >&2
        exit 1
    fi
    # SAY WHICH OF THE TWO THIS IS.  Reaching here means either the name is not
    # registered (the ordinary case on a fresh account) or it IS registered with
    # nothing behind it -- the split that wedged four CI runs (#226) and whose
    # first cause is still unproven (#208).  Both used to print "registering",
    # so a repair was indistinguishable from an install and a recurrence left no
    # trace: the state is healed now, which is exactly why nothing records that
    # it happened.  Name it, and the next occurrence is evidence instead of a
    # green run.
    if [ "$_vr" -eq 0 ]; then
        say "REPAIRING $1: a VOC record with nothing behind it (mv_git#208)"
        say "  this is the split that wedges an install; please report the run"
    else
        say "registering $1 as a UniVerse file"
    fi
    if [ -n "${2:-}" ] && [ -d "$1" ]; then mv "$1" "$1.staged"; else rm -rf "$1"; fi
    rm -rf "D_$1"
    # THE VOC RECORD OUTLIVES THE DIRECTORIES.  CREATE.FILE refuses a name that
    # is already a file definition, so a record left behind by an earlier failed
    # attempt -- or one that arrived with a clone, naming directories this
    # install is about to replace -- makes the name permanently uncreatable.
    # Clear it: the OS side is already gone by here, so the record names nothing.
    printf 'DELETE VOC %s\nQUIT\n' "$1" | uv >/dev/null 2>&1 || true
    mkfile "$1"
    if [ ! -e "$1" ]; then
        echo "install.sh: CREATE.FILE did not create $1.  It said:" >&2
        printf '%s\n' "$MKFILE_OUT" | sed -n '1,20p' | sed 's/^/    /' >&2
        exit 1
    fi
    if [ -d "$1.staged" ]; then
        for f in "$1.staged"/*; do [ -f "$f" ] && cp "$f" "$1/"; done
        rm -rf "$1.staged"
    fi
}

# BP holds the sources, so they are kept across the rebuild.
[ -d BP ] || [ -d BP.staged ] || cp -r "$SRC/BP.staged" BP.staged
if ! voc_has BP && [ ! -d BP ] && [ -d BP.staged ]; then mv BP.staged BP; fi
ensure_file BP keep
ensure_file BP.INC keep

# BP.O IS BUILD OUTPUT, and is rebuilt rather than carried.  It also has to
# EXIST as a UniVerse file before the compiler runs: `BASIC BP *` creates it
# implicitly, and cannot when an OS file of that name is already sitting there
# with no pointer naming it — "An operating system file already exists with the
# name D_BP.O. Unable to create BP.O file.", after which nothing compiles.
#
# Objects from somewhere else are worse than none: they compile clean and run
# code that is not the source beside them.  So they are dropped, not preserved.
ensure_file BP.O

# PLATFORM.H comes from the BUILD, not from here — the package ships the exact
# defines its sources were built against, and install puts them where the
# compiler looks.  Generating them here instead would mean the tarball and the
# installed account could disagree about what was compiled.
say "installing BP.INC/PLATFORM.H (UniVerse platform defines)"
mkdir -p BP.INC
if [ -f PLATFORM.H ] || [ -f "$SRC/PLATFORM.H" ]; then
    cp "$([ -f PLATFORM.H ] && echo PLATFORM.H || echo "$SRC/PLATFORM.H")" BP.INC/PLATFORM.H
else
    echo "install.sh: PLATFORM.H is missing from this package — it is produced" >&2
    echo "            by build-uv.sh; this tarball was not built properly." >&2
    exit 1
fi

# UniVerse's compiler rejects a source whose last line is unterminated.  The
# staged items already carry a trailing newline, but a hand-edited one may not.
for f in BP/*; do
    [ -f "$f" ] || continue
    [ -n "$(tail -c 1 "$f")" ] && printf '\n' >> "$f"
done

# ---- compile + catalog ------------------------------------------------------
say "compiling the GIT verb and its handlers"
printf 'BASIC BP *\nQUIT\n' | uv > /tmp/mvgit-compile.$$ 2>&1 || true
NOK=$(grep -ac 'Compilation Complete' /tmp/mvgit-compile.$$ || true)
NBAD=$(grep -ac 'Errors detected' /tmp/mvgit-compile.$$ || true)
say "compiled $NOK program(s)"
if [ "${NBAD:-0}" -gt 0 ]; then
    echo "install.sh: $NBAD program(s) failed to compile:" >&2
    grep -aB3 'Errors detected' /tmp/mvgit-compile.$$ | sed 's/^/    /' >&2
    rm -f /tmp/mvgit-compile.$$
    exit 1
fi
rm -f /tmp/mvgit-compile.$$

# Cataloged LOCAL: the verb and every handler it dispatches to by name have to
# be resolvable in this account.  LOCAL keeps them here rather than system-wide,
# so two accounts can run different versions without colliding.
say "cataloging into this account"
{ for p in $(ls BP); do echo "CATALOG BP $p LOCAL"; done; echo QUIT; } | uv >/dev/null 2>&1 || true

# ---- verify -----------------------------------------------------------------
# Prove it, rather than announce it: run the verb and see the version banner.
if printf 'GIT\nQUIT\n' | uv 2>&1 | grep -aq 'version-control hash-file records'; then
    say "verified: the GIT verb answers in this account"
else
    echo "install.sh: the GIT verb did not answer after cataloging" >&2
    exit 1
fi

cat <<EOF

Installed.
  In a session:   GIT status
  From the shell: uv-git -a $HERE status

mvgitd starts on demand and needs no setup; it runs as you and exits with your
session.  Run this installer in each account that needs the verb.
EOF
