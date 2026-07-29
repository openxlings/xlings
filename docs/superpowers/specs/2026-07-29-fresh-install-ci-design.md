# Fresh-install CI — design

> 2026-07-29 | status: approved

## Problem

Every existing xlings workflow builds xlings from the PR and tests *that* binary
against a warm, cached environment. Nothing verifies the path a real first-time
user takes: land on a clean machine, run `quick_install`, and use the tool.

That path has its own failure modes, none of which the current matrix can see:

- the published release artifact is broken or missing for a platform
- `quick_install.sh` / `.ps1` regresses against a bare host
- the bundled package index cannot resolve a package on a cold home
- a package installs but its shim does not resolve
- a version switch silently no-ops
- the static-musl binary fails to start on an old-glibc host

Modelled on `mcpp-community/mcpp`'s `ci-fresh-install.yml`.

## Scope

Verify **released** xlings, floating at latest — the exact thing a new user
gets — from bootstrap through the mainstream feature set, on a cold machine
with no caches, on Linux / CentOS 7 / Windows / macOS.

Explicitly out of scope: building xlings from source (covered by
`xlings-ci-*.yml`), and the E2E suite (covered by `xlings-ci-linux-e2e.yml`).

## Structure

The test body lives in the repo as scripts rather than inline YAML. The CentOS 7
leg executes inside `docker run`, where a mounted script beats escaping a long
heredoc through two shell layers; it also makes local reproduction a single
command.

```
tests/fresh-install/
  lib.sh      # assertion helpers (POSIX bash)
  smoke.sh    # Linux / CentOS 7 / macOS body
  lib.ps1     # assertion helpers (PowerShell)
  smoke.ps1   # Windows body
.github/workflows/xlings-ci-fresh-install.yml
```

Each script takes one argument — the suite name — so a red cell names the
subsystem instead of "fresh-install is broken":

```bash
bash tests/fresh-install/smoke.sh core   # lifecycle + mcpp version switch
bash tests/fresh-install/smoke.sh gcc    # gcc release-group switch
bash tests/fresh-install/smoke.sh llvm   # llvm version switch
```

The scripts bootstrap xlings themselves (they run `quick_install` as their first
phase) rather than relying on the workflow to do it. That keeps the Linux and
CentOS 7 legs byte-identical and lets the whole thing run under `docker run`
with no `$GITHUB_PATH` involvement.

## Job matrix

`suite` × platform, each cell doing its **own** fresh install — no shared warm
state, which is the entire point. Cells run in parallel, so wall-clock is one
bootstrap plus one toolchain, not the sum.

| Platform | Runner | Suites |
| --- | --- | --- |
| Linux | `ubuntu-24.04` | core, gcc, llvm |
| CentOS 7 | `ubuntu-24.04` + `docker run centos:7` | core, gcc, llvm |
| Windows | `windows-latest` | core, gcc\*, llvm |
| macOS | `macos-14` | core, llvm |

Two constraints come from the package index, not from a choice:

- **`gcc` on Windows has exactly one version** (`15.1.0`, via mingw64). That
  cell therefore cannot test a *switch*; it installs 15.1.0 and asserts the
  group members are present and mutually consistent — group registration, not
  group switching. Real switching is Linux/CentOS 7, where `9.4.0` … `16.1.0`
  are all available.
- **`gcc` does not exist on macOS** in the index, so macOS has no gcc cell.

## Suites

### `core` — lifecycle, with the multi-version switch on `mcpp`

```
quick_install (no version arg → floating latest)
xlings --version                        → non-empty
xlings config --mirror GLOBAL
xlings update                           → index refresh on a cold home
xlings search mcpp                      → finds it
xlings install ninja -y -g              → ninja --version == 1.12.1
xlings install mcpp@<A> mcpp@<B> -y -g
xlings use mcpp@<A>                     → mcpp --version reports A
xlings use mcpp@<B>                     → mcpp --version reports B   (must CHANGE)
xlings list                             → contains ninja and mcpp
<tmpdir>/.xlings.json + xlings -y install
                                        → .xlings/subos/_/bin/ninja resolves
xlings self doctor                      → exit 0
xlings self uninstall -y                → $XLINGS_HOME gone
```

`ninja` is the install-and-run probe because it is tiny, statically linked, and
present on all four platforms. `mcpp` is the version-switch probe because it
ships many versions on all four platforms.

### `gcc` — release-group switch (Linux / CentOS 7)

`gcc` registers a *group* of shims — `gcc`, `g++`, `c++`, `cpp`, `gcc-ar`,
`gcov`, … — and `xlings use` is specified to move the whole group together.

```
xlings install gcc@15.1.0 gcc@16.1.0 -y -g
xlings use gcc@15.1.0   → gcc AND g++ AND c++ AND cpp all report 15.1.0
xlings use gcc@16.1.0   → all four report 16.1.0
g++ hello.cpp && ./a.out                → the switched toolchain actually compiles
```

Asserting all four members move *together* is the point. A group switch that
moves `gcc` but strands `g++` on the previous version passes any check that only
looks at `gcc --version`.

On Windows the same suite degrades to a presence-and-consistency check, since
only one version exists.

### `llvm` — version switch (all platforms)

Same shape on `20.1.7 ↔ 22.1.8`, asserting `clang` and `clang++` move in sync,
then compiling and running a **C** file with the switched clang. C rather than
C++ on purpose: that asserts the driver and its runtime work end to end without
also betting on libc++ wiring, which is a separate concern from the version
switch under test.

**Known red as of 2026-07-29.** On a fresh home this suite fails on Linux, and
correctly so — see "First finding" below.

## Differential assertions

Every switch assertion captures the reported version **before and after** and
fails if it did not move. This is deliberate: xlings has a recurring bug class
where "never happened" and "succeeded" produce identical output, and a `use`
that silently no-ops would otherwise pass a check that merely confirms
`mcpp --version` still runs.

`assert_switch` therefore takes the expected version *and* refuses a result
equal to the pre-switch value.

## CentOS 7 mechanics

`docker run` inside a normal `ubuntu-24.04` job. A `container:` job cannot work:
`actions/checkout`, `actions/cache` and every other JS action need Node 20,
which needs glibc 2.28; CentOS 7 ships 2.17.

So JS actions run on the host, the repo is bind-mounted, and only the smoke
script runs in the container. Three fixups that leg needs and no other does:

1. **Dead yum mirrors.** CentOS 7 is EOL and `mirrorlist.centos.org` is gone;
   the repo files are rewritten to `vault.centos.org` before any `yum install`.
2. **Stale CA bundle.** `ca-certificates` must be reinstalled and
   `update-ca-trust` run, or TLS against the GitHub asset CDN fails.
3. **`quick_install.sh` prerequisites.** It hard-requires `curl` and `tar`, and
   locates the extracted directory with a glob; `which`, `findutils`, `gzip`,
   `xz` and `git` are installed explicitly rather than assumed.

This leg is load-bearing, not decorative. The xlings Linux binary is
static-musl so it *should* start on glibc 2.17, and gcc/llvm pull in a
self-contained `xim:glibc@2.39` rather than the host's — but whether that whole
stack works on a 2.17 host is unproven today.

## Triggers

Two stages, as requested. **Both are now done.**

**Stage 1** — `pull_request` was included so the workflow could be validated
before it was trusted. Four runs on #446 exercised all four platforms.

**Stage 2** — the `pull_request:` block is removed. Final state:

```yaml
on:
  workflow_dispatch:
  workflow_run:
    workflows: [release]
    types: [completed]
  schedule:
    - cron: '0 6 * * *'
```

Removing it is not only the agreed sequence, it is the right steady state. The
workflow tests the *released* binary, so on a PR it validates the workflow
rather than the change — near-zero signal, at the cost of four platforms and
eleven cold toolchain installs. And the defects it currently reports live in
xim-pkgindex; gating unrelated PRs on another repository's release state would
train people to ignore it.

`release: published` is deliberately **not** used. `release.yml` creates the
release with `GITHUB_TOKEN`, and GitHub suppresses workflow triggers from
`GITHUB_TOKEN`-generated events, so that trigger would never fire from the
pipeline. `workflow_run` is a platform-generated event, exempt from the
suppression, and needs no cross-repo PAT.

## Deliberate omission: no `wait-index` job

mcpp's workflow polls xim-pkgindex until it carries the just-released version,
because mcpp pins an exact version and would otherwise fail with
`version not found`.

We install floating-latest, so the same race degrades differently: a
post-release run that beats the index bump re-tests the *previous* release. That
is a weak result, not a red build — so the added complexity is not yet paid for.
Worth revisiting if the post-release run needs to be a hard gate on the new
version, which would also mean introducing a pin to bump each release.

## Results, and where each defect belongs

| Cell | Linux | CentOS 7 | Windows | macOS |
| --- | --- | --- | --- | --- |
| `core` | pass | pass | pass | pass |
| `gcc` | pass | pass | **fail** | n/a |
| `llvm` | **fail** | **fail** | **fail** | **fail** |

Every failure was traced to a specific layer and filed:

| Defect | Layer | Issue |
| --- | --- | --- |
| `mingw-w64` config hook uses xvm node kind `binding`; xlings accepts only `program`/`lib`/`group`/`files`, so the hook aborts and **no C++ compiler is obtainable on Windows at all** | xim-pkgindex | [#442](https://github.com/openxlings/xim-pkgindex/issues/442) |
| `gcc.lua`'s windows entry declares no payload and no deps despite its "deps mingw64" comment | xim-pkgindex | [#442](https://github.com/openxlings/xim-pkgindex/issues/442) (comment) |
| `llvm@22.1.8` does not carry its C++ runtime — missing `libstdc++.so.6` on Linux/CentOS 7, `__ZdaPv` on macOS | xim-pkgindex | [#443](https://github.com/openxlings/xim-pkgindex/issues/443) |
| `llvm`'s `collect_bin_apps` registers zero programs on Windows although `clang.exe` is on disk, and logs nothing | xim-pkgindex | [#444](https://github.com/openxlings/xim-pkgindex/issues/444) |
| `install` prints `✓ N package(s) installed` without checking the package's own declared `programs` registered | **xlings** | [#447](https://github.com/openxlings/xlings/issues/447) |

Only the last is an xlings defect, and it is the one that turns the other four
from "install failed, here is the missing program" into "everything said OK and
nothing works".

`core` green on all four platforms is the headline: bootstrap, index refresh,
install, run, multi-version switch, project mode, doctor and uninstall all work
from cold everywhere — **including CentOS 7**, which also passes the full gcc
release-group switch on glibc 2.17. That was the leg most likely to be
unsupportable, and it is not.

Every `llvm` cell fails, on all four platforms, for three distinct reasons.
None of them is a test artefact.

### Finding 1 — `llvm@22.1.8` cannot start on Linux / CentOS 7

```
$ xlings install llvm@22.1.8 -y -g && clang --version
.../xpkgs/xim-x-llvm/22.1.8/bin/clang: error while loading shared libraries:
libstdc++.so.6: cannot open shared object file: No such file or directory
```

Root cause, confirmed against the installed payloads:

- `clang-20` (20.1.7) does **not** link `libstdc++.so.6` — its C++ runtime is
  static, so it runs anywhere.
- `clang-22` (22.1.8) **does** link `libstdc++.so.6`.
- Both get the same baked RPATH — llvm's own `lib`, plus `glibc`, `zlib`,
  `libxml2`, and `subos/default/lib` — and **none of them ships libstdc++**.
- `llvm.lua`'s linux `deps` (`xim:glibc`, `xim:linux-headers`, `xim:zlib`,
  `xim:libxml2`) never gained `xim:gcc-runtime` when the 22.1.8 payload started
  linking libstdc++ dynamically.

Installing `gcc-runtime` afterwards does **not** repair it: the RPATH is baked
at llvm install time, so a later arrival is invisible to the loader. The fix
belongs in `xim-pkgindex`'s `llvm.lua` — declare the dep so its lib directory is
in the RPATH from the start.

Impact is wider than this test: `latest` resolves to 22.1.8, so plain
`xlings install llvm` is broken on any machine that does not already happen to
have a libstdc++ around. That is why it went unnoticed — dev machines and CI
images all have gcc installed.

### Finding 2 — `llvm@22.1.8` cannot link on macOS

The version switch itself passes (`clang` and `clang++` both move to 22.1.8),
then compilation dies at the linker:

```
dyld[2263]: Symbol not found: __ZdaPv
```

`__ZdaPv` is `operator delete[](void*)` — the same shape of defect as finding 1,
a C++ runtime the 22.1.8 payload expects and does not carry, surfacing on macOS
as a missing libc++/libc++abi rather than a missing libstdc++.

### Finding 3 — `xlings install llvm` registers no `clang` on Windows

```
$ xlings install llvm@20.1.7 -y -g
  ✓ 1 package(s) installed
$ xlings use llvm@20.1.7
[xlings] llvm -> 20.1.7
$ clang --version
[error] xlings: 'clang' is not installed
```

Install reports success, `use` reports success, and there is no compiler.
Affects both 20.1.7 and 22.1.8. The failure-state dump proves the payload
landed correctly:

```
...\xim-x-llvm\20.1.7\bin\clang.exe
...\xim-x-llvm\20.1.7\bin\clang++.exe
...\xim-x-llvm\20.1.7\bin\lld-link.exe
```

So this is registration, not extraction. `xvm.add(package.name)` — `config()`'s
first line — clearly worked, since `use` resolves. Everything after it
registered nothing, and **nothing was logged**: no error, not even the
`skip xvm add alias (not found)` warning the recipe emits per missing alias.

`llvm.lua` populates the per-program shims by shelling out —
`io.popen('dir /b "<bindir>"')` on Windows, `ls -1` on Linux, where it works.
A likely contributor is that libxpkg's `path.join` hardcodes `sep = "/"`
regardless of host, so `bindir` is `C:\...\20.1.7/bin` and `cmd.exe`'s `dir`
is unreliable with forward slashes. That mechanism is a lead, not a conclusion;
the confirmed facts are the four above. If it is `path.join`, the fix belongs in
libxpkg rather than the index.

This is the silent-success pattern in its purest form: two consecutive success
messages and a completely unusable install.

### Finding 4 — the Windows `gcc` cell: one test bug hiding one real bug

The first run failed with `'gcc' not found in version database`, which was
**this suite's** bug: `gcc.lua`'s `config()` returns early on Windows
("config in mingw-w64.lua") and its lone windows entry declares no payload, so
**`mingw-w64` is the package that registers the gcc/g++/c++ shims**. The suite
now installs `mingw-w64` there, and asserts group *consistency* rather than a
switch since only one version exists.

The corrected test then failed on a real defect:

```
$ xlings install mingw-w64@13.0.0 -y -g
[error] unsupported registration node kind 'binding'
[error] [mingw-w64] failed: config hook failed
```

`mingw-w64.lua` registers an umbrella placeholder with `type = "binding"`, and
`registration.cppm` accepts only `program` / `lib` / `group` / `files` — on
`main` too, so this is not release lag. The whole hook aborts, taking the
earlier `gcc`/`g++`/`c++` registrations with it.

Net effect: **there is currently no working way to get a C++ compiler on
Windows through xlings.** `gcc` installs nothing, `mingw-w64` aborts, and `llvm`
registers no `clang`.

That is worth stating plainly as the single most valuable thing this CI found,
and it was invisible to every existing workflow.

### Why the llvm suite is not being softened

A test adjusted until it passes against a broken package is exactly the
silent-success pattern this repo keeps getting bitten by. Findings 1–3 are real
user-facing breakage in the current release; the suite reports them, and the
`llvm` cells stay red until the recipes are fixed in `xim-pkgindex`.

## Failure modes this cannot catch

Stated so the green check is not over-read:

- CN mirror paths — every leg forces `GLOBAL`, since gitee/gitcode endpoints are
  not reachable from GitHub runners.
- aarch64 Linux and Windows ARM — no runners in the matrix.
- Anything about the PR's own code. This workflow tests the *released* binary;
  on a PR it therefore validates the workflow itself, not the change.
