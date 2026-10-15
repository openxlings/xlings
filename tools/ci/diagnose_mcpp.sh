#!/usr/bin/env bash
# After a failed mcpp build in CI: what index this runner actually holds.
#
# `mcpplibs.xpkg@<ver> not found` was diagnosed repeatedly from the outside --
# index commit, published artifact, rolling pointer, releases/latest, publish
# lag, the mcpp version pin, install-vs-use, and the lockfile hash were each
# checked and each was correct. All of that is inference about a file only the
# runner can see. A step of its own, `if: failure()`: under `bash -e` a
# diagnostic hung off the failing command never runs.
for idx in "$HOME/.mcpp/registry/data/mcpplibs/pkgs/x/xpkg.lua" \
           "$GITHUB_WORKSPACE/.mcpp/registry/data/mcpplibs/pkgs/x/xpkg.lua"; do
  if [ -f "$idx" ]; then
    echo "--- $idx"
    grep -oE '\["[0-9]+\.[0-9]+\.[0-9]+"\]' "$idx" | head -8
  else
    echo "--- $idx : ABSENT"
  fi
done
for root in "$HOME/.mcpp/registry" "$GITHUB_WORKSPACE/.mcpp/registry"; do
  [ -d "$root" ] || continue
  echo "--- $root"
  ls -1 "$root" 2>/dev/null | head -8
  ls -1 "$root/data" 2>/dev/null | head -8
  ls -1 "$root/index-snapshots" 2>/dev/null | head -8
done
mcpp --version 2>&1 | head -2 || true
mcpp self version 2>&1 | head -3 || true
xlings --version 2>&1 | head -2 || true
df -h / | tail -1
exit 0
