#!/usr/bin/env bash
# Regenerate the whole split from the pre-split source, deterministically.
#
#   regen.sh [base-ref]        (default: the commit before the split landed)
#
# `git clean` is NOT enough on its own: once the generated .cpp files are
# committed they are tracked, so clean leaves them and outline.py -- which
# appends -- would stack a second copy of every out-of-line definition on top of
# the first.  So the generated files are removed by comparing the tree against
# the base ref.
set -u
BASE="${1:-b1563fe}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
cd "$ROOT" || exit 1

git checkout "$BASE" -- src/ || exit 1
git clean -fq src/
python3 - "$BASE" <<'PY'
import subprocess, sys, os, glob
base = sys.argv[1]
keep = set(subprocess.run(['git', 'ls-tree', '-r', '--name-only', base, 'src/'],
                          capture_output=True, text=True).stdout.split())
n = 0
for f in glob.glob('src/**/*', recursive=True):
    if os.path.isfile(f) and f not in keep:
        os.remove(f); n += 1
print(f'removed {n} generated file(s)')
PY
python3 "$HERE/split.py" --all --write | tail -2
python3 "$HERE/outline.py" --all --write | tail -2
python3 - <<'PY'
import glob
c = sum(open(f).read().count('\n') for f in glob.glob('src/**/*.cppm', recursive=True))
p = sum(open(f).read().count('\n') for f in glob.glob('src/**/*.cpp', recursive=True))
print(f'interface {c} lines, implementation {p} lines')
PY
