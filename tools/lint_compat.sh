#!/usr/bin/env bash
# What exists only for a released older version lives in luban/os/compat
# (design 2026-10-19 §15.3): elsewhere in luban/ and apps/, no frozen edition
# name (subos:luban-*) and no released version written into code. A version
# constant (kVersion) and comments are not branches.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
report() { echo "lint_compat: $1"; fail=1; }

while IFS= read -r hit; do
    report "$hit: a frozen edition name outside luban/os/compat (use luban::compat)"
done < <(grep -rn --include='*.cpp' --include='*.cppm' '"subos:luban-' luban apps \
            | grep -v '^luban/os/compat/' | grep -v '^[^:]*:[0-9]*:[[:space:]]*//' || true)

while IFS= read -r hit; do
    report "$hit: a released version in code outside luban/os/compat (a compatibility judgement belongs there)"
done < <(grep -rnE --include='*.cpp' --include='*.cppm' '"20[0-9]{2}\.[0-9]{1,2}\.[0-9]{1,2}(\.[0-9]+)?"' luban apps \
            | grep -v '^luban/os/compat/' | grep -v '^luban/specs/' | grep -v 'kVersion' \
            | grep -v '^[^:]*:[0-9]*:[[:space:]]*//' || true)

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "compat lint: PASS (old versions are luban/os/compat's)"
