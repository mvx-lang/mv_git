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
# checkout it is two levels up; a standalone ev_git checkout points at an mvx
# tree via $MVX_ROOT.
ROOT="${MVX_ROOT:-$(cd "$PKG/../.." && pwd)}"

case "$(uname)" in
  Darwin) EXT=dylib ; UNDEF="-undefined dynamic_lookup" ; RPATH="@executable_path/../lib" ;;
  *)      EXT=so ; UNDEF="" ; RPATH='$ORIGIN/../lib' ;;
esac

CFLAGS="$(pkg-config --cflags libgit2 2>/dev/null || echo -I/opt/homebrew/include)"
LDFLAGS="$(pkg-config --libs libgit2 2>/dev/null || echo -L/opt/homebrew/lib -lgit2)"

mkdir -p "$PKG/LIB"
cc -O2 -fPIC -shared $UNDEF \
   -I"$ROOT/runtime/include" $CFLAGS \
   "$PKG/src/mvxgit.c" $LDFLAGS \
   -o "$PKG/LIB/libmvxgit.$EXT"
echo "  built LIB/libmvxgit.$EXT (native, libgit2)"

# mvx-git: drives the record-git engine directly (#58), so it compiles the
# engine in and links the runtime (mvx_ctx + storage API) and libgit2.  It
# finds libmvxrt beside itself once installed (rpath ../lib, like mvx).  For a
# non-record-git command it just execs the real git, so no runtime is needed at
# run time in that path — but the link is unconditional.
mkdir -p "$PKG/bin"
cc -O2 -I"$ROOT/runtime/include" -I"$PKG/src" $CFLAGS \
   "$PKG/src/mvx-git.c" "$PKG/src/mvxgit.c" \
   -L"$ROOT/build/lib" -lmvxrt $LDFLAGS \
   -Wl,-rpath,"$RPATH" -Wl,-rpath,"$ROOT/build/lib" \
   -o "$PKG/bin/mvx-git"
echo "  built bin/mvx-git (record-git engine + git wrapper)"
