# Module interface / implementation separation — report

**Branch:** `refactor/module-impl-separation` (PR #545) vs `main` @ `b1563fe`
**Date:** 2026-08-13
**Machine:** Linux 6.8, 32 cores, gcc@16.1.0 via mcpp 2026.8.11.3

## 1. What changed

Every `.cppm` in `src/` carried both the module interface and its own
implementation. Each is now a standard-conforming pair:

| | file | module declaration | produces a BMI? |
|---|---|---|---|
| interface unit | `X.cppm` | `export module M;` | yes |
| implementation unit | `X.cpp` | `module M;` | **no** |

No directories moved, no behaviour changed, no product code was written. A
partition's implementation goes into an implementation unit of the *primary*
module (`module M;`), because `module M:part;` would be another partition and
would still produce a BMI — and a module may have any number of implementation
units.

Four things left the interface:

1. **Namespace-scope implementations** — 81 files, 779 entities, 26,606 body lines.
2. **Class member bodies** — 29 files, 286 members, 3,845 body lines. A body
   defined inside a class is implicitly `inline`, so it stayed in the BMI.
3. **Module-private declarations** — a non-exported helper's declaration stays in
   the interface only if something still *there* names it (an exported template,
   an `inline`/`constexpr` body, a class member body). Otherwise it belongs in
   the implementation unit: in the interface it sits in the BMI, so changing its
   signature would recompile every importer for nothing.
4. **The imports the bodies took with them** — 153 of 554 interface import edges
   (28%). `split.py` *copies* the import list into the implementation unit, which
   is right for the implementation and wrong for the interface: it was left
   importing modules only the moved bodies used, and every such edge makes the
   interface's BMI depend on a module it does not name. Found by self-review, not
   by a compiler — nothing about it fails to build.

### Interface surface

| | lines | share of original |
|---|---|---|
| interface before | 46,253 | 100% |
| interface after phases 1–3 | **14,666** | **32%** |
| implementation after | 35,655 | |

110 interface units, 92 implementation units (90 generated + `main.cpp` +
`windows_syslibs.cpp`). Only one implementation unit is trivial, and it is the
pre-existing `windows_syslibs.cpp` — none of the 90 generated units is an empty
translation unit added for nothing.

The interface is what every importer reads through the BMI. That is the number
the build speed follows.

## 2. Why it should matter — the fan-out that was there before

Measured over the 99 local modules:

| module | direct importers | transitive downstream |
|---|---|---|
| `xlings.platform` | 44 | **66** |
| `xlings.core.log` | 40 | 54 |
| `xlings.core.utils` | 8 | 44 |
| `xlings.core.config` | 35 | 41 |
| `xlings.core.palette` | 8 | 53 |
| `xlings.libs.json` | 31 | 52 |

Average transitive downstream: **10.7 modules**. Before the split, editing one
line of `platform.cppm`'s implementation invalidated its BMI and recompiled 66
translation units. After it, that edit lands in `platform.cpp`, which produces
no BMI.

## 3. Method

Both refs are measured **in one worktree**, checking each out in turn: same
path, same filesystem, same toolchain fingerprint, same warm dependency cache.
The only variable is the source content.

Only `src/` is swapped — `mcpp.toml` is held at the branch's version for both
sides. That is deliberate: the dev target is `gcc@16.1.0` either way, so holding
the manifest constant keeps the comparison to the source content and keeps the
release-toolchain bump (§8a) out of it.

- **Cold build** — `rm -rf target && mcpp build`, repeated. The project's own
  target directory is removed so every project TU recompiles; the global
  dependency cache (`~/.mcpp/build-cache`) stays warm on purpose, because the
  dependencies are not what changed.
- **Incremental build** — one implementation edit, timed rebuild. The edit
  inserts a statement into a function *body*: a plain `touch` would not do,
  because mcpp preserves a BMI's timestamp when a recompile produces
  byte-identical content, so touching a `.cppm` skips every downstream unit and
  flatters the before-picture. Same function, same statement, on both refs — the
  file it lands in is the only difference, which is exactly what is being
  measured.
- **`mcpp test`** — the whole unit suite, 38 binaries. Each test file is a
  non-module TU that imports many modules, so this is where interface size shows
  up most directly. It is also the gate a developer actually waits on.

Probe functions span the fan-out range **and** the two kinds of body, because
they are not equivalent. A non-`inline` free function's body need not be in the
BMI at all, so main may already avoid downstream work for it; a body defined
inside a class **is** implicitly inline and therefore certainly in the BMI. If
the split only helped the second kind, the first kind's numbers would say so.

| probe | module | transitive downstream | on `main` it is |
|---|---|---|---|
| `parse_sudo_env` | `xlings.platform` | 66 | namespace-scope fn |
| `level_string` | `xlings.core.log` | 54 | namespace-scope fn |
| `strip_ansi` | `xlings.core.utils` | 44 | namespace-scope fn |
| `parse_index_repos_json` | `xlings.core.config` | 41 | namespace-scope fn |
| `shim_filename_` | `xlings.core.xself.doctor` | 16 | namespace-scope fn |
| `workspace_install_targets` | `xlings.core.config` | 41 | **class member** (in the BMI) |
| `compare_segment` | `xlings.core.semver` | 23 | **control** — `inline`, unmoved |

`compare_segment` is the control: it stays `inline` in `semver.cppm` on **both**
refs, so an edit to it invalidates semver's BMI either way and the **same**
downstream set rebuilds. It therefore separates two effects that would otherwise
be confounded:

- **rebuild set** — an implementation edit stops invalidating the BMI at all, so
  nothing downstream rebuilds. Only the moved probes get this.
- **rebuild cost** — whatever does rebuild downstream is now a small interface
  instead of a large one. The control gets this too.

The control is expected to improve, just much less. If it improved as much as the
moved probes, the split would not be what the numbers are measuring.

Harness: `.agents/tools/module-split/{cold.sh,incr.py}`, driver
`.agents/tools/module-split/measure-side.sh`.

## 4. Measurements

### Cold build — every project TU recompiled

| ref | runs (s) | median |
|---|---|---|
| `main` @ b1563fe | 67.17 / 55.27 / 56.40 | **56.40** |
| this branch | 36.97 / 27.28 / 26.85 | **27.28** |

**2.07× faster**, and the direction was not a given: translation units roughly
doubled (110 interfaces → 110 interfaces + 92 implementation units). Smaller
BMIs pay more than the extra TUs cost. The first run on each side is the high
one — that is page cache for freshly written files, which is why the median is
the figure quoted.

### Binary size

| ref | dev binary (`-O0 -g`) |
|---|---|
| `main` | 124,238,424 B |
| this branch | 124,368,072 B (**+0.10%**) |

Dropping implicit `inline` costs cross-TU inlining without LTO, so this was
worth checking rather than assuming. At `-O0` the answer is "no change worth
naming"; a release-mode (`-O2`) size and runtime comparison is **not** measured
here.

### One implementation edit

Two runs per probe. Both are shown rather than a median: with n=2 a median *is*
the mean, so one outlier would move it silently.

| probe | downstream | `main` (s) | branch (s) | best-to-best |
|---|---|---|---|---|
| `parse_sudo_env` | 66 | 63.12 / 54.04 | 4.46 / 4.60 | **12.1×** |
| `level_string` | 54 | 62.42 / 51.38 | 4.49 / 4.56 | **11.4×** |
| `strip_ansi` | 44 | 62.63 / 50.01 | 4.64 / 4.71 | **10.8×** |
| `workspace_install_targets` (class member) | 41 | 49.00 / 58.95 | 6.43 / 6.31 | **7.8×** |
| `parse_index_repos_json` | 41 | 62.13 / 49.08 | 19.69 / 6.36 | **7.7×** |
| `shim_filename_` | 16 | 45.55 / 35.89 | 7.86 / 7.78 | **4.6×** |
| `compare_segment` — **control**, `inline` on both | 23 | 43.58 / 54.53 | 17.24 / 17.54 | 2.5× |

**On `main`, editing one function body costs about as much as building the whole
project from scratch** — 54–63s against a 56.40s cold build. That is the shape
the fan-out predicted: a high-fan-out interface edit rebuilds nearly everything,
and before the split every body edit *was* an interface edit.

The control lands at 2.5× while the moved probes land at 4.6–12.1×. The control's
2.5× is the interfaces downstream simply being smaller to recompile; the gap
between 2.5× and 12× is what moving the body out actually bought.

The branch's `parse_index_repos_json` first run (19.69s against 6.36s on the
second) is an outlier, not a pattern — the same probe measured 6.51 / 6.32 in an
earlier run of the same harness.

### `mcpp test` — where the split LOSES

| ref | wall clock | result |
|---|---|---|
| `main` | **953.81s** | 38 passed, 0 failed |
| this branch | 1104.83s (**1.16× slower**) | 38 passed, 0 failed |

This is the one measurement that goes the wrong way, and the mechanism is not a
guess. Per test binary:

| ref | min | median | max | sum over 38 |
|---|---|---|---|---|
| `main` | 20.86s | 21.18s | 41.16s | 832.5s |
| this branch | 23.96s | 24.92s | 45.53s | 1011.7s |

**+4.72s per binary, and 38 × 4.72 = +179s is the entire gap.** The overhead is
near-constant across binaries regardless of the test file's own size — min +3.10,
median +3.74, max +4.37. A compile-side cost would scale with the file being
compiled; a link-side cost would not. Each test binary now links roughly 202
object files instead of 112.

So: **the split trades link time for compile time.** Compilation gets much
faster; linking gets slower, and `mcpp test` is link-dominated because it links
39 large static binaries. A workflow that links once (`mcpp build`) wins 2×; a
workflow that links 39 times loses 16%.

The obvious follow-up is on the test harness rather than the split: 38 binaries
each statically linking the whole project is what makes link cost dominate, and
one binary (or a shared library) would remove it. That is out of scope here.

The branch figure carries a ~27s pessimism: `src/` was regenerated part-way
through that run, forcing a module rebuild the main-side run did not pay. It
biases against the branch, so the 1.16× is if anything slightly overstated.

## 5. Two gcc@16.1.0 internal compiler errors

Both are compiler crashes, not code errors, and both blame the wrong entity.
Both are recorded with their evidence in `outline.py`'s `ICE_SKIP`, and the two
members keep their bodies inline.

**`Counts::issues()`** (`core/xself/doctor.cppm`) — a two-line accessor.
Defining it out-of-line segfaults cc1plus at `DoctorState st;` in
`doctor.cpp:55` — a different type, in a different function.

**`Config::instance_()`** (`core/config.cppm`) —
`static Config& instance_() { static Config inst; return inst; }`. Its
function-local static is where the module-attached `Config` is first completed.
Move that body and the first-instantiation point moves with it, after which
cc1plus segfaults compiling an **unrelated translation unit** — `doctor.cpp`, on
a different type, in a different module. Nothing in the message names anything
that changed. Found by bisecting config.cppm's 85 movable members
(`bisect-member.sh`): #34 is the boundary and the other 84 are fine.

A crashed cc1plus leaves a truncated `.gcm`, after which unrelated targets fail
with `Bad file data` on the next build. `target/*/gcm.cache` and `~/.mcpp/bmi`
both need clearing after one, or the next run reports a failure that has nothing
to do with what you changed.

## 6. Deliberately not done

**Vestigial `inline` is left alone. 1,953 lines still sit in `inline` bodies in
the interface:**

| lines | file |
|---|---|
| 315 | `core/semver.cppm` |
| 315 | `core/elf_same_source.cppm` |
| 259 | `ui/layout.cppm` |
| 217 | `core/closure_check.cppm` |
| 180 | `core/subos.cppm` |
| 71 | `core/palette.cppm` (53 transitive downstream) |

In a module interface, `inline` means "put this body in the BMI so importers can
inline it". Removing it is a performance-visibility decision, not a move — the
maintainer's call, not a mechanical migration's. Stripping it would take the
interface down by roughly another 13% and is the largest remaining lever.

Measured on `main`'s own tree, the figure is **1,953 lines** — identical. So these
are not lines the split left behind or failed to move; they are exactly what was
already `inline` before it started, carried over untouched.

## 7. Invariants checked statically

Three things this refactor must not change, checked against the pre-split tree
rather than assumed:

| invariant | before | after | tool |
|---|---|---|---|
| exported identifiers | 961 | **961** (0 lost, 0 gained) | `export-surface.py` |
| module names | 110 | **110, byte-identical** | — |
| comment lines | 10,316 | 10,612 | — |

The export-surface check had to be made depth-aware to mean anything: a
line-anchored regex over an `export namespace` body also matches the **local
variables** inside the function bodies that live there, so the first version
reported 914 "lost exports" with names like `1`, `a`, `acc`, `activeBin` — an
artefact of the bodies moving, not a lost export.

The comment surplus is the comment above an `#if`, emitted to both units along
with the directive. Getting there took a fix: dropping a module-private
declaration from the interface first deleted the comment above it, 230 lines
explaining helpers whose bodies are still there. Counting comment lines is what
found that; the compiler had nothing to say about it.

## 8. Three toolchains, three failures the dev build could not see

The dev loop is one compiler (`gcc@16.1.0`, x86_64-linux-gnu). This project ships
three more, and each one rejected something the dev build accepted.

### 8a. `gcc@15.1.0-musl` — a range pipeline in an implementation unit

`x86_64-linux-musl` (then `gcc@15.1.0-musl`) rejected `catalog.cpp`:

```
use of 'constexpr auto std::ranges::views::__adaptor::operator|(...)'
before deduction of 'auto'
```

The body is byte-identical to main's. The same `std::views::transform` pipeline
compiles in an interface unit and not in an implementation unit — under that
compiler. `mcpp build --target x86_64-linux-musl` reproduces it locally: one
error, one file.

**Fixed by moving the musl target to `gcc@16.1.0-musl`** rather than by keeping
the function in the interface. The reverse has bitten this project before:
`views::split | ranges::to` compiled under 15.1.0-musl and made a whole module
fail with "Bad file data" under 16.1.0, blaming an unmodified TU
(`.agents/docs/2026-08-06-subos-architecture-proposal.md` §590). Range adaptors
in modules are fragile in **both** directions across those two versions, so the
two targets now share one compiler major instead of trading one breakage for the
other.

A stale object file hid this on the dev toolchain too: several regeneration
rounds cleared only `target/*/gcm.cache` and kept the `.o` files, so
`catalog.o` was never recompiled. Before believing a green build: a cold
`rm -rf target`.

### 8b. `llvm@20.1.7` — the stream-less `std::print` in an interface template

macOS failed on the phase-2 push having passed on phase 1. Every TU
instantiating a zero-argument `log::` template died inside libc++'s `<print>`:

```
call to deleted constructor of
  'formatter<basic_format_string<char, basic_string<char>>, char>'
```

That is `std::print` no longer picking its `FILE*` overload and deducing `stdout`
**as** the format string. libc++ implements `print(fmt, args...)` as
`print(stdout, fmt, args...)`, so the `FILE*` overload has to win overload
resolution at the point of instantiation — and for a template that stays in the
interface, that point is in the **importer**. Once enough bodies leave the
interface, clang 20 stops picking it.

Two rules fix it, and neither needs a per-entity exclusion:

- **11 call sites across 4 interfaces name their stream** — `core/log.cppm` (8)
  and one each in `platform/{linux,macos,windows}.cppm`. `[print.fun]` defines the
  two-argument form *as* `print(stdout, ...)`, so behaviour is identical and the
  deduction is gone. Bodies that move to an implementation unit are left alone:
  nothing instantiates those from another TU. (The commit message says 23 sites
  in 6 files; that was the count of stream-less prints in the *pre-split*
  `.cppm` set, and 12 of them belong to bodies that then moved to a `.cpp` and
  were never normalised. 11 in 4 is what the tree carries.)
- **8 implementation units gained `<cstdio>`** — `config.cpp`, `cmdprocessor.cpp`,
  `xim/downloader.cpp`, `platform.cpp` and the four platform partitions — because
  a body that ends up there can name `stdout`/`stderr`/`FILE` while its interface
  has no global module fragment at all. `config.cppm` has none, and
  `Config::print_paths()` lands in `config.cpp`.

**Two wrong fixes came first, and both are reverted rather than left in the
tree.** The first restored three declarations to `log.cppm` on the theory that
interface reachability drove it — macOS then failed again, identically, which
disproved it. The second pinned in-class bodies containing a stream-less print
as "instantiation anchors"; it worked, but it was treating the symptom, and once
the streams were named it was unnecessary.

**The real lesson is the loop, not the bug.** `llvm@20.1.7` on **Linux**
reproduces the macOS failure exactly — main builds clean, the split does not — so
this was diagnosable in 40 seconds all along, and three CI cycles were spent
before trying it. A *minimal* probe of the same shape does **not** reproduce it;
it needs the whole project, which is what made the failure look
macOS-libc-specific. `.agents/tools/module-split/clang-variant.sh` builds any variant with clang
in an isolated copy; `clang-bisect.sh` binary-searches one file's members.

### 8c. My own bug, for the record

`open(f, 'w').write(ensure_cstdio(open(f).read()))` truncates the file before the
read runs, so every implementation unit taking that path came out **empty**. It
surfaced as undefined symbols at link time — never as an error in the file that
was emptied.

## 9. The gate this work should have started with

Four compilers, and each one caught something the others accepted. All four are
now runnable locally, and the first three take under a minute each:

```bash
rm -rf target && mcpp build                        # gcc@16.1.0   (dev)
mcpp build --target x86_64-linux-musl              # gcc@16.1.0-musl (release)
bash .agents/tools/module-split/clang-variant.sh check --all      # llvm@20.1.7  (macOS family)
python3 .agents/tools/module-split/export-surface.py
```

The cold `rm -rf target` matters as much as the extra compilers: a kept object
file hid §8a on the dev toolchain.

Windows (`llvm@20.1.7`, MSVC-flavoured) and real macOS remain CI-only.

## 10. What this report does not verify

- **728 lines of the moved code are never compiled by a Linux build** —
  `platform/windows.cpp` (326), `platform/macos.cpp` (151), `platform.cpp` (65),
  `xself/uninstall.cpp` (50) and a long tail. The macOS and Windows CI jobs are
  the gate for those, and both pass.

  An earlier draft of this report said 1,228. That counter flagged any block whose
  condition merely *mentioned* `_WIN32` or `__APPLE__`, which wrongly included
  `#if !defined(_WIN32)` — true on Linux, and 208 of the miscounted lines were
  `platform/unix.cpp`'s POSIX code that Linux does compile. The figure above comes
  from evaluating the conditions with `__linux__` defined and the others not.
- Runtime behaviour beyond the unit suite: the e2e block needs a release tarball
  and network access, and runs in CI (green).
- Release-mode (`-O2`) runtime performance. Dropping implicit `inline` costs
  cross-TU inlining without LTO; binary size is reported, a runtime comparison is
  not.

## 11. An mcpp observation

`mcpp` warns `module ':part' imported but not provided in this build` for a
partition import inside an implementation unit. The warning is cosmetic — the
generated dyndep edges are correct (`obj/part.o: dyndep | p4.m-part.gcm
p4.m.gcm`), verified on a throwaway project — but its check pass does not expand
the bare `:part` the way its dependency scanner does. The generated
implementation units avoid the warning by relying on the primary interface's
`export import :part;` rather than importing the partition directly.

## 12. What self-review caught that no compiler would

Everything below built green before it was found. That is the point: a green
build is not evidence that a refactor is complete or that a report is true.

| finding | how it surfaced |
|---|---|
| **153 of 554 interface import edges (28%) named nothing in the interface** — split.py copied the import list instead of moving it, so the interface kept depending on modules only the moved bodies used | reading the diff and asking what the interface still needs |
| **`cold.sh` never ran from where it was committed** — its root was `dirname/../..`, right in `build/bench` and pointing at `.agents/` from `.agents/tools/module-split/`. Broken since this branch's first commit, and cited by this report | running the documented command |
| **This report cited two scripts that are not in the repo** — `clang_variant.sh` and `measure_side.sh` lived under `build/`, which is gitignored, so the local gate could not be run by anyone reading it | checking that every path in the report exists |
| **The unverified surface was overstated by 69%** — 1,228 lines "behind platform guards" is really 728; the counter flagged any block mentioning `_WIN32`/`__APPLE__`, including `#if !defined(_WIN32)`, true on Linux | re-deriving a number instead of trusting the first script that produced it |
| **The control-probe claim was backwards** — the report said it "must show no improvement". It improves 2.5×, because the interfaces it still rebuilds are smaller | comparing the claim against the measurement |
| **A closure bug in the new tool** — `names \|= …` makes `names` local to the nested function | smoke-testing the tool against `main`, where it correctly finds only 4% to drop |
| **Tool files were 644 while every other tool in `.agents/tools` is 755** | listing the mode bits |

Four things the review checked and found clean, each stated as a number rather
than an impression:

| invariant | result |
|---|---|
| function definitions | **1,262 → 1,262**, none lost, none duplicated |
| exported identifiers | **961 → 961**, 0 lost, 0 gained |
| module names | 110, byte-identical |
| dynamic-initialisation order | one module has globals on both sides of the split, and the one that stayed is `std::atomic<bool>{false}` — constant-initialised, so there is no order to preserve |
| reconstructed `#if` guards | both verified against their originals; macOS and Windows CI confirm |
