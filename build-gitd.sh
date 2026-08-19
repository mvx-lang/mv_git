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
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"
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

# ---- the agent, embedded ----------------------------------------------------
# uv-git has to be able to put a working agent into an account that has NOTHING
# installed in it — a freshly created one, when cloning, and a throwaway stock
# one, when learning what stock looks like (#46, #52).  Reading BP/GIT.AGENT off
# disk at that moment means depending on where the package happens to be, which
# is exactly what is not knowable from an arbitrary working directory.
#
# So the binary carries it, generated HERE from the one true copy so the two can
# never drift: an embedded agent that disagrees with the shipped BASIC would be
# the worst kind of bug to chase.
sh "$HERE/embed-agent.sh"


# MVXGIT_GITD selects the shared value type in mvxgit.h; MVXGIT_NORECORDS says
# this binary has NO record backend at all, so the engine never asks one a
# question it would abort on.  uv-git sets the first but not the second — it has
# a real backend (agent_rt.c).  -D_FILE_OFFSET_BITS=64 keeps large blobs honest
# on 32-bit hosts.
$CC -O2 -fPIC -D_FILE_OFFSET_BITS=64 -DMVXGIT_GITD -DMVXGIT_NORECORDS \
    -I"$SRC" $LG2_CFLAGS \
    "$SRC/gitd.c" "$SRC/gitd_rt.c" "$SRC/mvxgit.c" \
    -o "$OUT/mvgitd" $LG2_LIBS

echo "built $OUT/mvgitd"

# uv-git — the shell-side entry point, and it DOES reach records: agent_rt.c
# implements the engine's record contract over a session running BP/GIT.AGENT
# (mv_git#47), which is the peer of udtgit_rt.c's InterCall implementation.  The
# session is needed because C cannot touch UniVerse records directly — GCI is
# licensed and dead in the TE, InterCall is a client SDK not available for Linux
# — but only the RECORD half goes through it; the git objects stay here, in
# process, which is why no verb and no mvgitd are involved.
#
# Same MVXGIT_GITD build of the engine (the value type and header are shared),
# with the recordless gitd_rt.c swapped for the real backend.  mvsession.c and
# agent_rt.c are NOT UniVerse-specific: udt-git will link the same two files and
# only name a different shell (mv_git#45).
$CC -O2 -fPIC -D_FILE_OFFSET_BITS=64 -DMVXGIT_GITD \
    -I"$SRC" $LG2_CFLAGS \
    "$SRC/uv-git.c" "$SRC/mvsession.c" "$SRC/agent_rt.c" "$SRC/mvxgit.c" \
    "$SRC/agentcmd.c" "$SRC/agentseed.c" \
    -o "$OUT/uv-git" $LG2_LIBS

echo "built $OUT/uv-git"
