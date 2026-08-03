#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FIXTURE_INDEX_DIR="${1:-$ROOT_DIR/tests/fixtures/xim-pkgindex}"
XIM_PKGINDEX_REF="${XIM_PKGINDEX_REF:-main}"
XIM_PKGINDEX_URL="${XIM_PKGINDEX_URL:-https://github.com/openxlings/xim-pkgindex.git}"

if [[ -d "$FIXTURE_INDEX_DIR/pkgs" ]]; then
  echo "[fixture] reuse existing fixture index: $FIXTURE_INDEX_DIR"
  exit 0
fi

rm -rf "$FIXTURE_INDEX_DIR"
mkdir -p "$(dirname "$FIXTURE_INDEX_DIR")"

echo "[fixture] cloning $XIM_PKGINDEX_URL (ref: $XIM_PKGINDEX_REF) -> $FIXTURE_INDEX_DIR"

# Every workflow starts by fetching this fixture, so one bad minute on the
# remote costs a whole platform's run: a Windows job has already been lost to
# `error: RPC failed; HTTP 502 ... fatal: expected 'packfile'` four minutes
# into the clone, after its entire build and unit suite had passed.
#
# Retries the FETCH only. A clone that lands and is still missing pkgs/ is a
# real failure and falls through to the check below unretried -- the point is
# to survive the network, not to paper over a wrong ref.
clone_fixture_index() {
  rm -rf "$FIXTURE_INDEX_DIR"
  # git clone --branch accepts branches/tags but not commit SHAs. Detect a
  # 40-char hex SHA and use the deeper clone-then-checkout path instead.
  if [[ "$XIM_PKGINDEX_REF" =~ ^[0-9a-f]{40}$ ]]; then
    git clone "$XIM_PKGINDEX_URL" "$FIXTURE_INDEX_DIR" \
      && git -C "$FIXTURE_INDEX_DIR" checkout --quiet "$XIM_PKGINDEX_REF"
  else
    git clone --depth 1 --branch "$XIM_PKGINDEX_REF" \
      "$XIM_PKGINDEX_URL" "$FIXTURE_INDEX_DIR"
  fi
}

for attempt in 1 2 3; do
  if clone_fixture_index; then break; fi
  if [[ $attempt -eq 3 ]]; then
    echo "[fixture] FAIL: could not clone $XIM_PKGINDEX_URL after 3 attempts" >&2
    exit 1
  fi
  echo "[fixture] clone attempt ${attempt} failed; retrying in $((attempt * 10))s" >&2
  sleep $((attempt * 10))
done

if [[ ! -d "$FIXTURE_INDEX_DIR/pkgs" ]]; then
  echo "[fixture] FAIL: missing pkgs directory after clone: $FIXTURE_INDEX_DIR" >&2
  exit 1
fi

echo "[fixture] ready: $FIXTURE_INDEX_DIR"
