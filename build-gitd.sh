#!/bin/sh
# mv_git — build mvgitd, the per-user background process (mv_git#40).
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only.
#
# mvgitd runs libgit2 on behalf of a BASIC session that cannot call it — on
# UniVerse, where GCI is licensed and non-functional in the Trial Edition.  It
# is the same engine as mvx-git and udt-git, built against a record backend that
# has no records (gitd_rt.c): the session keeps those and passes content over
# the pipe.  See src/gitproto.h for the wire protocol.
#
#   ./build-gitd.sh [outdir]        # default: ./bin
#
# Requires a C compiler and libgit2 under $LIBGIT2_PREFIX (default /usr/local).
set -eu

OUT="${1:-bin}"
SRC="$(cd "$(dirname "$0")/src" && pwd)"
CC="${CC:-cc}"
PREFIX="${LIBGIT2_PREFIX:-/usr/local}"

# Prefer pkg-config when it knows libgit2; fall back to the prefix layout that
# the udt builder uses, so both builders agree without extra configuration.
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libgit2 2>/dev/null; then
    LG2_CFLAGS="$(pkg-config --cflags libgit2)"
    LG2_LIBS="$(pkg-config --libs libgit2)"
else
    LG2_CFLAGS="-I$PREFIX/include"
    LG2_LIBS="-L$PREFIX/lib -L$PREFIX/lib64 -lgit2"
fi

mkdir -p "$OUT"

# MVXGIT_GITD selects the recordless backend in mvxgit.h.  -D_FILE_OFFSET_BITS=64
# keeps large blobs honest on 32-bit hosts.
$CC -O2 -fPIC -D_FILE_OFFSET_BITS=64 -DMVXGIT_GITD \
    -I"$SRC" $LG2_CFLAGS \
    "$SRC/gitd.c" "$SRC/gitd_rt.c" "$SRC/mvxgit.c" \
    -o "$OUT/mvgitd" $LG2_LIBS

echo "built $OUT/mvgitd"

# uv-git — the shell-side entry point.  It drives the in-session GIT verb rather
# than reaching records from C, because on UniVerse it cannot: GCI is licensed
# and dead in the TE, and InterCall is a client SDK not available for Linux.  It
# still links the engine, for `uv-git textconv`, which is pure git-object work
# and so needs no session — hence the same recordless backend as mvgitd.
$CC -O2 -fPIC -D_FILE_OFFSET_BITS=64 -DMVXGIT_GITD \
    -I"$SRC" $LG2_CFLAGS \
    "$SRC/uv-git.c" "$SRC/gitd_rt.c" "$SRC/mvxgit.c" \
    -o "$OUT/uv-git" $LG2_LIBS

echo "built $OUT/uv-git"
