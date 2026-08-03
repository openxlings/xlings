#!/usr/bin/env bash
# The quick installer, verified by RUNNING it.
#
# Its predecessor asserted that `quick_install.sh` CONTAINS the strings
# `verify_sidecar` and `"${url}.sha256"`, and separately re-implemented the
# version sort inline to check that `sort(1)` still works. None of that can
# fail when the installer is broken -- and it replaced the macOS and Windows
# CI steps that used to run the installer for real, so the `curl | sh` path
# most users take was left with no executable coverage at all, at the same
# time as a hard failure mode (a missing sidecar aborts the install) was
# added to it.
#
# Here the installer runs end to end against a local HTTP server holding a
# real archive: source selection, download, sidecar verification, unpacking,
# `self install`. Same code path a user reaches; only the host differs.
#
# The checksum check is asserted in both directions, because "it verified" and
# "it never looked" produce identical output on a good archive:
#   good sidecar     -> installs, binary present
#   tampered sidecar -> fails, nothing installed
#   missing sidecar  -> fails, nothing installed
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="$ROOT_DIR/tools/other/quick_install.sh"
VERSION="9999.0.0.1"

bash -n "$SCRIPT"

case "$(uname -s)" in
  Linux*)  PLATFORM=linux ;;
  Darwin*) PLATFORM=macosx ;;
  *) echo "quick_install: unsupported host, skipped"; exit 0 ;;
esac
case "$(uname -m)" in
  x86_64|amd64) ARCH=x86_64 ;;
  aarch64)      ARCH=aarch64 ;;
  arm64)        ARCH=arm64 ;;
  *) echo "quick_install: unsupported arch, skipped"; exit 0 ;;
esac
case "${PLATFORM}-${ARCH}" in
  linux-x86_64|linux-aarch64|macosx-arm64) ;;
  *) echo "quick_install: no release target for ${PLATFORM}-${ARCH}, skipped"; exit 0 ;;
esac
command -v python3 >/dev/null 2>&1 || { echo "quick_install: python3 needed, skipped"; exit 0; }

root=$(mktemp -d)
server_pid=
cleanup() {
  [[ -z "$server_pid" ]] || kill "$server_pid" 2>/dev/null || true
  rm -rf "$root"
}
trap cleanup EXIT

serve="$root/serve"
# The installer looks for a top-level `xlings-*` directory in the archive.
stage="$root/stage/xlings-${VERSION}"
mkdir -p "$serve" "$stage/bin" "$root/fakehome"

# A stand-in for the release binary. What is under test is the installer, and
# a stub makes the contract observable without depending on a build: `self
# install` copying the package into XLINGS_HOME is the whole handshake.
cat > "$stage/bin/xlings" <<'STUB'
#!/usr/bin/env bash
case "${1:-}" in
  --version) echo "xlings 9999.0.0.1" ;;
  self)
    [[ "${2:-}" == "install" ]] || exit 1
    dest="${XLINGS_HOME:-$HOME/.xlings}"
    mkdir -p "$dest/bin"
    cp "$0" "$dest/bin/xlings"
    ;;
  *) exit 0 ;;
esac
STUB
chmod +x "$stage/bin/xlings"

asset="xlings-${VERSION}-${PLATFORM}-${ARCH}.tar.gz"
tar -czf "$serve/$asset" -C "$root/stage" "xlings-${VERSION}"
if command -v sha256sum >/dev/null 2>&1; then
  (cd "$serve" && sha256sum "$asset" > "$asset.sha256")
else
  (cd "$serve" && shasum -a 256 "$asset" > "$asset.sha256")
fi
cp "$serve/$asset.sha256" "$root/good.sha256"

port=$((22000 + $$ % 20000))
python3 -m http.server "$port" --bind 127.0.0.1 --directory "$serve" \
  >"$root/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 50); do
  curl -fsS "http://127.0.0.1:$port/$asset.sha256" -o /dev/null 2>/dev/null && break
  sleep 0.1
done

install_home="$root/home"
run_installer() {
  rm -rf "$install_home"
  HOME="$root/fakehome" \
  XLINGS_HOME="$install_home" \
  XLINGS_NON_INTERACTIVE=1 \
  NO_COLOR=1 \
  XLINGS_VERSION="$VERSION" \
  XLINGS_BASE_URL="http://127.0.0.1:$port" \
    bash "$SCRIPT" 2>&1
}

fail() { echo "FAIL: $1" >&2; exit 1; }

# --- a good archive installs -----------------------------------------------
set +e
output=$(run_installer); status=$?
set -e
[[ $status -eq 0 ]] || { echo "$output"; fail "the installer rejected a valid archive"; }
[[ -x "$install_home/bin/xlings" ]] \
  || { echo "$output"; fail "no binary at \$XLINGS_HOME/bin/xlings after a successful run"; }
if grep -q $'\033' <<<"$output"; then
  fail "NO_COLOR=1 output still carries ANSI escapes"
fi

# --- a tampered sidecar must stop it ---------------------------------------
python3 - "$root/good.sha256" "$serve/$asset.sha256" <<'PY'
import sys, pathlib
line = pathlib.Path(sys.argv[1]).read_text().strip()
digest, _, rest = line.partition(" ")
flipped = ("0" if digest[0] != "0" else "1") + digest[1:]
pathlib.Path(sys.argv[2]).write_text(f"{flipped} {rest}\n")
PY
set +e
output=$(run_installer); status=$?
set -e
[[ $status -ne 0 ]] \
  || { echo "$output"; fail "a checksum mismatch was accepted -- the sidecar is not verified"; }
[[ ! -e "$install_home/bin/xlings" ]] \
  || fail "a checksum mismatch still left a binary installed"

# --- a missing sidecar must stop it, not be skipped ------------------------
rm -f "$serve/$asset.sha256"
set +e
output=$(run_installer); status=$?
set -e
[[ $status -ne 0 ]] \
  || { echo "$output"; fail "a missing sidecar was treated as 'nothing to verify'"; }
[[ ! -e "$install_home/bin/xlings" ]] \
  || fail "a missing sidecar still left a binary installed"

echo "quick_install.sh: ok (installed; tampered and missing sidecars both rejected)"
