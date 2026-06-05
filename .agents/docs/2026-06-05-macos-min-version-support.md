# macOS Minimum-Version Support for xlings + mcpp Releases

> Date: 2026-06-05
> Status: **SHIPPED & VERIFIED** — mcpp v0.0.50 + xlings v0.4.50 released
> (floor 14.0, declared-floor-implies-static), final verification green on
> macos-14 and macos-15 against the real release artifacts (§6);
> deferred work tracked in §5
> Goal: macosx-arm64 release binaries should run on as many macOS versions
> as possible — floor target **macOS 11.0** (first Apple Silicon release),
> which covers macOS 14 along the way. Rollout order: **xlings first**
> (mcpp depends on xlings at runtime and bundles it).

## 1. Research findings

### 1.1 Why current releases require macOS 15

Parsed from the released artifacts (mcpp 0.0.49 / xlings 0.4.49
`macosx-arm64`):

```
LC_BUILD_VERSION platform=macos minos=15.0 sdk=15.0
LC_LOAD_DYLIB    /usr/lib/libc++.1.dylib
LC_LOAD_DYLIB    /usr/lib/libSystem.B.dylib
```

- `minos=15.0` → dyld on macOS 14 (or older) **refuses to load** the
  binary. Root cause: both release jobs run on `macos-15` runners with no
  deployment target set, so clang takes the SDK default.
- Chain effect: since the xlings macOS binary is minos 15, mcpp cannot
  even bootstrap on macOS 14 (it copies the xlings binary into its
  registry sandbox).

### 1.2 The two knobs that decide the floor

1. **Deployment target** — clang (driver + lld) honors the
   `MACOSX_DEPLOYMENT_TARGET` env var (or `-mmacosx-version-min=`);
   mcpp's flag builder (`src/build/flags.cppm`) does no deployment-target
   handling of its own, so the env var passes straight through to every
   compile/link. Setting it in the release workflow is sufficient
   mechanically.
2. **C++ runtime linkage** — the binaries currently link the **system**
   `/usr/lib/libc++.1.dylib`. That caps how low the target can go:
   LLVM-20-era C++23 surface (`std::println`, `std::print` terminal
   checks, newer `to_chars`, …) maps to availability attributes that
   reject (at compile time) or trap (at run time, missing dylib symbols)
   on older system libc++. Lowering minos alone is therefore expected to
   fail or be fragile below ~15 — **the robust route is to statically
   link LLVM 20's own `libc++.a` + `libc++abi.a`**, removing the system
   libc++ dependency entirely (deps shrink to libSystem only, which is
   stable back past 11.0).

### 1.3 Static stdlib is ALREADY the declared default — just not implemented for clang

`mcpp` manifest: `BuildConfig.staticStdlib = true` (default). But
`flags.cppm:276`:

```cpp
std::string static_stdlib =
    (f.staticStdlib && !isClang && !mcpp::platform::is_windows)
        ? " -static-libstdc++" : "";
```

The `!isClang` guard silently skips the entire macOS route (macOS
toolchain is `llvm@20.1.7`). So the product-level fix is **completing the
missing clang implementation** of an existing manifest semantic, not
adding a new knob:

> macOS + clang-with-cfg + `staticStdlib`: replace `-stdlib=libc++` at
> link time with `-nostdlib++ <llvmroot>/lib/libc++.a
> <llvmroot>/lib/libc++abi.a` when the archives exist (fall back to the
> current dynamic link otherwise). Compile side keeps `-stdlib=libc++`
> (LLVM headers).

Supporting facts:

- The macOS `xim:llvm@20.1.7` payload is the official
  `LLVM-20.1.7-macOS-ARM64.tar.xz` bundle — official Apple-Darwin LLVM
  bundles ship `lib/libc++.a` / `libc++abi.a` (CI experiment asserts
  this).
- `mcpp.toml [build] ldflags` exists (and `[profile.*] ldflags`), so the
  CI experiment can inject the static archives **without any mcpp code
  change** — the released bootstrap mcpp (0.0.49) already honors
  manifest ldflags. This also gives the release workflows a working
  recipe before the flags.cppm change ships.
- Known risk, already encountered upstream: static libc++ on macOS can
  SIGABRT during static destruction. xlings's `main.cpp` already guards
  with `std::_Exit(rc)` on `__APPLE__`; **mcpp's `main.cpp` has no such
  guard** — to be added if the experiment reproduces the abort.

### 1.4 Runner / verification constraints

- GitHub hosted runners: `macos-15` (current), `macos-14` (available,
  ARM64), `macos-26` — nothing older for ARM64 (macos-13 was the last
  x86_64 image). So CI can *execute-verify* on 14/15 only; support below
  14 is by-construction (static libc++ + libSystem floor + minos) and
  documented as best-effort.
- Floor choice **11.0**: first Apple Silicon macOS; the products only
  ship arm64, so nothing older can exist. LLVM 20 libc++ supports far
  older targets than that.

## 2. CI experiments (mcpp PR #115, temp branch `test/macos14-support`)

| Run | Config | Question it answers |
| --- | --- | --- |
| A | `MACOSX_DEPLOYMENT_TARGET=14.0`, dynamic system libc++ (status quo linkage) | Does availability gating even allow target 14 with the current linkage? (expected: marginal/fails for some C++23 surface) |
| B | `MACOSX_DEPLOYMENT_TARGET=11.0` + injected static `libc++.a`/`libc++abi.a` via `[build] ldflags` | The proposed end state: minos=11.0, no libc++ dylib dep, runs e2e on macos-14 + macos-15 |

Both runs: build mcpp@HEAD and xlings@main on `macos-15`, assert
`LC_BUILD_VERSION minos`, assert `LC_LOAD_DYLIB` set, smoke on 15, then
on `macos-14`: control-check the released minos15 binary is refused by
dyld, run the new binaries `--version`, assemble an xlings home (release
layout + swapped binary), and run `mcpp new/build/run` end-to-end —
which also exercises the `xim:llvm` / `xim:ninja` toolchain payloads on
macOS 14.

Results: (updated as runs complete)

- **Run A — CONFIRMED the dynamic-libc++ ceiling.** Build job passed
  (availability gating did NOT reject target 14 at compile time;
  minos=14.0 asserted; smoke on macos-15 OK). But on macos-14 (14.8.7)
  both binaries die at launch:
  ```
  dyld: Symbol not found: __ZNSt3__119__is_posix_terminalEP7__sFILE
    Expected in: /usr/lib/libc++.1.dylib        → Abort trap: 6
  ```
  `__is_posix_terminal` is the `std::print` support symbol added in
  LLVM 18-era libc++ — macOS 14's system libc++ predates it. So the
  availability annotations under-report (compile passes, launch dies):
  lowering minos alone is NOT viable. Control check also confirmed the
  released 0.0.49 (minos 15) is refused by dyld with the same missing
  symbol + "built for macOS 15.0 which is newer than running OS".
- **Run B (iterations B2–B5) — the shipping route works end-to-end.**
  Findings along the way, each now part of the mcpp 0.0.50 change set:
  - `[build] ldflags` injection CANNOT produce the static link: mcpp's
    macOS link path hardcodes ` -lc++` BEFORE user ldflags
    (`flags.cppm`, `needs_explicit_libcxx` branch), and that resolves to
    the system dylib regardless of `-nostdlib++`/staged `-L` dirs in the
    user flags (B2/B3). → product fix: implement `staticStdlib` natively
    in that branch; release self-build uses a TWO-STAGE self-host
    (stage 1 bootstrap-built dynamic → stage 2 rebuilds itself static).
  - mcpp's BMI fingerprint did not include the deployment target: a
    cached `std.pcm` built for `arm64-apple-macosx14` poisons a
    `macosx15` build with a module config-mismatch error (B initial).
    → product fix: fold `MACOSX_DEPLOYMENT_TARGET` into the canonical
    fingerprint flags + mirror it onto compile/link command lines.
  - With static libc++ + minos 11.0 in place (B4), the macos-14 e2e
    still failed: Xcode 15.4's system `ld` (invoked by LLVM clang for
    the user project's link) aborted at launch with its libc++
    resolution diverted to the LLVM payload's `libc++.1.0.dylib`
    (missing `__ZdaPv`). The job env carries no DYLD vars (B5
    forensics), so the diversion arises somewhere inside the
    build-chain; → product fix that sidesteps the entire class: macOS
    links via `-fuse-ld=lld` (same as the Linux clang path) — the
    linker now ships with the exact toolchain doing the compile and the
    host-Xcode dependency disappears.
  - **B5: all green.** macos-15 two-stage self-host: mcpp + xlings both
    `minos 11.0`, no `libc++` in `LC_LOAD_DYLIB`, `--version` clean (no
    static-dtor abort; the `_Exit` guards hold). macos-14: released
    minos-15 binary refused (control), minos-11 binaries start, xlings
    home assembles, and `mcpp new/build/run` works end-to-end —
    including installing the `xim:llvm`/`xim:ninja` payloads on
    macOS 14 and linking the user binary with lld (built natively:
    minos 14.0 = host SDK default, expected for user projects).

## 3. Rollout plan (updated after run B: tool-dependency order)

The original plan injected ldflags so xlings could release first; runs
B2/B3 proved injection cannot beat the bootstrap's hardcoded `-lc++`, so
the static link can only come from the mcpp **doing the build**. That
forces the tool order (mcpp-as-tool first), while still delivering the
xlings binary support in the same wave:

1. **mcpp 0.0.50** — flags.cppm staticStdlib (clang/macOS) + lld link +
   deployment-target explicit flags + fingerprint coverage + `_Exit`
   guard; release workflow macOS job: `MACOSX_DEPLOYMENT_TARGET=11.0` +
   two-stage self-host with minos/no-dylib assertions. Sync mirrors +
   index (latest → 0.0.50).
2. **xlings 0.4.50** — release workflow macOS job only needs
   `MACOSX_DEPLOYMENT_TARGET=11.0`: `xlings install -y` picks mcpp
   0.0.50 from the index, whose flags produce the static minos-11 binary
   natively. (xlings's `_Exit` guard already exists.) Sync mirrors +
   index.
3. Verify: temp PR #115 workflow re-pointed at the released artifacts —
   macos-14 control + e2e — then close the temp PR.
4. Follow-ups: macos-14 CI lane in both repos' regular CI (optional);
   investigate the build-chain DYLD diversion seen in B4 (mitigated by
   lld, not root-caused).

## 4. Open questions

- Does run A fail at compile time (availability) or produce a binary
  that traps on 14? (Either way run B is the shipping route; A is the
  evidence.)
- Does the official LLVM macOS bundle's `libc++.a` build configuration
  (assertions/hardening) match what we ship today? (Check size/symbols
  in run B.)
- `mcpp run`-built **user** binaries on macOS keep the host default
  target unless the user sets `MACOSX_DEPLOYMENT_TARGET` — document
  this; consider a `[build] macos_deployment_target` manifest field as a
  follow-up.


## 4. Implementation evolution (what the CI iterations taught us)

The in-PR iterations after run B5 reshaped the design; each lesson is
recorded here because the failure modes are non-obvious:

1. **lld + `-Wl,-hidden-l` is a trap.** ld64's `-hidden-l` links the
   archive with hidden visibility — but lld resolves it like a plain
   `-l` and picks the SIBLING DYLIB in the same directory. Binaries
   carried `@rpath/libc++.1.dylib` with no rpath and died at load
   (`dyld: Library not loaded` → abort). Diagnosed with a 3-mode
   forensics matrix (dylib / direct-archive / hidden-l probe built with
   bare clang++, run + `otool -L` + exit code each). **Link archives by
   path** (the `LLVM_STATIC_LINK_CXX_STDLIB` form) — verified clean.
2. **The official LLVM darwin archives set the real floor: 14.0.**
   `ld64.lld: libc++.a(...) has version 14.0.0, newer than target
   minimum of 11.0.0`. Claiming 11.0 with these archives would be
   false advertising; the floor is now 14.0 everywhere (still fully
   covering the original macOS 14 goal).
3. **Per-unit stdlib linkage.** Statically linked test binaries
   (gtest's main has no `_Exit` guard) and the global-static experiment
   broke `mcpp test`; the link strategy is now per link-unit:
   distributable targets (Binary/SharedLibrary) static, TestBinary
   dynamic `-lc++` (tests only run on the build host — same stance as
   cargo test).
4. **Declared-floor-implies-static (the shipping semantic).** Static
   libc++ engages only when a deployment floor is explicitly declared
   (env var or `[build] macos_deployment_target`): the declaration is
   what static linking exists to honor. No declaration = dynamic system
   libc++, byte-for-byte the 0.0.49 behavior (zero regression), which
   also sidesteps deferred issue #1 below. Release pipelines declare
   14.0, so shipped mcpp/xlings binaries are static + portable.
5. **Forensics methodology.** Three rounds of log-level guessing were
   beaten by one minimal-probe matrix step in CI. When a macOS-only
   failure has no local repro, ship a bare-toolchain probe that
   enumerates the hypothesis space in a single round.

Landing the xlings side (PR #319, four CI rounds) added three more:

6. **Version pins live in three layers; bump ALL of them.** Each
   workflow carries its own `XIM_PKGINDEX_REF` + `BOOTSTRAP_XLINGS_VERSION`
   (release.yml was bumped first; ci-macos kept its stale pin → R1),
   and `.xlings.json` pins the *workspace* mcpp version independently
   of the index (a hidden third pin, found in R2 still at 0.0.36).
   Grep every workflow + every `*.json` manifest for the old version
   before declaring a bump complete.
7. **mcpp ≥0.0.47 injects a runtime lib path into test child
   processes**, which poisons host tools spawned by tests (`tar`,
   `zip`, `python`): they resolve the toolchain's libs instead of the
   system's and fail in non-obvious ways (`Extract.*` failures on
   linux, R2). Fixtures that shell out to host tools must scrub the
   injected env (`env -u LD_LIBRARY_PATH`) — done via a single
   `host_sys_()` wrapper in the unit-test helpers.
8. **Release assertions must select the NEWEST binary.** The two-stage
   self-host build leaves the stage-1 binary in an older fingerprint
   directory; `find | head -1` happily asserts against the stale one
   (this failed the first v0.0.50 release run). Use mtime ordering
   (`ls -t`). The assertions themselves are the regression net: R3's
   "failure" was quote typos in diagnostic echos while the substantive
   checks (minos 14.0, `libSystem`-only) had already passed.

## 5. Deferred work (tracked TODOs)

**Status updates (same-day forensics sprint):**

- **#1 RESOLVED — root cause + fix landed on main.** Crash-report
  forensics (mcpp PR #117, m1–m7 matrix): plain by-path archive linking
  leaves default-visibility libc++ symbols that dyld unifies with the
  system libc++ from the shared cache — a split-brain where
  `ostream << int` crosses from the static copy into the system copy's
  locale machinery (`collate`/`num_put`) and SIGSEGVs. Trigger surface =
  the locale-based formatting path (which is why `std::println`-only
  binaries like mcpp/xlings never crashed). Fix:
  `-Wl,-load_hidden,<archive>` — forces the archive AND hides its
  symbols; mixed-C/C++ and pure-C++ int-output probes both run clean.
  (`-hidden-l` stays unusable under lld: it resolves like plain `-l`
  and picks the sibling dylib.) Shipped inside v0.0.50.
- **#3 FEASIBLE — pipeline proven** (mcpp PR #118): LLVM 20.1.7
  runtimes built at `CMAKE_OSX_DEPLOYMENT_TARGET=11.0`
  (static libc++/libc++abi, `LIBCXXABI_USE_LLVM_UNWINDER=OFF`); archive
  members carry `minos 11.0`, a `std::cout` probe linked at floor 11.0
  runs clean with no libc++ dylib dep. Productization = publish via
  xlings-res + swap the archive source (data-only).
- **#2 IMPLEMENTED on a branch** — single resolver
  (`platform::macos::deployment_target`) consumed by flags, the
  fingerprint rule and the std-module prebuild (stdmod previously had
  no deployment-target concept at all). Open as mcpp PR #119
  (recreated on post-squash main as `feat/macos-dt-resolver-v2`);
  intended as the first 0.0.51 change and the prerequisite for
  re-landing the built-in default floor.

| # | Item | Status | Unblocks |
| --- | --- | --- | --- |
| 1 | **Mixed C/C++ static binaries SIGSEGV** — `answer.c` +
`std::cout` main.cpp linked with static libc++ dies at runtime
(exit 139, e2e `36_llvm_toolchain.sh`); pure-C++ binaries are fine.
Root cause not isolated (suspect: `.c` objects' compile/link flags vs
the static C++ runtime init). Needs a dedicated forensics round
(crash report + link-command diff). | open | default-static flip |
| 2 | **std-module staging vs fingerprint boundary** — injecting a
built-in default floor into the canonical fingerprint flags alone
left the test build's std.pcm unstaged (`import std` failed
wholesale). Fix: centralize deployment-target resolution in one
helper consumed by flags.cppm, the fingerprint rule AND the stdmod
prebuild/staging path; then re-land the rustc-style built-in default
floor. | direction clear | default-static flip + built-in floor |
| 3 | **Custom libc++ build for floor 11.0–13.0** — build LLVM
runtimes at `-mmacosx-version-min=11.0`, publish via xlings-res
(scode:gcc precedent), swap the archive source (data-only change in
mcpp). Deferred on demand: Apple has EOL'd 11/12, arm64 userbase is
overwhelmingly 14+. | no technical blocker | floor 11.0 |
| 4 | macos-14 lane in both repos' regular CI (today: macos-15 build +
release-time assertions; the PR #115 experiment covered 14 manually). | optional | continuous 14-floor verification |

End state when #1 + #2 land: static libc++ and a built-in 14.0 floor
become the unconditional macOS defaults (cargo-style "portable by
default"), with the declared-floor gate kept only as the opt-out
boundary (`static_stdlib = false`).

## 6. Shipped outcome (final verification against release artifacts)

Verification matrix (mcpp PR #115's temp workflow, final revision):
runners `macos-15` + `macos-14`, both green —
https://github.com/mcpp-community/mcpp/actions/runs/27007616498

| Check | macos-14 | macos-15 |
| --- | --- | --- |
| Control: released 0.0.49 (`minos=15.0`, dynamic libc++) | refused — `dyld: Symbol not found: __ZNSt3__119__is_posix_terminalEP7__sFILE` (the documented root cause) | runs |
| xlings v0.4.50 tarball self-install | OK | OK |
| mcpp v0.0.50 tarball | OK | OK |
| `mcpp new hello` → build → run (xlings-provisioned LLVM) | OK | OK |
| Split-brain probe: `cout << int`, declared floor → static libc++ | `int says 42`, no libc++ dylib | same |

Artifact facts (asserted offline on the released tarball as well):
`bin/xlings` carries `LC_BUILD_VERSION minos=14.0.0` and links only
`/usr/lib/libSystem.B.dylib`.

Distribution sync completed for both releases:
- mcpp v0.0.50: GitHub release + GitCode mirror + xim-pkgindex
  `latest → 0.0.50` (PR #279).
- xlings v0.4.50: GitHub release + xlings-res mirrors (GitHub + GitCode,
  sha256-verified) + xim-pkgindex `latest → 0.4.50` (PR #280).

Net effect: macOS floor moved 15.0 → **14.0** for both tools' release
binaries; lower floors (11.0–13.0) remain a data-only swap away (§5 #3).
