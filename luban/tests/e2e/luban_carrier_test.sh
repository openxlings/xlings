#!/usr/bin/env bash
# Luban on a carrier (part 4 §8.2), on a Linux host through the wsl2
# carrier's stand-in (tests/fixtures/carrier/fake-wsl.sh): the release's
# luban makes a Luban system in the carrier's machine, says what it is from
# the luban there, runs a command in it and removes it -- the path a Windows
# PC takes, with every step but wsl.exe itself. The machine there is the
# release's own Linux build (xlings and luban), as a Windows home gets it.
# xtest: covers=CARRIER-WSL2-LUBAN requires=linux,bwrap,network
set -euo pipefail
tarball="${1:?usage: luban_carrier_test.sh <release tarball>}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tar -xzf "$tarball" -C "$work"
pkg="$(find "$work" -maxdepth 1 -type d -name 'xlings-*-linux-*' | head -1)"
[[ -x "$pkg/bin/xlings" && -x "$pkg/bin/luban" ]] || { echo "FAIL: the release has no bin/xlings and bin/luban"; exit 1; }
mkdir -p "$work/wsl"
run() {
  env -u XLINGS_ACTIVE_SUBOS -u XLINGS_SUBOS_MODE -u XLINGS_TRACE_ID \
    XLINGS_HOME="$pkg" HOME="$work" XLINGS_WSL_EXE="$here/../../../tests/fixtures/carrier/fake-wsl.sh" FAKE_WSL_ROOT="$work/wsl" \
    XLINGS_CARRIER_GUEST_XLINGS="$pkg/bin/xlings" "$@"
}
fail() { echo "FAIL: $*"; exit 1; }
run "$pkg/bin/xlings" self init >/dev/null

out="$(run "$pkg/bin/luban" new cbox --edition tiny --carrier wsl2 -y 2>&1)" || fail "luban new on the carrier: $out"
[[ "$out" == *"cbox is ready"* ]] || fail "no creation report: $out"
distro="$(ls "$work/wsl/distros" | head -1)"
[[ -d "$work/wsl/distros/$distro/xlings/subos/cbox" ]] || fail "cbox is not in the carrier's machine"
[[ -x "$work/wsl/distros/$distro/xlings/bin/luban" ]] || fail "the carrier's machine has no luban"

# What it is, from the luban there.
st="$(run "$pkg/bin/luban" status cbox --json)"
python3 - "$st" <<'PY' || fail "status through the carrier: $st"
import json, sys
line = [l for l in sys.argv[1].splitlines() if l.strip()][-1]
j = json.loads(line)
j = j.get("result", j)
assert j["carrier"] == "wsl2", j
assert j["kind"] == "root", j
assert j["edition"].startswith("subos:luban-tiny"), j
assert j["packages"] > 0, j
assert j.get("generation", 0) >= 1, j
PY

# Every system at once: one question to the luban there, not one per system.
before=$(wc -l < "$work/wsl/calls.log")
ls_json="$(run "$pkg/bin/luban" ls --json)" || fail "luban ls through the carrier"
asked="$(tail -n +$((before + 1)) "$work/wsl/calls.log")"
[[ $(grep -c '/xlings/bin/luban ls' <<<"$asked") -eq 1 ]] || fail "luban ls did not ask the carrier once: $asked"
! grep -q '/xlings/bin/luban status' <<<"$asked" || fail "luban ls asked the carrier per system: $asked"
python3 - "$ls_json" <<'PY' || fail "luban ls --json: $ls_json"
import json, sys
j = json.loads([l for l in sys.argv[1].splitlines() if l.strip()][-1])
box = [e for e in j["environments"] if e["name"] == "cbox"][0]
assert box["carrier"] == "wsl2" and box["kind"] == "root" and box["packages"] > 0, box
PY

# A command in it, there.
got="$(run "$pkg/bin/luban" run cbox -- uname -s 2>&1)" || fail "luban run through the carrier: $got"
[[ "$got" == *Linux* ]] || fail "uname in the system said: $got"

# The machine's probe was kept for luban status.
python3 -c "import json,sys; j=json.load(open(sys.argv[1])); assert isinstance(j['kvm'], bool)" \
  "$pkg/carriers/wsl2/probe.json" || fail "no probe.json for the carrier"

run "$pkg/bin/luban" rm cbox -y >/dev/null 2>&1 || fail "luban rm through the carrier"
[[ ! -e "$work/wsl/distros/$distro/xlings/subos/cbox" ]] || fail "cbox is still in the carrier's machine"
echo "PASS: a Luban system in a carrier's machine -- made, described by the luban there, used, removed"
