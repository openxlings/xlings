#!/usr/bin/env bash
# Read-only commands must answer from local state and never turn a cold
# catalog lookup into an implicit index fetch.  The fake git below makes that
# boundary observable without permitting any network access.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

bin="$(find_xlings_bin)"
bin="$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")"
root="$(mktemp -d)"
home="$root/home"
fake_bin="$root/bin"
git_marker="$root/git-called"
index="$home/data/xim-pkgindex"
fake_user_home="$root/user"

cleanup() { rm -rf "$root"; }
trap cleanup EXIT

mkdir -p "$fake_bin" "$fake_user_home" "$index/pkgs/g" \
  "$index/pkgs/b" "$index/pkgs/p" "$home/subos/default/bin"

cat > "$fake_bin/git" <<'SH'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "${GIT_RECORDER_MARKER:?}"
exit 97
SH
chmod +x "$fake_bin/git"

case "$(uname -m)" in
  aarch64|arm64) foreign_arch=x86_64 ;;
  *)             foreign_arch=aarch64 ;;
esac

cat > "$index/pkgs/g/gcc.lua" <<'LUA'
package = { spec="1", name="gcc", description="local gcc fixture",
  type="package", programs={"gcc"}, xpm={
    linux={ ["1.0.0"]={} }, macosx={ ["1.0.0"]={} },
    windows={ ["1.0.0"]={} }
  } }
function install() return true end
function uninstall() return true end
LUA

for backend in bwrap proot; do
  cat > "$index/pkgs/${backend:0:1}/${backend}.lua" <<LUA
package = { spec="1", name="$backend", description="offline backend fixture",
  type="package", archs={"$foreign_arch"}, programs={"$backend"},
  xpm={linux={ ["1.0.0"]={ url="https://invalid.example/$backend.tar.gz" } }} }
function install() return true end
function uninstall() return true end
LUA
done

printf 'xim_indexrepos = {}\n' > "$index/xim-indexrepos.lua"
cat > "$home/.xlings.json" <<'JSON'
{
  "mirror": "GLOBAL",
  "activeSubos": "default",
  "subos": {"default": {"dir": ""}},
  "index_repos": [
    {"name": "xim", "url": "https://invalid.example/xim-pkgindex.git"}
  ]
}
JSON
cat > "$home/subos/default/.xlings.json" <<'JSON'
{"workspace": {}}
JSON

# The absent xim-indexrepos.json is deliberate: it is the cold-home condition
# that used to make get_catalog() sync even when its local rebuild succeeded.
[[ ! -e "$home/data/xim-index-repos/xim-indexrepos.json" ]] \
  || fail "fixture unexpectedly initialized the sub-index marker"

run_local() {
  local label="$1"
  local expected="$2"
  shift 2
  rm -f "$git_marker"

  local output rc clean
  set +e
  output="$(cd /tmp && env -u XLINGS_PROJECT_DIR \
    HOME="$fake_user_home" XLINGS_HOME="$home" NO_COLOR=1 \
    GIT_RECORDER_MARKER="$git_marker" PATH="$fake_bin:$PATH" \
    timeout 2s "$bin" "$@" </dev/null 2>&1)"
  rc=$?
  set -e
  clean="$(strip_ansi <<<"$output" | tr -d '\000')"

  [[ $rc -ne 124 ]] || fail "$label exceeded the cold-home 2s query budget"
  case "$rc" in
    0|1) ;;
    *) fail "$label exited $rc instead of returning local data/diagnostic:\n$clean" ;;
  esac
  [[ ! -e "$git_marker" ]] || fail "$label invoked git during local lookup:\n$(cat "$git_marker")\n$clean"
  grep -Eq "$expected" <<<"$clean" \
    || fail "$label returned neither expected local data nor diagnostic (exit $rc):\n$clean"
  log "  ✓ $label stayed local"
}

log "cold-home CLI queries"
run_local "list" 'no packages installed in current subos|package index not available' list
run_local "info" 'local gcc fixture|package index not available' info gcc
run_local "search" 'local gcc fixture|package index not available' search gcc
run_local "why" 'no packages are installed' why gcc
run_local "config" 'XLINGS_HOME|xlings config' config
run_local "subos list" 'default|SubOS' subos list
run_local "subos info" 'default|active' subos info
run_local "self doctor" 'workspace, shims, and payloads are all consistent|xlings self doctor' self doctor
run_local "help" 'Usage:|xlings' --help
run_local "version" '[0-9]+\.[0-9]+' --version

log "remove lookup"
run_local "remove" 'not installed in current subos|package index not available' remove gcc -y

# Interface mode owns stdin for control messages while a capability runs.
# Give it one list_packages request, close stdin immediately, and require the
# terminal result frame so the session/thread shutdown path is exercised.
log "one NDJSON request with clean stdin shutdown"
run_local "interface list_packages" '"kind":"result"' \
  interface list_packages --args '{}'

# On Linux the implicit sandbox-backend decision consults the catalog before
# it considers acquisition.  Both local fixtures exclude this host, so the
# lookup must return the causal local diagnostic without reaching install.
if [[ "$(uname -s)" == Linux ]]; then
  log "sandbox backend lookup"
  run_local "sandbox backend lookup" \
    'E_UNSUPPORTED_TARGET.*no sandbox backend' \
    subos use default --sandbox --cmd 'exit 37'
fi

log "PASS: local queries do not sync package indexes"
