# macOS Minimum-Version Support for xlings + mcpp Releases

> Date: 2026-06-05
> Status: research done; CI experiments in progress (mcpp-community/mcpp PR #115)
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
- Run B: _pending_

## 3. Rollout plan (xlings first)

1. **xlings 0.4.50** — release workflow macOS job sets
   `MACOSX_DEPLOYMENT_TARGET=11.0` and injects the static-libc++ ldflags
   (manifest/workflow level; works with bootstrap mcpp 0.0.49 today).
   xlings macOS binary: minos 11.0, static libc++. If the static-dtor
   abort appears for xlings it is already guarded (`_Exit`).
2. **mcpp flags.cppm change** — implement clang/macOS `staticStdlib`
   (§1.3) so every mcpp-built project (xlings included) gets the static
   link automatically; add the `__APPLE__` exit guard to mcpp `main.cpp`
   if run B shows the abort. Ship as **mcpp 0.0.50** with the release
   workflow also setting `MACOSX_DEPLOYMENT_TARGET=11.0` (the self-build
   by bootstrap 0.0.49 uses the workflow-level ldflags injection; from
   0.0.51 onward the flags.cppm path covers it natively).
3. Bump xlings release workflow's bootstrap mcpp pin to 0.0.50; future
   xlings releases need no workflow-level ldflags hack.
4. Sync as usual: xlings-res (github + gitcode), xim-pkgindex
   latest refs, byte-verified.
5. Verify: the temp PR #115 CI re-run against the released artifacts; a
   macos-14 user-flow equivalent of the Windows PR #114 repro.

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
