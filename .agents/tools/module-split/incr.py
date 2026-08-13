#!/usr/bin/env python3
"""Incremental-rebuild benchmark: how much does ONE implementation edit cost?

The edit must change code generation, not just the mtime.  mcpp preserves a
BMI's timestamp when a recompile produces byte-identical content, so a plain
`touch` of a .cppm would skip every downstream unit and flatter the
before-picture.  So we insert a statement into a function BODY:

  * before the split the body lives in the .cppm, so the BMI changes and every
    importer recompiles
  * after the split it lives in the .cpp, so one TU recompiles

Same function, same statement, on both branches — the file it lands in is the
only difference, which is exactly the thing being measured.

Usage:  incr.py <function-name> [<function-name> ...] [--runs N]
"""
from __future__ import annotations
import argparse, glob, os, re, subprocess, sys, time

MARK = 'volatile int _xl_bench_probe'


def find_body(fn: str):
    """Locate `fn`'s definition; return (path, insert_offset) just past its '{'."""
    pat = re.compile(r'(?:^|\n)[ \t]*(?:export\s+)?[\w:<>,&*\[\]\s]*?\b'
                     + re.escape(fn) + r'\s*\([^;{]*?\)[^;{]*?\{')
    for path in sorted(glob.glob('src/**/*.cpp', recursive=True)) + \
                sorted(glob.glob('src/**/*.cppm', recursive=True)):
        src = open(path).read()
        m = pat.search(src)
        if m:
            return path, m.end()
    return None, None


def timed_build():
    t = time.time()
    p = subprocess.run(['mcpp', 'build'], capture_output=True)
    return time.time() - t, p.returncode, p.stdout.decode(errors='replace')


def count_compiles(out: str) -> int:
    return len(re.findall(r'\bg\+\+\b.*?-c\s', out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('fns', nargs='+')
    ap.add_argument('--runs', type=int, default=2)
    a = ap.parse_args()

    branch = subprocess.run(['git', 'branch', '--show-current'],
                            capture_output=True).stdout.decode().strip()
    # make sure we start from an up-to-date tree
    s, rc, _ = timed_build()
    if rc != 0:
        print('baseline build FAILED — aborting', file=sys.stderr)
        return 1
    print(f'# branch={branch}  warm-up build {s:.2f}s')

    for fn in a.fns:
        path, off = find_body(fn)
        if not path:
            print(f'INCR {branch:34} {fn:26} NOT FOUND')
            continue
        for i in range(a.runs):
            src = open(path).read()
            patched = src[:off] + f' {MARK}_{i} = {i};' + src[off:]
            open(path, 'w').write(patched)
            secs, rc, out = timed_build()
            open(path, 'w').write(src)          # revert
            kind = 'impl(.cpp)' if path.endswith('.cpp') else 'iface(.cppm)'
            print(f'INCR {branch:34} {fn:26} run={i} rc={rc} '
                  f'secs={secs:6.2f} in={kind:12} {path}')
            if rc != 0:
                print(out[-800:])
                break
        # leave the tree rebuilt so the next measurement starts warm
        timed_build()
    return 0


if __name__ == '__main__':
    sys.exit(main())
