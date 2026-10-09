#!/usr/bin/env bash
# Lint: the SubOS code reaches external programs through one table
# (xlings.subos.tools) and administrator rights through one door
# (xlings.subos.elevation) -- SubOS design part 3 §6.4.
#
# In modules/subos, src/core/subos, src/core/home and luban/:
#   1. no std::system( -- a shell line quotes a path wrong the day it has a
#      quote in it, and names a tool by whatever PATH says;
#   2. no argv that starts with a tool's literal name: resolve it
#      (`tools::first("mkfs.ext4", ...)`), so where it came from is one
#      decision and `self doctor --isolation --json` can report it;
#   3. an argv that starts with an administrator's command (mount, chown,
#      install, apparmor_parser, sudo) only as the argument of
#      elevation::run, which records it.
# The table itself (modules/subos/src/sandbox/tools.cpp) names the tools; a
# line that spells a name for another reason (a package list, a JSON key, an
# argv handed to elevation::run below) says so with `tool-ok: <why>`.
# xtest: covers=TOOL-RESOLVE requires=
set -euo pipefail
cd "$(dirname "$0")/.."

dirs=(modules/subos src/core/subos src/core/home)
[ -d luban ] && dirs+=(luban)
bad=0
flag() { echo "::error::$1: $2"; bad=$((bad + 1)); }

while IFS= read -r hit; do flag "shell line" "$hit"; done < <(
  grep -rnE --include='*.cpp' --include='*.cppm' 'std::system\(' "${dirs[@]}" \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' || true)

tools='bwrap|proot|pasta|tar|cp|mkfs\.ext4|truncate|mountpoint|wsl|wsl\.exe'
while IFS= read -r hit; do flag "unresolved tool (use xlings.subos.tools)" "$hit"; done < <(
  grep -rnE --include='*.cpp' --include='*.cppm' "\{[[:space:]]*\"($tools)\"[[:space:]]*," "${dirs[@]}" \
    | grep -v '^modules/subos/src/sandbox/tools.cpp:' | grep -v 'tool-ok:' \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' || true)

admin='mount|umount|chown|install|apparmor_parser|sudo'
while IFS= read -r hit; do flag "administrator command outside elevation::run" "$hit"; done < <(
  grep -rnE --include='*.cpp' --include='*.cppm' \
       "\{[[:space:]]*\"($admin)\"[[:space:]]*,[[:space:]]*(\"-|[A-Za-z_>.-]+\.string\(\)|[a-z_]+ \+ \")" "${dirs[@]}" \
    | grep -v 'elevation::run' | grep -v 'tool-ok:' \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' || true)

if [ "$bad" -gt 0 ]; then
  echo "tool resolution lint: FAIL ($bad finding(s))"
  exit 1
fi
echo "tool resolution lint: PASS (one table, one elevation path)"
