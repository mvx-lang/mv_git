#!/bin/sh
# install.sh — standalone host install of udt-git for Rocket UniData (no MVPKG).
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see ../LICENSE).
#
# Run this from the unpacked release directory — which IS a UniData account
# (its BP holds the GIT verb source; `newacct` below turns it into a real one):
#
#     tar xzf mvx-lang_git-<ver>-udt-<os>-<arch>-le.tar.gz
#     cd git
#     ./install.sh                 # needs sudo for $UDTHOME + /usr/local/bin
#
# It does the host-level, once-per-install work — the same the MVPKG "CALLC" +
# global-catalog path does, but for a standalone box:
#   1. udt-git CLI  -> /usr/local/bin ; GIT.udt.b -> $UDTHOME/lib/mvgit
#   2. `newacct` this dir so udt can LOGTO it (keeps the shipped BP/GIT)
#   3. build libu2callc.so so the 8 GIT* CallC functions resolve (system-wide)
#   4. compile + GLOBALLY catalog the GIT verb from inside this account, so
#      `GIT status` works in every account — per account you then only `udt-git init`.
#
# Env: UDTHOME (default /usr/ud83), UDT_OWNER/UDT_GROUP (newacct owner; default
# the invoking user), LANG (a UniData-recognised locale; default en_US.UTF-8 —
# NOT C.UTF-8, which udt rejects).  Needs passwordless (or interactive) sudo.
set -e

UDTHOME="${UDTHOME:-/usr/ud83}"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LANG_OK="${LANG:-en_US.UTF-8}"; case "$LANG_OK" in C.UTF-8|POSIX|C) LANG_OK=en_US.UTF-8 ;; esac
OWNER="${UDT_OWNER:-${SUDO_USER:-$(id -un)}}"
GROUP="${UDT_GROUP:-$(id -gn "$OWNER" 2>/dev/null || id -gn)}"
UDT="$UDTHOME/bin/udt"

say() { printf 'install: %s\n' "$1"; }
die() { printf 'install: %s\n' "$1" >&2; exit 1; }
[ -d "$UDTHOME" ] && [ -x "$UDT" ] || die "UDTHOME=$UDTHOME is not a UniData install (set UDTHOME)"
command -v sudo >/dev/null 2>&1 || die "sudo not found — needed for \$UDTHOME and /usr/local/bin writes"
# Building libu2callc.so compiles the UniData CallC glue (gencdef/genefs/genfunc
# emit C, which gcc then compiles + links), so a C compiler is a hard prereq for
# the in-session GIT verb — the udt-git CLI alone does not need it.
command -v gcc >/dev/null 2>&1 || die "gcc not found — the CallC build needs a C compiler: sudo dnf install -y gcc"
# UniData's CallC link pulls -lncurses (and -lm/-lrt/-ldl, which gcc's glibc-devel
# provides); ncurses ships its linker symlink only in ncurses-devel.  Probe it by
# actually trying to link, so the message is right whatever the distro layout.
printf 'int main(void){return 0;}\n' | gcc -xc - -lncurses -o /dev/null 2>/dev/null \
  || die "the CallC link needs -lncurses — install ncurses-devel: sudo dnf install -y ncurses-devel"

# 0) validate the host (libgit2, shared libs, UniData up) — same check MVPKG runs
say "preflight"
UDTHOME="$UDTHOME" sh "$HERE/preflight.sh" || die "preflight failed — fix the FAIL lines above"

# 1) the CLI on PATH, and the verb source at the canonical fallback spot
say "installing udt-git -> /usr/local/bin, GIT.udt.b -> $UDTHOME/lib/mvgit"
sudo install -m 755 "$HERE/udt-git" /usr/local/bin/udt-git
sudo mkdir -p "$UDTHOME/lib/mvgit"
sudo cp "$HERE/GIT.udt.b" "$UDTHOME/lib/mvgit/GIT.udt.b"

# 2) make this dir a real UniData account (idempotent; keeps the shipped BP/GIT)
if [ -e "$HERE/VOC" ]; then
  say "account already provisioned (VOC present)"
else
  say "provisioning UniData account here (owner $OWNER:$GROUP)"
  ( cd "$HERE" && printf 'y\n%s\n%s\n' "$OWNER" "$GROUP" | "$UDTHOME/bin/newacct" ) >/dev/null \
    || die "newacct failed — run it by hand in $HERE to see the prompt"
fi

# 3) rebuild libu2callc.so so the GIT* CallC functions resolve (writes $UDTHOME).
#    CONVERGENCE INVARIANT: stage the fragment under the PACKAGE name mvx-lang/git
#    (-> $UDTHOME/callc.d/mvx-lang_git), exactly as MVPKG does (MVPKG.ONE passes
#    the package's meta name to `udt-callc-build add`).  Keying it on the unpack
#    dir name ("git") instead would leave a standalone box that later adopts
#    MVPKG with a DIFFERENT fragment than an MVPKG-native install — the two must
#    be byte-identical.  The MVPKG manifests themselves are written on adoption
#    by `udt-git register` (UDTGIT_PKG == "mvx-lang/git"), not here.
PKG="mvx-lang/git"
say "building libu2callc.so with the GIT CallC functions (fragment: $PKG)"
# $UDTHOME/bin holds the generators (gencdef/genefs/genfunc) the builder calls bare.
UDTHOME="$UDTHOME" PATH="$UDTHOME/bin:$PATH" sh "$HERE/udt-callc-build.sh" add "$HERE" "$PKG"

# 4) compile + catalog the GIT verb.  UniData catalogs GLOBALLY by default — there
#    is NO "GLOBAL" keyword (`CATALOG BP GIT GLOBAL` would try to catalog a second
#    program literally named GLOBAL and fail).  A global catalog entry
#    ($UDTHOME/sys/CTLG/<c>/GIT) makes GIT resolve as a verb in EVERY account with
#    no per-account VOC record.  Run as the invoking user: UniData keeps the
#    catalog dirs user-writable so any account can catalog (unlike $UDTHOME/bin,
#    which is why step 3 needs sudo).  Verify by the catalog ENTRY, not udt's exit
#    code — a piped `udt` exits 0 even when an internal command (CATALOG) fails.
say "compiling + globally cataloging the GIT verb"
( cd "$HERE" && printf 'BASIC BP GIT\nCATALOG BP GIT FORCE\n' | LANG="$LANG_OK" TERM=dumb "$UDT" ) >/dev/null 2>&1 || true
if ls "$UDTHOME"/sys/CTLG/*/GIT >/dev/null 2>&1; then
  say "GIT cataloged globally -> $(ls "$UDTHOME"/sys/CTLG/*/GIT 2>/dev/null | head -1)"
else
  die "global catalog of GIT failed — run by hand:
       (cd '$HERE' && printf 'BASIC BP GIT\\nCATALOG BP GIT FORCE\\n' | LANG=$LANG_OK $UDT)
     (needs a udt-recognised LANG — not C.UTF-8 — and write to $UDTHOME/sys/CTLG)"
fi

cat <<EOF
install: done.
  * udt-git CLI:        $(command -v udt-git 2>/dev/null || echo /usr/local/bin/udt-git)
  * in-session verb:    GIT (global) — works in every account
  * CallC library:      $UDTHOME/bin/libu2callc.so
Next, per account:  udt-git -a <account> init   (then add / commit / status / log)
EOF
