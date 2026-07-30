#!/usr/bin/env bash
# The fresh-install suite must install whatever `latest` resolves to.
#
# It is the only suite that tests what a first-time user actually gets: the
# published release artifact, `quick_install`, and an index that has to resolve
# on a cold home. Pin the xlings version and it stops testing that -- it starts
# testing a snapshot nobody installs, and it goes stale silently, one release
# at a time, with everything still green. That is this repository's recurring
# failure mode: the thing that did not happen looks exactly like the thing that
# did.
#
# The convention is written down (AGENTS.md, .agents/skills/xlings-contributing)
# and a written convention is not a guarantee, which is what this file is for.
# It is deliberately cheap: no build, no network, milliseconds.
#
#   bash tests/fresh-install/no_xlings_version_pin_check.sh
#
# NOT flagged: MCPP_OLD / MCPP_NEW / GCC_* / LLVM_* / NINJA_VERSION. Those pin
# the PACKAGES under test, which is the point -- the switch assertions are
# differential, and one version alone would pass even if `use` did nothing.
# What may never be pinned is the xlings BINARY doing the installing.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

FILES=(
  tests/fresh-install/smoke.sh
  tests/fresh-install/smoke.ps1
  tests/fresh-install/lib.sh
  tests/fresh-install/lib.ps1
  .github/workflows/xlings-ci-fresh-install.yml
)

fail_count=0
report() {
  printf 'FAIL %s:%s\n     %s\n     %s\n' "$1" "$2" "$3" "$4" >&2
  fail_count=$((fail_count + 1))
}

for f in "${FILES[@]}"; do
  [[ -f "$f" ]] || continue

  # 1. A version-shaped literal bound to an xlings-named variable.
  #    Catches XLINGS_VERSION=2026.7.30.2, $XlingsVer = '0.4.70', etc.
  while IFS=: read -r line text; do
    [[ -z "${line:-}" ]] && continue
    report "$f" "$line" "$text" \
      "an xlings version must never be pinned here — the suite installs \`latest\` on purpose"
  done < <(grep -nEi '^[^#]*\$?\bxlings[_a-z]*(ver|version)[a-z_]*\b *=' "$f" || true)

  # 2. An explicit xlings coordinate. `xlings@latest` is fine; a number is not.
  while IFS=: read -r line text; do
    [[ -z "${line:-}" ]] && continue
    report "$f" "$line" "$text" \
      "install xlings@latest, not a fixed version"
  done < <(grep -nEi '^[^#]*xlings@[0-9]' "$f" || true)

  # 3. A release tag pinned into the bootstrap URL, which would freeze both
  #    quick_install.sh AND the release it fetches.
  while IFS=: read -r line text; do
    [[ -z "${line:-}" ]] && continue
    report "$f" "$line" "$text" \
      "quick_install must be fetched from a branch ref, not a release tag"
  done < <(grep -nE '^[^#]*QUICK_INSTALL_URL.*(releases/(download|tag)|/v?20[0-9]{2}\.)' "$f" || true)
done

if [[ $fail_count -gt 0 ]]; then
  cat >&2 <<'WHY'

The fresh-install suite tests the RELEASED xlings a new user would get. Pinning
its version turns that into a test of a snapshot nobody installs.

If you need a specific version to reproduce a failure locally, pass it in the
environment for that run instead of committing it. If a release genuinely
requires a pin here, say why in this file and add the exception explicitly --
the point is that it cannot happen by accident.

See AGENTS.md ("Never pin a released xlings version into CI") and
.agents/skills/xlings-contributing/SKILL.md.
WHY
  exit 1
fi

echo "PASS: fresh-install tracks the published latest (no xlings version pinned)"
