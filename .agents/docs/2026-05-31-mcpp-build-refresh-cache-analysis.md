# mcpp Build Refresh And Cache Analysis For xlings

> Date: 2026-05-31 | Status: active coordination note

## Purpose

Track the xlings-side impact of the current `mcpp build` behavior and the local
validation plan for the mcpp fix branch.

This document does not switch xlings to a new build driver. It records the
issues observed while building xlings with the released `mcpp 0.0.36` and the
checks needed before xlings can depend on the fixed behavior.

## Observed Behavior

Running `mcpp build` in the xlings checkout showed:

```text
Updating package index (auto-refresh)
[1/7] awesome::/home/speak/.mcpp/registry/data/xim-index-repos/...
...
Compiling mcpplibs.tinyhttps v0.2.3
Compiling mcpplibs.xpkg v0.0.41
```

The package-index lines come from xlings internals invoked by mcpp, not from
xlings source code. The dependency `Compiling` lines are mcpp status labels.

## Local Evidence

The validation checkout used for the latest repro is:

```text
/home/speak/test/tmp/xlings
```

Current project dependency shape:

```toml
[dependencies.mcpplibs]
cmdline = "0.0.2"
xpkg = "0.0.41"
tinyhttps = "0.2.3"
capi.lua = "0.0.3"
```

The local no-change fast path is healthy:

```text
mcpp build
Finished release [optimized] in 0.01s

ninja -C target/x86_64-linux-gnu/d14fbbf7aeceb894 -n
ninja: no work to do.
```

So the slow/noisy path is specifically the full mcpp prepare path that runs
after source or manifest changes.

## Task Split

- [x] mcpp: fix quiet auto-refresh and reliable freshness marker.
- [x] mcpp: fix dependency cache label canonicalization.
- [ ] libxpkg: fix future package archives so `mcpp.toml` version matches the
      release tag.
- [ ] xlings: keep using released mcpp only after the fixed version is
      published or explicitly pinned in the validation job.
- [x] xlings: update this document with exact validation commands and output
      after the mcpp PR branch is built locally.

## Validation Criteria For xlings

- [x] `mcpp build` can build xlings from a dirty-source rebuild without
      printing xlings internal `[N/M] index::path` update lines.
- [x] A second full prepare inside the index TTL does not run package-index
      update again.
- [x] `mcpplibs.tinyhttps` and `mcpplibs.xpkg` either show `Cached` when they
      hit BMI cache or the log clearly reflects actual work performed.
- [x] The resulting xlings binary still runs basic CLI smoke checks.

## Current Decision

Do not make xlings CI depend on this behavior yet. First land and release the
mcpp-side fix, then add or update the xlings mcpp validation path.

## Local Validation With mcpp Fix Branch

Binary:

```text
/home/speak/workspace/github/mcpp-community/mcpp/target/x86_64-linux-gnu/4d24c8b57fdbbbb4/bin/mcpp
```

Commands:

```bash
MCPP_BIN=/home/speak/workspace/github/mcpp-community/mcpp/target/x86_64-linux-gnu/4d24c8b57fdbbbb4/bin/mcpp
"$MCPP_BIN" build --print-fingerprint > /tmp/xlings-mcpp-build-1.log 2>&1
"$MCPP_BIN" build --print-fingerprint > /tmp/xlings-mcpp-build-2.log 2>&1
```

Results:

```text
FIRST_RC=0 SECOND_RC=0
```

First full prepare showed the mcpp-level refresh only:

```text
Updating package index (auto-refresh)
Finished release [optimized] in 55.38s
```

The first run had no xlings `[N/M] index::path` output. The longer runtime is
expected once because the cache identity changed from short/stale names to the
canonical dependency keys.

Second full prepare stayed inside the marker TTL and reused cache:

```text
Cached mcpplibs.tinyhttps v0.2.3
Cached mcpplibs.xpkg v0.0.41
Finished release [optimized] in 0.02s
```

Smoke check:

```bash
target/x86_64-linux-gnu/d14fbbf7aeceb894/bin/xlings -h
```

Output included `xlings 0.4.47`, `USAGE`, and `xlings [OPTIONS] [SUBCOMMAND]`.
