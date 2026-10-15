#!/usr/bin/env bash
# A development build's state, carried between the machines of one CI round
# and between rounds (tests/README.md, "Parallel runs"):
#
#   build_state.sh pack <file>       what the build produced: target/ without the
#                                    test binaries, and mcpp's BMIs and dependency
#                                    builds (~/.mcpp, .mcpp), with the commit it
#                                    was built from
#   build_state.sh unpack <file>     put it back, where it was built
#   build_state.sh settle [<sha>]    make the build see what changed, and only that
#   build_state.sh clear             drop it all (a restored state that does not build)
#
# ninja decides by time, and a checkout's times say nothing (every file is as
# new as the clone). settle gives every tracked file, and every package payload
# (immutable at its versioned path), one fixed old time; the files that differ
# from <sha> -- the commit the state was built from -- get the present. So an
# object is rebuilt exactly when something it was built from changed. Without
# <sha>, nothing changed (the state is this commit's, or there is no state);
# a <sha> that cannot be fetched makes every source new (a rebuild of the
# project, on the state's dependency builds).
set -euo pipefail
cd "${GITHUB_WORKSPACE:-$(git rev-parse --show-toplevel)}"
old=946684800   # 2000-01-01, older than anything built
paths=(target .mcpp/bmi .mcpp/build-cache "$HOME/.mcpp/bmi" "$HOME/.mcpp/build-cache")

case "${1:-}" in
pack)
  out="$2"
  git rev-parse HEAD > target/.build-state-sha
  present=()
  for p in "${paths[@]}"; do [[ -e "$p" ]] && present+=("$(realpath -s "$p")"); done
  tar -P --zstd -cf "$out" \
    --exclude='target/*/*/bin/unit' --exclude='target/*/*/bin/e2e' \
    --exclude='target/xdev' --exclude='target/xtest-artifacts' --exclude='target/*-musl' \
    "${present[@]}"
  ls -lh "$out" | awk '{print "build state:", $5}'
  ;;
unpack)
  tar -P --zstd -xf "$2"
  cat target/.build-state-sha 2>/dev/null || true
  ;;
settle)
  sha="${2:-}"
  git ls-files -z | xargs -0 touch -h -d "@$old" 2>/dev/null || true
  for store in "$HOME/.mcpp/registry/data/xpkgs" "$HOME/.xlings/data/xpkgs"; do
    [[ -d "$store" ]] && find "$store" -xdev -exec touch -h -d "@$old" {} + 2>/dev/null || true
  done
  if [[ -n "$sha" ]] && { git cat-file -e "$sha^{commit}" 2>/dev/null || git fetch -q --depth=1 origin "$sha" 2>/dev/null; }; then
    changed="$(git diff --name-only "$sha" HEAD | wc -l)"
    git diff --name-only -z "$sha" HEAD | xargs -0 -r sh -c 'for f; do [ -e "$f" ] && touch -h "$f"; done; :' _
    echo "settled: $changed file(s) changed since ${sha:0:12}"
  elif [[ -n "$sha" ]]; then
    git ls-files -z | xargs -0 touch -h
    echo "settled: ${sha:0:12} cannot be fetched; every source is new"
  else
    echo "settled: the state's own commit"
  fi
  ;;
clear)
  for p in "${paths[@]}"; do rm -rf "$p"; done
  ;;
*)
  sed -n '2,22p' "$0"; exit 2 ;;
esac
