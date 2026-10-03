#!/bin/sh
# MVX — a native compiler and runtime for Pick/MultiValue BASIC.
# Copyright (C) 2026 Gordon Heydon.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2, as
# published by the Free Software Foundation.  There is NO WARRANTY, to
# the extent permitted by law; see the LICENSE file for details.
#
# SPDX-License-Identifier: GPL-2.0-only
# Build the git package's native subroutine library (libgit2-backed).
# mkpkg.sh runs this if present, after cleaning LIB/.  Runtime symbols
# (mv_*, mvx_*) resolve from the host program at load, exactly like the
# storage drivers; libgit2 links into this library alone.
set -e
PKG="$(cd "$(dirname "$0")" && pwd)"
# The MVX source root supplies the runtime headers (runtime/include) and
# libmvxrt (build/lib) that mvx-git links.  As a submodule under an mvx
# checkout it is two levels up; a standalone mv_git checkout points at an mvx
# tree via $MVX_ROOT.
ROOT="${MVX_ROOT:-$(cd "$PKG/../.." && pwd)}"

# The mvx runtime headers + libmvxrt come from either an mvx source tree with a
# build (runtime/include + build/lib) or an installed toolchain prefix (setup-mvx
# ships them at $MVXHOME/include + $MVXHOME/lib).  Prefer the build tree; fall
# back to the install so a package builds in CI with no mvx checkout.
# THE CLIENT LIBRARY IS SOMEWHERE ELSE IN A SOURCE TREE (#267 stage 2).  An
# install puts mvxc.h beside mvx_runtime.h and libmvxc beside libmvxrt, so one
# include and one lib path serve both; a build tree does not -- the header is in
# client/include and libmvxc lands in build/ rather than build/lib.  Hence the
# second pair, empty when there is nothing extra to add.
MVXINC2="" ; MVXLIB2=""
if [ -d "$ROOT/runtime/include" ]; then
  MVXINC="$ROOT/runtime/include" ; MVXLIB="$ROOT/build/lib"
  MVXINC2="$ROOT/client/include" ; MVXLIB2="$ROOT/build"
elif [ -n "${MVXHOME:-}" ] && [ -f "$MVXHOME/include/mvx_runtime.h" ]; then
  MVXINC="$MVXHOME/include" ; MVXLIB="$MVXHOME/lib"
else
  echo "build-native.sh: no mvx runtime headers (set MVX_ROOT to an mvx build tree, or MVXHOME to an installed toolchain)" >&2
  exit 1
fi

case "$(uname)" in
  Darwin) EXT=dylib ; UNDEF="-undefined dynamic_lookup" ; RPATH="@executable_path/../lib" ;;
  *)      EXT=so ; UNDEF="" ; RPATH='$ORIGIN/../lib' ;;
esac

CFLAGS="$(pkg-config --cflags libgit2 2>/dev/null || echo -I/opt/homebrew/include)"
LDFLAGS="$(pkg-config --libs libgit2 2>/dev/null || echo -L/opt/homebrew/lib -lgit2)"

. "$PKG/version.sh"
UGVER="${MV_GIT_VERSION:-$(mv_git_version "$PKG")}"
mv_git_require_version "$UGVER" || exit 1     # a tag build must know its version

mkdir -p "$PKG/LIB"
cc -O2 -fPIC -shared $UNDEF \
   -I"$MVXINC" $CFLAGS -DMVXGIT_VERSION="\"$UGVER\"" \
   "$PKG/src/mvxgit.c" $LDFLAGS \
   -o "$PKG/LIB/libmvxgit.$EXT"
echo "  built LIB/libmvxgit.$EXT (native, libgit2)"

# mvx-git: drives the record-git engine directly (#58), so it compiles the
# engine in and links the runtime (mvx_ctx + storage API) and libgit2.  It
# finds libmvxrt beside itself once installed (rpath ../lib, like mvx).  For a
# non-record-git command it just execs the real git, so no runtime is needed at
# run time in that path — but the link is unconditional.
mkdir -p "$PKG/bin"

# THROUGH THE CLIENT LIBRARY (#267 stage 2).  mvx-git used to reach into
# libmvxrt, the runtime it happened to be linked against; it goes through
# libmvxc now -- the same contract mv-connect and every language binding uses --
# so it is no longer a special case with privileged access to MVX's internals.
# MVXGIT_MVXC selects the transport; MVXGIT_MVXRT stays set by mvxgit.h because
# the PLATFORM is still MVX and the engine's behavioural guards read it.
#
# MVXGIT_BACKEND=mvxrt still builds the old way.  Not a fallback anybody is meant
# to need, but the two arms differ only in this file's link line, which makes it
# the cheapest possible way to answer "is it the backend?" when something breaks.
: "${MVXGIT_BACKEND:=mvxc}"
case "$MVXGIT_BACKEND" in
  mvxc)
    GITARM='-DMVXGIT_MVXC'
    GITARM_SRC="$PKG/src/mvxcgit_rt.c"
    GITARM_LIB='-lmvxc'
    GITARM_WHAT='via libmvxc' ;;
  mvxrt)
    GITARM=''
    GITARM_SRC=''
    GITARM_LIB='-lmvxrt'
    GITARM_WHAT='via libmvxrt' ;;
  *) echo "build-native.sh: MVXGIT_BACKEND must be mvxc or mvxrt" >&2; exit 1 ;;
esac

cc -O2 -I"$MVXINC" ${MVXINC2:+-I"$MVXINC2"} -I"$PKG/src" \
   $CFLAGS $GITARM -DMVXGIT_VERSION="\"$UGVER\"" \
   "$PKG/src/mvx-git.c" "$PKG/src/mvxgit.c" "$PKG/src/mvconn.c" $GITARM_SRC \
   -L"$MVXLIB" ${MVXLIB2:+-L"$MVXLIB2"} $GITARM_LIB $LDFLAGS \
   -Wl,-rpath,"$RPATH" -Wl,-rpath,"$MVXLIB" \
   ${MVXLIB2:+-Wl,-rpath,"$MVXLIB2"} \
   -o "$PKG/bin/mvx-git"
echo "  built bin/mvx-git (record-git engine + git wrapper, $GITARM_WHAT)"
