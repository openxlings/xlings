#!/usr/bin/env bash
# openxlings/xlings#524 -- offline reproduction of the dep_install_dir regression.
#
# Loads the REAL pkginfo.lua from a libxpkg checkout, at each revision named on
# the command line, and replays the call shapes the published recipes actually
# use. No xlings, no network, no install, no home touched.
#
#   ./repro-dep-install-dir.sh                          # 0.0.54 / 0.0.55 / 0.0.56
#   ./repro-dep-install-dir.sh HEAD my-fix-branch       # any revs you like
#   ./repro-dep-install-dir.sh worktree                 # UNCOMMITTED working tree
#
# `worktree` matters: every other form reads the file out of git, so an
# uncommitted fix silently tests as if it were not there -- which reads as
# "my fix does nothing" rather than "I forgot to commit".
#
# LIBXPKG env var overrides the checkout location.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
libxpkg=${LIBXPKG:-$here/../../../libxpkg}
[[ -d $libxpkg/.git ]] || { echo "no libxpkg checkout at $libxpkg (set LIBXPKG=)" >&2; exit 2; }

# Default revs are the two that bracket the regression plus the diagnostics-only
# follow-up, resolved by tag-in-subject rather than by hash so this keeps working.
if (( $# )); then revs=("$@"); else
    revs=()
    for tag in 0.0.54 0.0.55 0.0.56; do
        r=$(git -C "$libxpkg" log --all --format='%H %s' \
            | grep -m1 -F "($tag)" | cut -d' ' -f1) || true
        [[ -n ${r:-} ]] && revs+=("$r@$tag")
    done
fi

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
for spec in "${revs[@]}"; do
    rev=${spec%%@*}; label=${spec#*@}; [[ $label == "$rev" ]] && label=$rev
    if [[ $rev == worktree ]]; then
        cp "$libxpkg/src/lua-stdlib/xim/libxpkg/pkginfo.lua" "$work/pkginfo.lua"
        [[ $label == worktree ]] && label="worktree (uncommitted)"
    elif ! git -C "$libxpkg" show "$rev:src/lua-stdlib/xim/libxpkg/pkginfo.lua" \
            > "$work/pkginfo.lua" 2>/dev/null; then
        echo "!! cannot read pkginfo.lua at $rev -- skipped" >&2; continue
    fi
    lua5.4 "$here/repro-dep-install-dir.lua" "$work/pkginfo.lua" "libxpkg $label"
    echo
done
