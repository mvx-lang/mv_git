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
ROOT="$(cd "$PKG/../.." && pwd)"

case "$(uname)" in
  Darwin) EXT=dylib ; UNDEF="-undefined dynamic_lookup" ;;
  *)      EXT=so ; UNDEF="" ;;
esac

CFLAGS="$(pkg-config --cflags libgit2 2>/dev/null || echo -I/opt/homebrew/include)"
LDFLAGS="$(pkg-config --libs libgit2 2>/dev/null || echo -L/opt/homebrew/lib -lgit2)"

mkdir -p "$PKG/LIB"
cc -O2 -fPIC -shared $UNDEF \
   -I"$ROOT/runtime/include" $CFLAGS \
   "$PKG/src/mvxgit.c" $LDFLAGS \
   -o "$PKG/LIB/libmvxgit.$EXT"
echo "  built LIB/libmvxgit.$EXT (native, libgit2)"

# mvx-git: a drop-in git wrapper that rebuilds an MVX account after any
# tree-changing command.  Plain C — it shells out to git and mvx — so it
# needs no libraries of its own.
mkdir -p "$PKG/bin"
cc -O2 "$PKG/src/mvx-git.c" -o "$PKG/bin/mvx-git"
echo "  built bin/mvx-git (git wrapper)"
