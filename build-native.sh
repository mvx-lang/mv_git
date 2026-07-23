#!/bin/sh
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
