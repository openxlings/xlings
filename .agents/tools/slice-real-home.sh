#!/usr/bin/env bash
# Build a runnable slice of a real ~/.xlings into a scratch directory.
#
# Why a slice and not a copy: a real home is tens of gigabytes and the disk it
# lives on is usually the one with no room left. Everything that decides what
# `self doctor` sees is small -- the version DB, the subos state files, the
# index -- so those are copied for real. The payload store is reproduced as a
# HARDLINK FARM, which costs directory entries and nothing else.
#
# The hardlink farm is safe against deletion (unlink drops the slice's name,
# the real file survives) but NOT against in-place rewriting: an install hook
# that does io.writefile() over an existing path truncates the shared inode.
# Two defences, and the second is the one to trust:
#   1. --real <store-dir> forces a genuine copy for packages a repair is
#      expected to reinstall.
#   2. `verify-untouched` proves after the fact that nothing under the real
#      store changed. Run it every time; do not reason about it instead.
#
# Never point --home at a home you are willing to lose. This script only reads
# it, but what you run against the slice afterwards is up to you.
set -euo pipefail

SRC="${HOME}/.xlings"
DST=""
BIN=""
REAL_STORES=()
MODE="build"

usage() {
    cat <<'EOF'
usage:
  slice-real-home.sh --dst <dir> [--home <src>] [--bin <xlings>] [--real <store-name>]...
  slice-real-home.sh verify-untouched --marker <file> [--home <src>]

  --dst    where to build the slice (must not exist)
  --home   source home (default: ~/.xlings)
  --bin    xlings binary to install as the slice's bin/xlings
  --real   payload store dir to copy for real instead of hardlinking,
           repeatable. Use for any package the run will reinstall.
           Default: xim-x-llvm

  verify-untouched --marker F   fail if anything under <home>/data/xpkgs is
                                newer than F. Create F with `touch` before the
                                run under test.
EOF
}

if [[ "${1:-}" == "verify-untouched" ]]; then
    MODE="verify"; shift
fi

MARKER=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dst)    DST="$2"; shift 2 ;;
        --home)   SRC="$2"; shift 2 ;;
        --bin)    BIN="$2"; shift 2 ;;
        --real)   REAL_STORES+=("$2"); shift 2 ;;
        --marker) MARKER="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

if [[ "$MODE" == "verify" ]]; then
    [[ -n "$MARKER" ]] || { echo "verify-untouched needs --marker" >&2; exit 2; }
    changed="$(find "$SRC/data/xpkgs" -newer "$MARKER" 2>/dev/null || true)"
    if [[ -n "$changed" ]]; then
        echo "FAIL: the real payload store was modified:" >&2
        printf '%s\n' "$changed" | head -40 >&2
        exit 1
    fi
    echo "OK: $SRC/data/xpkgs unchanged since $MARKER"
    exit 0
fi

[[ -n "$DST" ]] || { echo "--dst is required" >&2; usage; exit 2; }
[[ -d "$SRC" ]] || { echo "source home not found: $SRC" >&2; exit 1; }
[[ ! -e "$DST" ]] || { echo "destination already exists: $DST" >&2; exit 1; }
[[ ${#REAL_STORES[@]} -gt 0 ]] || REAL_STORES=(xim-x-llvm)

# Same filesystem, or `cp -al` cannot link.
src_dev="$(stat -c %d "$SRC")"
mkdir -p "$(dirname "$DST")"
dst_dev="$(stat -c %d "$(dirname "$DST")")"
if [[ "$src_dev" != "$dst_dev" ]]; then
    echo "refusing: $DST is on a different filesystem than $SRC;" >&2
    echo "the payload store could only be copied for real (tens of GB)." >&2
    exit 1
fi

echo "==> slice $SRC -> $DST"
mkdir -p "$DST/data" "$DST/subos" "$DST/bin"

# The version DB. The whole point of the slice.
cp -a "$SRC/.xlings.json" "$DST/.xlings.json"
[[ -d "$SRC/config" ]] && cp -a "$SRC/config" "$DST/config"

# Index + repo metadata: small, and doctor's remedy probe reads it.
for d in xim-pkgindex xim-index-repos local-indexrepo scode ros2; do
    [[ -e "$SRC/data/$d" ]] && cp -a "$SRC/data/$d" "$DST/data/$d"
done
[[ -f "$SRC/data/github-mirrors.json" ]] \
    && cp -a "$SRC/data/github-mirrors.json" "$DST/data/"
# data/runtimedir is download scratch; recreated on demand, gigabytes, skipped.

# The payload store, as a hardlink farm.
#
# A few files cannot be hardlinked at all: setuid binaries under
# fs.protected_hardlinks (bwrap, code's chrome-sandbox). Those get a real copy
# -- there are single digits of them, and leaving them out would change what
# doctor sees. cp's failure is expected here, so it must not take the run down.
echo "==> payload store (hardlink farm)"
link_errors="$(mktemp)"
cp -al "$SRC/data/xpkgs" "$DST/data/xpkgs" 2>"$link_errors" || true
# `cp: cannot create hard link 'DST/…' to 'SRC/…': …`
sed -n "s|^cp: cannot create hard link '\\([^']*\\)' to '\\([^']*\\)'.*|\\1\\t\\2|p" \
    "$link_errors" | while IFS=$'\t' read -r dstfile srcfile; do
    echo "    real copy (unlinkable): ${srcfile#$SRC/}"
    mkdir -p "$(dirname "$dstfile")"
    cp -a "$srcfile" "$dstfile"
done
# Anything else cp complained about is a genuine failure.
if grep -qv 'cannot create hard link' "$link_errors" 2>/dev/null \
   && grep -q . "$link_errors"; then
    if grep -v 'cannot create hard link' "$link_errors" | grep -q .; then
        echo "FAIL: payload store copy reported errors:" >&2
        grep -v 'cannot create hard link' "$link_errors" >&2
        exit 1
    fi
fi
rm -f "$link_errors"

# Break the links the installer is KNOWN to rewrite in place.
#
# A payload dir carries `.xpkg.lua`, the recipe snapshot, and every install
# rewrites it -- io.writefile() truncates in place, so through a hardlink that
# is a write to the real home. Measured: one `--fix` pass touched seven of
# them. The content came out byte-identical (same recipe, same bytes), so
# nothing was damaged, but `verify-untouched` reported the real store as
# modified and it was right to.
#
# These files are ~10KB each and there are a few hundred, so copying them for
# real removes the whole class for about the price of nothing.
echo "==> unlink rewritable metadata"
broken=0
while IFS= read -r meta; do
    cp -a --remove-destination "$(readlink -f "$meta")" "$meta.slice-tmp"
    mv -f "$meta.slice-tmp" "$meta"
    broken=$((broken + 1))
done < <(find "$DST/data/xpkgs" -name '.xpkg.lua' -type f)
echo "    real copy: $broken metadata file(s)"

for store in "${REAL_STORES[@]}"; do
    if [[ -d "$DST/data/xpkgs/$store" ]]; then
        echo "==> real copy: $store"
        rm -rf "$DST/data/xpkgs/$store"
        cp -a "$SRC/data/xpkgs/$store" "$DST/data/xpkgs/$store"
    fi
done

# subos: `default` in full (sysroot teardown has to be faithful), every other
# one as its state file only -- those are what pin payloads and what the
# cross-subos checks read.
echo "==> subos"
for dir in "$SRC"/subos/*/; do
    name="$(basename "$dir")"
    [[ "$name" == "current" ]] && continue          # symlink, handled last
    [[ -L "${dir%/}" ]] && continue
    if [[ "$name" == "default" ]]; then
        cp -a "${dir%/}" "$DST/subos/default"
    elif [[ -f "$dir/.xlings.json" ]]; then
        mkdir -p "$DST/subos/$name"
        cp -a "$dir/.xlings.json" "$DST/subos/$name/.xlings.json"
    fi
done

# Repoint every absolute path at the slice.
#
# The version DB stores payload paths absolutely (`/home/you/.xlings/data/…`),
# so a slice left as-is would have its records pointing back into the REAL
# store: doctor would grade the original, and any repair would rewrite it. This
# rewrite is what makes the slice self-contained, and it is a plain prefix
# substitution so pathological records survive it intact -- a Windows-style
# `/home/you/.xlings\data\xpkgs\…` keeps its backslashes, which is exactly the
# state under test.
echo "==> repoint paths to the slice"
python3 - "$SRC" "$DST" <<'PY'
import os, pathlib, sys
src, dst = sys.argv[1].rstrip('/'), sys.argv[2].rstrip('/')
root = pathlib.Path(dst)
targets = [root / '.xlings.json'] + sorted(root.glob('subos/*/.xlings.json'))

# The index cache maps every package name to the ABSOLUTE path of its recipe.
# Left alone it points at the real home, so the slice installs packages by
# reading the real home's recipes -- and a recipe change under test is silently
# not the one being tested. Measured: editing glibc.lua in the slice's own
# data/xim-pkgindex changed nothing at all, twice, with no diagnostic; the
# install kept running the version in ~/.xlings.
#
# That is the trap this whole tool exists to avoid, one level deeper than the
# payload store: an experiment that reports on a home other than the one it
# claims to. Enumerated per directory rather than by a `data/**` glob, because
# `data/xpkgs` is tens of gigabytes and holds JSON belonging to payloads, which
# must keep whatever paths they were installed with.
for d in ('', 'data', 'data/xim-pkgindex', 'data/xim-index-repos'):
    base = root / d if d else root
    if not base.is_dir():
        continue
    targets += sorted(p for p in base.glob('*.json') if p.is_file())
    targets += sorted(p for p in base.glob('.*.json') if p.is_file())

n = 0
seen = set()
for path in targets:
    if path in seen or not path.is_file() or path.is_symlink():
        continue
    seen.add(path)
    text = path.read_text(encoding='utf-8')
    if src not in text:
        continue
    path.write_text(text.replace(src, dst), encoding='utf-8')
    n += 1
print(f"    rewrote {n} state file(s)")

# Assert it, rather than trust the list above. A new state file that nobody
# added to that list would otherwise keep pointing at the real home, and the
# only symptom would be a measurement that quietly describes the wrong home.
missed = []
for d in ('', 'data', 'data/xim-pkgindex', 'data/xim-index-repos'):
    base = root / d if d else root
    if not base.is_dir():
        continue
    for path in list(base.glob('*.json')) + list(base.glob('.*.json')):
        if not path.is_file() or path.is_symlink():
            continue
        if src in path.read_text(encoding='utf-8', errors='replace'):
            missed.append(str(path))
if missed:
    raise SystemExit("slice-real-home: these still name the real home after "
                     "repointing: " + ", ".join(missed))

# The sysroot is made of symlinks INTO the payload store, and `cp -a` copies a
# symlink's text verbatim -- so every one of them still points at the real
# home. Left that way the slice does not merely lose fidelity, it invents a
# defect: `xvm-sysroot-drift` fires for every declared destination whose link
# does not start with this home's payload root, which on the measured home is
# 126 findings that do not exist outside the slice.
links = 0
for path in root.glob('subos/**/*'):
    if not path.is_symlink():
        continue
    target = os.readlink(path)
    if not target.startswith(src):
        continue
    path.unlink()
    path.symlink_to(target.replace(src, dst, 1))
    links += 1
print(f"    repointed {links} sysroot symlink(s)")
PY

if [[ -n "$BIN" ]]; then
    cp -a "$BIN" "$DST/bin/xlings"
    chmod +x "$DST/bin/xlings"
fi

# LAST. A `subos/*/.xlings.json` loop that skips `default` still reaches it
# through `current`, so anything scripted above would have silently edited the
# subos under test.
ln -sfn default "$DST/subos/current"

echo "==> done: $DST"
du -sh "$DST" 2>/dev/null || true
