#!/bin/sh
# mv_git — build the UniVerse release tree and stage it into $1.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
#
# Runs INSIDE the uv-builder container (driven by the uv-build action's
# build-release.sh / uv-run): make a scratch UniVerse account, compile the
# portable BASIC into it, and stage the result — sources plus their objects —
# into the directory given as $1.
#
#   sh build-uv.sh <stagedir>
#
# STATUS: the cmd/flag layer builds; the engine handlers do not yet.  Everything
# that talks to libgit2 goes through UniData's CallC today (`R = CALLC GIT...`),
# and UniVerse's equivalent is GCI — a different bridge that this package does
# not have yet (mv_git#37).  So this stages the platform-independent half and
# says plainly what is missing, rather than pretending to a complete port.
set -e
STAGE="${1:?usage: build-uv.sh <stagedir>}"
SRC="$(cd "$(dirname "$0")" && pwd)"
ACCT="${ACCT:-/tmp/uvbuild}"

# --- a scratch UniVerse account --------------------------------------------
# mkaccount lays down the VOC; the account is then unusable until its VOC is
# brought up to release (Y) and given a flavour.  The packages target classic
# Pick, so 3.  The flavour is an ACCOUNT ATTRIBUTE — it changes how the account
# behaves — which is why the open account format has to carry it (mv_git#15).
rm -rf "$ACCT"; mkdir -p "$ACCT"; cd "$ACCT"
echo "build-uv: creating a UniVerse account with mkaccount"
/usr/uv/bin/mkaccount . >/dev/null 2>&1 || true
[ -e VOC ] || { echo "build-uv: mkaccount did not create a VOC" >&2; exit 1; }
printf 'Y\n3\nQUIT\n' | uv >/dev/null 2>&1 || true

# --- BP, as a real UniVerse directory file ----------------------------------
# Not just `mkdir BP`: the directory needs a VOC pointer (F / BP / D_BP), which
# CREATE.FILE writes.  It prompts for the DICTionary file first and then the
# DATA file — modulo, separation and type for each — and only the DATA type
# matters here: 19 is a directory file.  The positional forms do NOT set the
# type; `CREATE.FILE BP 19` quietly makes a hashed file instead.
printf 'CREATE.FILE BP\n1\n2\n3\n1\n2\n19\nBASIC source\nQUIT\n' | uv >/dev/null 2>&1 || true
[ -d BP ] || { echo "build-uv: BP was not created as a directory file" >&2; exit 1; }

# --- compile what is portable ------------------------------------------------
# The cmd dispatch and the flag decoding are pure MultiValue BASIC and carry no
# platform branch, so they build here untouched.  One BASIC per program: the
# compiler reports per program, and a failure names itself.
PORTABLE="GIT.CMD.INIT GIT.CMD.ADD GIT.CMD.FLAG GIT.CMD.RUN GIT.CMD.OPT \
GIT.CMD.PARSE GIT.CMD.TOKENS GIT.CMD.VAL GIT.CMD.HAS GIT.CMD.ARG \
GIT.CMD.NARGS GIT.CMD.ERR GIT.CMD.USAGE GIT.SENT GIT.HAS"
NOK=0
for p in $PORTABLE; do
  [ -f "$SRC/BP/$p" ] || continue
  # UniVerse's compiler requires a final newline — a source whose last line is
  # `RETURN` with no newline fails with "End of File unexpected".  MVX and
  # UniData both accept it, so the sources carry it either way; normalise here
  # rather than making every package chase trailing whitespace.
  sed -e '$a\' "$SRC/BP/$p" > "BP/$p"
  # Capture the transcript rather than piping into `grep -q`: grep exits on the
  # first match and SIGPIPEs uv mid-write, which leaves the session shut down
  # untidily and the NEXT compile fails for no reason of its own.
  out="$(printf 'BASIC BP %s\nQUIT\n' "$p" | uv 2>&1)"
  if printf '%s' "$out" | grep -q "Compilation Complete"; then
    NOK=$((NOK + 1))
  else
    echo "build-uv: $p did not compile" >&2
    printf '%s\n' "$out" | grep -viE "^$|Copyright|logged on|^UniVerse Command" | head -8 >&2
    exit 1
  fi
done
echo "build-uv: compiled $NOK portable program(s)"

# --- stage -------------------------------------------------------------------
# Sources and their objects.  UniVerse compiles BP/<prog> to BP.O/<prog> — a
# parallel type-19 file it creates on the first BASIC — rather than UniData's
# _<prog> beside the source, so both directories travel.
mkdir -p "$STAGE/BP" "$STAGE/BP.O"
cp BP/* "$STAGE/BP/" 2>/dev/null || true
cp BP.O/* "$STAGE/BP.O/" 2>/dev/null || true
cp "$SRC/PKG" "$SRC/mvpkg.json" "$SRC/LICENSE" "$SRC/README.md" "$STAGE/" 2>/dev/null || true
echo "build-uv: staged the UniVerse tree ($(ls "$STAGE/BP" | wc -l | tr -d ' ') source(s), $(ls "$STAGE/BP.O" 2>/dev/null | wc -l | tr -d ' ') object(s))"
echo "build-uv: NOTE — the GIT verb itself is not in this tree yet: its handlers"
echo "build-uv:        reach libgit2 through CallC, and the UniVerse GCI bridge"
echo "build-uv:        is still to be written (mv_git#37)."
