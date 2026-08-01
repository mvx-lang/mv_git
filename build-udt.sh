#!/bin/sh
# build-udt.sh — build udt-git, the Rocket UniData build of the record-git
# engine.  The engine (src/mvxgit.c) is shared with mvx-git; here it is compiled
# with -DMVXGIT_UDT so its record primitives bind to the UniData InterCall
# backend (src/udtgit_rt.c) instead of libmvxrt.  No runtime indirection — the
# backend is chosen at compile time.
#
# Requires, on a UniData host:
#   - a C compiler
#   - libgit2 (built to match mvx-git, 1.9.x) under $LIBGIT2_PREFIX (/usr/local)
#   - UniData: $UDTHOME set, with InterCall headers ($UDTHOME/bin/include) and
#     libuvic ($UDTHOME/bin/lib)
#
# Produces ./udt-git.  At run time it opens a UniObjects/InterCall session; set
# UDT_HOST / UDT_USER / UDT_PASSWORD / UDT_SERVICE (default localhost/udcs).
set -e

: "${UDTHOME:?set UDTHOME to your UniData installation}"
SRC="$(cd "$(dirname "$0")" && pwd)/src"
LG2="${LIBGIT2_PREFIX:-/usr/local}"
LG2LIB="$LG2/lib64"
[ -f "$LG2LIB/libgit2.so" ] || LG2LIB="$LG2/lib"
CC="${CC:-cc}"

"$CC" -std=c11 -O2 -DMVXGIT_UDT \
    -I"$SRC" -I"$LG2/include" -I"$UDTHOME/bin/include" \
    "$SRC/mvxgit.c" "$SRC/udtgit_rt.c" "$SRC/udt-git.c" \
    -L"$LG2LIB" -Wl,-rpath,"$LG2LIB" -lgit2 \
    -L"$UDTHOME/bin/lib" -luvic \
    -o udt-git

echo "built udt-git"

# The in-session GIT verb's CallC objects: gitcallcb.c (the eight GIT* CallC
# entry points) + the engine (mvxgit.c) + the UniData record backend
# (udtgit_rt.c).  Shipped in udt-callc/ so MVPKG's CALLC op aggregates them into
# UniData's libu2callc.so on install (with udt-callc/funcs + libs).  The
# git-object path never opens the lazily-created InterCall session, so the verb
# uses no second session / licence.
mkdir -p udt-callc
for c in gitcallcb mvxgit udtgit_rt; do
    "$CC" -m64 -fPIC -O2 -DMVXGIT_UDT \
        -I"$SRC" -I"$LG2/include" -I"$UDTHOME/bin/include" \
        -c "$SRC/$c.c" -o "udt-callc/$c.o"
done
echo "built udt-callc/{gitcallcb,mvxgit,udtgit_rt}.o"
