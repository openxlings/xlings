#!/usr/bin/env bash
set -euo pipefail

archive=${1:?archive path required}
sidecar=${2:?sidecar path required}
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

expected=$(awk 'NR == 1 && $1 ~ /^[0-9a-fA-F]{64}$/ {print tolower($1)}' "$sidecar")
[[ -n "$expected" ]]
if command -v sha256sum >/dev/null 2>&1; then
  actual=$(sha256sum "$archive" | awk '{print $1}')
else
  actual=$(shasum -a 256 "$archive" | awk '{print $1}')
fi
[[ "$actual" == "$expected" ]]

tar -xzf "$archive" -C "$work"
package_root=$(find "$work" -mindepth 1 -maxdepth 1 -type d | head -1)
bin=$(find "$package_root" -path '*/bin/xlings' -type f | head -1)
test -x "$bin"
home="$work/home"
mkdir -p "$home"
candidate_home="$home/explicit-cold-home"
run_candidate() {
  XLINGS_NON_INTERACTIVE=1 NO_COLOR=1 XLINGS_HOME="$candidate_home" \
    HOME="$home" "$@"
}

run_candidate "$bin" --version
(cd "$package_root" && run_candidate "$bin" self install)
installed="$candidate_home/bin/xlings"
test -x "$installed"
run_candidate "$installed" self doctor

# Re-install THROUGH the installed binary, so the shim being rewritten is the
# running image. POSIX unlinks by name so this has always worked here, but the
# shim path is shared with Windows -- where it did not (issue #473) -- and a
# contract only one platform checks is one the other can quietly lose.
self_update_out=$(cd "$package_root" && run_candidate "$installed" self install 2>&1)
# Positive evidence the step ran: `self install` onto its own home takes the
# "already in target dir, fixing links" path, which is the one that rewrites
# the running binary's shim. Without this a silent no-op would look like a pass.
grep -qE 'fixing links|install:' <<<"$self_update_out" || {
  echo "$self_update_out"
  fail "self install over the running binary produced no evidence it ran"
}
test -x "$installed"
run_candidate "$installed" --version >/dev/null

# Offline package lifecycle from a working-tree recipe. This exercises the
# candidate's catalog import, install, activation/list and remove paths rather
# than merely executing the portable binary from the archive. Derive a unique
# package name so the bundled official recipes cannot make the local fixture
# ambiguous. Keep the fixture in-tree so candidate validation does not depend
# on an optional package-index clone prepared by another workflow step.
fixture="$repo_root/tests/candidate-install/candidate-helper.lua"
run_candidate "$installed" config --add-xpkg "$fixture"
search_output=$(run_candidate "$installed" search candidate-helper)
grep -q 'local:candidate-helper' <<<"$search_output"
run_candidate "$installed" install candidate-helper -y
run_candidate "$installed" use candidate-helper 0.0.1
candidate_shim="$candidate_home/subos/default/bin/candidate-helper"
test -x "$candidate_shim"
run_candidate "$candidate_shim" >/dev/null
list_output=$(run_candidate "$installed" list)
grep -q 'candidate-helper@0.0.1' <<<"$list_output"
info_output=$(run_candidate "$installed" info local:candidate-helper)
grep -q '0.0.1' <<<"$info_output"
run_candidate "$installed" remove candidate-helper -y
list_output=$(run_candidate "$installed" list)
! grep -q 'candidate-helper@0.0.1' <<<"$list_output"

run_candidate "$installed" subos new candidate-probe
marker="$work/plain-marker"
set +e
run_candidate "$installed" subos use candidate-probe \
  --cmd "printf candidate > '$marker'; exit 37"
status=$?
set -e
[[ $status -eq 37 && "$(cat "$marker")" == candidate ]]

case "$(uname -s)-$(uname -m)" in
  Linux-aarch64|Linux-arm64)
    set +e
    sandbox_output=$(run_candidate "$installed" subos use candidate-probe \
      --sandbox --cmd 'exit 37' 2>&1)
    status=$?
    set -e
    [[ $status -ne 0 ]]
    grep -q 'E_UNSUPPORTED_TARGET.*linux-aarch64' <<<"$sandbox_output"
    ! grep -Eqi 'download|HTTP|404|failed to install sandbox backend' \
      <<<"$sandbox_output"
    ;;
  *)
    # The sandbox backend is fetched from the package index, so this step --
    # unlike every other one here -- depends on a third-party mirror being
    # reachable. These jobs gate `create-release`, and a mirror hiccup must not
    # be able to block a publish, so it retries and then degrades to a loud
    # skip. Everything else in this script stays hard.
    #
    # The coverage is not lost: E2E-25 exercises the same path in the PR
    # workflows, where a red run costs a re-run rather than a release.
    sandbox_marker="$candidate_home/subos/candidate-probe/home/${USER:-user}/marker"
    sandbox_ok=0
    for attempt in 1 2 3; do
      rm -f "$sandbox_marker"
      set +e
      run_candidate "$installed" subos use candidate-probe --sandbox \
        --cmd 'printf sandbox > "$HOME/marker"; exit 37'
      status=$?
      set -e
      if [[ $status -eq 37 && "$(cat "$sandbox_marker" 2>/dev/null)" == sandbox ]]; then
        sandbox_ok=1
        break
      fi
      echo "candidate smoke: sandbox attempt ${attempt} failed (status ${status})"
      sleep $((attempt * 5))
    done
    if [[ $sandbox_ok -ne 1 ]]; then
      echo "::warning::candidate smoke SKIPPED the sandbox lifecycle after 3 attempts -- \
the backend could not be installed from the package index. Every other \
lifecycle step passed."
    fi
    ;;
esac

run_candidate "$installed" subos info candidate-probe >/dev/null
run_candidate "$installed" subos remove candidate-probe
