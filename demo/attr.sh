#!/usr/bin/env bash
# GIT ATTR walkthrough on MVX (mv_git#15) — the attribute editor, end to end.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see ../LICENSE).
#
# Stands up a throwaway account and shows each thing the editor does: both
# scopes, --set/--unset, that it stages and does NOT commit, that an ordinary
# `add -A` cannot undo it, the registry refusing bad values, a file that is not
# in the account at all, drift and --sync, and the full-screen editor driven
# from a pipe.  Every command it runs is echoed, so the transcript IS the
# documentation.
#
# The account is left standing at the end, so you can carry on in it by hand —
# which is the only way to try the editor with real arrow keys.
#
#   bash demo/attr.sh [workdir]           default: a fresh mktemp -d
#
# Needs a built mvx and a built git package:
#   cmake --build build && ./scripts/mkpkg.sh packages/git
set -u
PKG="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="${MVX_ROOT:-$(cd "$PKG/../.." && pwd)}"
MVX="$ROOT/build/bin/mvx"
W="${1:-$(mktemp -d)}"; A="$W/demo"
say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
run() { printf '\033[36m$ GIT %s\033[0m\n' "$*"; "$MVX" -a "$A" -c "GIT $*" 2>&1; }
blob(){ printf '\033[36m$ git cat-file -p :%s\033[0m\n' "$1"; ( cd "$A" && git cat-file -p ":$1" 2>&1 ); }

say "a fresh account with one file, committed in the open (portable) format"
"$ROOT/scripts/mkaccount.sh" "$A" >/dev/null 2>&1
"$MVX" -a "$A" -c "LINK-PKG $ROOT/packages/git" >/dev/null 2>&1
"$MVX" -a "$A" -c "CREATE-FILE CUST"            >/dev/null 2>&1
run INIT
run CONFIG mvx.openaccount 1
run ADD -A -o
run COMMIT -m base

say "1. what git records, at both scopes"
run ATTR
run ATTR CUST

say "2. set a file's geometry.  [UDT]/[UV] rows apply on those platforms —"
say "   in the OPEN form they are editable anyway, which is the point of it"
run ATTR CUST --set modulo=997 --set dynamic=yes --set minmod=11
blob 'CUST.DICT/%FILE%'

say "3. it staged, it did NOT commit"
run STATUS
run LOG

say "4. an ordinary add -A does not undo it"
run ADD -A
blob 'CUST.DICT/%FILE%'
run COMMIT -m geometry

say "5. the registry is what makes a value legal"
run ATTR CUST --set dynamic=maybe
run ATTR CUST --set modulo=abc
run ATTR CUST --set wibble=1

say "6. attributes live in the git objects, so the file need not be here"
run ATTR ORDERS --set modulo=1009 --set dynamic=yes
run ADD -A
run ATTR ORDERS

say "7. drift: the live file against what is recorded"
say "   (forced here by recording DIR for a file that is really a hash file —"
say "    on udt/uv a real resize shows up the same way, on modulo)"
run ATTR CUST --set type=DIR
run ATTR CUST
run ATTR CUST --sync
run ATTR CUST --sync

say "8. the account scope, and keys the registry does not name survive"
run ATTR --set version=2
blob '.mv-account'

say "9. dynamic lives on the class line beside a modulo, so it needs one"
run ATTR CUST --unset modulo
run ATTR CUST --set dynamic=yes
run ATTR CUST --set modulo=997

say "10. the full-screen editor — jj to 'dynamic', ENTER to cycle, s to stage"
printf '\033[36m$ printf "jj\\ns" | GIT ATTR CUST --edit\033[0m\n'
printf 'jj\ns' | "$MVX" -a "$A" -c "GIT ATTR CUST --edit" 2>&1 \
  | perl -pe 's/\e\[[0-9;?]*[A-Za-z]//g'
run ATTR CUST

printf '\n\033[1maccount left at: %s\033[0m\n' "$A"
