# version.sh — one version string for every build script.  Sourced, not run.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
#
#   mv_git_version [source-dir]
#
# A release carries its version and nothing else: a tag is a promise that the
# name identifies the tree, so 2.0.0 stays 2.0.0.  A dev build cannot make that
# promise, so it carries the short commit too (2.0.0-rc5+g4579f6b) -- a bug
# reported against a bare "2.0.0-rc5" that is really several commits past the
# tag costs an afternoon to work out.  Whatever runs the engine answers
# GIT --VERSION with this string, so it has to name the tree exactly.
#
# A version handed in through $MV_GIT_VERSION or $GITHUB_REF_NAME is taken as a
# release stamp and used verbatim -- that is how the release workflow passes the
# tag, and how a source tarball (no .git at all) still gets a real version.
mv_git_version() {
    _d="${1:-$(dirname "$0")}"

    _v="${MV_GIT_VERSION:-${GITHUB_REF_NAME:-}}"
    if [ -n "$_v" ]; then printf '%s' "$_v"; return 0; fi

    if ! git -C "$_d" rev-parse --git-dir >/dev/null 2>&1; then
        printf '0'; return 0
    fi
    if _t=$(git -C "$_d" describe --exact-match --tags HEAD 2>/dev/null); then
        printf '%s' "$_t"; return 0          # sitting on a tag: the tag alone
    fi
    _v=$(git -C "$_d" describe --tags --abbrev=0 2>/dev/null) || _v=0
    _h=$(git -C "$_d" rev-parse --short HEAD 2>/dev/null) || _h=
    if [ -n "$_h" ]; then printf '%s+g%s' "$_v" "$_h"; else printf '%s' "$_v"; fi
}
