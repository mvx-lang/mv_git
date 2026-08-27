#!/bin/sh
# release-notes.sh <tag> — the release notes for <tag>, on stdout.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
#
# Every issue in the release, grouped by THE SYSTEM IT WAS ON.  This package
# ships four ports from one tree, and "what changed" is a different answer for
# each of them: a UniVerse site reading a list that is nine tenths jBASE has to
# work out which nine tenths, every time.
#
# Where the system comes from, in order, because none of the three is reliable
# on its own:
#
#   the issue's label   -- said outright, and the only one that is a decision
#                          rather than a guess;
#   the title prefix    -- "jBASE: ..." is how these have been written for
#                          months, so the information is there even unlabelled;
#   the files touched   -- the last resort and the most honest, since a commit
#                          that only changes jbase/ was about jBASE whatever
#                          anyone called it.
#
# Anything touching only shared code lands under "all ports", which is correct:
# BP/ and src/mvxgit.c are every platform's.
set -eu

TAG="${1:?usage: release-notes.sh <tag>}"

# The previous RELEASE, not the previous tag: dev moves constantly and would
# make every release's notes say "since last night".
PREV="$(git tag --list --sort=-v:refname \
        | grep -vx dev \
        | grep -v "^${TAG}$" \
        | head -1 || true)"
RANGE="${PREV:+${PREV}..}${TAG}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# One line per issue: "<system>\t<issue>\t<subject>".  An issue can appear in
# several commits; it is listed once, under the system of the first commit that
# named it.
git log --format='%H%x09%s' "$RANGE" | while IFS="$(printf '\t')" read -r sha subj; do
    issue="$(printf '%s' "$subj" | grep -oE '#[0-9]+' | head -1 | tr -d '#' || true)"
    [ -n "$issue" ] || continue
    grep -q "	$issue	" "$work/rows" 2>/dev/null && continue

    # 1. the labels.  An issue can be on more than one system, and one that is
    #    on ALL of them is not four entries -- it is shared work, which is what
    #    "all ports" means.  Anything narrower is listed under each system it
    #    was on, because that is the answer a reader of one port needs.
    sys=""
    labels="$(gh issue view "$issue" --json labels --jq '[.labels[].name] | map(select(. == "mvx" or . == "udt" or . == "uv" or . == "jbase")) | sort | join(" ")' 2>/dev/null || true)"
    case "$labels" in
        "jbase mvx udt uv") sys="all" ;;
        "")                 sys="" ;;
        *)                  sys="$labels" ;;
    esac
    # 2. the title prefix, as the issue itself was written
    if [ -z "$sys" ]; then
        case "$subj" in
            *jBASE:*|*jbase:*) sys=jbase ;;
            *UniVerse:*|*uv:*) sys=uv ;;
            *UniData:*|*udt:*) sys=udt ;;
            *MVX:*|*mvx:*)     sys=mvx ;;
        esac
    fi
    # 3. what the commit actually touched
    if [ -z "$sys" ]; then
        files="$(git show --name-only --format= "$sha")"
        printf '%s' "$files" | grep -qE '^(jbase/|src/jb)' && sys=jbase
        [ -z "$sys" ] && printf '%s' "$files" | grep -qE '^(udt/|src/udt)'        && sys=udt
        [ -z "$sys" ] && printf '%s' "$files" | grep -qE '^(uv/|src/uv|src/gitd)' && sys=uv
        [ -z "$sys" ] && printf '%s' "$files" | grep -qE '^(src/mvx-git)'         && sys=mvx
    fi
    [ -n "$sys" ] || sys=all

    # One row per system, so an issue on two of them appears under both.
    for one in $sys; do
        printf '%s\t%s\t%s\n' "$one" "$issue" "$subj" >> "$work/rows"
    done
done

[ -f "$work/rows" ] || { echo "No issues recorded for $RANGE."; exit 0; }

printf '## What changed'
[ -n "$PREV" ] && printf ' since %s' "$PREV"
printf '\n'

for sys in all mvx udt uv jbase; do
    grep -q "^$sys	" "$work/rows" || continue
    case "$sys" in
        all)   printf '\n### All ports\n\n' ;;
        mvx)   printf '\n### MVX\n\n' ;;
        udt)   printf '\n### UniData\n\n' ;;
        uv)    printf '\n### UniVerse\n\n' ;;
        jbase) printf '\n### jBASE\n\n' ;;
    esac
    # Newest first, as the log gave them.
    while IFS="$(printf '\t')" read -r s issue subj; do
        [ "$s" = "$sys" ] || continue
        # The subject already opens with "#N "; the issue link replaces it so a
        # line reads as a sentence rather than a number said twice.
        text="$(printf '%s' "$subj" | sed -E "s/^#$issue +//; s/ \(#[0-9]+\)$//")"
        printf -- '- %s (#%s)\n' "$text" "$issue"
    done < "$work/rows"
done
