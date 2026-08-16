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
while [ $# -gt 0 ]; do
    case "$1" in
        -p|--prefix) PREFIX="$2"; shift 2 ;;
        *) echo "usage: ./install.sh [-p prefix]" >&2; exit 2 ;;
    esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
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
say "installing mvgitd + uv-git to $PREFIX/bin"
mkdir -p "$PREFIX/bin"
install -m 0755 bin/mvgitd  "$PREFIX/bin/mvgitd"
install -m 0755 bin/uv-git  "$PREFIX/bin/uv-git"

# ---- make this directory an account ----------------------------------------
# A fresh UniVerse directory becomes an account on its first `uv`, which asks to
# update the VOC's RELLEVEL and then for a flavour.  The packages target classic
# Pick, so the flavour is 3.  An existing account skips both prompts and the
# extra answers are harmless.
if [ ! -e VOC ]; then
    say "making this directory a UniVerse account (Pick flavour)"
    printf 'Y\n3\nQUIT\n' | uv >/dev/null 2>&1 || true
fi
[ -e VOC ] || { echo "install.sh: could not create the account (no VOC)" >&2; exit 1; }

# ---- the BASIC source and include files ------------------------------------
# CREATE.FILE prompts for the DICT part and then the data part — six answers,
# with type 19 (directory) for the data — so both are fed positionally.  An
# existing file just reports itself and the answers fall through harmlessly.
mkfile() {
    printf 'CREATE.FILE %s\n1\n2\n3\n1\n2\n19\nsrc\nQUIT\n' "$1" | uv >/dev/null 2>&1 || true
}

# BP arrives as a plain directory of sources, which is NOT yet a UniVerse file:
# it needs a VOC pointer and a dictionary, and `BASIC BP *` cannot see it
# without them — the failure is a silent "compiled 0 programs", not an error.
#
# Rather than hand-build the VOC record, let CREATE.FILE do it properly and move
# the sources in afterwards: it exists to create the pointer, the dictionary and
# the directory together, and refuses when the directory is already there.
if [ ! -e D_BP ]; then
    say "registering BP as a UniVerse file"
    mv BP BP.staged
    mkfile BP
    [ -d BP ] || { echo "install.sh: CREATE.FILE did not create BP" >&2; exit 1; }
    for f in BP.staged/*; do [ -f "$f" ] && cp "$f" "BP/"; done
    rm -rf BP.staged
fi
mkfile BP.INC

say "writing BP.INC/PLATFORM.H (UniVerse platform defines)"
mkdir -p BP.INC
printf '* PLATFORM.H - UniVerse (UV) platform defines, written by install.sh.\n$DEFINE MV\n$DEFINE UV\n' > BP.INC/PLATFORM.H

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
