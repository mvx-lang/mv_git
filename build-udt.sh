#!/bin/sh
# build-udt.sh — build udt-git (Rocket UniData) and stage its release package.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
#
# Runs INSIDE the udt-builder container (driven by the udt-build action's
# build-release.sh / udt-run).  The engine (src/mvxgit.c) is shared with mvx-git;
# here it is compiled with -DMVXGIT_UDT so its record primitives bind to the
# UniData InterCall backend (src/udtgit_rt.c) instead of libmvxrt.  Produces
# ./udt-git and udt-callc/*.o, then stages the full release tree (contents at the
# root) into $1.  The action tars $1 as
# mvx-lang_git-<ver>-udt-<os>-<arch>-<endian>.tar.gz and chowns it back.
#
# Requires: a C compiler; libgit2 1.9.x under $LIBGIT2_PREFIX (/usr/local); and
# UniData ($UDTHOME with InterCall headers + libuvic) — all in the container.
#
#   sh build-udt.sh <stagedir>
set -e
STAGE="${1:?usage: build-udt.sh <stagedir>}"
: "${UDTHOME:?set UDTHOME to your UniData installation}"
SRC="$(cd "$(dirname "$0")" && pwd)/src"
LG2="${LIBGIT2_PREFIX:-/usr/local}"
LG2LIB="$LG2/lib64"
[ -f "$LG2LIB/libgit2.so" ] || LG2LIB="$LG2/lib"
CC="${CC:-cc}"
UGVER="${UDTGIT_VERSION:-${GITHUB_REF_NAME:-0}}"   # stamped for MVPKG self-registration

"$CC" -std=c11 -O2 -DMVXGIT_UDT -DUDTGIT_VERSION="\"$UGVER\"" \
    -I"$SRC" -I"$LG2/include" -I"$UDTHOME/bin/include" \
    "$SRC/mvxgit.c" "$SRC/udtgit_rt.c" "$SRC/udt-git.c" \
    -L"$LG2LIB" -Wl,-rpath,"$LG2LIB" -lgit2 \
    -L"$UDTHOME/bin/lib" -luvic \
    -o udt-git
echo "built udt-git"

# The in-session GIT verb's CallC objects: gitcallcb.c (the eight GIT* CallC
# entry points) + the engine (mvxgit.c) + the UniData record backend
# (udtgit_rt.c).  Shipped in udt-callc/ so MVPKG's CALLC op aggregates them into
# UniData's libu2callc.so on install (with udt-callc/funcs + libs).
mkdir -p udt-callc
for c in gitcallcb mvxgit udtgit_rt; do
    "$CC" -m64 -fPIC -O2 -DMVXGIT_UDT \
        -I"$SRC" -I"$LG2/include" -I"$UDTHOME/bin/include" \
        -c "$SRC/$c.c" -o "udt-callc/$c.o"
done
echo "built udt-callc/{gitcallcb,mvxgit,udtgit_rt}.o"

# ---- stage the release into $STAGE (contents at the root) -------------------
# A proper MVPKG package:
#   BP/GIT       the in-session verb source — CATGLOBAL compiles + globally
#                catalogs it (after CALLC registers its functions), then
#                deploy.verbs:[GIT] points the account's VOC at it.
#   udt-callc/   the CallC objects + funcs/libs — CALLC aggregates them into
#                libu2callc.so so GITINIT/GITSTAGE/... resolve.
#   mvpkg.json   the manifest (its deploy block drives the verb deploy).
# GIT.udt.b + udt-git are also kept at the root for a manual/standalone install
# (udt-git finds GIT.udt.b beside itself; see INSTALL.txt).
ARCH="$(uname -m)"; OS="$(uname -s | tr '[:upper:]' '[:lower:]')"; case "$OS" in *linux*) OS=linux ;; esac
mkdir -p "$STAGE/BP" "$STAGE/udt-callc"
cp udt/GIT.udt.b "$STAGE/BP/GIT"
cp udt/GIT.udt.b "$STAGE/GIT.udt.b"
cp udt-git "$STAGE/"
cp mvpkg.json PKG LICENSE README.md "$STAGE/" 2>/dev/null || true
cp udt-callc/*.o udt-callc/funcs udt-callc/libs "$STAGE/udt-callc/" 2>/dev/null || true
cat > "$STAGE/INSTALL.txt" <<EOF
mv_git for Rocket UniData ($OS/$ARCH).

The easy path is 'MVPKG install mvx-lang/git' — it builds the CallC
library (from udt-callc/), globally catalogs the in-session GIT verb
(BP/GIT), and deploys it into the account (mvpkg.json deploy.verbs).

Manual/standalone install:
  1. Copy udt-git AND GIT.udt.b together onto the host PATH
     (e.g. both into \$UDTHOME/bin) — udt-git finds GIT.udt.b beside
     itself (or via \$MVGIT_VERB / \$UDTHOME/lib/mvgit/GIT.udt.b).
  2. Ensure libgit2 1.9.x is present at runtime (/usr/local/lib64).
'udt-git init'/'clone' then set up the in-session GIT verb in the
account (needs the CallC library built).

If you installed standalone and later add MVPKG, run 'udt-git register'
(or any 'udt-git init') so git registers itself with MVPKG — it is then
managed like an MVPKG-installed package (MVPKG list / update).

Run:  udt-git -a <account> <clone|status|...>  |  GIT <status|...>
Session env: UDT_HOST / UDT_USER / UDT_PASSWORD / UDT_SERVICE.
EOF
echo "build-udt: staged the udt-git package"
