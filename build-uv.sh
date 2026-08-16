#!/bin/sh
# build-uv.sh — build the UniVerse pieces and stage their release package.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
#
# Runs INSIDE the uv-builder container.  The counterpart of build-udt.sh, and it
# stages a different set because UniVerse reaches libgit2 differently: there are
# no CallC objects, since BASIC cannot call libgit2 in-process here at all (GCI
# is licensed and dead in the TE; InterCall is a client SDK unavailable for
# Linux).  The git-object work runs in mvgitd, a background process the session
# talks to over a pipe, and uv-git is the shell-side entry point that drives the
# in-session verb.  See mv_git#43.
#
#   ./build-uv.sh [stagedir]        # default: ./stage
#
# Requires a C compiler, libgit2 under $LIBGIT2_PREFIX (or pkg-config), and
# UniVerse for nothing at all — the build itself needs no uv, only the install
# does.
set -eu

STAGE="${1:-stage}"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# ---- binaries ---------------------------------------------------------------
sh ./build-gitd.sh bin                     # mvgitd + uv-git

# ---- stage the release as a UniVerse account dir named 'git' ----------------
# The tar wraps this one dir, so the tarball unpacks to ./git/ — a self-sufficient
# UniVerse account you LOGTO and catalog from, exactly as on UniData.
#   BP/            the verb + its whole handler/sub set
#   bin/           mvgitd (the background process) + uv-git (the CLI)
#   install.sh     host installer: binaries, account, compile + catalog
ARCH="$(uname -m)"; OS="$(uname -s | tr '[:upper:]' '[:lower:]')"; case "$OS" in *linux*) OS=linux ;; esac
ACCT="$STAGE/git"
rm -rf "$ACCT"
mkdir -p "$ACCT/BP" "$ACCT/bin"

# FILES only — a working tree carries a generated BP/BP.INC/PLATFORM.H directory
# that a plain `cp BP/*` would choke on (the same trap build-udt.sh hit).
#
# EVERY staged BP item gets a trailing newline.  UniVerse's compiler rejects a
# source whose last line is unterminated — "End of File unexpected" — and the
# repo's items do not have one, so this is not cosmetic: without it nothing
# compiles on the target.
for f in BP/*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    cp "$f" "$ACCT/BP/$b"
    # append a newline only when the file lacks one, so re-staging is idempotent
    [ -n "$(tail -c 1 "$ACCT/BP/$b")" ] && printf '\n' >> "$ACCT/BP/$b"
done

cp bin/mvgitd bin/uv-git "$ACCT/bin/"
cp uv/install.sh "$ACCT/install.sh"; chmod +x "$ACCT/install.sh"
cp mvpkg.json PKG LICENSE README.md "$ACCT/" 2>/dev/null || true

# PLATFORM.H is deliberately NOT shipped: it is per-platform build output, and
# on the target install.sh creates the BP.INC file and writes the UniVerse
# defines into it just before compiling.

cat > "$ACCT/INSTALL.txt" <<EOF
mv_git for Rocket UniVerse ($OS/$ARCH).  This directory IS the package/account.

What is different from the UniData package, and why:

  UniVerse gives BASIC no in-process route to libgit2 — GCI is licensed and
  non-functional in the Trial Edition, and InterCall is a separate client SDK.
  So the git-object work runs in 'mvgitd', a background process owned by the
  user who starts it, which the session reaches over a named pipe.  It starts
  on demand: there is nothing to administer and no service to install.

Standalone install, from inside this directory:
  1. Host prerequisites — the runtime libgit2 this build links:
       sudo dnf install epel-release
       sudo dnf install libgit2_1.7
     (EL8 base libgit2 is 0.26, far too old.)
  2. Run the installer:
       ./install.sh
     It installs mvgitd and uv-git to /usr/local/bin, makes THIS directory a
     UniVerse account, then compiles and catalogs the GIT verb into it.
  3. Use it, from a session:      GIT status
     or from the shell:           uv-git -a <account> status

Per account: the verb is cataloged LOCAL, so run ./install.sh (or catalog the
BP items) in each account that needs it.  mvgitd is shared and needs no setup.
EOF

echo "build-uv: staged the UniVerse package as $ACCT/"
