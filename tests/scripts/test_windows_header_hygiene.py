#!/usr/bin/env python3
"""Every module that includes <windows.h> must define NOMINMAX first.

windows.h defines `min` and `max` as function-like macros. A module that
includes it without NOMINMAX compiles fine until someone writes
`std::min({a, b, c})` in it, and then clang reports:

    error: too many arguments provided to function-like macro invocation
    note: macro 'min' defined here

The error names the call site and says nothing about the include four hundred
lines above it, so it reads as a problem with the algorithm rather than with
the header. It also cannot fail on Linux or macOS, which is how
src/core/subos.cppm went four consecutive Windows runs red while every local
gate stayed green.

Three of the four modules that include windows.h already had NOMINMAX. This
check exists so the fourth cannot happen again from a machine that has no way
to observe it.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"

INCLUDE = re.compile(r'^\s*#\s*include\s*<windows\.h>', re.IGNORECASE)
DEFINE = re.compile(r'^\s*#\s*define\s+NOMINMAX\b')

failures = []
checked = 0

for path in sorted(SRC.rglob("*.cppm")) + sorted(SRC.rglob("*.cpp")):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    include_at = next((i for i, l in enumerate(lines) if INCLUDE.match(l)), None)
    if include_at is None:
        continue
    checked += 1
    # NOMINMAX has to come BEFORE the include to have any effect.
    if not any(DEFINE.match(l) for l in lines[:include_at]):
        rel = path.relative_to(ROOT)
        failures.append(f"{rel}:{include_at + 1}: includes <windows.h> "
                        f"without a preceding #define NOMINMAX")

if failures:
    print("windows header hygiene: FAILED", file=sys.stderr)
    for f in failures:
        print(f"  {f}", file=sys.stderr)
    print("\n  Add `#define NOMINMAX` above the include, as "
          "src/platform.cppm already does.", file=sys.stderr)
    sys.exit(1)

print(f"windows header hygiene: ok ({checked} module(s) include <windows.h>, "
      f"all define NOMINMAX first)")
