#!/usr/bin/env bash
# luban-init (SubOS design part 3 §8): stage-0 as its own binary, shipped in
# the Linux release, static, linking nothing of the frontend -- and refusing to
# run as anything but a machine's first process.
# xtest: covers=LUBAN-INIT requires=linux
set -euo pipefail
tarball="${1:?usage: luban_init_test.sh <release tarball>}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tar -xzf "$tarball" -C "$work"
init="$(find "$work" -path '*/bin/luban-init' -type f | head -1)"
[[ -x "$init" ]] || { echo "FAIL: the release ships no bin/luban-init"; exit 1; }
if command -v file >/dev/null 2>&1; then
  file "$init" | grep -qi 'statically linked' || { echo "FAIL: luban-init is not static: $(file "$init")"; exit 1; }
fi
xl="$(dirname "$init")/xlings"
size_init=$(wc -c < "$init"); size_xl=$(wc -c < "$xl")
# Its own package: a fraction of the client, not a copy of it.
(( size_init * 2 < size_xl )) || { echo "FAIL: luban-init ($size_init) is not much smaller than xlings ($size_xl) -- does it link the frontend?"; exit 1; }
set +e
out="$("$init" 2>&1)"; rc=$?
set -e
[[ $rc -ne 0 ]] || { echo "FAIL: luban-init ran as a non-PID-1 process"; exit 1; }
[[ "$out" == *"luban-init:"*"not PID 1"* ]] || { echo "FAIL: unexpected refusal: $out"; exit 1; }
echo "PASS: luban-init shipped, static, ${size_init} bytes (xlings ${size_xl}); refuses outside PID 1"
