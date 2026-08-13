#!/usr/bin/env bash
# Build one split variant with clang 20.1.7 (the macOS toolchain family) in an
# isolated copy under build/ (gitignored), so the diagnosis loop is ~40s instead of
# a 13-minute CI cycle.  main builds clean under this toolchain, so any failure
# here belongs to the split.
#
#   clang_variant.sh <label> [outline-file.cppm ...]     # phase 1 + phase 2 on those files
#   clang_variant.sh <label> --all                        # phase 1 + phase 2 everywhere
#   clang_variant.sh <label>                              # phase 1 only
#
# main + clang builds clean, so any failure here is the split's.
set -u
LABEL="${1:?label}"; shift
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
P="$ROOT/build/cv"
cd "$ROOT" || exit 1

rm -rf "$P"; mkdir -p "$P"
python3 - <<'PY'
import subprocess, os
P = 'build/cv'
for f in subprocess.run(['git','ls-tree','-r','--name-only','b1563fe','src/'],
                        capture_output=True, text=True).stdout.split():
    t = subprocess.run(['git','show',f'b1563fe:{f}'], capture_output=True).stdout
    os.makedirs(os.path.join(P, os.path.dirname(f)), exist_ok=True)
    open(os.path.join(P, f), 'wb').write(t)
toml = open('mcpp.toml').read().replace('default = "gcc@16.1.0"',
                                        'default = "llvm@20.1.7"')
open(P + '/mcpp.toml', 'w').write(toml)
PY
cp mcpp.lock "$P/mcpp.lock"

cd "$P" || exit 1
python3 "$ROOT/.agents/tools/module-split/split.py" --all --write >/dev/null
if [ "$#" -gt 0 ]; then
  python3 "$ROOT/.agents/tools/module-split/outline.py" --write ${LIMIT:+--limit $LIMIT} "$@" >/dev/null
fi
mcpp build >"clang.$LABEL.log" 2>&1
rc=$?
errs=$(grep -ac 'error: ' "clang.$LABEL.log")
failed=$(grep -a -oE 'failed: obj/\S+' "clang.$LABEL.log" | sort -u | tr '\n' ' ')
echo "CLANG $LABEL  rc=$rc errors=$errs  $failed  (n=$#)"
