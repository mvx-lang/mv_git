#!/bin/sh
# mv_git — preflight validation for a STANDALONE UniData deploy (outside MVPKG).
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see ../LICENSE).
#
# MVPKG normally verifies a package's runtime before deploying it; a manual
# install skips that.  Run this from the unpacked release directory to check
# that a prebuilt udt-git can run and reach UniData on this host BEFORE you wire
# up the GIT verb:
#
#   sh preflight.sh                    # uses UDTHOME=/usr/ud83 by default
#   UDTHOME=/usr/ud83 sh preflight.sh
#
# Exit 0 if every hard prerequisite passes (warnings are non-fatal), 1 otherwise.
# Hard: udt-git present + all shared libs resolve (incl. libgit2 — the binary
# links a specific libgit2.so.<major.minor>, and a different series will not
# load; the check reports the exact one it needs), and a real UniData home.
# Warn: unirpcd not up, GIT.udt.b missing, session env unset.

fail=0 ; warn=0
ok()   { printf '  ok   %s\n' "$1"; }
bad()  { printf ' FAIL  %s\n' "$1"; fail=1; }
note() { printf ' warn  %s\n' "$1"; warn=1; }

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
UDTHOME="${UDTHOME:-/usr/ud83}"

echo "mv_git (udt-git) standalone-deploy preflight"
echo "--------------------------------------------"

# 1) the udt-git binary (beside this script, or on PATH)
UDTGIT="$HERE/udt-git"
[ -x "$UDTGIT" ] || UDTGIT=$(command -v udt-git 2>/dev/null)
if [ -n "$UDTGIT" ] && [ -x "$UDTGIT" ]; then
  ok "udt-git binary: $UDTGIT"
else
  bad "udt-git binary not found (expected beside this script or on PATH)"
  UDTGIT=""
fi

# 2) shared-library resolution — libgit2 is the one that bites: distros ship a
#    different major.minor than the binary was built against, and libgit2's
#    soname is major.minor, so the wrong series won't load.  Report the exact
#    soname this binary needs, not a hard-coded version.
if [ -n "$UDTGIT" ] && command -v ldd >/dev/null 2>&1; then
  need=$(ldd "$UDTGIT" 2>/dev/null | sed -n 's/^[[:space:]]*\(libgit2\.so[0-9.]*\).*/\1/p' | head -1)
  miss=$(ldd "$UDTGIT" 2>/dev/null | awk '/not found/{print $1}' | tr '\n' ' ')
  if [ -n "$miss" ]; then
    bad "unresolved shared libraries: $miss"
    case "$miss" in *libgit2*)
      vv=$(printf '%s' "$need" | sed -n 's/libgit2\.so\.\([0-9]*\.[0-9]*\).*/\1/p')
      note "  -> this binary needs ${need:-libgit2}; install a matching libgit2"
      [ -n "$vv" ] && note "     (EPEL: dnf install libgit2_${vv} , or build ${vv}.x from source + ldconfig)" ;;
    esac
  else
    ok "all shared libraries resolve"
  fi
  lg=$(ldd "$UDTGIT" 2>/dev/null | awk '/libgit2/{print $3; exit}')
  [ -n "$lg" ] && ok "libgit2: ${need:-libgit2} -> $(readlink -f "$lg" 2>/dev/null || echo "$lg")"
elif [ -n "$UDTGIT" ]; then
  note "ldd unavailable — cannot verify the libgit2 soname; ensure a matching libgit2 is on the runtime path"
fi

# 3) the in-session GIT verb source, co-located or findable
VERB="$MVGIT_VERB"
[ -n "$VERB" ] && [ -f "$VERB" ] || VERB="$HERE/GIT.udt.b"
[ -f "$VERB" ] || VERB="$UDTHOME/lib/mvgit/GIT.udt.b"
if [ -f "$VERB" ]; then
  ok "GIT verb source: $VERB"
else
  note "GIT.udt.b not found beside udt-git / at \$MVGIT_VERB / \$UDTHOME/lib/mvgit"
  note "  -> the udt-git CLI still works; the in-session GIT verb won't deploy without it"
fi

# 4) a real UniData home
if [ -d "$UDTHOME" ] && [ -x "$UDTHOME/bin/udt" ]; then
  ok "UniData home: $UDTHOME"
else
  bad "UDTHOME is not a UniData install ($UDTHOME) — set UDTHOME to your UniData dir"
fi

# 5) UniData up (InterCall sessions go over unirpc)
if pgrep -x unirpcd >/dev/null 2>&1; then
  ok "unirpcd is running (InterCall sessions available)"
else
  note "unirpcd not running — start UniData (startud) before udt-git opens a session"
fi

# 6) the credentials udt-git uses to log a UniData session
if [ -n "$UDT_USER" ] || [ -n "$UDT_HOST" ]; then
  ok "session env set (UDT_HOST=${UDT_HOST:-localhost} UDT_USER=${UDT_USER:-?} UDT_SERVICE=${UDT_SERVICE:-udcs})"
else
  note "UDT_USER/UDT_PASSWORD (and optionally UDT_HOST/UDT_SERVICE) unset —"
  note "  -> udt-git needs them to open a UniData session for its record I/O"
fi

echo "--------------------------------------------"
if [ "$fail" = 0 ]; then
  if [ "$warn" = 0 ]; then echo "PASS — the host is ready to deploy udt-git standalone."
  else echo "PASS (with warnings) — review the 'warn' lines above."; fi
  exit 0
else
  echo "FAIL — resolve the FAIL items above, then re-run."
  exit 1
fi
