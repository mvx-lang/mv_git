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

# ---- stage the release as a UniData account dir named 'git' -----------------
# The tar wraps this one dir, so the tarball unpacks to ./git/ — a self-sufficient
# UniData account (install.sh runs `newacct` on it) you LOGTO and catalog from,
# exactly as an MVPKG-installed package is an account.  MVPKG consumes the same
# tarball with `tar --strip-components=1`, landing the contents at its store root.
#   BP/GIT           the in-session verb source (compiled + GLOBALLY cataloged)
#   udt-callc/       CallC objects + funcs + libs — aggregated into libu2callc.so
#   install.sh       standalone host installer (preflight, CallC, global catalog)
#   udt-callc-build.sh   the CallC aggregator, vendored so standalone needs no MVPKG
#   mvpkg.json/PKG   package metadata (name mvx-lang/git — the convergence key)
# GIT.udt.b + udt-git are also kept at the root for a bare manual install.
ARCH="$(uname -m)"; OS="$(uname -s | tr '[:upper:]' '[:lower:]')"; case "$OS" in *linux*) OS=linux ;; esac
ACCT="$STAGE/git"
mkdir -p "$ACCT/BP" "$ACCT/udt-callc"
# The GIT verb is one $IFDEF source in BP/GIT: MVX takes the CMD-dispatch branch,
# UniData takes the $ELSE (Model B) branch — so udt compiles the same file.
cp BP/GIT "$ACCT/BP/GIT"
cp BP/GIT "$ACCT/GIT.udt.b"
cp udt/preflight.sh "$ACCT/preflight.sh"
cp udt/install.sh "$ACCT/install.sh";            chmod +x "$ACCT/install.sh"
cp udt/udt-callc-build.sh "$ACCT/udt-callc-build.sh"; chmod +x "$ACCT/udt-callc-build.sh"
cp udt-git "$ACCT/"
cp mvpkg.json PKG LICENSE README.md "$ACCT/" 2>/dev/null || true
cp udt-callc/*.o udt-callc/funcs "$ACCT/udt-callc/" 2>/dev/null || true
# Generate udt-callc/libs from the ACTUAL libgit2 link flags this build used, so
# the CallC library links the SAME libgit2 the udt-git binary does (a static
# shipped libs would drift — e.g. an EPEL 1.7 build must not carry a 1.9 flag).
printf '%s\n' "$LG2LIBS" > "$ACCT/udt-callc/libs"
cat > "$ACCT/INSTALL.txt" <<EOF
mv_git for Rocket UniData ($OS/$ARCH).  This directory IS the package/account.

Easiest:  MVPKG install mvx-lang/git   (if you run the MVPKG client)

Standalone (no MVPKG), from inside this directory:
  1. Install the host prerequisites.  The runtime libgit2 this binary links
     (libgit2.so.1.7 on EL8) comes straight from EPEL; building libu2callc.so
     for the in-session GIT verb also needs a C compiler and ncurses' linker
     lib (UniData's CallC link pulls -lncurses):
       sudo dnf install epel-release
       sudo dnf install libgit2_1.7 gcc ncurses-devel
     (RHEL/Rocky 8 base libgit2 is 0.26, too old.  A build against another
     libgit2 needs that series instead — preflight.sh prints the exact soname.
     The udt-git CLI alone needs only libgit2; gcc/ncurses-devel are for the
     in-session verb's CallC library.)
  2. Run the installer:
       ./install.sh
     It validates the host (preflight.sh), installs the udt-git CLI to
     /usr/local/bin and GIT.udt.b to \$UDTHOME/lib/mvgit, runs 'newacct' on
     THIS dir so it is a real account, builds libu2callc.so with the GIT*
     CallC functions, and compiles + GLOBALLY catalogs the GIT verb — so
     'GIT status' works in every account.  Needs sudo (writes \$UDTHOME).
  3. Per account:  udt-git -a <account> init   (the verb + lib are global).

Adopting into MVPKG later:  run 'udt-git register' (or any 'udt-git init'
once MVPKG is set up).  install.sh stages the CallC fragment under the
package name mvx-lang/git — identical to an MVPKG-native install — so a
standalone box that later adopts MVPKG ends up in the same state as one
where MVPKG installed git from the start.

Run:  udt-git -a <account> <clone|status|...>  |  GIT <status|...>
Session env: UDT_HOST / UDT_USER / UDT_PASSWORD / UDT_SERVICE.
EOF
echo "build-udt: staged the udt-git package as ./git/"
