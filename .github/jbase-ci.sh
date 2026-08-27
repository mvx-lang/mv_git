#!/bin/sh
# What CI runs inside the jBASE container.  Kept out of the workflow so the
# quoting stays sane and so it can be run by hand, which is how it was
# developed: `sh .github/jbase-run 'sh /pkg/.github/jbase-ci.sh'`.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
set -e

rm -rf dist && mkdir -p dist
sh build-jbase.sh /pkg/dist

# BOTH ARMS, because they are different code.  The CLI drives jb-git from the
# shell; the verb runs the handlers inside a jsh session and reaches the engine
# through DEFC.  A green CLI arm says nothing about the verb -- that is how
# thirteen handlers stayed uncompilable for a year (mv_git#175).
for via in cli verb; do
    echo "== jBASE arm: $via =="
    PLATFORM=jbase MVX=jsh \
    GITPKG=/pkg/dist/mv_git MVXGIT=/pkg/dist/mv_git/jb-git \
    JBGIT_VIA="$via" SKIP_NET=0 bash tests/git-tests.sh
done
