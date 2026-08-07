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
# Requires: a C compiler; libgit2 under $LIBGIT2_PREFIX (/usr/local); and UniData
# ($UDTHOME with InterCall headers + libuvic) — all in the container.
#
# libgit2 version: minimum 1.0.0 to build (the engine uses git_blob_create_from_buffer,
# introduced in 1.0).  The compiled udt-git dynamically links libgit2.so.<major.minor>
# for whatever it is built against, and a different major.minor will NOT load at
# runtime — so build and run against the same series.  Official EL8 releases build
# against EPEL's libgit2_1.7 (soname .so.1.7) so end users install the runtime with
# a plain `dnf install libgit2_1.7` — no source build.  See LIBGIT2_LIBS below.
# (Rocky/RHEL 8 *base* ships only 0.26, which is too old; EPEL supplies 1.7.)
#
#   sh build-udt.sh <stagedir>
set -e
STAGE="${1:?usage: build-udt.sh <stagedir>}"
: "${UDTHOME:?set UDTHOME to your UniData installation}"
SRC="$(cd "$(dirname "$0")" && pwd)/src"
CC="${CC:-cc}"
UGVER="${UDTGIT_VERSION:-${GITHUB_REF_NAME:-0}}"   # stamped for MVPKG self-registration

# libgit2 build flags.  Two ways to supply them:
#   * explicit LIBGIT2_CFLAGS / LIBGIT2_LIBS — for a distro/EPEL versioned lib
#     that has no unversioned libgit2.so or pkgconfig.  E.g. building against
#     EPEL 8's libgit2_1.7 (so end users just `dnf install libgit2_1.7`):
#       LIBGIT2_LIBS='-l:libgit2.so.1.7'   (headers are the standard /usr/include)
#     The lib is then on the default loader path, so no rpath is added.
#   * else a prefix install under LIBGIT2_PREFIX (default /usr/local) — a
#     from-source 1.9.x build — linked with -lgit2 and an rpath to its libdir.
if [ -n "${LIBGIT2_LIBS:-}" ]; then
    LG2CFLAGS="${LIBGIT2_CFLAGS:-}"
    LG2LIBS="$LIBGIT2_LIBS"
else
    LG2="${LIBGIT2_PREFIX:-/usr/local}"
    LG2LIB="$LG2/lib64"
    [ -f "$LG2LIB/libgit2.so" ] || LG2LIB="$LG2/lib"
    LG2CFLAGS="-I$LG2/include"
    LG2LIBS="-L$LG2LIB -Wl,-rpath,$LG2LIB -lgit2"
fi

"$CC" -std=c11 -O2 -DMVXGIT_UDT -DUDTGIT_VERSION="\"$UGVER\"" \
    -I"$SRC" $LG2CFLAGS -I"$UDTHOME/bin/include" \
    "$SRC/mvxgit.c" "$SRC/udtgit_rt.c" "$SRC/udt-git.c" \
    $LG2LIBS \
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
        -I"$SRC" $LG2CFLAGS -I"$UDTHOME/bin/include" \
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
cp udt/preflight.sh "$STAGE/preflight.sh"
cp udt-git "$STAGE/"
cp mvpkg.json PKG LICENSE README.md "$STAGE/" 2>/dev/null || true
cp udt-callc/*.o udt-callc/funcs udt-callc/libs "$STAGE/udt-callc/" 2>/dev/null || true
cat > "$STAGE/INSTALL.txt" <<EOF
mv_git for Rocket UniData ($OS/$ARCH).

The easy path is 'MVPKG install mvx-lang/git' — it builds the CallC
library (from udt-callc/), globally catalogs the in-session GIT verb
(BP/GIT), and deploys it into the account (mvpkg.json deploy.verbs).

Manual/standalone install:
  1. Install the libgit2 runtime this binary links (see step 3), then
     validate the host — MVPKG would do this for you:
       UDTHOME=$UDTHOME sh preflight.sh
     It reports the exact libgit2 soname needed; fix any FAIL first.
  2. Copy udt-git AND GIT.udt.b together onto the host PATH
     (e.g. both into \$UDTHOME/bin) — udt-git finds GIT.udt.b beside
     itself (or via \$MVGIT_VERB / \$UDTHOME/lib/mvgit/GIT.udt.b).
  3. Ensure the matching libgit2 is present at runtime.  This EL8 build
     links libgit2.so.1.7, from EPEL:
       sudo dnf install epel-release && sudo dnf install libgit2_1.7
     (A different major.minor will not load; RHEL/Rocky 8 base is 0.26,
     too old.  'preflight.sh' prints the exact soname this binary needs.)
'udt-git init'/'clone' then set up the in-session GIT verb in the
account (needs the CallC library built).

If you installed standalone and later add MVPKG, run 'udt-git register'
(or any 'udt-git init') so git registers itself with MVPKG — it is then
managed like an MVPKG-installed package (MVPKG list / update).

Run:  udt-git -a <account> <clone|status|...>  |  GIT <status|...>
Session env: UDT_HOST / UDT_USER / UDT_PASSWORD / UDT_SERVICE.
EOF
echo "build-udt: staged the udt-git package"
