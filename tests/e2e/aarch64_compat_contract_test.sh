#!/usr/bin/env bash
set -euo pipefail

case "$(uname -m)" in aarch64|arm64) ;; *) echo "aarch64 contracts: skipped"; exit 0;; esac
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
home="$root/home"
index="$root/index"
cp -a "$FIXTURE_INDEX_DIR" "$index"
printf 'xim_indexrepos = {}\n' > "$index/xim-indexrepos.lua"
rm -f "$index/.xlings-index-cache.json"
mkdir -p "$index/pkgs/a" "$root/server"
port=$((20000 + $$ % 20000))
request_log="$root/requests.log"
python3 -m http.server "$port" --bind 127.0.0.1 \
  --directory "$root/server" >"$request_log" 2>&1 &
server_pid=$!

cat > "$index/pkgs/a/arm-compatible.lua" <<'LUA'
package = { spec="1", name="arm-compatible", type="package",
  archs={"arm64"}, xpm={linux={ ["1.0.0"]={} }} }
import("xim.libxpkg.pkginfo")
function install()
  os.mkdir(pkginfo.install_dir())
  io.writefile(path.join(pkginfo.install_dir(), "installed"), "aarch64")
  return true
end
LUA
cat > "$index/pkgs/a/arm-incompatible.lua" <<LUA
package = { spec="1", name="arm-incompatible", type="package",
  archs={"x86_64"}, xpm={linux={ ["1.0.0"]={
    url="http://127.0.0.1:$port/payload.tar.gz"
  }}} }
import("xim.libxpkg.pkginfo")
function install()
  io.writefile(path.join(pkginfo.install_dir(), "hook-ran"), "bad")
  return true
end
LUA
mkdir -p "$home"
cat > "$home/.xlings.json" <<EOF
{"mirror":"GLOBAL","index_repos":[{"name":"xim","url":"$index"}]}
EOF
run() { HOME="$root/user" XLINGS_HOME="$home" NO_COLOR=1 "$bin" "$@"; }

run install arm-compatible@1.0.0 -y
[[ -f "$home/data/xpkgs/xim-x-arm-compatible/1.0.0/installed" ]]

set +e
output=$(run install arm-incompatible@1.0.0 -y 2>&1)
status=$?
set -e
[[ $status -ne 0 ]]
grep -q 'E_UNSUPPORTED_TARGET.*linux-aarch64' <<<"$output"
[[ ! -e "$home/data/xpkgs/xim-x-arm-incompatible/1.0.0/hook-ran" ]]
! grep -Eq '"(GET|HEAD) ' "$request_log"

run subos new arm-probe
set +e
sandbox=$(run subos use arm-probe --sandbox --cmd 'exit 37' 2>&1)
status=$?
set -e
[[ $status -ne 0 ]]
grep -q 'E_UNSUPPORTED_TARGET.*linux-aarch64' <<<"$sandbox"
! grep -q 'failed to install sandbox backend' <<<"$sandbox"
! grep -Eq '"(GET|HEAD) ' "$request_log"
echo "native aarch64 compatibility contracts: ok"
