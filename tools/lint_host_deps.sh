#!/usr/bin/env bash
# Lint: what CI and the tests take from the machine is the T0 list (the
# aarch64 design §2.1, HOST-DEPS-T0). A machine's package manager installing
# something is a failure, unless the line says why it must be the machine's
# with `host-dep-ok: <why>` -- the package manager IS the subject (AUR), the
# command runs inside a pinned build container (the factory), or it prepares
# a bare distribution to install xlings at all (curl, sudo, a user).
# xtest: covers=HOST-DEPS-T0 requires=
set -euo pipefail
cd "$(dirname "$0")/.."

pattern='(apt-get|apt|dnf|yum|zypper|pacman|apk|brew|choco|winget|scoop)[[:space:]]+(-[^[:space:]]+[[:space:]]+)*(install|add|-S[a-zA-Z]*|-U)\b|pip3?[[:space:]]+install'
# What runs and is ours: tracked workflows, scripts and test programs. Not
# the index fixture (its recipes are data), not prose, not a test's scratch.
files="$(git ls-files .github/workflows tests tools | grep -E '\.(ya?ml|sh|py)$' | grep -v '^tests/fixtures/' || true)"
hits=""
[[ -n "$files" ]] && hits="$(grep -nE "$pattern" $files 2>/dev/null | grep -v 'host-dep-ok' \
  | grep -vE '^[^:]+:[0-9]+:[[:space:]]*#' | grep -v '^tools/lint_host_deps.sh:' || true)"
if [[ -n "$hits" ]]; then
  while IFS= read -r h; do echo "::error::a machine's package manager installs here: $h"; done <<<"$hits"
  echo "install it through xlings, or say why it must be the machine's: host-dep-ok: <why>" >&2
  exit 1
fi
echo "host deps: only the T0 list"
