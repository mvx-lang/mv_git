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
#   UDT_NEWACCT = UniData's newacct          [PLATFORM=udt only, default
#                                             $UDTHOME/bin/newacct]
#   SKIP_NET = 1 to skip network ops (clone/fetch/pull/push) — e.g. a libgit2
#              built without https, or an offline runner
#
# Exit non-zero if any assertion fails.
set -u
PLATFORM="${PLATFORM:-mvx}"
UDT_NEWACCT="${UDT_NEWACCT:-${UDTHOME:-/usr/ud83}/bin/newacct}"
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
  DF()   { "$MVX" -a "$1" -c "DELETE-FILE $2" >/dev/null 2>&1; }
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
  # An account that is going to be version-controlled says what it IS: the
  # descriptor carries the VOC flavour, which UniVerse records nowhere readable
  # and a clone therefore cannot recover any other way (mv_git#15, #52).  Real
  # users get this from `uv-git adopt`; here it is written directly.
  ACCT() { mkdir -p "$1"; ( cd "$1" && printf 'Y\n3\nQUIT\n' | "$MVX" ) >/dev/null 2>&1
           printf '# UV account descriptor\nname = %s\nversion = 1\nflavour = PICK\n' \
                  "$(basename "$1")" > "$1/.uv"; }
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
  # DELETE.FILE confirms before it acts; an unanswered prompt leaves the file.
  DF()   { ( cd "$1" && printf 'DELETE.FILE %s\nY\nY\nQUIT\n' "$2" | "$MVX" ) >/dev/null 2>&1; }
  # strip the leading verb: uv-git takes the subcommand, not the whole sentence
  # TWO WAYS IN, and both are tested, because both ship (DECISIONS.md).
  #
  #   UV_VIA=verb (the DEFAULT) drives the in-session GIT verb, exactly as the
  #     mvx and udt shims do — and it is the default because it is what most MV
  #     developers use: they work at the TCL prompt and never see a unix shell.
  #     It also costs no extra licence, since the session is already theirs.
  #   UV_VIA=cli drives uv-git from the shell, which is the route a build
  #     script, a CI job or a multi-account operation takes.
  #
  # Testing only one of them is how the primary interface goes unexercised: for
  # a while this file tested the CLI on UniVerse and the verb everywhere else.
  if [ "${UV_VIA:-verb}" = cli ]; then
    GITV() { local a="$1"; shift; local s="$*"; "$UVGIT" -a "$a" ${s#GIT } 2>&1; }
  else
    # `GIT -M` fences the verb's own output with <<<GIT-BEGIN>>>/<<<GIT-END>>>,
    # which is precisely what makes a session's output assertable: UniVerse
    # greets every session and prompts between commands, and an exact-match
    # assertion against a banner fails no matter how right the verb was.  That
    # is what the fence is FOR, so the tests use it rather than scraping.
    GITV() { local a="$1"; shift; local s="$*"
             ( cd "$a" && printf 'GIT -M %s\nQUIT\n' "${s#GIT }" | "$MVX" ) 2>&1 \
               | awk '/<<<GIT-BEGIN>>>/{f=1;next} /<<<GIT-END>>>/{f=0} f'; }
  fi
  CT()   { ( cd "$1" && printf 'CT %s %s\nQUIT\n' "$2" "$3" | "$MVX" ) 2>&1; }
  SEED() { local a="$1" body="$2"
           printf '%s\n' "$body" > "$a/BP/SEEDT"
           ( cd "$a" && printf 'BASIC BP SEEDT\nRUN BP SEEDT\nQUIT\n' | "$MVX" ) >/dev/null 2>&1; }
else
  # udt: the runtime IS udt; GIT is a cataloged verb; accounts are UniData accounts.
  #
  # PREFLIGHT: THE LICENCE MUST BE FREE BEFORE ANY OF THIS MEANS ANYTHING.
  # UniData TE licenses two concurrent sessions, and a session that dies leaves a
  # PHANTOM entry — `listuser` showing a udt user with no process — which holds a
  # slot until deleteuser clears it.  Once the slots are gone every failure is
  # misleading: what the reader sees is "the account I/O agent did not answer",
  # and three separate wrong diagnoses of mv_git#54 came from measuring against
  # an exhausted licence without knowing it.  So: say what the table holds, and
  # clear entries no live process owns.
  UDTBIN="${UDTHOME:-/usr/ud83}/bin"
  udt_users() { "$UDTBIN/listuser" 2>/dev/null | awk '/\) *\/ *[0-9]/{print $NF; exit}'; }
  if [ -x "$UDTBIN/listuser" ]; then
    n=$(udt_users); n=${n:-0}
    if [ "$n" -gt 0 ] && ! pgrep -x udt >/dev/null 2>&1; then
      say "-- licence: $n phantom session(s) with no process; clearing --"
      "$UDTBIN/listuser" 2>/dev/null | awk 'NR>3 && NF {print $1}' | while read -r u; do
        [ -n "$u" ] && yes 2>/dev/null | "$UDTBIN/deleteuser" "$u" >/dev/null 2>&1
      done
      n=$(udt_users); n=${n:-0}
    fi
    say "-- licence: $n session(s) in use before the run --"
    [ "$n" -gt 0 ] && say "   (a busy licence makes every failure below suspect — see mv_git#54)"
  fi
  # Self-contained, like the uv shims: `newacct` makes an account out of a bare
  # directory, so nothing outside has to supply one.  It prompts for confirmation
  # then owner and group; the answers are piped in.
  ACCT() { mkdir -p "$1"
           ( cd "$1" && printf 'y\n%s\n%s\n' "$(id -un)" "$(id -gn)" \
             | "$UDT_NEWACCT" ) >/dev/null 2>&1
           [ -f "$1/VOC" ] || [ -d "$1/VOC" ]; }
  LINK() { :; }                               # udt-git installs globally
  # NOT "CREATE.FILE <name> DYNAMIC": UniData accepts that sentence and silently
  # creates NOTHING — no file, no error, no output.  The modulo form works.
  CF()   { printf 'CREATE.FILE %s 2 101\nQUIT\n' "$2" \
           | ( cd "$1" && "$MVX" ) >/dev/null 2>&1; }
  # One confirmation, and UniData removes the dictionary with it.
  DF()   { printf 'DELETE.FILE %s\nY\nQUIT\n' "$2" \
           | ( cd "$1" && "$MVX" ) >/dev/null 2>&1; }
  # -M fences the verb's own output so the session banner and TCL prompts do not
  # reach the assertions — the same reason the uv verb path uses it.
  # `local s="$*"` FIRST, then strip: "${*#GIT }" applies the pattern to each
  # positional parameter separately, so it strips nothing and the sentence goes
  # out as "GIT -M GIT STATUS".
  GITV() { local a="$1"; shift; local s="$*"
           ( cd "$a" && printf 'GIT -M %s\nQUIT\n' "${s#GIT }" | "$MVX" ) 2>&1 \
             | awk '/<<<GIT-BEGIN>>>/{f=1;next} /<<<GIT-END>>>/{f=0} f'; }
  CT()   { ( cd "$1" && printf 'CT %s %s\nQUIT\n' "$2" "$3" | "$MVX" ) 2>&1; }
  SEED() { local a="$1" body="$2"
           printf '%s\n' "$body" > "$a/BP/SEEDT"
           ( cd "$a" && printf 'BASIC BP SEEDT\nRUN BP SEEDT\nQUIT\n' | "$MVX" ) \
             >/dev/null 2>&1; }
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

say "-- a deleted RECORD leaves history too --"
# Staging only ever ADDS, so a record deleted from the account used to stay in
# the index and go on being committed — and a clone brought it back.  status has
# always reported it correctly, so the two disagreed about the same account.
SEED "$A" 'OPEN "CUST" TO F ELSE STOP
WRITE "gone soon" ON F, "CZ"'
GITV "$A" GIT ADD -A >/dev/null
GITV "$A" GIT COMMIT -m withcz >/dev/null
t  "record committed" "gone soon"  "$(GITV "$A" GIT SHOW CUST CZ)"
SEED "$A" 'OPEN "CUST" TO F ELSE STOP
DELETE F, "CZ"'
t  "delete shows D"   "CZ"         "$(GITV "$A" GIT STATUS)"
GITV "$A" GIT ADD -A >/dev/null
GITV "$A" GIT COMMIT -m nocz >/dev/null
case "$(GITV "$A" GIT SHOW CUST CZ)" in
  *"gone soon"*) bad "record gone from HEAD" "no CUST/CZ" "still there";;
  *)             ok "record gone from HEAD";;
esac

say "-- DELETE.FILE: %FILE% is the file, so the whole file goes --"
# %FILE% is a file's existence in git, so deleting the file must behave like
# removing a directory: every record under <file>/ AND <file>.DICT/ is a
# deletion, the control included.  Before this, a deleted file was invisible —
# status skipped anything it could not open, the records stayed in HEAD, and a
# clone brought the whole file back.
CF "$A" TMPF
SEED "$A" 'OPEN "TMPF" TO F ELSE STOP
WRITE "one" ON F, "T1"
WRITE "two" ON F, "T2"'
GITV "$A" GIT ADD -A >/dev/null
GITV "$A" GIT COMMIT -m withfile >/dev/null
t  "file committed"   "one"       "$(GITV "$A" GIT SHOW TMPF T1)"
DF "$A" TMPF
t  "file delete shows D" "TMPF"   "$(GITV "$A" GIT STATUS)"
GITV "$A" GIT ADD -A >/dev/null
GITV "$A" GIT COMMIT -m nofile >/dev/null
# The file and its dictionary are gone from the commit.  NOT asserting a fully
# clean status: the VOC POINTER record for the file is a record deletion inside
# a live file, and the BASIC add (UniVerse/UniData) has never staged record
# deletions — only the C add reconciles those.  That gap predates this change
# and is its own fix; asserting it here would just mark it "expected".
case "$(GITV "$A" GIT SHOW TMPF T1)" in
  *one*) bad "file gone from HEAD" "no TMPF/T1" "still there";;
  *)     ok "file gone from HEAD";;
esac

if [ "$SKIP_NET" = 1 ]; then
  skip "remote/clone/fetch/pull/push" "SKIP_NET=1"
else
  say "-- remotes: bare remote + push + clone + pull (fast-forward) --"
  REM="$WORK/rem.git"; git init --bare -q "$REM"
  # Point the bare repo's HEAD at the branch the ENGINE creates.  `git init
  # --bare` sets HEAD from the host's init.defaultBranch (still `master` on a
  # stock git), while mv_git_init makes `main` — so a clone of this remote
  # resolved an UNBORN branch, materialise found no HEAD tree and reported
  # "empty", and the clone produced an account with no records.  A harness
  # detail, but one that reads exactly like a product bug.
  git --git-dir="$REM" symbolic-ref HEAD refs/heads/main
  GITV "$A" GIT REMOTE ADD origin "$REM" >/dev/null
  t  "remote list"   "origin"        "$(GITV "$A" GIT REMOTE)"
  t  "push"          "pushed"        "$(GITV "$A" GIT PUSH origin main || GITV "$A" GIT PUSH origin master)"
  B="$WORK/B"
  # clone via the engine (GITCLONE) into a materialised account
  # --flavour: a UniVerse account is created WITH a flavour and cannot be asked
  # afterwards, and this repository's history carries no record of one (mv_git#15),
  # so uv-git refuses to guess and asks.  PICK matches the flavour ACCT() answers
  # (menu 3).  Harmless on the platforms that do not need it.
  clone_out="$(GITV "$A" GIT CLONE "$REM" "$B" --flavour=PICK)"
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
