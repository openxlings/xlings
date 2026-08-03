#!/usr/bin/env bash
# Architecture refusal is graded by evidence, and this asserts both grades on
# whatever host runs it -- the aarch64 contract test covers the same ground but
# only on ARM, which is a fraction of CI capacity and none of most people's
# machines.
#
# Strong: the resource enumerates its architectures. Omitting this host is a
#         fact about artifacts, so it is refused before any request.
# Weak:   the only signal is the package-level `archs` union. That field went
#         unenforced for all of spec V1 and is routinely under-declared -- `go`
#         claims `{"x86_64"}` and ships a darwin-arm64 tarball -- so a mismatch
#         is an advisory and the install proceeds.
#
# Refusing on the weak signal is what takes git, make, gcc, binutils and ~60
# other index recipes off aarch64 Linux, and go/rustup/npm/pnpm/nvm/ollama off
# Apple Silicon.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"
require_fixture_index

bin=$(find_xlings_bin)
root=$(mktemp -d)
server_pid=
cleanup() {
  [[ -z "$server_pid" ]] || kill "$server_pid" 2>/dev/null || true
  rm -rf "$root"
}
trap cleanup EXIT

case "$(uname -m)" in
  aarch64|arm64) host_arch=aarch64; foreign_arch=x86_64 ;;
  *)             host_arch=x86_64;  foreign_arch=aarch64 ;;
esac

home="$root/home"
index="$root/index"
cp -a "$FIXTURE_INDEX_DIR" "$index"
printf 'xim_indexrepos = {}\n' > "$index/xim-indexrepos.lua"
rm -f "$index/.xlings-index-cache.json"
mkdir -p "$index/pkgs/e" "$root/server"

port=$((21000 + $$ % 20000))
request_log="$root/requests.log"
python3 -m http.server "$port" --bind 127.0.0.1 \
  --directory "$root/server" >"$request_log" 2>&1 &
server_pid=$!

# Strong: a per-arch resource map naming only the arch we are NOT on.
cat > "$index/pkgs/e/ev-strong-foreign.lua" <<LUA
package = { spec="1", name="ev-strong-foreign", type="package",
  archs={"$foreign_arch"}, xpm={linux={ macosx={}, ["1.0.0"]={
    $foreign_arch = { url="http://127.0.0.1:$port/payload.tar.gz" }
  }}} }
import("xim.libxpkg.pkginfo")
function install()
  io.writefile(path.join(pkginfo.install_dir(), "hook-ran"), "bad")
  return true
end
LUA

# Weak: one artifact, and a package-level union that does not mention us.
cat > "$index/pkgs/e/ev-weak-foreign.lua" <<LUA
package = { spec="1", name="ev-weak-foreign", type="package",
  archs={"$foreign_arch"}, xpm={linux={ ["1.0.0"]={} }, macosx={ ["1.0.0"]={} },
  windows={ ["1.0.0"]={} }} }
import("xim.libxpkg.pkginfo")
function install()
  os.mkdir(pkginfo.install_dir())
  io.writefile(path.join(pkginfo.install_dir(), "installed"), "weak")
  return true
end
LUA

mkdir -p "$home"
cat > "$home/.xlings.json" <<EOF
{"mirror":"GLOBAL","index_repos":[{"name":"xim","url":"$index"}]}
EOF
run() { HOME="$root/user" XLINGS_HOME="$home" NO_COLOR=1 "$bin" "$@"; }

# --- weak evidence installs, and says why it is unsure ---------------------
# Captured with `set +e` so a refusal reaches the diagnostic below instead of
# killing the script under `set -e` with no explanation.
set +e
weak=$(run install ev-weak-foreign@1.0.0 -y 2>&1)
set -e
[[ -f "$home/data/xpkgs/xim-x-ev-weak-foreign/1.0.0/installed" ]] || {
  echo "$weak"
  fail "a package-level archs union refused an install on $host_arch"
}
grep -q "$host_arch" <<<"$weak" \
  || fail "the mismatch was never mentioned: $weak"
if grep -q 'E_UNSUPPORTED_TARGET' <<<"$weak"; then
  fail "a package-level union must advise, never refuse: $weak"
fi

# --- strong evidence refuses, before any request --------------------------
set +e
strong=$(run install ev-strong-foreign@1.0.0 -y 2>&1)
status=$?
set -e
[[ $status -ne 0 ]] || fail "an enumerated per-arch resource was not refused: $strong"
grep -q "E_UNSUPPORTED_TARGET.*$host_arch" <<<"$strong" \
  || fail "the refusal was not causal: $strong"
[[ ! -e "$home/data/xpkgs/xim-x-ev-strong-foreign/1.0.0/hook-ran" ]] \
  || fail "the install hook ran for a refused target"
! grep -Eq '"(GET|HEAD) ' "$request_log" \
  || fail "a refused target still issued download requests"

echo "arch evidence contracts: ok ($host_arch host, $foreign_arch fixtures)"
