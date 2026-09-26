#!/bin/sh
# install.sh — put mv_git where OpenQM / ScarletDME can find it.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
#
# There are TWO PACKAGES with this name, and which one you have was decided by
# build-qm.sh when it looked at your QM:
#
#   * the ENGINE package, with libqmgit.so in it.  The GIT verb is the real
#     one -- the shared handlers, calling the record-git engine in the session
#     that typed the sentence, through CCALL into QM's own dh_ layer.  It needs
#     a QM built with geneb/ScarletDME#109, #111 and #113.
#
#   * the STOCK package, with no library and a single BP/GIT in it.  That GIT
#     is a shim: it hands the sentence to qm-git and prints what comes back.
#
# Both install qm-git, the shell driver.  It is an ordinary binary -- but it
# links QMClient (libqmcli.so), which lives in QM's own bin/ and is on nobody's
# default library path.  Without help it fails at startup with
#
#     error while loading shared libraries: libqmcli.so
#
# which reads as a broken build rather than a missing path.  So the binary is
# built with an RPATH pointing at $QMDIR/bin -- see build-qm.sh -- and this
# script CHECKS THAT IT RUNS rather than assuming.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
QMDIR="${QMDIR:-/usr/qmsys}"
QMBIN="${QMBIN:-$QMDIR/bin/qm}"

say() { printf 'install: %s\n' "$*"; }

[ -x "$HERE/qm-git" ] || { say "no qm-git here; run build-qm.sh first"; exit 1; }

BIN="${MVGIT_BIN:-/usr/local/bin}"
if [ -w "$BIN" ]; then
    cp "$HERE/qm-git" "$BIN/qm-git"
    say "qm-git -> $BIN"
else
    say "NOTE: $BIN is not writable; copy qm-git there yourself, or set MVGIT_BIN"
    BIN="$HERE"
fi

# PROVE IT RUNS.  A binary that cannot find libqmcli installs perfectly and
# then fails on first use, in an account, where it looks like an mv_git bug.
if "$BIN/qm-git" version >/dev/null 2>&1; then
    say "qm-git runs ($("$BIN/qm-git" version 2>/dev/null | head -1))"
else
    say "WARNING: qm-git will not start.  It needs $QMDIR/bin on the library"
    say "         path: try  LD_LIBRARY_PATH=$QMDIR/bin $BIN/qm-git version"
    say "         (build-qm.sh sets an RPATH; a binary moved between hosts"
    say "          with QM installed elsewhere will need the variable.)"
fi

if [ ! -f "$HERE/libqmgit.so" ]; then
    cat <<TXT

Installed (stock QM: the CLI, and a verb that calls it).
  CLI:   qm-git [-a <account>] <command>     (run it in the account directory,
                                              or name the account with -a)

Per account, once, for the GIT verb at the TCL prompt:

  cp $HERE/BP/GIT <account>/BP/GIT
  $QMBIN -a<ACCOUNT> 'BASIC BP GIT'
  $QMBIN -a<ACCOUNT> 'CATALOG BP GIT GLOBAL'

then:  GIT STATUS   (and INIT / ADD / COMMIT / LOG / DIFF / SHOW / RESTORE)

GLOBAL, NOT LOCAL, even though only one account is being set up.  LOCAL writes
VOC/GIT as a "V"/"CS" record, and a V record is a system verb the destination
supplies -- so it is never committed, and `GIT CHECKOUT' materialises VOC from
the commit and deletes it.  The verb removes itself the first time anybody
switches branch.  A globally catalogued GIT needs no VOC record: the TCL
processor reaches the global catalogue when the account's VOC does not name the
word.

That verb is a shim over the CLI.  The in-session verb -- the engine called
directly, the way it is on MVX and jBASE -- needs a QM carrying
geneb/ScarletDME#109, #111 and #113.  Build against one and this package will
carry libqmgit.so and install the real thing.
TXT
    exit 0
fi

# --- the engine package ---------------------------------------------------
#
# THE LIBRARY HAS TO BE FINDABLE BY NAME.  GIT.QM dlopen()s "libqmgit.so" with
# no path, deliberately -- the pcode carries a string and a site may want its
# own copy -- so it is the loader that has to find it, not mv_git.
LIBDIR="${MVGIT_LIBDIR:-/usr/local/lib}"
if [ -w "$LIBDIR" ]; then
    cp "$HERE/libqmgit.so" "$LIBDIR/libqmgit.so"
    command -v ldconfig >/dev/null 2>&1 && ldconfig 2>/dev/null || true
    say "libqmgit.so -> $LIBDIR"
else
    say "NOTE: $LIBDIR is not writable; put libqmgit.so on the loader path"
    say "      yourself, or set MVGIT_LIBDIR."
fi

# --- the BASIC, catalogued ONCE -------------------------------------------
#
# GLOBALLY, NOT PER ACCOUNT, AND THAT INCLUDES THE VERB.  There are ninety-odd
# programs here and every account needs all of them; cataloguing them per
# account would recompile the lot for each one.  QM's global catalogue
# ($QMDIR/gcat) is exactly the right shelf -- load_object() looks there after
# the account's own, and so does the TCL processor, so a globally catalogued
# GIT is a verb in every account with no VOC record at all.
#
# THAT IS ALSO THE ONLY ROUTE THAT SURVIVES A CHECKOUT.  `CATALOG BP GIT LOCAL'
# writes VOC/GIT as "V"/"CS"/<path>, and a V record is a system verb the
# destination supplies -- never committed.  `GIT CHECKOUT' materialises VOC
# from the commit, does not find GIT in it, and deletes it: the verb removes
# itself the first time anybody switches branch.
#
# They are compiled FROM QMSYS, which needs two things of its own: a directory
# file to hold the sources, and BP.INC/PLATFORM.H, because every one of them
# $INCLUDEs it.  Without the header each compile fails with a single error and
# the catalogue step then reports the object as missing, which reads as a
# packaging fault rather than a missing include.
"$QMBIN" -aQMSYS "CREATE-FILE BP.INC DIRECTORY" </dev/null >/dev/null 2>&1 || true
"$QMBIN" -aQMSYS "CREATE-FILE MVGIT.BP DIRECTORY" </dev/null >/dev/null 2>&1 || true
[ -d "$QMDIR/BP.INC" ] && [ -d "$QMDIR/MVGIT.BP" ] || {
    say "cannot create $QMDIR/BP.INC and $QMDIR/MVGIT.BP -- run this as the QM"
    say "administrator (the user that owns $QMDIR)."
    exit 1; }
cp "$HERE/PLATFORM.H" "$QMDIR/BP.INC/PLATFORM.H"
# A STALE CATALOGUE IS WORSE THAN AN EMPTY ONE: a program dropped or renamed
# since the last install would still be in gcat, and a session would call it.
# Clear this package's own entries -- source AND object -- before staging the
# new set, and touch nothing else in gcat, which is QM's own library.
for p in "$QMDIR"/MVGIT.BP/*; do
    [ -f "$p" ] || continue
    b=$(basename "$p"); rm -f "$QMDIR/gcat/$b" "$p"
done
cp "$HERE/BP/"* "$QMDIR/MVGIT.BP/"

say "compiling and cataloguing $(ls "$QMDIR/MVGIT.BP" | wc -l | tr -d ' ') programs into $QMDIR/gcat ..."
LOG="${TMPDIR:-/tmp}/mvgit-catalog.$$"
# QUIT AT THE END, OR THIS NEVER RETURNS.  A QM session fed from a pipe does not
# take end-of-input as a reason to stop: it redraws the TCL prompt for ever, and
# the install hangs with the catalogue already complete.
{ for p in "$QMDIR"/MVGIT.BP/*; do
      b=$(basename "$p")
      printf 'BASIC MVGIT.BP %s\nCATALOG MVGIT.BP %s GLOBAL\n' "$b" "$b"
  done
  printf 'QUIT\n'; } | "$QMBIN" -aQMSYS >"$LOG" 2>&1 || true

# ASSERT THE POSITIVE FACT.  QM reports a failed compile on stdout and carries
# on, and the exit status is the last CATALOG's, so a package that half
# compiled installs silently and fails one verb at a time.  Count the objects.
miss=""
for p in "$QMDIR"/MVGIT.BP/*; do
    b=$(basename "$p")
    [ -f "$QMDIR/gcat/$b" ] || miss="$miss $b"
done
if [ -n "$miss" ]; then
    say "these programs did not catalogue:$miss"
    say "  full log: $LOG"
    exit 1
fi
rm -f "$LOG"
say "cataloged globally; every account can now CALL them"

cat <<TXT

Installed (the in-session verb, and the CLI beside it).
  CLI:   qm-git [-a <account>] <command>
  Verb:  GIT STATUS   at the TCL prompt, in any account

There is NO per-account step.  GIT and everything behind it are in QM's global
catalogue, which the TCL processor reaches when the account's own VOC does not
name the word -- so every account on this system has the verb, now and after
any account is created.

  GIT INIT / ADD / COMMIT / LOG / DIFF / SHOW / RESTORE / BRANCH / CHECKOUT ...

To remove it again:  rm $QMDIR/gcat/GIT $QMDIR/gcat/GIT[.A-Z]*
TXT
