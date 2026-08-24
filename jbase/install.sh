#!/bin/sh
# install.sh — put mv_git where jBASE can find it.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
#
# Two things have to be resolvable, and only one of them is obvious.
#
#   1. jb-git, the shell driver -- an ordinary binary on $PATH.
#
#   2. libjbgit.so, the GIT* entry points the in-session verb reaches through
#      DEFC.  A CATALOGed subroutine is linked into the account's own lib0.so,
#      and nothing tells that link where our library is -- jBASE has no
#      "extra libraries" setting for CATALOG (JBCDEV_LIB names the OUTPUT).  So
#      the library has to be somewhere the loader already looks, and the answer
#      is $JBCRELEASEDIR/lib.  Without it a cataloged handler compiles and
#      catalogs cleanly and then fails at RUN time with
#
#          lib0.so.2: undefined symbol: JBGITSTATUS
#
#      which reads as a problem with the handler rather than with packaging.
#
#      UniData's install.sh does the same thing for the same reason, putting
#      libu2callc.so into $UDTHOME/bin (mv_git#114).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${JBCRELEASEDIR:?set JBCRELEASEDIR first: . /opt/jbase/<version>/jbase_env.sh}"

say() { printf 'install: %s\n' "$*"; }

# 1) the DEFC library, beside jBASE's own
if [ -f "$HERE/libjbgit.so" ]; then
    cp "$HERE/libjbgit.so" "$JBCRELEASEDIR/lib/libjbgit.so"
    say "libjbgit.so -> $JBCRELEASEDIR/lib (the in-session GIT* entry points)"
else
    say "WARNING: no libjbgit.so here; the in-session verb will not resolve"
fi

# 2) the CLI
BIN="${MVGIT_BIN:-/usr/local/bin}"
if [ -w "$BIN" ]; then
    cp "$HERE/jb-git" "$BIN/jb-git"; say "jb-git -> $BIN"
else
    say "NOTE: $BIN is not writable; copy jb-git there yourself, or set MVGIT_BIN"
fi

cat <<TXT

Installed.
  CLI:              jb-git [-a <account>] <command>
  in-session verb:  per account, see below

Per account, once:

  cd <account>
  # BP.INC/PLATFORM.H tells the sources which platform they are being
  # compiled for; the handlers do not work without it.
  mkdir -p BP.INC && cp $HERE/PLATFORM.H BP.INC/PLATFORM.H
  cp $HERE/BP/* BP/
  jsh <<'EOS'
  BASIC BP GITSTATUS
  CATALOG BP GITSTATUS
  EOS

then:  GIT STATUS   (and add / commit / log)
TXT
