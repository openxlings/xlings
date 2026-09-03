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

# Second line of defence behind "pick the newest": timestamps can still lie
# (a restored cache, a touched file). Comparing against the version in the
# source makes a stale pick loud instead of showing up as the feature under
# test simply not working.
#
# A warning, not a failure: XLINGS_BIN is the supported way to point the
# suite at some other build deliberately, and that path returns before this.
warn_if_stale_bin() {
  local bin="$1" want got
  want="$(sed -n 's/.*VERSION = "\([^"]*\)".*/\1/p' \
            "$ROOT_DIR/src/core/config.cppm" 2>/dev/null | head -1)"
  [[ -n "$want" ]] || return 0
  got="$("$bin" --version 2>/dev/null | head -1 | tr -d '\r')"
  case "$got" in
    *"$want"*) return 0 ;;
  esac
  echo "[project-e2e] WARN: testing '$bin' (${got:-unknown}) but the source" >&2
  echo "[project-e2e]       says $want — rebuild, or set XLINGS_BIN" >&2
}

find_xlings_bin() {
  local candidate="${XLINGS_BIN:-}"
  if [[ -n "$candidate" && -f "$candidate" && -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi

  # Newest, not first. `target/` accumulates a directory per build
  # configuration and `find | head -1` returns them in directory order, so a
  # months-old binary from some earlier config wins as often as not. That is
  # not a flaky test, it is every E2E silently exercising the wrong build --
  # it cost a full debugging cycle on 2026.7.27.1, where a gate absent from a
  # 0.4.66 binary read as the feature not working.
  # `ls -t` rather than find's -printf: BSD find (macOS) has no -printf, and
  # `find -exec ls -t {} +` sorts by mtime on both.
  candidate="$(find "$ROOT_DIR/target" -path '*/bin/xlings' -type f \
                 -exec ls -t {} + 2>/dev/null | head -1)"
  if [[ -n "$candidate" && -x "$candidate" ]]; then
    warn_if_stale_bin "$candidate"
    printf '%s\n' "$candidate"
    return 0
  fi

  candidate="$(find "$BUILD_DIR" -path '*/release/xlings' -type f \
                 -exec ls -t {} + 2>/dev/null | head -1)"
  [[ -n "$candidate" && -x "$candidate" ]] || fail "xlings binary not found; set XLINGS_BIN"
  printf '%s\n' "$candidate"
}

require_fixture_index() {
  [[ -d "$FIXTURE_INDEX_DIR/pkgs" ]] || fail "fixture index repo not found at $FIXTURE_INDEX_DIR"
}

# Where a test's isolated XLINGS_HOME lives.
#
# NOT under $ROOT_DIR by default. Four home-related defects in the 2026-08-06
# review were asymptomatic under `~/.xlings`, and the repo checkout is usually
# under $HOME too -- so a test home there shares a long prefix with the real
# one, and every "did we use the right home?" bug stays invisible in exactly
# the same way.
#
# The 2026-08-06 measurements happened to use a path under /tmp, which put the
# home BELOW a directory the sandbox privatises before binding it. That was an
# accident, and it was the hardest ordering in the bind list. Making it
# deliberate is the point of E1.
#
# E2E_RUNTIME_ROOT overrides it -- CI runners with a small /tmp need that --
# but the default has to be the awkward path, not the comfortable one.
E2E_RUNTIME_ROOT="${E2E_RUNTIME_ROOT:-${TMPDIR:-/tmp}/xlings-e2e-$(id -u)}"

runtime_home_dir() {
  local name="$1"
  printf '%s\n' "$E2E_RUNTIME_ROOT/$name"
}

# Assert the isolation the test relies on, rather than assume it. A home that
# shares a prefix with $HOME cannot distinguish "we used the home under test"
# from "we used the developer's" -- which is the entire class E1 exists for.
assert_home_is_isolated() {
  local home_dir="$1"
  local real="${HOME%/}/.xlings"
  case "$home_dir" in
    "$real"|"$real"/*) fail "the test home IS the real home: $home_dir" ;;
  esac
  [[ "$home_dir" == "${HOME%/}"/* ]] \
    && log "  note: test home shares a prefix with \$HOME ($home_dir)"
  return 0
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

# An isolated home defaults to the GLOBAL mirror, and from inside China every
# index sync then sits on an unreachable host until it times out — which does
# not look like a network problem, it looks like the command under test
# hanging. Measured on this suite: subos_events 817s, subos_profile_upgrade
# 650s, cli_short_alias_removal 406s, all of it waiting on GLOBAL endpoints.
#
# So the default stays GLOBAL (CI runs on github.com and cannot reach the CN
# endpoints, which need auth), and the knob is an env var:
#
#     XLINGS_TEST_MIRROR=CN bash tests/e2e/run_all.sh <tarball>
#
# Set it for every local run. A caller passing $2 explicitly still wins.
write_home_config() {
  local home_dir="$1"
  local mirror="${2:-${XLINGS_TEST_MIRROR:-GLOBAL}}"
  local index_dir="${3:-$FIXTURE_INDEX_DIR}"
  local index_name="${4:-xim}"
  mkdir -p "$home_dir"
  mkdir -p "$home_dir/subos/default/bin"
  cp "$(find_xlings_bin)" "$home_dir/xlings"
  cat > "$home_dir/.xlings.json" <<EOF
{
  "mirror": "$mirror",
  "index_repos": [
    {
      "name": "$index_name",
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

ninja_archive_name() {
  case "$(platform_name)" in
    macosx) printf 'ninja-%s-macosx-arm64.tar.gz\n' "$1" ;;
    linux) printf 'ninja-%s-linux-x86_64.tar.gz\n' "$1" ;;
    *) fail "unsupported platform for ninja archive" ;;
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
