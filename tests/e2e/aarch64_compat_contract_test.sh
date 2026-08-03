#!/usr/bin/env bash
# Native aarch64 target contracts.
#
# Two halves, and the second one is the point:
#
#   a resource that ENUMERATES its architectures and omits this one is refused
#   before any request is made -- that is a fact about artifacts.
#
#   a spec-V1 recipe whose only arch signal is the package-level `archs` union
#   INSTALLS. That union went unenforced for the whole of V1 and is routinely
#   under-declared: `go` says `{"x86_64"}` and ships a darwin-arm64 tarball,
#   and `fish.lua` says so in a comment. Refusing on it rejects the archive the
#   recipe was about to fetch, and would take ~60 of the index's recipes --
#   git, make, gcc, binutils, zlib -- off aarch64 entirely.
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
# Strong evidence: a per-arch resource map that names x86_64 and nothing else.
# The author is describing artifacts, so "no aarch64" is a real answer.
cat > "$index/pkgs/a/arm-incompatible.lua" <<LUA
package = { spec="1", name="arm-incompatible", type="package",
  archs={"x86_64"}, xpm={linux={ ["1.0.0"]={
    x86_64 = { url="http://127.0.0.1:$port/payload.tar.gz" }
  }}} }
import("xim.libxpkg.pkginfo")
function install()
  io.writefile(path.join(pkginfo.install_dir(), "hook-ran"), "bad")
  return true
end
LUA

# Weak evidence: the exact shape of a spec-V1 index recipe -- one artifact for
# the platform, and a package-level `archs` that says x86_64 because that is
# what the author happened to test. This MUST install.
cat > "$index/pkgs/a/arm-legacy-v1.lua" <<'LUA'
package = { spec="1", name="arm-legacy-v1", type="package",
  archs={"x86_64"}, xpm={linux={ ["1.0.0"]={} }} }
import("xim.libxpkg.pkginfo")
function install()
  os.mkdir(pkginfo.install_dir())
  io.writefile(path.join(pkginfo.install_dir(), "installed"), "legacy")
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

# --- the regression guard --------------------------------------------------
legacy=$(run install arm-legacy-v1@1.0.0 -y 2>&1)
[[ -f "$home/data/xpkgs/xim-x-arm-legacy-v1/1.0.0/installed" ]] || {
  echo "$legacy"
  fail "a spec-V1 recipe was refused on its package-level archs union -- that \
takes git, make, gcc and ~60 other index recipes off aarch64"
}
grep -q 'linux-aarch64' <<<"$legacy" \
  || fail "the arch mismatch was not mentioned at all: $legacy"
if grep -q 'E_UNSUPPORTED_TARGET' <<<"$legacy"; then
  fail "a package-level union must advise, never refuse: $legacy"
fi

# --- strong evidence still refuses, with zero requests ---------------------
set +e
output=$(run install arm-incompatible@1.0.0 -y 2>&1)
status=$?
set -e
[[ $status -ne 0 ]]
grep -q 'E_UNSUPPORTED_TARGET.*linux-aarch64' <<<"$output"
[[ ! -e "$home/data/xpkgs/xim-x-arm-incompatible/1.0.0/hook-ran" ]]
! grep -Eq '"(GET|HEAD) ' "$request_log"

# --- sandbox backends: the refusal comes from the index, not the compiler ---
#
# Whether a backend exists for this target is a property of the package index,
# which changes without anyone rebuilding xlings. Answering it with
# `#if defined(__aarch64__)` freezes the claim into the binary: the day
# xim-pkgindex publishes an aarch64 bwrap, every client already in the field
# still refuses. So the fixture states the fact and the binary reads it.
for backend in bwrap proot; do
  cat > "$index/pkgs/${backend:0:1}/${backend}.lua" <<LUA
package = { spec="1", name="$backend", type="package",
  archs={"x86_64"}, programs={"$backend"},
  xpm={linux={ ["1.0.0"]={
    x86_64 = { url="http://127.0.0.1:$port/$backend.tar.gz" }
  }}} }
import("xim.libxpkg.pkginfo")
function install() return true end
LUA
done
rm -f "$index/.xlings-index-cache.json"

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
