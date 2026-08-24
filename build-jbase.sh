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
      -I"$SRC" -I"$JBCRELEASEDIR/include" $LG2_CFLAGS \
      "$SRC/jb-git.c" "$SRC/mvxgit.c" "$SRC/jbasegit_rt.c" \
      -L"$JBCRELEASEDIR/lib" -ljbase -ljbaseutil \
      $LG2_LIBS -lm -lncurses -ldl -lpthread -lrt \
      -o "$HERE/bin/jb-git"
echo "  built bin/jb-git (record-git engine + jBASE Jedi record layer)"

mkdir -p "$STAGE/mv_git"
cp "$HERE/bin/jb-git" "$STAGE/mv_git/"
[ -f "$HERE/LICENSE" ] && cp "$HERE/LICENSE" "$STAGE/mv_git/"
[ -f "$HERE/README.md" ] && cp "$HERE/README.md" "$STAGE/mv_git/"
echo "build-jbase: staged the jBASE package as $STAGE/mv_git/"
