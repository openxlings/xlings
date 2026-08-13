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

Three things left the interface:

1. **Namespace-scope implementations** — 81 files, 779 entities, 26,606 body lines.
2. **Class member bodies** — 29 files, 286 members, 3,845 body lines. A body
   defined inside a class is implicitly `inline`, so it stayed in the BMI.
3. **Module-private declarations** — a non-exported helper's declaration stays in
   the interface only if something still *there* names it (an exported template,
   an `inline`/`constexpr` body, a class member body). Otherwise it belongs in
   the implementation unit: in the interface it sits in the BMI, so changing its
   signature would recompile every importer for nothing.

### Interface surface

| | lines | share of original |
|---|---|---|
| interface before | 46,253 | 100% |
| interface after | **14,819** | **32%** |
| implementation after | 35,112 | |

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
refs, so it must show no improvement. If it does, the measurement is noise
rather than an effect.

Harness: `.agents/tools/module-split/{cold.sh,incr.py}`, driver
`build/bench/run_compare.sh`.

## 4. Measurements

*(filled in below from the runs)*

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

## 7. What this run does not verify

- **1,228 lines of the moved code sit behind `_WIN32` / `__APPLE__` guards** that
  a Linux build never compiles — `platform/windows.cpp` (330),
  `platform/macos.cpp` (181), `subos/sandbox.cpp` (121) and others. The macOS
  and Windows CI jobs are the gate for those, not this report.
- The release (musl static, `gcc@15.1.0-musl`) toolchain is a second compiler
  this branch has not been through locally.
- Runtime behaviour beyond the unit suite: the e2e block needs a release tarball
  and network access.

## 8. An mcpp observation

`mcpp` warns `module ':part' imported but not provided in this build` for a
partition import inside an implementation unit. The warning is cosmetic — the
generated dyndep edges are correct (`obj/part.o: dyndep | p4.m-part.gcm
p4.m.gcm`), verified on a throwaway project — but its check pass does not expand
the bare `:part` the way its dependency scanner does. The generated
implementation units avoid the warning by relying on the primary interface's
`export import :part;` rather than importing the partition directly.
