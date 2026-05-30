#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
FIXTURE_INDEX_DIR="$ROOT_DIR/tests/fixtures/xim-pkgindex"

# Ensure git identity is configured (CI runners may not have one)
git config user.name >/dev/null 2>&1 || git config --global user.name "xlings-ci"
git config user.email >/dev/null 2>&1 || git config --global user.email "ci@xlings.test"

log()  { echo "[project-e2e] $*"; }
fail() { echo "[project-e2e] FAIL: $*" >&2; exit 1; }

find_xlings_bin() {
  local candidate="${XLINGS_BIN:-}"
  if [[ -n "$candidate" && -f "$candidate" && -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi

  candidate="$(find "$ROOT_DIR/target" -path '*/bin/xlings' -type f 2>/dev/null | head -1)"
  if [[ -n "$candidate" && -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi

  candidate="$(find "$BUILD_DIR" -path '*/release/xlings' -type f | head -1)"
  [[ -n "$candidate" && -x "$candidate" ]] || fail "xlings binary not found; set XLINGS_BIN"
  printf '%s\n' "$candidate"
}

require_fixture_index() {
  [[ -d "$FIXTURE_INDEX_DIR/pkgs" ]] || fail "fixture index repo not found at $FIXTURE_INDEX_DIR"
}

runtime_home_dir() {
  local name="$1"
  printf '%s\n' "$ROOT_DIR/tests/e2e/runtime/$name"
}

prepare_scenario() {
  local scenario_dir="$1"
  local home_dir="$2"
  local backup_file
  backup_file="$(mktemp)"
  cp "$scenario_dir/.xlings.json" "$backup_file"
  rm -rf "$home_dir" "$scenario_dir/.xlings"
  printf '%s\n' "$backup_file"
}

restore_scenario() {
  local scenario_dir="$1"
  local home_dir="$2"
  local backup_file="$3"
  rm -rf "$home_dir" "$scenario_dir/.xlings"
  if [[ -f "$backup_file" ]]; then
    cp "$backup_file" "$scenario_dir/.xlings.json"
    rm -f "$backup_file"
  fi
}

write_home_config() {
  local home_dir="$1"
  local mirror="${2:-GLOBAL}"
  local index_dir="${3:-$FIXTURE_INDEX_DIR}"
  mkdir -p "$home_dir"
  mkdir -p "$home_dir/subos/default/bin"
  cp "$(find_xlings_bin)" "$home_dir/xlings"
  cat > "$home_dir/.xlings.json" <<EOF
{
  "mirror": "$mirror",
  "index_repos": [
    {
      "name": "xim",
      "url": "$index_dir"
    }
  ]
}
EOF
}

run_xlings() {
  local home_dir="$1"
  local workdir="$2"
  shift 2
  # Default: run from a neutral cwd outside the repo so an ancestor-
  # search for `.xlings.json` (the repo now ships its own at /.xlings.json
  # for CI self-host purposes) doesn't accidentally activate project mode
  # for tests that don't want it. Project-context tests wrap call sites
  # with `(cd "$SCENARIO_DIR" && run_xlings ...)`; we respect such
  # explicit chdirs by skipping our own when cwd is already inside a
  # scenario / fixture tree.
  #
  # We also `unset` XLINGS_PROJECT_DIR which xlings auto-exports whenever
  # it loads a project config — it leaks into the test environment from
  # any prior xlings invocation in the user's shell and would override
  # our cwd-based isolation.
  local cwd
  cwd="$PWD"
  if [[ "$cwd" == "$ROOT_DIR" ]]; then
    # Caller didn't cd — they don't want project context. Use a neutral
    # cwd so the spawned xlings doesn't pick up the repo's CI-self-host
    # `.xlings.json`.
    ( cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$home_dir" \
        "$(find_xlings_bin)" --verbose "$@" )
  else
    # Caller explicitly cd'd somewhere — respect it (project-context
    # tests rely on the project search starting from cwd).
    env -u XLINGS_PROJECT_DIR XLINGS_HOME="$home_dir" \
      "$(find_xlings_bin)" --verbose "$@"
  fi
}

platform_name() {
  case "$(uname -s)" in
    Darwin) printf 'macosx\n' ;;
    Linux) printf 'linux\n' ;;
    *) fail "unsupported OS: $(uname -s)" ;;
  esac
}

node_archive_name() {
  case "$(platform_name)" in
    macosx) printf 'node-v%s-darwin-arm64.tar.gz\n' "$1" ;;
    linux) printf 'node-v%s-linux-x64.tar.xz\n' "$1" ;;
    *) fail "unsupported platform for node archive" ;;
  esac
}

mdbook_archive_name() {
  case "$(platform_name)" in
    macosx) printf 'mdbook-%s-macosx-arm64.tar.gz\n' "$1" ;;
    linux) printf 'mdbook-%s-linux-x86_64.tar.gz\n' "$1" ;;
    *) fail "unsupported platform for mdbook archive" ;;
  esac
}

strip_ansi() {
  perl -pe 's/\e\[[0-9;]*[a-zA-Z]//g; s/\e\[\?[0-9]*[a-zA-Z]//g'
}

assert_contains() {
  local haystack="$1"
  local needle="$2"
  local message="$3"
  echo "$haystack" | strip_ansi | grep -F "$needle" >/dev/null || fail "$message"
}
