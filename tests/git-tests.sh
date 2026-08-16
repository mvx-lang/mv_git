#!/usr/bin/env bash
# mv_git — comprehensive git test suite, driven through the GIT verb so it runs
# identically on MVX (mvx-git), UniData (udt-git) and UniVerse (uv-git).
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only.
#
# Assertion-based (not golden files), so it is platform-agnostic and legible in
# CI logs.  Requires a BUILT git package and a runtime that can run its verbs:
#
#   MVX      = the TCL / runtime            (mvx  |  udt-git's host — see below)
#   MVXC     = the BASIC compiler           (mvx-basic)      [MVX builds only]
#   GITPKG   = path to the built git package (has LIB/ + VOC/ + BP/ cataloged)
#   PLATFORM = mvx | udt | uv               (default mvx)
#   UVGIT    = the uv-git binary            [PLATFORM=uv only]
#   SKIP_NET = 1 to skip network ops (clone/fetch/pull/push) — e.g. a libgit2
#              built without https, or an offline runner
#
# Exit non-zero if any assertion fails.
set -u
PLATFORM="${PLATFORM:-mvx}"
SKIP_NET="${SKIP_NET:-0}"
: "${MVX:?set MVX to the runtime (mvx)}"
: "${GITPKG:?set GITPKG to the built git package dir}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
PASS=0; FAIL=0; SKIP=0

say()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  FAIL %s\n     expected: %s\n     actual:   %s\n' "$1" "$2" "$3"; }
skip() { SKIP=$((SKIP+1)); printf '  skip %s (%s)\n' "$1" "$2"; }
# t NAME EXPECTED ACTUAL  — substring assertion (EXPECTED must appear in ACTUAL)
t()    { case "$3" in *"$2"*) ok "$1";; *) bad "$1" "$2" "$3";; esac; }
# te NAME EXPECTED ACTUAL — exact-equality assertion
te()   { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "$2" "$3"; fi; }

# --- runtime shims: one place that knows mvx vs udt --------------------------
# GITV <acct> <sentence...>  — run a GIT verb sentence in <acct>, echo its output
# SEED <acct> <basic-body>   — write records by compiling+running a tiny program
if [ "$PLATFORM" = mvx ]; then
  : "${MVXC:?set MVXC to mvx-basic}"
  ACCT() { "$GITPKG/../../scripts/mkaccount.sh" "$1" >/dev/null 2>&1 || \
           "$MKACCOUNT" "$1" >/dev/null 2>&1; }
  LINK() { "$MVX" -a "$1" -c "LINK-PKG $GITPKG" >/dev/null 2>&1; }
  CF()   { "$MVX" -a "$1" -c "CREATE-FILE $2" >/dev/null 2>&1; }
  GITV() { local a="$1"; shift; "$MVX" -a "$a" -c "$*" 2>&1; }
  CT()   { "$MVX" -a "$1" -c "CT $2 $3" 2>&1; }
  SEED() { local a="$1" body="$2" b="$WORK/seed.$RANDOM.b"
           printf '%s\n' "$body" > "$b"
           "$MVXC" "$b" -o "$b.bin" >/dev/null 2>&1
           ( cd "$a" && MVXACCOUNT=. "$b.bin" ) >/dev/null 2>&1; }
elif [ "$PLATFORM" = uv ]; then
  # uv: accounts are UniVerse accounts and GIT is cataloged LOCAL into each, so
  # every account needs the package installed rather than picking up a global.
  # An account is born on its first `uv` in the directory, which asks to update
  # RELLEVEL and then for a flavour — Y and 3 (Pick), the flavour the packages
  # target.
  #
  # The verb sentence goes through uv-git rather than a raw session: it fences
  # the verb's own output (GIT -M) so an account's LOGIN paragraph cannot mix
  # its banner into what the assertions read.  That is the same path a user
  # takes from the shell, so the tests exercise the shipped entry point.
  : "${UVGIT:?set UVGIT to the uv-git binary}"
  ACCT() { mkdir -p "$1"; ( cd "$1" && printf 'Y\n3\nQUIT\n' | "$MVX" ) >/dev/null 2>&1; }
  # The package directory IS an account, and install.sh installs into itself —
  # so an account gets git by receiving a copy of the package and running it
  # there.  That is exactly what a user does with the tarball, and it means each
  # test account is independently installed rather than sharing a global verb.
  LINK() { cp -r "$GITPKG"/* "$1"/ 2>/dev/null
           ( cd "$1" && ./install.sh ) >/dev/null 2>&1; }
  # Seven answers, and the seventh is a FILE DESCRIPTION which UniVerse stores
  # in VOC attribute 1 as "F <description>" — leave it EMPTY or the account
  # scan, which matches attribute 1 against "F", cannot see the file.
  CF()   { ( cd "$1" && printf 'CREATE.FILE %s\n1\n2\n3\n1\n2\n19\n\nQUIT\n' "$2" | "$MVX" ) >/dev/null 2>&1; }
  # strip the leading verb: uv-git takes the subcommand, not the whole sentence
  GITV() { local a="$1"; shift; local s="$*"; "$UVGIT" -a "$a" ${s#GIT } 2>&1; }
  CT()   { ( cd "$1" && printf 'CT %s %s\nQUIT\n' "$2" "$3" | "$MVX" ) 2>&1; }
  SEED() { local a="$1" body="$2"
           printf '%s\n' "$body" > "$a/BP/SEEDT"
           ( cd "$a" && printf 'BASIC BP SEEDT\nRUN BP SEEDT\nQUIT\n' | "$MVX" ) >/dev/null 2>&1; }
else
  # udt: the runtime IS udt; GIT is a cataloged verb; accounts are UniData accounts.
  ACCT() { "$UDT_NEWACCT" "$1"; }             # provided by the udt runner env
  LINK() { :; }                               # udt-git installs globally
  CF()   { echo "CREATE.FILE $2 DYNAMIC" | ( cd "$1" && "$MVX" ) >/dev/null 2>&1; }
  GITV() { local a="$1"; shift; ( cd "$a" && echo "$*" | "$MVX" ) 2>&1; }
  CT()   { ( cd "$1" && echo "CT $2 $3" | "$MVX" ) 2>&1; }
  SEED() { local a="$1" body="$2"; ( cd "$a" && printf '%s\nSAVEDLIST\n' "$body" | "$MVX" ) >/dev/null 2>&1; }
fi

# ---------------------------------------------------------------------------
say "== mv_git comprehensive suite — platform=$PLATFORM  net=$([ "$SKIP_NET" = 1 ] && echo off || echo on)"

A="$WORK/A"; ACCT "$A"; LINK "$A"; CF "$A" CUST
SEED "$A" 'OPEN "CUST" TO F ELSE STOP
WRITE "Ada":@AM:"London" ON F, "C1"'

say "-- lifecycle: init / config / add -A / status / commit / log --"
t  "init"        "repository"        "$(GITV "$A" GIT INIT)"
GITV "$A" GIT CONFIG user.name Test >/dev/null
GITV "$A" GIT CONFIG user.email test@example.com >/dev/null
GITV "$A" GIT CONFIG mvx.openaccount true >/dev/null
te "config get"  "Test"              "$(GITV "$A" GIT CONFIG user.name | tr -d '\r\n')"
t  "add -A"      "staged"            "$(GITV "$A" GIT ADD -A)"
t  "commit"      "["                 "$(GITV "$A" GIT COMMIT -m base)"
t  "log"         "base"              "$(GITV "$A" GIT LOG)"
t  "status clean" "clean"            "$(GITV "$A" GIT STATUS)"
t  "record round-trips" "London"     "$(CT "$A" CUST C1)"

say "-- change / show / diff / restore --"
SEED "$A" 'OPEN "CUST" TO F ELSE STOP
WRITE "Ada":@AM:"Paris" ON F, "C1"'
t  "status modified" "C1"            "$(GITV "$A" GIT STATUS)"
t  "show committed"  "London"        "$(GITV "$A" GIT SHOW CUST C1)"
GITV "$A" GIT RESTORE CUST >/dev/null
t  "restore reverts" "London"        "$(CT "$A" CUST C1)"

say "-- branch / checkout / cherry-pick / merge --"
SEED "$A" 'OPEN "CUST" TO F ELSE STOP
WRITE "Bob":@AM:"Rome" ON F, "C2"'
GITV "$A" GIT ADD -A >/dev/null; GITV "$A" GIT COMMIT -m c2 >/dev/null
t  "branch create"   "site"          "$(GITV "$A" GIT BRANCH site; GITV "$A" GIT BRANCH)"
t  "checkout"        "site"          "$(GITV "$A" GIT CHECKOUT site)"

say "-- tag: create / list / annotated / delete --"
GITV "$A" GIT CHECKOUT main >/dev/null 2>&1 || GITV "$A" GIT CHECKOUT master >/dev/null 2>&1
t  "tag create"      "tagged"        "$(GITV "$A" GIT TAG v1.0)"
t  "tag annotated"   "tagged"        "$(GITV "$A" GIT TAG -a v2.0 -m release-two)"
t  "tag list"        "v1.0"          "$(GITV "$A" GIT TAG)"
t  "tag delete"      "deleted"       "$(GITV "$A" GIT TAG -d v1.0)"
te "tag gone"        "v2.0"          "$(GITV "$A" GIT TAG | tr -d '\r' | paste -sd, -)"

say "-- ignore: a gitignored record stays out --"
printf 'CUST/C9\n' > "$A/.gitignore"
SEED "$A" 'OPEN "CUST" TO F ELSE STOP
WRITE "x" ON F, "C9"'
case "$(GITV "$A" GIT STATUS)" in *CUST/C9*) bad "ignore hides record" "no CUST/C9" "shown";; *) ok "ignore hides record";; esac

if [ "$SKIP_NET" = 1 ]; then
  skip "remote/clone/fetch/pull/push" "SKIP_NET=1"
else
  say "-- remotes: bare remote + push + clone + pull (fast-forward) --"
  REM="$WORK/rem.git"; git init --bare -q "$REM"
  GITV "$A" GIT REMOTE ADD origin "$REM" >/dev/null
  t  "remote list"   "origin"        "$(GITV "$A" GIT REMOTE)"
  t  "push"          "pushed"        "$(GITV "$A" GIT PUSH origin main || GITV "$A" GIT PUSH origin master)"
  B="$WORK/B"
  # clone via the engine (GITCLONE) into a materialised account
  clone_out="$(GITV "$A" GIT CLONE "$REM" "$B")"
  t  "clone"         "cloned"        "$clone_out"
  LINK "$B"
  GITV "$B" GIT CONFIG user.name Test >/dev/null
  GITV "$B" GIT CONFIG user.email test@example.com >/dev/null
  # A adds C3, pushes; B pulls -> fast-forward re-materialises
  SEED "$A" 'OPEN "CUST" TO F ELSE STOP
WRITE "Cy":@AM:"Oslo" ON F, "C3"'
  GITV "$A" GIT ADD -A >/dev/null; GITV "$A" GIT COMMIT -m c3 >/dev/null
  GITV "$A" GIT PUSH origin main >/dev/null 2>&1 || GITV "$A" GIT PUSH origin master >/dev/null 2>&1
  t  "pull fast-forward" "fast-forward" "$(GITV "$B" GIT PULL origin main 2>&1 || GITV "$B" GIT PULL origin master 2>&1)"
  t  "pulled record"  "Oslo"         "$(CT "$B" CUST C3)"
fi

say "== $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
