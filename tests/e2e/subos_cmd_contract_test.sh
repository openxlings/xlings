#!/usr/bin/env bash
set -euo pipefail
[[ "$(uname -s)" == Darwin ]] || { echo "subos cmd macOS contract: skipped"; exit 0; }
source "$(dirname "$0")/project_test_lib.sh"
bin=$(find_xlings_bin)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
home="$root/.xlings"
XLINGS_HOME="$home" HOME="$root" "$bin" self init
XLINGS_HOME="$home" HOME="$root" "$bin" subos new probe
set +e
XLINGS_HOME="$home" HOME="$root" "$bin" subos use probe --sandbox \
  --cmd 'printf "%s" "$HOME" > "$HOME/marker"; exit 37'
status=$?
set -e
[[ $status -eq 37 ]]
[[ "$(cat "$home/subos/probe/sandbox-home/marker")" == "$home/subos/probe/sandbox-home" ]]
echo "subos cmd macOS contract: ok"
