#!/bin/sh
# VENDORED VERBATIM from mvx-lang/mv_package (udt/udt-callc-build.sh).  Keep the
# body a byte-for-byte copy of upstream: a standalone udt-git install (install.sh)
# stages its CallC fragment EXACTLY as an MVPKG install would, so a box that
# installs git standalone and later adopts MVPKG converges on the identical
# libu2callc.so.  If upstream changes, re-vendor rather than editing here.
#
# mv_package — rebuild UniData's shared CallC library from every installed
# package that compiles native code into UniData.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only.
#
# UniData loads exactly one libu2callc.so, so native add-ons cannot each own
# it — they must be aggregated.  Every package that reaches into UniData
# through CallC contributes a fragment; this rebuilds the library from the
# UNION of all currently-installed fragments.  Install or remove a package,
# run this, and the library reflects exactly what is installed — no package
# has to know about any other.
#
# A contribution is a directory (staged as $UDTHOME/callc.d/<pkg>/) holding:
#   *.c            C sources compiled into the library
#   *.o            OR pre-built objects (binary release — no source shipped)
#   funcs          cfuncdef declarations, one per line:
#                     name:rettype:nargs:argtypes    (curses/git use string(string))
#   libs           optional: one line of extra linker flags for this package
#                     (e.g. the git bridge's  -Wl,-rpath,/usr/local/lib64 -L... -lgit2)
#
# The system base functions are injected by gencdef, so a fragment lists
# only its own functions.  Needs gcc and the UniData generators
# (gencdef/genefs/genfunc, on PATH) with efsdef + libuvic.a in the work dir.
#
# Usage:
#   udt-callc-build.sh                       rebuild from every staged fragment
#   udt-callc-build.sh add <pkgdir> <name>   stage <pkgdir>/udt-callc as <name>, rebuild
#   udt-callc-build.sh remove <name>         unstage <name>, rebuild
# The package manager calls `add`/`remove`; a bare call is a plain rebuild.
set -e

: "${UDTHOME:?set UDTHOME to your UniData home (e.g. /usr/ud83)}"
# The UniData generators this runs (gencdef/genefs/genfunc) and the other udt
# tools it relies on live in $UDTHOME/bin.  A UniData LOGIN shell has that on
# PATH, but MVPKG's MVPKGOS "CALLC" op invokes this from a piped, NON-login udt
# session (EXECUTE '!udt-callc-build ...') whose PATH may not — so put it on PATH
# here rather than trust the caller's environment (else: "gencdef: not found").
PATH="$UDTHOME/bin:$PATH"; export PATH
SUDO=${SUDO-sudo}
CALLCD="${UDT_CALLCD:-$UDTHOME/callc.d}"   # one subdir per contributing package
WORK="$UDTHOME/bin/work"                   # UniData's generators + efsdef + libuvic.a
LIB="$UDTHOME/bin/libu2callc.so"           # the shared library every session loads

# --- optional staging: add/remove a package's contribution --------------
case "$1" in
add)
	src="$2/udt-callc" ; pkg=$(printf '%s' "$3" | tr '/' '_')   # scope slash -> _
	: "${pkg:?add: need <pkgdir> <name>}"
	if [ ! -d "$src" ]; then
		echo "udt-callc: $2 ships no udt-callc/ contribution — nothing to build"
		exit 0                       # not a native package; a normal install
	fi
	$SUDO mkdir -p "$CALLCD/$pkg"
	$SUDO rm -f "$CALLCD/$pkg"/* 2>/dev/null || true
	$SUDO cp "$src"/* "$CALLCD/$pkg/"
	echo "udt-callc: staged $pkg" ;;
remove)
	pkg="$2" ; : "${pkg:?remove: need <name>}"
	$SUDO rm -rf "${CALLCD:?}/$pkg"
	echo "udt-callc: unstaged $pkg" ;;
esac

# Every path below (re)links the library and needs a compiler.  A no-callc `add`
# already exited 0 above, so this only gates packages that actually contribute
# native code — fail early and clearly rather than deep in the gcc invocations.
command -v gcc >/dev/null 2>&1 || {
	echo "udt-callc: gcc not found — CallC packages need a compiler to (re)link $LIB." >&2
	echo "udt-callc: install it (e.g. 'sudo dnf install -y gcc') plus any package -devel libs, then retry." >&2
	exit 3
}

BUILD=$(mktemp -d "${TMPDIR:-/tmp}/udtcallc.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT
cd "$BUILD"
cp "$WORK/efsdef" "$WORK/libuvic.a" .

echo "udt-callc: rebuilding $LIB from $CALLCD"

# --- collect fragments --------------------------------------------------
: > FUN ; : > OBJ ; : > LIBS
OBJPATHS=""
NPKG=0
for d in "$CALLCD"/*/ ; do
	[ -d "$d" ] || continue
	pkg=$(basename "$d")
	NPKG=$((NPKG + 1))
	echo "udt-callc:   + $pkg"
	[ -f "$d/funcs" ] && cat "$d/funcs" >> FUN
	[ -f "$d/libs"  ] && cat "$d/libs"  >> LIBS
	# compile any sources, namespaced so two packages can share a basename
	for c in "$d"*.c ; do
		[ -f "$c" ] || continue
		o="$BUILD/${pkg}__$(basename "${c%.c}").o"
		gcc -m64 -fPIC -O2 -c "$c" -o "$o"
		OBJPATHS="$OBJPATHS $o"
		basename "$o" >> OBJ
	done
	# include any pre-built objects (binary-only release)
	for o in "$d"*.o ; do
		[ -f "$o" ] || continue
		OBJPATHS="$OBJPATHS $o"
		basename "$o" >> OBJ
	done
done
echo "udt-callc: $NPKG package(s), $(grep -c ':' FUN 2>/dev/null || echo 0) function(s)"

# --- assemble the add-on cfuncdef (system base is added by gencdef) -----
{ echo '$$FUN'; cat FUN; echo '$$OBJ'; cat OBJ; echo '$$LIB'; } > cfuncdef

# --- regenerate dispatch/glue and compile -------------------------------
rm -f cdef
gencdef ; genefs ; genfunc
CF="-I$UDTHOME/bin/include -m64 -DUV_64PORT -DU2_64_BUILD -fPIC -DLINUX9 \
    -DU_LINUX -DU2_LINUX -DU_NO_POLL -DUNIDATAon -D_LARGEFILE64_SOURCE \
    -D_FILE_OFFSET_BITS=64 -DNDEBUG -O2"
for c in callcf interfunc efs_init funchead; do gcc $CF -c $c.c -o $c.o; done

# --- link: base glue + every package object + base libs + package libs --
EXTRALIBS=$(tr '\n' ' ' < LIBS)
gcc -m64 -shared -fPIC -z muldefs $EXTRALIBS -L/lib64 -L/usr/lib64 \
    funchead.o interfunc.o callcf.o efs_init.o $OBJPATHS libuvic.a \
    /lib64/libglib-2.0.so.0 -lm -lncurses -lrt /lib64/libcrypt.so.1 \
    /lib64/libgdbm.so.6 -ldl /lib64/libpam.so.0 \
    -o libu2callc.so

# --- install the library and the canonical definition -------------------
# Replace the library by ATOMIC RENAME, never an in-place cp.  UniData mmaps
# $UDTHOME/bin/libu2callc.so per session; a cp overwrites the SAME inode
# (truncate + rewrite), corrupting that mapping in every live session — the
# session that triggered the rebuild then segfaults on exit.  Writing a fresh
# inode and rename(2)-ing it into place leaves existing mappings on the old
# inode intact (it survives until the last session drops it); only sessions
# started after the swap pick up the new library.  $LIB.new is in the same
# directory as $LIB, so mv is a true same-filesystem rename, not a copy.
# ($UDTHOME/bin/work/cfuncdef is the registry CALLC.EXISTS reads.)
$SUDO cp -p "$LIB" "$LIB.prev" 2>/dev/null || true
$SUDO cp libu2callc.so "$LIB.new"
$SUDO mv -f "$LIB.new" "$LIB"
$SUDO cp cfuncdef "$WORK/cfuncdef"

echo "udt-callc: installed — $(nm -D "$LIB" | grep -c ' T ') exported CallC function(s)"
