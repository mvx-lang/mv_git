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
# BR <account> — the repository's current branch.
#
# The tests used to guess: `PUSH origin main || PUSH origin master`, because
# git's default branch name depends on version and config.  That fallback was
# UNREACHABLE.  GITV ends in a pipeline (`| awk` for the fenced verb output),
# so it exits with awk's status -- always 0 -- and the right-hand side never
# ran.  A first attempt against the wrong branch therefore produced empty
# output that the assertion took as the answer, which is one of the shapes the
# intermittent uv failures wore (mv_git#165).
#
# Ask the repository instead.  There is nothing to fall back to when you know.
BR() { git --git-dir="$1/.git" symbolic-ref --short HEAD 2>/dev/null || echo main; }

# t NAME EXPECTED ACTUAL  — substring assertion (EXPECTED must appear in ACTUAL)
t()    { case "$3" in *"$2"*) ok "$1";; *) bad "$1" "$2" "$3";; esac; }
# tn NAME UNWANTED ACTUAL — asserts the output does NOT contain UNWANTED.
# For "it should not ask": a question nobody should be asked cannot be checked
# by looking for the right words, only by their absence.
#
# AN ABSENCE ASSERTED AGAINST NOTHING IS NOT AN ASSERTION.  If ACTUAL is empty
# this passes no matter what, so it passes just as well when the command under
# test did nothing at all -- and these shims fail QUIETLY on a path that is not
# an account, so "did nothing" is the common case, not a rare one.  Three tests
# in this file were found doing exactly that, each of them green against a build
# with the fix deliberately removed (mv_git#134).
#
# The cure is always the same: put a positive CONTROL in the same output -- some
# record that MUST be there -- so emptiness cannot pass for success.  This says
# so out loud rather than trusting anyone to remember.
tn()   { if [ -z "$3" ]; then
             printf '  VACUOUS %s — asserted "not %s" against EMPTY output;\n' "$1" "$2"
             printf '          add a positive control that must appear.\n'
             FAIL=$((FAIL+1)); return
         fi
         case "$3" in *"$2"*) bad "$1" "NOT: $2" "$3";; *) ok "$1";; esac; }

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
  # GITK <acct> <keys> <sentence...> — run a sentence with KEYSTROKES on stdin,
  # so the full-screen editor can be driven headlessly.  A prompt no automated
  # test can reach is a prompt that rots, and this suite has already found two
  # paths that were untested for exactly that reason.
  GITK() { local a="$1" k="$2"; shift 2
           printf '%s' "$k" | "$MVX" -a "$a" -c "$*" 2>&1; }
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
  # AN ACCOUNT NEEDS THE PACKAGE'S SOURCES, NOT ITS ACCOUNT.  install.sh makes
  # the package directory a UniVerse account, so the package carries a VOC — and
  # `cp -r "$GITPKG"/*` dropped that straight on top of the target's, taking
  # every file pointer the account had with it.  Its build output (BP.O) went
  # the same way, and objects from another account are worse than none: they
  # compile clean and run code that is not the source beside them.
  #
  # The damage then disguised itself.  A cloned account lost its pointer to
  # CUST; the next checkout opens files BY NAME, so it could not open CUST and
  # skipped every one of its records — then restored VOC/CUST later in the same
  # pass, putting the pointer back and leaving no trace.  The PULL got the blame
  # for years (mv_git#65), and a second pull always "fixed" it.
  LINK() { ( cd "$GITPKG" && tar cf - \
               --exclude=./VOC    --exclude=./D_VOC \
               --exclude=./VOCLIB --exclude=./D_VOCLIB \
               --exclude=./BP.O   --exclude=./D_BP.O \
               --exclude='./&SAVEDLISTS&' --exclude='./D_&SAVEDLISTS&' . ) \
             | ( cd "$1" && tar xf - ) 2>/dev/null
           ( cd "$1" && ./install.sh ) >/dev/null 2>&1; }
  # THE LICENCE IS TWO SEATS, AND THIS SUITE IS FASTER THAN THEY ARE RELEASED.
  #
  # `uvlictool` reports them; a session that has just quit does not give its seat
  # back instantly, so a rapid sequence of shim calls transiently wants a third.
  # What the caller sees when that happens is never the same twice: the explicit
  # "UniVerse user limit has been reached", or EMPTY output (the verb never ran,
  # so there is nothing between the <<<GIT-BEGIN>>> fences to extract), or the
  # NEXT command reporting a cold session's login banner.  Three presentations,
  # one cause, and each one reads like a code bug (mv_git#165).
  #
  # So wait for a seat rather than race for it.  The udt arm has had this lesson
  # written down since mv_git#54 -- "once the slots are gone every failure is
  # misleading" -- and the uv arm was left to learn it again.
  uv_seats_free() {
    "${UVHOME:-/usr/uv}/bin/uvlictool" 2>/dev/null \
      | awk '/license seats are available/ {print $1; exit}'
  }
  uv_wait_seat() {
    local n i=0
    while [ $i -lt 100 ]; do                 # ~10s, then proceed and let it fail loudly
      n="$(uv_seats_free)"
      case "$n" in ''|*[!0-9]*) return 0 ;; esac   # no tool / unparsable: do not block
      [ "$n" -gt 0 ] && return 0
      i=$((i+1)); sleep 0.1
    done
    say "   (waited for a licence seat and none came free — see mv_git#165)"
    return 0
  }
  # PREFLIGHT: say what the licence holds before anything is measured against it.
  if [ -x "${UVHOME:-/usr/uv}/bin/uvlictool" ]; then
    uvfree="$(uv_seats_free)"
    say "-- licence: ${uvfree:-?} seat(s) free before the run --"
    case "$uvfree" in
      ''|*[!0-9]*) ;;
      *) [ "$uvfree" -lt 1 ] &&
           say "   (no free seat — every failure below is suspect, see mv_git#165)" ;;
    esac
  fi

  # Seven answers, and the seventh is a FILE DESCRIPTION which UniVerse stores
  # in VOC attribute 1 as "F <description>" — leave it EMPTY or the account
  # scan, which matches attribute 1 against "F", cannot see the file.
  CF()   { uv_wait_seat; ( cd "$1" && printf 'CREATE.FILE %s\n1\n2\n3\n1\n2\n19\n\nQUIT\n' "$2" | "$MVX" ) >/dev/null 2>&1; }
  # DELETE.FILE confirms before it acts; an unanswered prompt leaves the file.
  DF()   { uv_wait_seat; ( cd "$1" && printf 'DELETE.FILE %s\nY\nY\nQUIT\n' "$2" | "$MVX" ) >/dev/null 2>&1; }
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
    GITV() { uv_wait_seat; local a="$1"; shift; local s="$*"; "$UVGIT" -a "$a" ${s#GIT } 2>&1; }
  else
    # `GIT -M` fences the verb's own output with <<<GIT-BEGIN>>>/<<<GIT-END>>>,
    # which is precisely what makes a session's output assertable: UniVerse
    # greets every session and prompts between commands, and an exact-match
    # assertion against a banner fails no matter how right the verb was.  That
    # is what the fence is FOR, so the tests use it rather than scraping.
    GITV() { uv_wait_seat; local a="$1"; shift; local s="$*"
             ( cd "$a" && printf 'GIT -M %s\nQUIT\n' "${s#GIT }" | "$MVX" ) 2>&1 \
               | awk '/<<<GIT-BEGIN>>>/{f=1;next} /<<<GIT-END>>>/{f=0} f'; }
  fi
  # The session script IS stdin, so the editor's keystrokes simply follow the
  # sentence that starts it.
  GITK() { uv_wait_seat; local a="$1" k="$2"; shift 2; local s="$*"
           ( cd "$a" && printf 'GIT -M %s\n%sQUIT\n' "${s#GIT }" "$k" | "$MVX" ) 2>&1 \
             | awk '/<<<GIT-BEGIN>>>/{f=1;next} /<<<GIT-END>>>/{f=0} f'; }
  CT()   { uv_wait_seat; ( cd "$1" && printf 'CT %s %s\nQUIT\n' "$2" "$3" | "$MVX" ) 2>&1; }
  SEED() { uv_wait_seat; local a="$1" body="$2"
           printf '%s\n' "$body" > "$a/BP/SEEDT"
           ( cd "$a" && printf 'BASIC BP SEEDT\nRUN BP SEEDT\nQUIT\n' | "$MVX" ) >/dev/null 2>&1; }
elif [ "$PLATFORM" = jbase ]; then
  # jbase: an account is simply a DIRECTORY -- there is no VOC and no account
  # bootstrap -- and $MVX is jsh, jBASE's own shell, which reads TCL from stdin.
  #
  # This arm drives the CLI (jb-git), the way UV_VIA=cli does for UniVerse.  The
  # in-session verb exists and works, but a CATALOGed subroutine cannot resolve
  # a DEFC function without the library being forced into the process -- which is
  # jBASE's behaviour for DEFC generally, not something mv_git does (mv_git#114).
  # Driving the verb here would test that packaging question rather than mv_git.
  ACCT() { mkdir -p "$1"
           ( cd "$1" && printf 'CREATE-FILE BP 1 11 TYPE=UD\n' | "$MVX" ) >/dev/null 2>&1
           printf '# jBASE account descriptor\nname = %s\nversion = 1\n' \
                  "$(basename "$1")" > "$1/.jbase"; }
  # JBGIT_VIA=verb drives the in-session verb, as the mvx and udt arms do; the
  # DEFAULT is the CLI, because the verb path's catalog step does not work yet
  # (the shared library is not being produced, so the session cannot find GIT)
  # and a default that fails tells you nothing about mv_git.  UV_VIA has the
  # same shape for the same kind of reason.
  #
  # The handlers and the jBASE shims are CATALOGed ONCE into a shared library
  # every test account reaches through $JBCOBJECTLIST -- udt catalogs globally
  # for the same reason, and doing it per account would recompile forty programs
  # for every test.
  JBLIB="$WORK/jblib"
  LINK() {
      mkdir -p "$1/BP.INC"
      cp "$GITPKG/PLATFORM.H" "$1/BP.INC/PLATFORM.H" 2>/dev/null
      cp "$GITPKG"/BP/* "$1/BP/" 2>/dev/null
      [ -f "$JBLIB/.done" ] && return 0
      mkdir -p "$JBLIB/bin"
      ( cd "$1" && for p in BP/*; do
            printf 'BASIC BP %s\nCATALOG BP %s\n' "$(basename "$p")" "$(basename "$p")"
        done | JBCDEV_LIB="$JBLIB" JBCDEV_BIN="$JBLIB/bin" "$MVX" ) >/dev/null 2>&1
      touch "$JBLIB/.done"; }
  # JP is a hash file; UD is a unix DIRECTORY, which is what the open form's DIR
  # means.  JD is NOT a directory -- it is another regular file.
  CF()   { ( cd "$1" && printf 'CREATE-FILE %s 1 11\n' "$2" | "$MVX" ) >/dev/null 2>&1; }
  DF()   { ( cd "$1" && printf 'DELETE-FILE %s\n' "$2" | "$MVX" ) >/dev/null 2>&1; }
  # LD_PRELOAD is not a workaround for anything mv_git does: a CATALOGed
  # subroutine cannot resolve a DEFC function unless the library is forced into
  # the process, and that is jBASE's behaviour for DEFC generally -- reproduced
  # with five lines of C and no mv_git at all (mv_git#114).
  JBPRE="${JBGIT_LIB:-$GITPKG/libjbgit.so}"
  if [ "${JBGIT_VIA:-cli}" = cli ]; then
    GITV() { local a="$1"; shift; local s="$*"; "$MVXGIT" -a "$a" ${s#GIT } 2>&1; }
    GITK() { local a="$1" k="$2"; shift 2; local s="$*"
             printf '%s' "$k" | "$MVXGIT" -a "$a" ${s#GIT } 2>&1; }
  else
    GITV() { local a="$1"; shift; local s="$*"
             ( cd "$a" && printf '%s\n' "$s" \
               | PATH="$JBLIB/bin:$PATH" LD_PRELOAD="$JBPRE" \
                 JBCOBJECTLIST="$JBLIB" "$MVX" ) 2>&1; }
    GITK() { local a="$1" k="$2"; shift 2; local s="$*"
             ( cd "$a" && printf '%s\n%s' "$s" "$k" \
               | PATH="$JBLIB/bin:$PATH" LD_PRELOAD="$JBPRE" \
                 JBCOBJECTLIST="$JBLIB" "$MVX" ) 2>&1; }
  fi
  CT()   { ( cd "$1" && printf 'CT %s %s\n' "$2" "$3" | "$MVX" ) 2>&1; }
  SEED() { local a="$1" body="$2"
           printf '%s\n' "$body" > "$a/BP/SEEDT"
           ( cd "$a" && printf 'BASIC BP SEEDT\nRUN BP SEEDT\n' | "$MVX" ) >/dev/null 2>&1; }
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
  # The session script IS stdin, so the editor's keystrokes simply follow the
  # sentence that starts it.
  GITK() { local a="$1" k="$2"; shift 2; local s="$*"
           ( cd "$a" && printf 'GIT -M %s\n%sQUIT\n' "${s#GIT }" "$k" | "$MVX" ) 2>&1 \
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

# VERSION, as a command and as the switch people type first.  Both, and then a
# flagged command AFTER them: the first attempt at the switch read @SENTENCE and
# assigned the uppercased result to the COMMON the handlers parse, so `-m` came
# back "unknown option 'M'" while the switch itself never fired (mv_git#85).
# The regression was not in the switch; it was in everything else.
t  "version command"  "libgit2"     "$(GITV "$A" GIT VERSION)"
t  "version switch"   "libgit2"     "$(GITV "$A" GIT --VERSION)"
t  "flags still parse after it" "[" "$(SEED "$A" 'OPEN "CUST" TO F ELSE STOP
WRITE "Zoe":@AM:"Perth" ON F, "CV"'; GITV "$A" GIT ADD -A >/dev/null; GITV "$A" GIT COMMIT -m after-version)"

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

# A record the account HAS that git does not know about.  This had no test at
# all until mv_git#59, which is exactly why status could go on never reporting
# one: modified, deleted and clean were all asserted, and the fourth state was
# not.  It must come BEFORE the ignore test, which reuses CUST/C9.
say "-- untracked: a new record shows as ?? --"
SEED "$A" 'OPEN "CUST" TO F ELSE STOP
WRITE "new" ON F, "C8"'
t  "untracked shown"  "?? CUST/C8"  "$(GITV "$A" GIT STATUS)"
GITV "$A" GIT ADD -A >/dev/null 2>&1
GITV "$A" GIT COMMIT -m untracked-gone >/dev/null 2>&1
case "$(GITV "$A" GIT STATUS)" in *CUST/C8*) bad "untracked gone" "no CUST/C8" "shown";; *) ok "untracked gone";; esac

say "-- ignore: a gitignored record stays out --"
printf 'CUST/C9\n' > "$A/.gitignore"
SEED "$A" 'OPEN "CUST" TO F ELSE STOP
WRITE "x" ON F, "C9"'
case "$(GITV "$A" GIT STATUS)" in *CUST/C9*) bad "ignore hides record" "no CUST/C9" "shown";; *) ok "ignore hides record";; esac
# ...AND `add` MUST LEAVE IT OUT, which is the half that was never asserted.
# `status` matched the ignore lists and `add` did not read them at all, so the
# verb COMMITTED a record the user had gitignored while the CLI left it out --
# one account, two different commits (mv_git#133).  Testing only status hid it:
# once a record is staged it is tracked, and the ignore lists never apply to a
# tracked path, so status went quiet about it for the opposite reason.
GITV "$A" GIT ADD -A >/dev/null
igst="$( cd "$A" && git ls-files )"   # the INDEX, not the diff: C1 is already committed
t  "the ordinary record is staged" "CUST/C1"  "$igst"
tn "but the ignored one is not"    "CUST/C9"  "$igst"

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

# GIT ATTR is an IN-SESSION VERB, so the UniVerse CLI path cannot reach it —
# and must not: giving uv-git its own copy would mean a second implementation of
# the registry, the validation and the staging, in C, which is exactly what
# "verbs are BASIC, not C" exists to prevent.  The verb path (the default) is
# where this is tested on UniVerse, and it runs there in full.
# GIT ATTR is an in-session VERB -- registry, validation, staging and a
# full-screen editor, all BASIC -- and deliberately has no shell twin (uv-git
# says so and exits 2).  So it is untestable through ANY CLI-driven arm, not
# just UniVerse's: the guard used to name uv, and jBASE, whose arm defaults to
# the CLI, therefore ran all 32 of these against a command that cannot exist
# and reported them as port failures.  Ask how the arm is DRIVEN, not which
# platform it is.
case "$PLATFORM" in
  uv)    ATTR_VIA="${UV_VIA:-verb}" ;;
  jbase) ATTR_VIA="${JBGIT_VIA:-cli}" ;;
  *)     ATTR_VIA=verb ;;
esac
if [ "$ATTR_VIA" = cli ]; then
  skip "GIT ATTR" "in-session verb; not reachable through a CLI-driven arm"
else
say "-- GIT ATTR: the attribute editor (mv_git#15) --"
# Every one of these runs through the SWITCHES, which is why the switches exist:
# the full-screen editor is the same machinery with a screen on it, and a path
# only reachable by hand is a path that rots.
#
# The attributes are carried by the OPEN interchange control, so the account is
# staged in that form first.  `-o` is the flag on UniData/UniVerse; on MVX the
# open form comes from mvx.openaccount, which this suite set at the top — so one
# sentence serves all three.
GITV "$A" GIT ADD -A -o >/dev/null
GITV "$A" GIT COMMIT -m openform >/dev/null

t  "attr lists account"  "name"      "$(GITV "$A" GIT ATTR)"
t  "attr lists file"     "modulo"    "$(GITV "$A" GIT ATTR CUST)"
t  "attr set"            "997"       "$(GITV "$A" GIT ATTR CUST --set modulo=997)"
t  "attr set is staged"  "997"       "$(GITV "$A" GIT ATTR CUST)"
# IT NEVER COMMITS.  The edit is a staged modification and nothing more, so it
# has to show in status and must not have produced a commit of its own.
t  "attr edit shows in status" "%FILE%" "$(GITV "$A" GIT STATUS)"
case "$(GITV "$A" GIT LOG)" in
  *modulo*|*attr*) bad "attr does not commit" "no commit from GIT ATTR" "one was made";;
  *)               ok "attr does not commit";;
esac
# AN ORDINARY ADD MUST NOT UNDO IT.  Two staging paths restage a file's control
# from the account — the working-tree sweep and the record pass — and either one
# putting the live geometry back would drop the edit before it was ever
# committed, with status then going clean as though it had landed.
GITV "$A" GIT ADD -A >/dev/null
t  "attr survives add -A" "997"      "$(GITV "$A" GIT ATTR CUST)"
GITV "$A" GIT COMMIT -m geometry >/dev/null
t  "attr survives commit" "997"      "$(GITV "$A" GIT ATTR CUST)"

# The registry is what makes a value legal, so each kind of refusal is asserted.
t  "attr rejects bad enum"   "must be one of" "$(GITV "$A" GIT ATTR CUST --set dynamic=maybe)"
t  "attr rejects bad number" "is a number"    "$(GITV "$A" GIT ATTR CUST --set modulo=abc)"
t  "attr rejects unknown"    "no attribute"   "$(GITV "$A" GIT ATTR CUST --set wibble=1)"
# A refusal must change nothing, or a rejected edit would still be half applied.
t  "refusal changes nothing" "997"   "$(GITV "$A" GIT ATTR CUST)"

t  "attr unset"          "997 -> -"  "$(GITV "$A" GIT ATTR CUST --unset modulo)"
case "$(GITV "$A" GIT ATTR CUST)" in
  *997*) bad "attr unset takes effect" "no modulo" "still 997";;
  *)     ok "attr unset takes effect";;
esac

# ATTRIBUTES LIVE IN THE GIT OBJECTS, NOT IN THE ACCOUNT.  Recording a file's
# geometry BEFORE the file exists — so a clone builds it right the first time —
# is a use of this, and the declaration has to outlive the next `add -A`: prune
# once counted the control itself as evidence the file had been live and swept
# it away again.
t  "attr declares an absent file" "1009" "$(GITV "$A" GIT ATTR ORDERS --set modulo=1009)"
GITV "$A" GIT ADD -A >/dev/null
t  "declaration survives add -A"  "1009" "$(GITV "$A" GIT ATTR ORDERS)"

# A CLASS FIELD MUST NOT LEAK INTO THE ONES IT IS NOT CHANGING.  type, modulo
# and dynamic share one line, so setting one means re-reading the other two —
# and those reads answer only their own key, returning early for a class that
# has no modulo.  Anything not cleared first therefore came back as the PREVIOUS
# read's answer: `--set type=DIR` then `--sync` wrote "hash hash DYNAMIC", with
# the modulo holding the string "DIR" left over from the type read.
GITV "$A" GIT ATTR CUST --set modulo=997 --set dynamic=yes >/dev/null
GITV "$A" GIT ATTR CUST --set type=DIR >/dev/null
case "$(GITV "$A" GIT ATTR CUST | sed -n 's/^ *modulo *//p')" in
  *DIR*|*hash*) bad "class fields do not leak" "no modulo" "leaked the type";;
  *)            ok "class fields do not leak";;
esac
# A value that DIFFERS from the committed one, so there is a delta to see:
# staging the same bytes HEAD already has is correctly no change at all.
GITV "$A" GIT ATTR CUST --set type=hash --set modulo=1201 --set dynamic=yes >/dev/null

# WHAT DID I JUST STAGE?  status says a path changed; --staged says how.  Built
# from ops rather than a per-platform diff, so this is the same body everywhere.
t  "diff --staged shows the edit" "+hash 1201" "$(GITV "$A" GIT DIFF --staged)"
t  "diff --cached is the same"    "+hash 1201" "$(GITV "$A" GIT DIFF --cached)"
t  "diff --staged filters by file" "CUST"      "$(GITV "$A" GIT DIFF --staged CUST)"
case "$(GITV "$A" GIT DIFF --staged CUST)" in
  *ORDERS*) bad "diff --staged filter excludes others" "no ORDERS" "included it";;
  *)        ok "diff --staged filter excludes others";;
esac
# -u is the SAME comparison told properly.  The hunk header is the part the
# compact form has never had, and it is the part that says WHERE in the record
# the change is — so its presence is the assertion.
t  "diff --staged -u has hunk headers" "@@" "$(GITV "$A" GIT DIFF --staged -u)"
GITV "$A" GIT ADD -A >/dev/null; GITV "$A" GIT COMMIT -m staged-diff >/dev/null
t  "nothing staged after a commit" "nothing staged" "$(GITV "$A" GIT DIFF --staged)"
# ...and on the UNSTAGED side too, which is a different body on every platform:
# C on MVX, BASIC on the session ones.  Both render through the same libgit2
# call, so both must produce a header.
SEED "$A" 'OPEN "CUST" TO F ELSE STOP
WRITE "Ada":@AM:"Vienna" ON F, "C1"'
t  "diff -u has hunk headers" "@@"  "$(GITV "$A" GIT DIFF CUST -u)"
case "$(GITV "$A" GIT DIFF CUST)" in
  *@@*) bad "diff without -u has no header" "no @@" "printed one";;
  *)    ok "diff without -u has no header";;
esac
GITV "$A" GIT RESTORE CUST >/dev/null

# DRIFT.  A resize is local operational tuning: it must NOT show as a diff and
# must not ride out to other clones as a new default (540c066).  So the editor
# is the one place the divergence becomes intent — it says the live file no
# longer matches, and --sync is the explicit answer.
#
# The drift is made on `type` rather than `modulo` because every platform can
# probe which KIND of file something is, while MVX has no modulo to differ
# about — so the same two assertions run everywhere.
GITV "$A" GIT ATTR CUST --set type=DIR >/dev/null
t  "drift reported"      "no longer matches" "$(GITV "$A" GIT ATTR CUST)"
t  "sync adopts live"    "DIR -> hash"       "$(GITV "$A" GIT ATTR CUST --sync)"
t  "nothing left to sync" "nothing to sync"  "$(GITV "$A" GIT ATTR CUST --sync)"
case "$(GITV "$A" GIT ATTR CUST)" in
  *"no longer matches"*) bad "drift gone after sync" "no drift" "still reported";;
  *)                     ok "drift gone after sync";;
esac

# THE FULL-SCREEN EDITOR, driven headlessly.  It is the same registry, the same
# validation and the same staging the switches use, with a screen on it — which
# is why the switches were built first — but "same machinery" is a claim, and an
# untested screen is how it stops being true.
#
# `jj` moves to `dynamic`, ENTER cycles the enum, `s` stages.  Letters as well
# as arrows: a piped test cannot send a decoded key, and the letters are what
# work when the terminal sends something the platform has not decoded.
# `dynamic` is part of the class line — "hash <modulo> STATIC|DYNAMIC" — so it
# needs a modulo to sit beside.  Recorded first, and asserted on its own: the
# refusal is the behaviour, not an accident of ordering.
t  "dynamic needs a modulo" "set modulo first" "$(GITV "$A" GIT ATTR CUST --unset modulo; GITV "$A" GIT ATTR CUST --set dynamic=yes)"
GITV "$A" GIT ATTR CUST --set modulo=997 >/dev/null
t  "editor stages an edit" "staged" "$(GITK "$A" 'jj
s' GIT ATTR CUST --edit)"
t  "editor edit took"      "yes"    "$(GITV "$A" GIT ATTR CUST | sed -n 's/^ *dynamic *//p')"
# ABANDON MEANS ABANDON.  `d` clears the value and `q` leaves without staging,
# so the recorded value must be exactly what it was.
t  "editor abandons"       "abandoned" "$(GITK "$A" 'jjdq' GIT ATTR CUST --edit)"
t  "abandoned edit is not kept" "yes" "$(GITV "$A" GIT ATTR CUST | sed -n 's/^ *dynamic *//p')"
GITV "$A" GIT ATTR CUST --unset dynamic >/dev/null

# The account scope is the same editor with no file argument.
GITV "$A" GIT ATTR --set version=2 >/dev/null
t  "attr sets account version" "2"   "$(GITV "$A" GIT ATTR | sed -n 's/^ *version *//p')"
# Descriptor keys the registry does not name are NOT the editor's to discard:
# `openaccount` is what marks the account open, and rewriting the descriptor
# without it would quietly turn the open form off.
t  "attr keeps unknown keys" "name"  "$(GITV "$A" GIT ATTR)"
# THE DESCRIPTOR IS ONE THING IN TWO SPELLINGS, and an edit has to reach both.
# Git carries the portable form; MVX keeps the native one beside the account and
# the engine converts each way, so an edit landing only in git was regenerated
# straight back out of the untouched native copy by the next `add -A` — the
# value was gone and the commit said "nothing to commit", as though nothing had
# been asked for.  It showed in BOTH status columns first, staged and modified
# at once, which is the same disagreement seen from the other end.
#
# The suite could not have caught it: it edited, added, committed and never
# looked again.  So the assertion is specifically that it is still there AFTER
# an add, and that status reports it once.
GITV "$A" GIT ADD -A >/dev/null
t  "account edit survives add -A" "2" "$(GITV "$A" GIT ATTR | sed -n 's/^ *version *//p')"
te "account edit reported once" "1" "$(GITV "$A" GIT STATUS | grep -c 'account\|\.mvx' || true)"
GITV "$A" GIT COMMIT -m attrs >/dev/null
t  "account edit survives commit" "2" "$(GITV "$A" GIT ATTR | sed -n 's/^ *version *//p')"
t  "clean after committing it" "clean" "$(GITV "$A" GIT STATUS)"
fi

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
  t  "push"          "pushed"        "$(GITV "$A" GIT PUSH origin "$(BR "$A")")"
  B="$WORK/B"
  # clone via the engine (GITCLONE) into a materialised account
  # --flavour: a UniVerse account is created WITH a flavour and cannot be asked
  # afterwards, and this repository's history carries no record of one (mv_git#15),
  # so uv-git refuses to guess and asks.  PICK matches the flavour ACCT() answers
  # (menu 3).  Harmless on the platforms that do not need it.
  clone_out="$(GITV "$A" GIT CLONE "$REM" "$B" --flavour=PICK)"
  t  "clone"         "cloned"        "$clone_out"

  # --- the account descriptor -------------------------------------------
  #
  # It is a FILE only on MVX, where it carries local state (permit/deny policy
  # and the rest of what an account is configured with).  Everywhere else it
  # lives in the git objects, and its job THERE is to be the indicator that
  # tells a clone to build an ACCOUNT rather than just a directory of files.
  # There is no exception.  UniVerse used to write .uv for the VOC flavour --
  # the one fact about a UniVerse account that cannot be regenerated, since it
  # is fixed at creation and readable nowhere afterwards.  It belongs in the git
  # objects like everything else about the account, where GIT ATTR can edit it,
  # not in a file beside the records.
  #
  # None of this was tested, and all four write sites spelled the name ".mvx"
  # outright -- so every platform wrote MVX's descriptor into the account and
  # the suite stayed green.  On UniData the stray turned out to be LOAD-BEARING:
  # materialise wrote it, the walk staged it as an ordinary file, and the
  # open-form conversion turned that into the committed .mv-account.  Removing
  # the stray -- which is right -- silently stopped the indicator travelling,
  # and a freshly cloned account reported ` M .mv-account` with nothing to fix.
  #
  # So both halves are asserted: what is on DISK, and what is in GIT.  Testing
  # only one of them is how this survived.
  # "" IS THE EXPECTED ANSWER ON THREE OF FOUR PLATFORMS, so this function has to
  # be able to say something OTHER than "" when it is asked the wrong question.
  # Without the directory check it returned "" for a path that did not exist at
  # all, and `te "no descriptor file on disk" ""` then passed for a clone that
  # had never happened -- an assertion that cannot fail (mv_git#134).
  descfiles() { local d="$1" out="" n
                [ -d "$d" ] || { printf 'NO-SUCH-DIR:%s' "$d"; return; }
                for n in .mvx .udt .uv .jbase; do
                    [ -f "$d/$n" ] && out="$out$n"
                done
                printf '%s' "$out"; }
  case "$PLATFORM" in
    mvx)   te "descriptor is a file here"      ".mvx" "$(descfiles "$B")" ;;
    uv)    te "no descriptor file on disk"     ""     "$(descfiles "$B")" ;;
    *)     te "no descriptor file on disk"     ""     "$(descfiles "$B")" ;;
  esac
  # ...and the indicator is in the COMMIT, which is what a clone reads to know
  # this is an account at all.
  t  "indicator is committed" ".mv-account" \
     "$(git --git-dir="$B/.git" ls-tree --name-only HEAD 2>/dev/null)"

  # The flavour is what the file used to be FOR, so removing the file has to
  # leave it reachable: git's config carries it until the next `add` stages a
  # descriptor that does, and the git objects from then on.
  if [ "$PLATFORM" = uv ]; then
    t "flavour survives the clone" "PICK" \
      "$(git --git-dir="$B/.git" config mvx.flavour 2>/dev/null)$(git --git-dir="$B/.git" show HEAD:.mv-account 2>/dev/null | grep flavour)"
  fi

  LINK "$B"
  GITV "$B" GIT CONFIG user.name Test >/dev/null
  GITV "$B" GIT CONFIG user.email test@example.com >/dev/null
  # A adds C3, pushes; B pulls -> fast-forward re-materialises
  SEED "$A" 'OPEN "CUST" TO F ELSE STOP
WRITE "Cy":@AM:"Oslo" ON F, "C3"'
  GITV "$A" GIT ADD -A >/dev/null; GITV "$A" GIT COMMIT -m c3 >/dev/null
  GITV "$A" GIT PUSH origin "$(BR "$A")" >/dev/null 2>&1
  t  "pull fast-forward" "fast-forward" "$(GITV "$B" GIT PULL origin "$(BR "$B")" 2>&1)"
  t  "pulled record"  "Oslo"         "$(CT "$B" CUST C3)"

  # --- adopt: a PLAIN-GIT checkout becomes a live account ----------------
  #
  # The path nobody had tested, and the one that was broken.  `git` gives you
  # files; adopt gives you an account.  A plain checkout puts the OPEN FORM on
  # disk -- CLIENTS lands as a DIRECTORY of record files -- and those are the
  # names the native files want, so creating them failed and the records went
  # nowhere: 4 materialised instead of 118, while `status` read clean because
  # it was comparing the open form against itself.
  #
  # So the assertion is NOT "status is clean".  It is that adopt and clone
  # produce the SAME ACCOUNT: same record count, dictionary present, records
  # readable, and no descriptor left on disk off MVX (mv_git#122).
  # Every platform that HAS adopt, because the point is that they AGREE: an
  # adopted account and a cloned one must be the same account, and two CLIs
  # building accounts differently is #108 waiting to happen.  Scoping these to
  # one platform is how that would go unnoticed -- and while they were scoped,
  # uv-git failed four of them.
  case "$PLATFORM" in
    udt|uv|jbase)
      P="$WORK/plainco"
      git clone -q "$REM" "$P" 2>/dev/null
      t  "plain checkout has the open form" "CUST" \
         "$(ls "$P" 2>/dev/null | tr '\n' ' ')"
      adopt_flav=""; [ "$PLATFORM" = uv ] && adopt_flav="--flavour=PICK"
      adopt_out="$( cd "$P" && "$MVXGIT" adopt $adopt_flav 2>&1 )"
      t  "adopt reports an account"    "account"  "$adopt_out"
      # The record itself, not CT's whole output: the session banner carries
      # each account's own path, so two good accounts never match exactly.
      t  "adopt materialises records"  "Oslo"     "$(CT "$P" CUST C3)"
      te "adopt leaves no descriptor"  ""         "$(descfiles "$P")"
      # ...and an EDIT ON DISK is what gets adopted.  This is the whole point:
      # 99 times in 100 the checkout matches HEAD, and when it does not it is
      # because somebody edited a record -- they expect their edit in the
      # account, not the committed version of it.  Taking HEAD would silently
      # prefer the wrong one, and nothing would say so.
      Q="$WORK/plainedit"
      git clone -q "$REM" "$Q" 2>/dev/null
      printf 'EDITED ON DISK\n' > "$Q/CUST/C1"
      ( cd "$Q" && "$MVXGIT" adopt $adopt_flav >/dev/null 2>&1 )
      t  "adopt carries a disk edit in" "EDITED ON DISK" "$(CT "$Q" CUST C1)"

      # ...and an account that is a SUBDIRECTORY of a repository (#44, #49).
      # There is no .git in it -- the repository's is above -- and adopt passed
      # the literal ".git", so the account was built EMPTY and adopt reported
      # success anyway.  The sibling assertion is the important one: `git add -A`
      # stages the whole worktree wherever it is run, and write-tree writes the
      # whole index, so without scoping adopt would have built acctB's files
      # inside acctA and cleared what it did not own.
      M="$WORK/adoptmulti"   # NOT $WORK/multi: the several-accounts test owns that
      mkdir -p "$M" && ( cd "$M" && git init -q . )
      # AD, not A: `A` is the suite's own account, set once at the top and used
      # by every block after this one.  A loop variable named `A` overwrote it
      # with a bare relative name, so the next test to reach for $A got acctB --
      # and since these shims fail quietly on a path that is not an account, it
      # showed up as empty output rather than an error (mv_git#130).
      for AD in acctA acctB; do
          mkdir -p "$M/$AD/CUST" "$M/$AD/CUST.DICT"
          printf '%s-one\n' "$AD" > "$M/$AD/CUST/C1"
          printf 'DIR' > "$M/$AD/CUST.DICT/%FILE%"
          printf '# .mv-account - open (portable) account descriptor\nname = %s\nversion = 1\nopenaccount = 1\n' \
                 "$AD" > "$M/$AD/.mv-account"
      done
      ( cd "$M" && git add -A >/dev/null 2>&1 &&
        git -c user.email=t@t -c user.name=t commit -qm base >/dev/null 2>&1 )
      ( cd "$M/acctA" && MVXGIT_OPEN_ACCOUNT=1 "$MVXGIT" adopt $adopt_flav >/dev/null 2>&1 )
      t  "adopt in a subdirectory"     "acctA-one" "$(CT "$M/acctA" CUST C1)"
      te "the sibling account is untouched" "acctB-one" \
         "$(head -1 "$M/acctB/CUST/C1" 2>/dev/null)"

      # --- what adopt ASKS -------------------------------------------------
      #
      # Three rules, and the wording matters as much as the trigger:
      #
      #   open form, flag off   ask to ENABLE.  The data is already portable;
      #                         the flag is what is missing, and it does not
      #                         travel with a clone (#88).  Asking to "convert"
      #                         something that already IS open is nonsense to
      #                         whoever answers.
      #   native to ANOTHER MV  ask to CONVERT.  It is being converted either
      #                         way to be usable here, so that is the one moment
      #                         "should it also be portable?" is fair.
      #   native to THIS MV     say nothing.  Nothing is being converted, and
      #                         asking invites converting a working account for
      #                         no reason.
      #
      # None of this was covered until now; it lived in one CLI and in one
      # person's hand-testing.
      askdir() { local d="$1" desc="$2" body="$3"
                 rm -rf "$d"; mkdir -p "$d/CUST" "$d/CUST.DICT"
                 printf 'x\n' > "$d/CUST/C1"; printf 'DIR' > "$d/CUST.DICT/%FILE%"
                 printf '%s' "$body" > "$d/$desc"
                 ( cd "$d" && git init -q . && git add -A >/dev/null 2>&1 &&
                   git -c user.email=t@t -c user.name=t commit -qm x >/dev/null 2>&1 ); }
      OPENDESC='# .mv-account - open (portable) account descriptor
name = t
version = 1
openaccount = 1
'
      # 1. open form, flag NOT set -> asks to ENABLE
      askdir "$WORK/ask1" ".mv-account" "$OPENDESC"
      t "asks to ENABLE when the flag is off" "flag" \
        "$( cd "$WORK/ask1" && MVXGIT_OPEN_ACCOUNT=0 "$MVXGIT" adopt $adopt_flav 2>&1 )"
      # 2. native to ANOTHER system -> asks to CONVERT
      case "$PLATFORM" in udt) FOREIGN=".uv" ;; *) FOREIGN=".udt" ;; esac
      askdir "$WORK/ask2" "$FOREIGN" "# native descriptor
name = t
version = 1
"
      t "asks to CONVERT a foreign-native account" "convert" \
        "$( cd "$WORK/ask2" && MVXGIT_OPEN_ACCOUNT=0 "$MVXGIT" adopt $adopt_flav 2>&1 )"
      # 3. native to THIS system -> silent about the open form
      case "$PLATFORM" in udt) OWN=".udt" ;; *) OWN=".uv" ;; esac
      askdir "$WORK/ask3" "$OWN" "# native descriptor
name = t
version = 1
"
      tn "silent for an account already native here" "open account" \
         "$( cd "$WORK/ask3" && MVXGIT_OPEN_ACCOUNT=0 "$MVXGIT" adopt $adopt_flav 2>&1 )"

      # A SUBMODULE account takes its account type from its OWN repository.
      #
      # mvx.openaccount lives in .git/config, so it is a property of the
      # repository: two accounts under one repo share one flag (#44, #49).  A
      # submodule is the exception, and for the right reason -- it HAS its own
      # repository, so it has its own config and its own answer, independent of
      # whatever contains it.  That is a real shape, not a hypothetical: it is
      # how packages/git sits inside mvx.
      #
      # libgit2 resolves a submodule's gitlink `.git` FILE to the submodule's
      # own repository, which is what makes this work.  Untested until now, and
      # the kind of thing that gets "simplified" into reading the parent's
      # config by someone who has not hit the case.
      S="$WORK/subm"
      mkdir -p "$S/inner" && ( cd "$S" && git init -q . )
      ( cd "$S/inner" && git init -q . &&
        git config mvx.openaccount true &&
        printf 'x\n' > f && git add -A >/dev/null 2>&1 &&
        git -c user.email=t@t -c user.name=t commit -qm inner >/dev/null 2>&1 )
      # the parent says NOT open; the submodule says open
      ( cd "$S" && git config mvx.openaccount false )
      te "submodule keeps its own account type" "true" \
         "$( cd "$S/inner" && git config --get mvx.openaccount )"
      te "the parent keeps its own"             "false" \
         "$( cd "$S" && git config --get mvx.openaccount )"
      ;;
  esac

  # Cloning an OPEN-format repo sets mvx.openaccount in the cloner's own config
  # and that decides how every later commit here is written, so the CLI asks
  # (mv_git#88).  With no terminal the answer is yes; --no-open-account and
  # $MVXGIT_OPEN_ACCOUNT are how a script says otherwise.  A bare repo holding
  # just the open descriptor is enough to exercise the decision, and keeps this
  # off the suite's own fixture.  uv-git has no clone flags (it asks in adopt),
  # so this is mvx/udt only.
  case "$PLATFORM" in
  mvx|udt)
    OA="$WORK/oa"; mkdir -p "$OA"
    printf '# .mv-account - open (portable) account descriptor\nname = oa\nversion = 1\nopenaccount = 1\nhash = hash\n' > "$OA/.mv-account"
    ( cd "$OA" && git init -q . && git add -A && \
      git -c user.name=Test -c user.email=test@example.com commit -qm oa ) >/dev/null 2>&1
    "$MVXGIT" clone "$OA" "$WORK/oa-yes" >/dev/null 2>&1
    te "clone open: opt-in by default" "true" \
      "$(git -C "$WORK/oa-yes" config --get mvx.openaccount 2>&1)"
    # DECLINING AND NOT CLONING AT ALL LOOK THE SAME to `config --get`: both
    # answer nothing, so `te "" ...` passed either way and the two negative cases
    # were asserting nothing (mv_git#134).  Reporting whether the clone landed
    # alongside the flag makes the difference visible -- a clone that failed now
    # says `no-clone` and the assertion fails.
    declined() { local d="$1"
                 [ -d "$d/.git" ] || { printf 'no-clone'; return; }
                 printf 'cloned:%s' "$(git -C "$d" config --get mvx.openaccount 2>&1)"; }
    "$MVXGIT" clone --no-open-account "$OA" "$WORK/oa-no" >/dev/null 2>&1
    te "clone open: --no-open-account declines" "cloned:" "$(declined "$WORK/oa-no")"
    MVXGIT_OPEN_ACCOUNT=0 "$MVXGIT" clone "$OA" "$WORK/oa-env" >/dev/null 2>&1
    te "clone open: env declines" "cloned:" "$(declined "$WORK/oa-env")"
    ;;
  esac
fi

# --- several accounts in one repository (mv_git#44) --------------------------
say "-- the form belongs to the ACCOUNT, so the verb and the CLI agree (mv_git#135) --"
# `mvx.openaccount` marks an account open.  The ENGINE honoured it; the VERB read
# the form from its own command line and nothing else, so the same account
# committed a `.mv-account` through the CLI and a native descriptor through
# `GIT ADD -A` at the TCL prompt.  Two commits from one account, decided by which
# way you came in -- exactly what #81 settled must not happen.
#
# ASSERTED AS PARITY, not just as "the verb is right": the failure mode is the
# two DISAGREEING, and a test that only checked one of them would have passed
# throughout -- the CLI was correct the whole time.
PAR="$WORK/parity"; ACCT "$PAR"; LINK "$PAR"; CF "$PAR" CUST
( cd "$PAR" && git init -q . >/dev/null 2>&1; git config mvx.openaccount true )
GITV "$PAR" GIT INIT   >/dev/null
GITV "$PAR" GIT ADD -A >/dev/null
descof() { ( cd "$1" && git diff --cached --name-only ) \
           | grep -E '^\.(mv-account|mvx|udt|uv|jbase)$' | tr '\n' ' '; }
pverb="$(descof "$PAR")"
( cd "$PAR" && git reset -q >/dev/null 2>&1; "$MVXGIT" add -A >/dev/null 2>&1 )
pcli="$(descof "$PAR")"
t  "the verb honours mvx.openaccount" ".mv-account" "$pverb"
te "and the CLI stages the same"      "$pverb"      "$pcli"

say "-- a file's dictionary travels once, as records (mv_git#151) --"
# UniVerse keeps a file's dictionary beside the data as `D_<name>`, a hash file
# of its own.  The disk pass that #148 gave the verb took it as 2KB of opaque
# binary -- while `<file>.DICT/@ID` and `<file>.DICT/%FILE%` were carrying that
# same dictionary as RECORDS.  The dictionary went into the commit twice, once
# in a form no other MV system can read.
#
# BOTH HALVES, because dropping D_CUST is only right if the records are still
# there: a skip that took the dictionary out altogether would pass a test that
# only looked for the blob.
case "$PLATFORM" in
uv)
  DCT="$WORK/dictblob"; ACCT "$DCT"; LINK "$DCT"; CF "$DCT" CUST
  ( cd "$DCT" && git init -q . >/dev/null 2>&1; git config mvx.openaccount true )
  GITV "$DCT" GIT INIT   >/dev/null
  GITV "$DCT" GIT ADD -A >/dev/null
  dst="$( cd "$DCT" && git ls-files )"
  # the file really has a dictionary on disk, or this asserts nothing
  t  "the dictionary file exists on disk" "D_CUST" \
     "$( ls "$DCT" 2>/dev/null | grep '^D_CUST$' )"
  t  "and travels as records"             "CUST.DICT/" "$dst"
  tn "but not as a blob"                  "D_CUST"     "${dst}x"
  ;;
esac

say "-- an account's ORDINARY files travel too, both ways in (mv_git#148) --"
# `GIT ADD -A` stages RECORDS.  It had no pass for anything else, so a README, a
# script, notes -- and `.gitignore` itself -- were silently left out of every
# commit made in a session, while the CLI staged them.  On UniData NEITHER route
# staged them, because udt-git has its own add_all with no disk pass either.
#
# Three implementations of one command, and only one of them did this.
#
# The pass has to know which top-level names are MV FILES, so it leaves their
# records to the record walk; on UniVerse the engine cannot look, so the caller
# supplies the list -- the same shape as the furniture and VOCDROP asks (#133).
# The control below is exactly that: CUST's records must still arrive as
# RECORDS, not swept up as blobs.
PLN="$WORK/plain"; ACCT "$PLN"; LINK "$PLN"; CF "$PLN" CUST
SEED "$PLN" 'OPEN "CUST" TO F ELSE STOP
WRITE "Ada" ON F, "C1"'
printf '# project notes\n' > "$PLN/NOTES.md"
( cd "$PLN" && git init -q . >/dev/null 2>&1; git config mvx.openaccount true )
GITV "$PLN" GIT INIT   >/dev/null
GITV "$PLN" GIT ADD -A >/dev/null
pverb="$( cd "$PLN" && git ls-files )"
t  "the verb stages an ordinary file" "NOTES.md" "$pverb"
t  "and the records are still records" "CUST/C1" "$pverb"
( cd "$PLN" && git rm -r --cached . -q >/dev/null 2>&1; "$MVXGIT" add -A >/dev/null 2>&1 )
pcli="$( cd "$PLN" && git ls-files )"
t  "the CLI stages it too"             "NOTES.md" "$pcli"
t  "and its records too"               "CUST/C1"  "$pcli"

say "-- an object FILE is excluded by name, and a NUL is not an object (mv_git#145) --"
# UniVerse keeps compiled objects in a file beside the source: BP -> BP.O.  That
# is an ordinary F record in the master file, so the BASIC walk saw it as a file
# like any other and staged every object in it -- 61 of them, measured.  Nothing
# caught that except the NUL-content guess in `add`.
#
# So the guess was load-bearing for the wrong reason, and removing it (which is
# right -- it also drops legitimate records) broke UniVerse outright until the
# walk learned the engine's rule: a file <X>.O whose <X> is also a file here is
# an object file.
#
# Both halves are asserted, because each alone would have passed at some point
# during this: the object file stays out, AND a record that merely holds a NUL
# is the user's and travels.
case "$PLATFORM" in
uv)
  OBF="$WORK/objfile"; ACCT "$OBF"; LINK "$OBF"
  printf 'PRINT "x"\n' > "$OBF/BP/OBJPROG"
  ( cd "$OBF" && printf 'BASIC BP OBJPROG\nQUIT\n' | "$MVX" ) >/dev/null 2>&1
  ( cd "$OBF" && git init -q . >/dev/null 2>&1; git config mvx.openaccount true )
  GITV "$OBF" GIT INIT   >/dev/null
  GITV "$OBF" GIT ADD -A >/dev/null
  # it really compiled -- otherwise BP.O holds nothing and this asserts nothing
  t  "the object file exists" "OBJPROG" "$( ls "$OBF/BP.O" 2>/dev/null | tr '\n' ' ' )"
  tn "but its records stay out" "BP.O/" "$( cd "$OBF" && git ls-files | grep -E '^(BP|BP\.O)/' | tr '\n' ' ' )x"
  ;;
esac
case "$PLATFORM" in
udt|uv)
  NUA="$WORK/nulrec"; ACCT "$NUA"; LINK "$NUA"; CF "$NUA" MYDATA
  SEED "$NUA" 'OPEN "MYDATA" TO F ELSE STOP
WRITE "before":CHAR(0):"after" ON F, "HASNUL"
WRITE "ordinary" ON F, "PLAIN"'
  ( cd "$NUA" && git init -q . >/dev/null 2>&1; git config mvx.openaccount true )
  GITV "$NUA" GIT INIT   >/dev/null
  GITV "$NUA" GIT ADD -A >/dev/null
  nst="$( cd "$NUA" && git ls-files )"
  t  "an ordinary record travels"          "MYDATA/PLAIN"  "$nst"
  t  "and so does one holding a NUL"       "MYDATA/HASNUL" "$nst"
  ;;
esac

say "-- a compiled object is named, and a NUL is only a guess (mv_git#133) --"
# OBJECT DETECTION IS TWO TESTS AND THEY ARE NOT EQUAL.  The NAMING rule -- an
# id `_PROG` whose base `PROG` is really in the same file -- is exact.  The
# CONTENT rule -- "the record holds a NUL" -- is a guess in both directions, so
# the engine keeps it behind a switch.
#
# There were THREE implementations: the engine had both with the guess gated,
# `status` had both ungated, and `add` had only the guess.  On UniVerse they
# disagreed in the direction that loses data -- a record holding CHAR(0) was
# dropped by the verb and staged by the CLI, from the same account.
#
# So: the object must go by NAME, and the ordinary record must survive wherever
# the guess is off.  The second is asserted against the CLI, which is the route
# whose switch setting the platform actually chooses.
case "$PLATFORM" in
udt|uv)
  OBA="$WORK/objs"; ACCT "$OBA"; LINK "$OBA"; CF "$OBA" MYDATA
  SEED "$OBA" 'OPEN "MYDATA" TO F ELSE STOP
WRITE "the source" ON F, "PROG"
WRITE "pretend object" ON F, "_PROG"
WRITE "lonely" ON F, "_ORPHAN"'
  ( cd "$OBA" && git init -q . >/dev/null 2>&1; git config mvx.openaccount true )
  GITV "$OBA" GIT INIT   >/dev/null
  GITV "$OBA" GIT ADD -A >/dev/null
  ost="$( cd "$OBA" && git diff --cached --name-only )"
  t  "the source travels"                 "MYDATA/PROG"    "$ost"
  tn "its object does not, by NAME"       "MYDATA/_PROG"   "$ost"
  # `_ORPHAN` has no `ORPHAN` beside it, so it is somebody's record whose name
  # begins with an underscore -- not an object.  The naming rule is exact in
  # both directions or it is just another guess.
  t  "an underscore with no source is content" "MYDATA/_ORPHAN" "$ost"
  ;;
esac

say "-- the CLI and the verb stage the same wholesale set (mv_git#141, #142) --"
# TWO WAYS TO SKIP THE EXCLUSIONS, and UniVerse hit both:
#
#   #141  BP/GIT.ADD set BLANKET only for -A, so `add VOC` turned every
#         wholesale exclusion off -- while the ENGINE treats a file-only add as
#         wholesale.  Naming a FILE is not naming a record.
#   #142  the engine matched the WHOLE of attribute 1 against the type table,
#         and UniVerse writes an empty CREATE.FILE description as "F " -- with
#         the trailing space -- so `mv_voc_class` answered 0 for every file in
#         the account and the class-2 rules never fired at all.  `CT` trims that
#         space on the way to the screen, which is what makes it easy to miss.
#
# Neither showed as a wrong ANSWER, only as the two routes disagreeing -- so the
# assertion is that they agree, on a set that exercises both: furniture (the
# account's own work file), a derived pointer, and a record that must survive.
case "$PLATFORM" in
udt|uv)
  WSA="$WORK/wholesale"; ACCT "$WSA"; LINK "$WSA"; CF "$WSA" OWN
  SEED "$WSA" 'OPEN "VOC" TO V ELSE STOP
P = "" ; P<1> = "PA" ; P<2> = "HELLO"
WRITE P ON V, "MYPARA"'
  ( cd "$WSA" && git init -q . >/dev/null 2>&1; git config mvx.openaccount true )
  GITV "$WSA" GIT INIT >/dev/null
  vocof() { ( cd "$1" && git diff --cached --name-only ) | grep '^VOC/' | sort | tr '\n' ' '; }
  GITV "$WSA" GIT ADD VOC >/dev/null
  wverb="$(vocof "$WSA")"
  ( cd "$WSA" && git reset -q >/dev/null 2>&1; "$MVXGIT" add VOC >/dev/null 2>&1 )
  wcli="$(vocof "$WSA")"
  # ASSERTED PER ROUTE rather than by comparing the two sets outright.  They are
  # not identical on UniVerse for a reason that has nothing to do with either
  # bug: the stock baseline (#46) is built lazily by uv-git, so a verb-only
  # account has none and keeps the records a CLI-touched one subtracts.  What
  # #141 and #142 are about is whether the EXCLUSIONS run at all, and that is
  # what these four say.
  t  "a file-only add keeps the account's own record" "VOC/MYPARA" "$wverb"
  tn "and drops the derived pointer"                  "VOC/OWN"    "$wverb"
  t  "the CLI keeps it too"                           "VOC/MYPARA" "$wcli"
  tn "and drops the derived pointer too"              "VOC/OWN"    "$wcli"
  ;;
esac

say "-- a pointer to what the repository does NOT carry travels too (mv_git#132) --"
# CLASS 2 MEANS "a pointer to a file THIS REPOSITORY CARRIES", and nothing else.
# Such a pointer is derived -- CREATE.FILE writes it and <file>.DICT/%FILE%
# rebuilds it -- so it is dropped in every form (#131).  The open interchange
# used to drop the whole CLASS instead, on that same reasoning applied to
# pointers the reasoning does not fit:
#
#   Q       names a file in ANOTHER account
#   R       names a program in another file
#   F/DIR   whose path leaves the account
#
# No %FILE% describes any of them and no CREATE.FILE on the far side writes
# them, so dropping them lost the account's own configuration -- silently, and
# only in the portable form, which is the one that travels between systems.
#
# ASSERTED IN THE OPEN FORM, since that is the only form they were ever lost in,
# and with a local pointer alongside as the CONTROL: if the derived rule stopped
# working, VOC/OWN would appear and this test would say so.
case "$PLATFORM" in
udt|uv)
  PTR="$WORK/ptrs"; ACCT "$PTR"; LINK "$PTR"; CF "$PTR" OWN
  SEED "$PTR" 'OPEN "VOC" TO V ELSE STOP
Q = "" ; Q<1> = "Q" ; Q<2> = "OTHERACCT" ; Q<3> = "SHARED"
WRITE Q ON V, "QPTR"
R = "" ; R<1> = "R" ; R<2> = "BP" ; R<3> = "MYPROG"
WRITE R ON V, "RPTR"
D = "" ; D<1> = "DIR" ; D<2> = "/tmp/elsewhere" ; D<3> = "/tmp/elsewhere"
WRITE D ON V, "FARDIR"'
  # THROUGH THE VERB, and `-A` rather than `add VOC`.  Both matter:
  #
  #   `add VOC` is not wholesale everywhere -- BP/GIT.ADD sets BLANKET only for
  #   -A, while the engine treats a file-only add as wholesale, so the exclusions
  #   apply on UniData and not on UniVerse (mv_git#141).
  #
  #   The CLI is not one route either.  On UniVerse it reaches records through
  #   mvgitd, which has NO record access, so the derived-pointer test
  #   (`is this file here?`) cannot fire and the control below fails for a
  #   reason that has nothing to do with the rule under test (mv_git#142).
  #
  # The verb runs the shared policy on every platform, which is what this is
  # about: the VOCDROP op decides it once for all of them (#133).
  ( cd "$PTR" && git init -q . >/dev/null 2>&1; git config mvx.openaccount true )
  GITV "$PTR" GIT INIT   >/dev/null
  GITV "$PTR" GIT ADD -A >/dev/null
  pst="$( cd "$PTR" && git diff --cached --name-only )"
  t  "a Q pointer travels"              "VOC/QPTR"   "$pst"
  t  "an R pointer travels"             "VOC/RPTR"   "$pst"
  t  "a foreign DIR pointer travels"    "VOC/FARDIR" "$pst"
  tn "but the account's own file pointer does not" "VOC/OWN" "$pst"
  ;;
esac

say "-- an X record is data, not a pointer, so it travels (mv_git#136) --"
# X is UniVerse's miscellaneous-DATA type: RELLEVEL (14.2.1 / PICK), INTR.KEY,
# QUIT.KEY -- values, not references.  It was grouped with Q and R as an
# "account/remote pointer" and so dropped in the open interchange, which silently
# lost the settings a site keeps in its own VOC.  Nothing points anywhere and
# nothing on the far side recreates it, so it is the account's own and travels.
#
# In the OPEN form deliberately: that is the only form it was ever lost in, and
# the stock X records are subtracted as furniture (#46) either way, so this can
# only be tested with one the account put there itself.
case "$PLATFORM" in
udt|uv)
  XA="$WORK/xrec"; ACCT "$XA"; LINK "$XA"
  SEED "$XA" 'OPEN "VOC" TO V ELSE STOP
X = ""
X<1> = "X"
X<2> = "our-site-setting-42"
WRITE X ON V, "SITECFG"'
  ( cd "$XA" && git init -q . >/dev/null 2>&1
    git config mvx.openaccount true
    "$MVXGIT" init >/dev/null 2>&1
    "$MVXGIT" add VOC >/dev/null 2>&1 )
  t  "the site's own X record travels" "VOC/SITECFG" \
     "$( cd "$XA" && git diff --cached --name-only )"
  ;;
esac

say "-- a catalogued item is recreated by cataloguing, so it does not travel (mv_git#137) --"
# The same rule as a file's own pointer: what the platform writes for you when
# you catalogue is plumbing, and cataloguing on the far side writes it again.
#
# It bit hardest on UniData's LOCAL catalog, whose VOC record is type `C` and
# holds an ABSOLUTE path into CTLG -- a directory that is furniture and never
# committed -- so every clone got an entry naming a path from the machine that
# made the commit.  UniVerse writes `V` for the same act, which was already
# dropped; UniData's GLOBAL catalog writes no VOC record at all.
#
# Only where the platform puts catalogued items in the master file: on MVX the
# verb is a `V` record the account owns, and that is a separate decision (#137).
case "$PLATFORM" in
udt|uv)
  CATA="$WORK/catv"; ACCT "$CATA"; LINK "$CATA"
  printf 'PRINT "hi"\n' > "$CATA/BP/CATPROG" 2>/dev/null || \
    { mkdir -p "$CATA/BP"; printf 'PRINT "hi"\n' > "$CATA/BP/CATPROG"; }
  if [ "$PLATFORM" = udt ]; then
    ( cd "$CATA" && printf 'BASIC BP CATPROG\nCATALOG BP CATPROG LOCAL\nQUIT\n' | "$MVX" ) >/dev/null 2>&1
  else
    ( cd "$CATA" && printf 'BASIC BP CATPROG\nCATALOG BP CATPROG LOCAL\nQUIT\n' | "$MVX" ) >/dev/null 2>&1
  fi
  # IT REALLY WAS CATALOGUED.  Otherwise the assertion below is the absence of
  # something that was never there, which is no assertion at all -- and the
  # obvious spelling, `t "..." "CATPROG"`, is exactly that trap: UniData answers
  # a missing record with "CATPROG is not a record in VOC.", which contains the
  # name.  So assert on what only a PRESENT record can say.
  tn "the program catalogued" "not a record" "$(CT "$CATA" VOC CATPROG)"
  ( cd "$CATA" && git init -q . >/dev/null 2>&1
    "$MVXGIT" init >/dev/null 2>&1
    "$MVXGIT" add VOC >/dev/null 2>&1 )
  # A CONTROL THAT MUST BE STAGED.  `add VOC` on a fresh account stages nothing
  # else -- every other record is stock -- so the absence of VOC/CATPROG was
  # being asserted against EMPTY output, which an `add` that did nothing at all
  # would also produce.  MYPARA is the account's own and has to come through, so
  # emptiness can no longer pass for success (mv_git#134).
  SEED "$CATA" 'OPEN "VOC" TO V ELSE STOP
P = ""
P<1> = "PA"
P<2> = "HELLO"
WRITE P ON V, "MYPARA"'
  ( cd "$CATA" && "$MVXGIT" add VOC >/dev/null 2>&1 )
  cstaged="$( cd "$CATA" && git diff --cached --name-only )"
  t  "the account's own record is staged" "VOC/MYPARA"  "$cstaged"
  tn "but its VOC entry stays out"        "VOC/CATPROG" "$cstaged"
  ;;
esac

say "-- a wholesale add reaches EVERY master-file record (mv_git#131) --"
# THE WHOLE FILE, not the first few.  backend_has_file() answers from a cached
# file list, and building that list means a SELECT of its own -- so asked lazily
# from inside this loop it clobbered the select the loop was reading, READNEXT
# stopped early, and `add` walked off the end having staged almost nothing.
#
# Silent, and that is why it needs its own test: no error, and the "ignored"
# count still looked plausible because records never reached are never counted
# either.  Measured on UniData 8.3, `udt-git add VOC` on an account holding two
# records of the user's own: "2 staged / 616 ignored" became "0 staged / 20
# ignored", and the suite stayed green.
#
# ITS OWN ACCOUNT, and a COUNT rather than a sample.  The walk stops at the
# first pointer it has to ask about, so how much survives depends on where the
# hash happens to put things -- in the shared account these same records landed
# early and every sampled assertion passed while the file WAS being cut off.
# Thirty records and an exact count is the assertion that cannot be lucky.
#
# THROUGH THE CLI, deliberately: the verb and the CLI walk the master file with
# different code -- the verb's loop is in BP/GIT.ADD, the CLI's is the engine's
# -- and this bug was in the engine's.  The udt arm drives the verb everywhere
# else, which is exactly why nothing saw it.
MF="$WORK/mfwalk"; ACCT "$MF"; LINK "$MF"
SEED "$MF" 'OPEN "VOC" TO V ELSE STOP
P = ""
P<1> = "PA"
P<2> = "HELLO"
FOR I = 1 TO 30
   WRITE P ON V, "MYPARA":I
NEXT I'
( cd "$MF" && git init -q . >/dev/null 2>&1
  "$MVXGIT" init >/dev/null 2>&1
  "$MVXGIT" add VOC >/dev/null 2>&1 )
te "all thirty of the account's own master-file records travel" "30" \
   "$( cd "$MF" && git diff --cached --name-only | grep -c '^VOC/MYPARA' )"

say "-- a file's own pointer is derived, not content (mv_git#131) --"
# CREATE.FILE writes the VOC/MD pointer and DELETE.FILE removes it, and
# <file>.DICT/%FILE% carries the geometry to write it again -- so committing the
# pointer is the same fact twice, free to disagree.  It is dropped in EVERY
# form now, native as much as open.
#
# ASSERTED ON BOTH SIDES, because the engine and BP/GIT.ADD each carry a copy of
# this rule and drift between them is the recurring bug here (#51, #95, #108).
CF "$A" PTRTEST
SEED "$A" 'OPEN "PTRTEST" TO F ELSE STOP
WRITE "row" ON F, "R1"'
GITV "$A" GIT ADD -A >/dev/null
st="$(GITV "$A" GIT STATUS)"
t  "the file's records travel"  "PTRTEST/R1"           "$st"
t  "and its geometry"           "PTRTEST.DICT/%FILE%"  "$st"
tn "but not its VOC pointer"    "VOC/PTRTEST"          "$st"
GITV "$A" GIT COMMIT -m ptrtest >/dev/null
# ...and the deletion still lands without the pointer to carry it: %FILE% and
# the records both go.  This is what the pointer used to signal.
DF "$A" PTRTEST
st="$(GITV "$A" GIT STATUS)"
# THE RECORDS are the portable assertion.  What accompanies them differs by
# platform and neither spelling is wrong: on MVX %FILE% is a real record on disk
# and goes with the file, so status reports `D PTRTEST.DICT/%FILE%`; on UniData
# the control is SYNTHESISED at add time from mv_fileclass, which cannot
# describe a file that is gone, so the dictionary's own `@ID` item carries the
# deletion instead.  Asserting either one here would pass on one platform and
# fail on the other while the behaviour was right on both.
t  "deleting the file shows its records gone" "D PTRTEST/R1" "$st"

say "-- catalog: an account's object directories are not content (mv_git#130) --"
# A catalogued program is BUILT from the source beside it: the account gets its
# own copy back by compiling, and a clone that carried one would carry a binary
# for whatever host committed it.  So the directory it lands in is plumbing --
# CATALOG, LIB and bin on MVX, bin and lib on jBASE, CTLG on UniData -- and a
# wholesale add leaves the file, its dictionary AND its VOC pointer out.
#
# STATUS IS ASSERTED, not just the index, and that is the point of testing them
# together: a record `add` will never stage is a record `status` must never
# report, or the account is untracked for ever and no commit can clear it.  That
# asymmetry is what #51 and #95 both were, and it is what this found on the
# native form -- add skipped the pointer, status named it.
# LIB, not CATALOG, and the difference is the point of testing on more than one
# platform: `CATALOG` is a stock VOC *verb* on UniData ("V" / CATALOG), so
# CREATE.FILE refuses the name outright -- "CATALOG is a record in VOC file".
# LIB is in the same shared list, is nobody's stock record on any of them, and
# is one of the three MVX's own runtime reserves.
CF "$A" LIB
SEED "$A" 'OPEN "LIB" TO F ELSE STOP
WRITE "objectcode" ON F, "PROG"'
GITV "$A" GIT ADD -A >/dev/null
tn "catalog stays out entirely" "LIB/PROG" "$(GITV "$A" GIT STATUS)"
tn "and so does its geometry"   "LIB.DICT" "$(GITV "$A" GIT STATUS)"
# The escape hatch every other exclusion has: naming it is a deliberate act.
GITV "$A" GIT ADD LIB >/dev/null
t  "an explicit add still stages it" "LIB/PROG" "$(GITV "$A" GIT STATUS)"

# One repo, one index, one commit, with accounts as subdirectories beside
# ordinary files.  uv-git has walked such a repository for some time; mvx-git
# fell through to plain git, which cannot see records and staged the backend
# store as a blob — the one thing the format says must never be committed.
# All three walk now (mv_git#44).
case "$PLATFORM" in
mvx|uv|udt)
  say "-- several accounts in one repository --"
  MR="$WORK/multi"; mkdir -p "$MR"
  ( cd "$MR" && git init -q . && printf '# repo\n' > README.md )
  for m in mA mB; do
    ACCT "$MR/$m"; LINK "$MR/$m"; CF "$MR/$m" CUST
    SEED "$MR/$m" 'OPEN "CUST" TO F ELSE STOP
WRITE "Ada":@AM:"London" ON F, "M1"'
  done
  ( cd "$MR" && "$MVXGIT" add -A >/dev/null 2>&1 )
  paths="$( cd "$MR" && git ls-files )"
  t  "both accounts staged"   "mA/CUST/M1"  "$paths"
  t  "and the second"         "mB/CUST/M1"  "$paths"
  t  "repo files staged too"  "README.md"   "$paths"
  case "$paths" in
    *lmdb*|*.uvdata*) bad "backend store kept out" "no store" "staged";;
    *) ok "backend store kept out";;
  esac
  ;;
esac

say "== $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
