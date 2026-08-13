# Module interface / implementation separation

**Branch:** `refactor/module-impl-separation`
**Date:** 2026-08-13

## Goal

Split every C++23 module in `src/` into a standard-conforming pair:

- `X.cppm` — **module interface unit** (`export module M;`): types, declarations,
  templates, `constexpr`. Produces the BMI.
- `X.cpp` — **module implementation unit** (`module M;`): the function bodies.
  Produces **no BMI**.

No directory moves, no behaviour changes, no new product code.

## Why: the BMI is the recompile trigger

Today every function body lives in the interface unit, so **every body edit
changes the BMI**, and everything that imports the module recompiles.

Measured fan-out over the 99 local modules (`build/bench/analyze.py` +
import-graph scan):

| module | direct importers | transitive downstream |
|---|---|---|
| `xlings.platform` | 45 | **63** |
| `xlings.core.palette` | 8 | 53 |
| `xlings.libs.json` | 31 | 52 |
| `xlings.core.log` | 39 | 47 |
| `xlings.core.config` | 34 | 35 |

Average transitive downstream: **10.7 modules**. Editing one line in
`platform.cppm`'s implementation rebuilds 64 translation units. After the
split it rebuilds one.

## Mechanism verified before writing any code

`build/bench/probe2` (a throwaway 3-file mcpp project) established that mcpp
and gcc@16.1.0 support the standard shape, and that the four rules the
migration depends on actually hold:

1. mcpp scans `.cpp` implementation units with P1689 dyndep and orders them
   after their interface's BMI. `thing.cppm` → `thing.m.o` + `p2.thing.gcm`;
   `thing.cpp` → `thing.o` and **no `.gcm`**.
2. A **non-exported** helper declared in the interface and defined in the
   implementation unit is callable from an **exported template** that gets
   instantiated in a downstream TU. It links and runs.
3. An `extern` module-linkage global declared in the interface and defined in
   the implementation unit is readable from such a template.
4. The `export` keyword must be omitted in the implementation unit; a default
   argument must appear only in the interface; `constexpr` + `static_assert`
   stay in the interface.

Incremental behaviour on that probe:

| edit | recompiled | time |
|---|---|---|
| implementation body (`thing.cpp`) | `thing.cpp` only | 0.29s |
| interface (`thing.cppm`) | `.cppm` + `.cpp` + every importer | 0.79s |

mcpp additionally preserves a BMI's mtime when a recompile produces
byte-identical content, so an interface edit that does not change the BMI
already avoids downstream work. That is why the bodies are the thing to move.

## Classification rules

Applied per namespace-scope (or class-scope) entity.

**STAY in the interface — whole:**
- type definitions (`struct` / `class` / `union` / `enum`), `using`, `typedef`,
  namespace aliases, `concept`, `static_assert`
- anything `template<...>` (11 sites, all variadic log/format wrappers)
- `constexpr` / `consteval` functions and variables, `inline` variables
- preprocessor conditionals that select declarations

**SPLIT — declaration in the interface, definition in the implementation unit:**
- non-template, non-`constexpr` function definitions at namespace scope,
  exported or not (a non-exported helper still needs its declaration in the
  interface when a template or another staying entity calls it)
- out-of-line-able member functions of non-template classes, including
  `static` member functions (defined as `T C::f(...)`, no `static` keyword)
- namespace-scope variable definitions with dynamic initialisation

**MOVE WHOLE to the implementation unit:**
- anonymous-namespace blocks (4 files, all under `src/core/mirror/`)
- namespace-scope `static` free functions **not** referenced by a staying
  entity (internal linkage cannot span two units)

## Ordering invariant

All namespace-scope variable *definitions* of a module move to the same unit
(the implementation unit), so their relative dynamic-initialisation order is
preserved. Never split a module's globals across the two units.

## Scope sizing

| | lines | share |
|---|---|---|
| outside class bodies (free functions) | 39,258 | 84.9% |
| inside class/struct bodies (need out-of-line members) | 6,995 | 15.1% |
| total across 110 `.cppm` | 46,253 | |

71 `static ... (...) {` definitions; indent 0 = namespace-scope internal
linkage, indent 4 = static member functions (ordinary out-of-line definitions).

Both groups are in scope. The class-heavy files are the high-fan-out ones
(`config.cppm` is 93% class body **and** has 34 direct importers), so
skipping them would forfeit much of the benefit.

## Known trade-off, to be measured not assumed

Moving a body out of the interface drops its implicit `inline`. Without LTO
the release build loses those cross-TU inlining opportunities. The dev build
is `-O0`; release is `-O2`. The report must carry binary size and, where
cheap, a runtime check — not a claim that this is free.

Cold-build direction is genuinely uncertain: TU count roughly doubles
(110 → ~220), which costs, while every BMI gets smaller, which pays. Measure
both cold and incremental.

## Verification

1. `mcpp build` succeeds.
2. `mcpp test` — full unit suite green (must run `mcpp build` first;
   `test_interface_protocol` drives the real binary).
3. e2e suite via `tests/e2e/run_all.sh`.
4. Benchmark main vs branch in this one worktree by switching branches, so
   path, filesystem and toolchain fingerprint are identical and only source
   content differs.

## Baseline captured (main, this worktree)

Cold `mcpp build` after `rm -rf target`, warm global dependency cache, 32 cores:
**68.85s / 55.14s / 64.12s**. Variance is ~25%, so the comparison needs
repeats and a median, not a single pair of numbers.
