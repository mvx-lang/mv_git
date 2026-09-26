#!/bin/sh
# build-qm.sh — build qm-git (OpenQM / ScarletDME) and stage its release package.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
#
# FIRST PASS (mv_git#243).  The engine (src/mvxgit.c) is shared with mvx-git,
# udt-git and jb-git; here it is compiled with -DMVXGIT_QM so its record
# primitives bind to QMClient (src/qmgit_rt.c).
#
# QM is the MVX/jBASE case rather than the UniData one: a standalone process
# makes its own connection with QMConnectLocal() and calls the record API
# itself, so there are no CallC objects to build and no background daemon.
#
# ONLY THE CLI IS BUILT HERE, DELIBERATELY.  The in-session verb wants the
# CCALL route (QM's dh_ layer, in-process), and that needs three fixes that are
# not yet in ScarletDME — geneb/ScarletDME#109 (-rdynamic, so a CCALL'd library
# can resolve dh_open at all), #111 (CCALL truncated every pointer argument to
# 32 bits) and #113 (a heap overflow in CCALL's own argument handling).  The
# CLI needs none of them: QMClient is a supported public API and ships with
# stock ScarletDME, which is why this half can be built today.
#
# Requires: a C compiler; libgit2 under $LIBGIT2_PREFIX (or LIBGIT2_CFLAGS /
# LIBGIT2_LIBS); and a ScarletDME installation ($QMDIR, default /usr/qmsys,
# with bin/libqmcli.so) plus its qmclilib.h (in $QMDIR/SYSCOM, or point
# $QMINCLUDE at the source tree's qmsys/SYSCOM).
#
#   sh build-qm.sh [stagedir]        # default: ./stage
set -e
STAGE="${1:-./stage}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"
CC="${CC:-cc}"
QMDIR="${QMDIR:-/usr/qmsys}"
QMINCLUDE="${QMINCLUDE:-$QMDIR/SYSCOM}"
# The in-session verb compiles against QM's OWN headers (qm.h, dh.h, syscom.h),
# which ship with the source rather than the install -- so it needs the source
# tree, and says so rather than failing on a missing include.
QMSRC="${QMSRC:-/usr/src/ScarletDME}"

[ -f "$QMINCLUDE/qmclilib.h" ] || {
    echo "build-qm: no qmclilib.h under $QMINCLUDE" >&2
    echo "  set QMINCLUDE (the ScarletDME source tree has it in qmsys/SYSCOM)" >&2
    exit 1
}

. "$HERE/version.sh"
UGVER="${MV_GIT_VERSION:-$(mv_git_version "$HERE")}"

LG2_CFLAGS="${LIBGIT2_CFLAGS:-$(pkg-config --cflags libgit2 2>/dev/null || echo "-I${LIBGIT2_PREFIX:-/usr/local}/include")}"
LG2_LIBS="${LIBGIT2_LIBS:-$(pkg-config --libs libgit2 2>/dev/null || echo "-L${LIBGIT2_PREFIX:-/usr/local}/lib -lgit2")}"

# -rpath, so the binary finds libqmcli without LD_LIBRARY_PATH.  QMClient lives
# in QM's own bin/ and is on nobody's default library path, so without this
# qm-git installs perfectly and then fails at startup with "error while loading
# shared libraries: libqmcli.so" -- which reads as a broken build rather than a
# missing path.
mkdir -p "$HERE/bin"
"$CC" -std=c11 -O2 -DMVXGIT_QM -DMVXGIT_VERSION="\"$UGVER\"" \
      -I"$SRC" -I"$QMINCLUDE" $LG2_CFLAGS \
      "$SRC/qm-git.c" "$SRC/mvxgit.c" "$SRC/qmgit_rt.c" \
      -L"$QMDIR/bin" -Wl,-rpath,"$QMDIR/bin" -lqmcli \
      $LG2_LIBS -lm -ldl -lpthread \
      -o "$HERE/bin/qm-git"
echo "  built bin/qm-git (record-git engine + QMClient record layer)"

# The in-session verb: the same engine again, as a shared library the BASIC
# reaches through CCALL.  It calls QM's OWN dh_ layer (qmgit_dh.c), in the
# caller's process, so the verb reads the records of the session that typed
# the sentence -- which is the whole reason for a verb rather than shelling
# out to the CLI.
#
# IT NEEDS A QM BUILT WITH THREE FIXES, and is skipped rather than shipped
# broken when they are absent: geneb/ScarletDME#109 (-rdynamic, or the library
# cannot resolve dh_open and dlopen returns NULL), #111 (CCALL truncated every
# pointer argument to 32 bits) and #113 (a heap overflow in CCALL's own
# argument handling).  The -rdynamic one is testable from here, and stands for
# all three: they are the same three-patch set.
QMBIN="${QMBIN:-$QMDIR/bin/qm}"
ENGINE=no
if [ -x "$QMBIN" ] && nm -D --defined-only "$QMBIN" 2>/dev/null | grep -q ' dh_open$'; then
    ENGINE=yes
fi

if [ "$ENGINE" = yes ]; then
    "$CC" -std=c2x -O2 -fPIC -shared \
          -D_DEFAULT_SOURCE=1 -DLINUX -D_FILE_OFFSET_BITS=64 -DGPL \
          -DMVXGIT_QM -DMVXGIT_VERSION="\"$UGVER\"" \
          -I"$SRC" -I"$QMSRC/gplsrc" $LG2_CFLAGS \
          "$SRC/qmcallc.c" "$SRC/mvxgit.c" "$SRC/qmgit_dh.c" \
          $LG2_LIBS -lm -ldl -lpthread \
          -o "$HERE/bin/libqmgit.so"
    echo "  built bin/libqmgit.so (the QMGIT entry point for the in-session verb)"
else
    echo "  SKIPPED bin/libqmgit.so: $QMBIN exports no dh_open, so this QM was"
    echo "          built without -rdynamic (geneb/ScarletDME#109).  The CLI is"
    echo "          unaffected; the in-session verb needs that fix and #111/#113."
fi

# PLATFORM.H -- the compile-time platform defines the BASIC sources $INCLUDE.
# Built here for the same reason build-jbase.sh builds its own: it is build
# output.
#
# ENGINE says the record-git engine can be CALLED here rather than
# reimplemented in BASIC, and is written only when libqmgit.so was built just
# above -- so a stock QM gets a PLATFORM.H that does not claim an engine it
# has not got, and its handlers take the CLI arm instead.
mkdir -p "$STAGE/mv_git"
{
cat <<'PLATEOF'
* PLATFORM.H - compile-time platform defines for the MV BASIC sources.
*
* Generated by build-qm.sh.  MV is every MultiValue platform; QM is this one.
*
* ENGINE says the record-git engine is callable IN-PROCESS, so a handler calls
* it rather than reimplementing it in BASIC.  It is written here only when
* libqmgit.so was actually built -- which needs a QM carrying
* geneb/ScarletDME#109, #111 and #113.  Against a stock QM the library is not
* built and this file does not claim ENGINE, so the handlers take the same arm
* UniData and UniVerse take and the CLI does the record work.  A PLATFORM.H
* that promised an engine which is not there would fail at RUN time, one
* handler at a time.
*
* ASCII ONLY, deliberately: every BASIC source on every platform $INCLUDEs this
* file, so it is the worst possible place to find out that some compiler does
* not like a byte above 127 in a comment.
*
* THERE IS NO `$DEFINE QM' HERE, AND THAT IS NOT AN OMISSION.  QM's own
* compiler already defines the token QM, so writing it again is a hard error --
*
*     26.18: Duplicate $DEFINE/EQUATE token
*
* -- reported against the $INCLUDE line, in every source that includes this
* file, which is all of them.  `$IFDEF QM' works here whether or not anyone
* declares it; the platform arm is the compiler's, and this file only has to
* not fight it.
$DEFINE MV
*
* MVMASTER is the account's own master file, under the name THIS platform uses
* for it -- VOC here, as everywhere except jBASE, which calls it MD.  An EQU
* rather than a valued $DEFINE: UniData's $DEFINE takes no value at all, so the
* four ports agree on this form.
      EQU MVMASTER TO "VOC"
PLATEOF
[ "$ENGINE" = yes ] && echo '$DEFINE ENGINE'
} > "$STAGE/mv_git/PLATFORM.H"
if [ "$ENGINE" = yes ]; then
    echo "  wrote PLATFORM.H (MV, ENGINE; QM is the compiler's own)"
    cp "$HERE/bin/libqmgit.so" "$STAGE/mv_git/"
else
    echo "  wrote PLATFORM.H (MV; QM is the compiler's own)"
fi

cp "$HERE/bin/qm-git" "$STAGE/mv_git/"

# --- the BASIC ------------------------------------------------------------
#
# WHAT SHIPS DEPENDS ON WHETHER THERE IS AN ENGINE, because the two routes have
# no source in common:
#
#   ENGINE -- the real verb.  BP/GIT and the handlers behind it are the SHARED
#             sources, the same ones MVX and jBASE compile, plus this port's
#             wrappers (GITADD ...) and the CCALL bridge they all go through.
#
#   stock  -- qm/BP/GIT.CLI, installed as BP/GIT, and nothing else.  The shared
#             handlers would compile against an engine that is not there.
#
# A STALE STAGE SHIPS A PROGRAM NOBODY BUILT, so the directory is emptied first
# rather than copied over -- the trap build-jbase.sh documents.
rm -rf "$STAGE/mv_git/BP"; mkdir -p "$STAGE/mv_git/BP"
if [ "$ENGINE" = yes ]; then
    # FILES only, and every one gets a trailing newline if it lacks one: an
    # unterminated last line is the kind of thing that compiles on one platform
    # and not another, and costs nothing to settle here.
    for f in "$HERE"/BP/*; do
        [ -f "$f" ] || continue
        b=$(basename "$f")
        cp "$f" "$STAGE/mv_git/BP/$b"
        [ -n "$(tail -c 1 "$STAGE/mv_git/BP/$b")" ] && printf '\n' >> "$STAGE/mv_git/BP/$b"
    done
    # ...then this port's own on top.  They cannot collide: a wrapper is
    # GITSTATUS, a handler is GIT.STATUS.  GIT.CLI is the stock shim and has no
    # job in an engine package.
    for f in "$HERE"/qm/BP/*; do
        [ -f "$f" ] || continue
        b=$(basename "$f")
        [ "$b" = GIT.CLI ] && continue
        cp "$f" "$STAGE/mv_git/BP/$b"
    done
    echo "  staged BP/ ($(ls "$STAGE/mv_git/BP" | wc -l | tr -d ' ') programs: the shared verb, the wrappers and the bridge)"

    # EVERY ENGINE OP MUST HAVE A WRAPPER THAT AGREES WITH THE SHARED CALL.
    #
    # GIT.OBJ's engine arm is written against MVX's subroutine signatures.  A
    # wrapper whose parameter list disagrees is NOT a compile error on QM -- it
    # is a run-time fault, on whichever assertion happens to reach that op.
    # That is exactly how GITVOCDROP shipped taking three arguments and sending
    # PRUNEGONE, which is a different engine function altogether.  build-jbase.sh
    # makes the same comparison for the same reason; this is its QM half.
    #
    # EVERY handler, not just GIT.OBJ: GIT.OBJ holds fifteen of the thirty-five
    # engine calls and the rest are made straight from the handler that needs
    # them (GIT.ADD calls GITADD itself).  Checking only the one file is
    # checking less than half of them.
    mismatch=0
    for f in "$HERE"/BP/*; do
        [ -f "$f" ] && sed -n '/\$IFDEF ENGINE/,/\$ENDIF/p' "$f"
    done | grep -oE 'CALL GIT[A-Z]+\([^)]*\)' | sort -u > "$STAGE/.engineops" || true
    while read -r call; do
        [ -n "$call" ] || continue
        name=$(printf '%s\n' "$call" | sed 's/CALL \([A-Z.]*\)(.*/\1/')
        # printf '%s\n', not '%s': without the trailing newline `wc -l` counts
        # one separator fewer than there are arguments and every op reads as a
        # mismatch.
        want=$(printf '%s\n' "$call" | sed 's/.*(\(.*\))/\1/' | tr ',' '\n' | wc -l | tr -d ' ')
        wrap="$HERE/qm/BP/$name"
        [ -f "$wrap" ] || { echo "  ERROR: $name is called under \$IFDEF ENGINE and has no QM wrapper" >&2
                            mismatch=1; continue; }
        got=$(grep -oE "SUBROUTINE $name\([^)]*\)" "$wrap" \
              | sed 's/.*(\(.*\))/\1/' | tr ',' '\n' | wc -l | tr -d ' ')
        if [ "$want" != "$got" ]; then
            echo "  ERROR: $name — the handlers call it with $want args, wrapper declares $got" >&2
            mismatch=1
        fi
    done < "$STAGE/.engineops"
    nops=$(wc -l < "$STAGE/.engineops" | tr -d ' ')
    rm -f "$STAGE/.engineops"
    [ "$mismatch" = 0 ] || { echo "build-qm.sh: engine wrapper signatures disagree" >&2; exit 1; }
    echo "  engine wrappers agree with the handlers ($nops calls)"
else
    cp "$HERE/qm/BP/GIT.CLI" "$STAGE/mv_git/BP/GIT"
    echo "  staged BP/GIT (the shim over the CLI; no engine here to call)"
fi
[ -f "$HERE/qm/install.sh" ] && { cp "$HERE/qm/install.sh" "$STAGE/mv_git/"; chmod +x "$STAGE/mv_git/install.sh"; }
[ -f "$HERE/LICENSE" ] && cp "$HERE/LICENSE" "$STAGE/mv_git/"
[ -f "$HERE/README.md" ] && cp "$HERE/README.md" "$STAGE/mv_git/"
echo "build-qm: staged the OpenQM package as $STAGE/mv_git/"
