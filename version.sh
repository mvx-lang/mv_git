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

# mv_git_stamp_manifests <staged-dir> <version>
#
# Write the release's version into the PKG and mvpkg.json it ships.
#
# These two files are what MVPKG and the registry read, and keeping them in step
# with the tag by hand does not work: 2.0.0 shipped declaring itself 2.0.0-rc5,
# so the package manager would install the stable release and then report a
# release candidate.  The binaries were never wrong -- they take their version
# from the tag through mv_git_version -- so the fix is to give the manifests the
# same source of truth rather than a second one that has to be remembered.
#
# In-tree PKG/mvpkg.json are now only a default for a dev build.  A release
# stamps over them.
mv_git_stamp_manifests() {
    _dir="$1"; _ver="$2"
    [ -n "$_dir" ] && [ -n "$_ver" ] || return 0

    if [ -f "$_dir/PKG" ]; then
        # line 2 is the version (line 1 name, 3 description, 4 systems)
        awk -v v="$_ver" 'NR==2 {print v; next} {print}' "$_dir/PKG" > "$_dir/PKG.$$" \
            && mv "$_dir/PKG.$$" "$_dir/PKG"
    fi
    if [ -f "$_dir/mvpkg.json" ]; then
        sed 's/^\([[:space:]]*"version"[[:space:]]*:[[:space:]]*\)"[^"]*"/\1"'"$_ver"'"/' \
            "$_dir/mvpkg.json" > "$_dir/mvpkg.json.$$" \
            && mv "$_dir/mvpkg.json.$$" "$_dir/mvpkg.json"
    fi
    printf 'stamped manifests: %s\n' "$_ver"
}
