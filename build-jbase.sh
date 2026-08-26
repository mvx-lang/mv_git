#!/bin/sh
# build-jbase.sh — build jb-git (Rocket jBASE) and stage its release package.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
#
# FIRST PASS (mv_git#114).  The engine (src/mvxgit.c) is shared with mvx-git and
# udt-git; here it is compiled with -DMVXGIT_JBASE so its record primitives bind
# to jBASE's Jedi* API (src/jbasegit_rt.c).
#
# jBASE is the MVX case rather than the UniData one: a standalone process makes
# its own session with JBASESessionObjectFactory() and calls JediOpen /
# JediReadRecord / ... itself, so there are no CallC objects to build and no
# background daemon.  Plain cc, not jcompile -- there is no BASIC here.
#
# Requires: a C compiler; libgit2 under $LIBGIT2_PREFIX (or LIBGIT2_CFLAGS /
# LIBGIT2_LIBS); and jBASE ($JBCRELEASEDIR, with include/ and lib/).
#
#   sh build-jbase.sh [stagedir]        # default: ./stage
set -e
STAGE="${1:-./stage}"
: "${JBCRELEASEDIR:?set JBCRELEASEDIR to your jBASE installation (jbase_env.sh)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"
CC="${CC:-cc}"

. "$HERE/version.sh"
UGVER="${MV_GIT_VERSION:-$(mv_git_version "$HERE")}"

LG2_CFLAGS="${LIBGIT2_CFLAGS:-$(pkg-config --cflags libgit2 2>/dev/null || echo "-I${LIBGIT2_PREFIX:-/usr/local}/include")}"
LG2_LIBS="${LIBGIT2_LIBS:-$(pkg-config --libs libgit2 2>/dev/null || echo "-L${LIBGIT2_PREFIX:-/usr/local}/lib -lgit2")}"

mkdir -p "$HERE/bin"
"$CC" -std=c11 -O2 -DMVXGIT_JBASE -DMVXGIT_VERSION="\"$UGVER\"" \
      -DJBGIT_VERSION="\"$UGVER\"" \
      -I"$SRC" -I"$JBCRELEASEDIR/include" $LG2_CFLAGS \
      "$SRC/jb-git.c" "$SRC/mvxgit.c" "$SRC/jbasegit_rt.c" \
      -L"$JBCRELEASEDIR/lib" -ljbase -ljbaseutil \
      $LG2_LIBS -lm -lncurses -ldl -lpthread -lrt \
      -o "$HERE/bin/jb-git"
echo "  built bin/jb-git (record-git engine + jBASE Jedi record layer)"

# The in-session verb: the same engine again, as a shared library the compiled
# BASIC links against.  jBASE declares these with DEFC and is handed its own
# session, so the verb reads the records of the session that called it.
"$CC" -std=c11 -O2 -fPIC -shared -DMVXGIT_JBASE -DMVXGIT_VERSION="\"$UGVER\"" \
      -I"$SRC" -I"$JBCRELEASEDIR/include" $LG2_CFLAGS \
      "$SRC/jbasecallc.c" "$SRC/mvxgit.c" "$SRC/jbasegit_rt.c" \
      -L"$JBCRELEASEDIR/lib" -ljbase -ljbaseutil \
      $LG2_LIBS -lm -ldl -lpthread \
      -o "$HERE/bin/libjbgit.so"
echo "  built bin/libjbgit.so (the GIT* entry points for the in-session verb)"

# PLATFORM.H -- the compile-time platform defines the BASIC sources $INCLUDE.
# Built here for the same reason build-udt.sh builds its own: it is build
# output.  jBASE takes `$INCLUDE BP.INC PLATFORM.H`, `$DEFINE` and `$IFDEF`
# exactly as the others do (verified on 6.2.1.1).
#
# ENGINE is the interesting one: it says the record-git engine can be CALLED
# here rather than reimplemented in BASIC.  True on MVX and on jBASE, false on
# UniData and UniVerse -- a handler asks that instead of naming a platform.
mkdir -p "$STAGE/mv_git"
cat > "$STAGE/mv_git/PLATFORM.H" <<'PLATEOF'
* PLATFORM.H - compile-time platform defines for the MV BASIC sources.
*
* GENERATED FILE - DO NOT EDIT.  It is written by build-jbase.sh every time the
* package is built, so any change made here is lost on the next build.  To
* change what it says, change the script.
*
* MV is every MultiValue platform; JBASE is this one.  ENGINE means the
* record-git engine is callable in-process, so a handler can call it rather
* than reimplement it in BASIC.
*
* ASCII ONLY, deliberately: every BASIC source on every platform $INCLUDEs this
* file, so it is the worst possible place to find out that some compiler does
* not like a byte above 127 in a comment.
$DEFINE MV
$DEFINE JBASE
$DEFINE ENGINE
*
* MVMASTER is the account's own master file, under the name THIS platform
* uses for it -- VOC everywhere except jBASE, which calls it MD.  An EQU
* rather than a valued $DEFINE: UniData's $DEFINE takes no value at all
* (it is a flag for $IFDEF), and a quoted value fails to compile there
* while compiling fine on jBASE and MVX.  Verified on all four.
      EQU MVMASTER TO "MD"
PLATEOF
echo "  wrote PLATFORM.H (MV, JBASE, ENGINE)"
cp "$HERE/bin/jb-git" "$HERE/bin/libjbgit.so" "$STAGE/mv_git/"
mkdir -p "$STAGE/mv_git/BP"

# The GIT verb and its whole handler set -- the thing the shims exist to SERVE.
#
# This staged only jbase/BP/ (the 32 DEFC shims) and not BP/ (the verb itself),
# so the package had GITADD and GITCAT and no GIT.  An account built from it had
# nothing to catalog, and every verb-mode test failed with "GIT: No such file or
# directory" -- not a symbol-resolution problem, which is what it looked like,
# but a verb that had never shipped (mv_git#114).  The CLI path was carrying the
# whole suite.
#
# FILES only: a plain `cp BP/*` would take the D_BP directory with it, the trap
# build-udt.sh hit.  And every item gets a trailing newline if it lacks one --
# jBASE compiles BASIC through C, and an unterminated last line is the kind of
# thing that fails on one compiler and not another; uv needs this outright, and
# doing it here costs nothing and removes the question.
for f in "$HERE"/BP/*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    cp "$f" "$STAGE/mv_git/BP/$b"
    [ -n "$(tail -c 1 "$STAGE/mv_git/BP/$b")" ] && printf '\n' >> "$STAGE/mv_git/BP/$b"
done

# ...then the shims on top.  They cannot collide: a shim is GITSTATUS, a handler
# is GIT.STATUS.
cp "$HERE/jbase/BP/"* "$STAGE/mv_git/BP/" 2>/dev/null || true
cp "$HERE/jbase/install.sh" "$STAGE/mv_git/"; chmod +x "$STAGE/mv_git/install.sh"
[ -f "$HERE/LICENSE" ] && cp "$HERE/LICENSE" "$STAGE/mv_git/"
[ -f "$HERE/README.md" ] && cp "$HERE/README.md" "$STAGE/mv_git/"
echo "build-jbase: staged the jBASE package as $STAGE/mv_git/"
