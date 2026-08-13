#!/usr/bin/env bash
# Measure one side of the comparison end to end, unattended.
#
#   measure_side.sh <ref> <label> [cold-runs] [with-test]
#
# Checks out src/ from <ref> into this worktree (HEAD is untouched), then runs
# cold builds, incremental-edit builds, and optionally the whole unit suite.
set -u
REF="${1:?ref}"; LABEL="${2:?label}"; RUNS="${3:-3}"; WITH_TEST="${4:-no}"
TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TOOLS/../../.." && pwd)"
# logs and timings land in build/ (gitignored), never in the tools directory
HERE="$ROOT/build/bench"
mkdir -p "$HERE"
cd "$ROOT" || exit 1
OUT="$HERE/measure.$LABEL.txt"
: >"$OUT"

log() { echo "$@" | tee -a "$OUT"; }

git checkout -q "$REF" -- src/ || { log "checkout failed"; exit 1; }
python3 - "$REF" <<'PY' | tee -a "$OUT"
import subprocess, sys, os, glob
ref = sys.argv[1]
keep = set(subprocess.run(['git', 'ls-tree', '-r', '--name-only', ref, 'src/'],
                          capture_output=True, text=True).stdout.split())
n = 0
for f in glob.glob('src/**/*', recursive=True):
    if os.path.isfile(f) and f not in keep:
        os.remove(f); n += 1
c = sum(open(f).read().count('\n') for f in glob.glob('src/**/*.cppm', recursive=True))
p = sum(open(f).read().count('\n') for f in glob.glob('src/**/*.cpp', recursive=True))
print(f'TREE removed={n} cppm_files={len(glob.glob("src/**/*.cppm", recursive=True))} '
      f'cpp_files={len(glob.glob("src/**/*.cpp", recursive=True))} '
      f'iface_lines={c} impl_lines={p}')
PY

for i in $(seq 1 "$RUNS"); do
  rm -rf target
  s=$(date +%s.%N)
  mcpp build >"$HERE/m.$LABEL.cold.$i.log" 2>&1
  rc=$?
  e=$(date +%s.%N)
  log "$(printf 'COLD %-8s run=%s rc=%s secs=%.2f' "$LABEL" "$i" "$rc" "$(echo "$e - $s" | bc)")"
  [ "$rc" -ne 0 ] && { tail -25 "$HERE/m.$LABEL.cold.$i.log" | tee -a "$OUT"; exit 1; }
done

find target -path '*/bin/xlings' -type f -printf "SIZE $LABEL %s bytes\n" | tee -a "$OUT"

# workspace_install_targets is a CLASS MEMBER on main (implicitly inline, so its
# body is in the BMI) and an out-of-line definition on the branch -- the case the
# split is supposed to fix.  compare_segment is the CONTROL: `inline` in
# semver.cppm on both refs, so it must show no improvement; if it does, the
# measurement is noise.
python3 "$TOOLS/incr.py" --runs 2 \
    parse_sudo_env level_string strip_ansi parse_index_repos_json shim_filename_ \
    workspace_install_targets compare_segment \
  2>&1 | sed "s/^/[$LABEL] /" | tee -a "$OUT"

if [ "$WITH_TEST" = "with-test" ]; then
  s=$(date +%s.%N)
  mcpp test >"$HERE/m.$LABEL.test.log" 2>&1
  rc=$?
  e=$(date +%s.%N)
  log "$(printf 'TEST %-8s rc=%s secs=%.2f' "$LABEL" "$rc" "$(echo "$e - $s" | bc)")"
  grep -a 'test result' "$HERE/m.$LABEL.test.log" | tee -a "$OUT"
fi
log "DONE $LABEL"
