# Adaptive Mirroring for GitHub Asset URLs (no explicit mirror config)

> Date: 2026-06-04
> Status: implemented (0.4.49 — stall watchdog + latency-ordered candidates)
> Related: `2026-05-01-mirror-fallback-step1.md` (mirror registry),
>          `2026-06-04-shim-owner-anchoring-design.md` (0.4.48)

## TL;DR

When a package only carries a plain GitHub URL and the user never set a
mirror (`mirror=GLOBAL` default, `mirror_fallback=auto`), a degraded
network ("connects, then trickles") can pin a download to the original
URL for up to `maxTimeSec` (600 s) × retries before any mirror is tried —
the user experiences a hang.

Two changes fix this **without any explicit configuration**:

1. **Stall watchdog** (tinyhttps): abort a transfer whose windowed average
   speed stays under a floor (default 10 KB/s over 15 s) and move to the
   next candidate. This is the root-cause fix — latency probes cannot see
   "handshake fast, transfer throttled".
2. **Latency-ordered candidates** (mirror::adaptive): probe each candidate
   host's TCP connect latency once per process (1.5 s cap, cached),
   order candidates by measured latency, and demote hosts that stalled
   earlier in the session. Guarded by a sha256 integrity gate.

## 1. Current state (what exists, what's missing)

The mirror machinery has three tiers today:

| Tier | Mechanism | Quality |
| --- | --- | --- |
| index repos / `.git` sources | `mirror::expand(type=Git)` + CN swaps the index URL to the official gitee mirror (`config.cppm`) | official mirror, deterministic |
| `XLINGS_RES` assets | resource-server map (GLOBAL→github / CN→gitcode) + **latency-probed selection** (`config.cppm selected_resource_server_for_`, `tinyhttps::probe_latency`) | official mirror, adaptive |
| plain GitHub asset URLs | `mirror::expand()` in `downloader.cppm` appends third-party proxies (ghfast / ghproxy / kkgithub) **after** the original URL | proxy-grade, **static order** |

Gaps for tier 3:

- **Order is static**: original URL is always tried first (`Mode::Auto`),
  regardless of how the local network reaches github.com.
- **No stall detection**: `DownloadOptions` has `connectTimeoutSec=30`
  (handshake only) and `maxTimeSec=600` (total). A connection that
  *establishes* but transfers at KB/s-level — the typical degraded-network
  shape for github.com — triggers neither. Worst case before the first
  mirror attempt: 600 s × (1 + retryCount).
- The latency probing that already powers `XLINGS_RES` server selection
  and `xlings self install`'s GLOBAL/CN auto-pick is **not applied** to
  tier-3 candidates.

## 2. Design

### 2.1 Stall watchdog (tinyhttps)

New fields on `DownloadOptions` (defaults ON; `0` disables):

```cpp
int lowSpeedLimitBytes { 10 * 1024 }; // windowed average floor
int lowSpeedTimeSec    { 15 };        // window length
```

Semantics (curl `--speed-limit/--speed-time` style, windowed):

- Track `(windowStartTime, windowStartBytes)`. Once a window of
  `lowSpeedTimeSec` has elapsed, compute the windowed average; if it is
  below `lowSpeedLimitBytes`, abort the attempt with a `stalled: ...`
  error; otherwise slide the window forward.
- Implemented as a small pure class `StallDetector` (unit-testable) fed
  from the progress callback; the abort is delivered by wrapping the
  existing `isCancelled` hook — no changes to the underlying
  `mcpplibs.tinyhttps` client.
- Total-silence connections never fire the progress callback, so the
  per-read socket timeout is lowered from `maxTimeSec` to
  `max(30, 2 × lowSpeedTimeSec)` when the watchdog is enabled — a server
  that sends nothing for that long is treated as failed and the next
  candidate is tried.
- A stalled attempt is an *attempt failure*, not a cancellation: the
  driver loop proceeds to the next retry/candidate as for any error.

Escape hatch: `XLINGS_DOWNLOAD_LOW_SPEED=off` (or `0`) disables the
watchdog process-wide; `XLINGS_DOWNLOAD_LOW_SPEED=<bytes>:<secs>` tunes
it. Users on genuinely-slow-but-working links (< 10 KB/s sustained) are
the only regression surface, and they get a copy-pasteable knob in the
error message.

### 2.2 Latency-ordered candidates (`mirror::adaptive`)

New partition `src/core/mirror/adaptive.cppm`:

```cpp
// per-host latency, probed once per process (TCP connect, 1.5 s cap)
double host_latency(const std::string& url, const ProbeFn& probe = {});

// session-scoped demotion: a stalled host is as bad as an unreachable one
void penalize_host(const std::string& url);

// order candidates for download. has_sha256 gates how aggressive we get.
std::vector<std::string> reorder(std::vector<std::string> urls, bool has_sha256,
                                 const ProbeFn& probe = {});
```

Rules:

- `urls.size() < 2` → returned unchanged (nothing to decide; also keeps
  `Mode::Off` behavior intact since `expand()` returns one URL there).
- Probe the **unique hosts** of the candidates concurrently-cheap
  (sequential 1.5 s-capped TCP connects; results memoized per process in
  a mutex-guarded map, so one `xlings install` with 40 packages probes
  each host exactly once).
- **sha256 integrity gate**:
  - `has_sha256 == true`: full stable sort ascending by latency. Mirrors
    may be tried before the original URL — the declared sha256 pins the
    exact bytes, so a malicious/corrupt proxy cannot inject content.
  - `has_sha256 == false`: the original (author-declared) URL keeps first
    position **unless its host is unreachable/penalized** (latency = ∞);
    only the relative order of the remaining candidates is
    latency-sorted. Third-party proxies must not silently become the
    authoritative source for unpinned content.
- `penalize_host()` writes ∞ into the same cache. The downloader calls it
  when an attempt for that URL died with a `stalled:` error, so the next
  package in the same session skips straight past the degraded host —
  this folds the "session network profile" idea into the latency cache
  with no extra state.
- Escape hatch: `XLINGS_ADAPTIVE_MIRROR=off` returns candidates unchanged
  (CI determinism / debugging).

### 2.3 Downloader integration

In `downloader.cppm` the HTTP path already builds
`[primary, fallbackUrls..., expand() mirrors...]`. Two added lines of
behavior:

- pass the list through `mirror::adaptive::reorder(urls, !task.sha256.empty())`
  before handing it to `tinyhttps::download_file`;
- supply tinyhttps's new per-URL failure hook
  (`onUrlAttemptFailed(url, error)`) and call `penalize_host(url)` when
  the error is a stall.

This also benefits `XLINGS_RES` tasks: their github/gitcode fallback pair
now gets latency-ordered with the same machinery that
`selected_resource_server_for_` uses, instead of a fixed order.

### 2.4 Explicitly out of scope (follow-ups)

- **Racing / happy-eyeballs** (start best mirror in parallel after N
  silent seconds): best UX, heaviest implementation; revisit if the
  watchdog+reorder combination proves insufficient.
- **Hot-updating the proxy registry** via the index repo (the compiled-in
  list rots — kkgithub is already dead); separate change.
- **Official mirror form for arbitrary release URLs** (rewrite
  `github.com/<o>/<r>/releases/download/...` to a gitcode twin when one
  exists); needs a mirrored-repo existence signal.
- Index lint: warn on non-GitHub, non-`XLINGS_RES` direct URLs (the
  lua.org outage class).

## 3. Implementation map (as landed in 0.4.49)

| Change | Where |
| --- | --- |
| `StallDetector` (pure, windowed-average) + `lowSpeedLimitBytes/lowSpeedTimeSec` + reduced per-read timeout + `onUrlAttemptFailed` hook + `XLINGS_DOWNLOAD_LOW_SPEED` | `src/libs/tinyhttps.cppm` |
| `mirror::adaptive` — cached `host_latency`, `penalize_host`, sha256-gated `reorder`, `XLINGS_ADAPTIVE_MIRROR` | `src/core/mirror/adaptive.cppm` (+ export in `src/core/mirror.cppm`) |
| reorder + stall-penalty wiring in the HTTP download path | `src/core/xim/downloader.cppm` |
| Unit tests: detector windows (ok/stall/slide/disable), reorder gates (sha-full-sort, no-sha original-first, dead-host demotion, probe memoization, penalty, env off) | `tests/unit/test_adaptive_mirror.cpp` |
| Version bump 0.4.49 | `src/core/config.cppm`, `mcpp.toml` |

## 4. Risk notes

- **Sustained-slow-but-legit links** (< 10 KB/s for 15 s+): the watchdog
  will rotate through mirrors and ultimately fail if every source is that
  slow. The stall error message names the env knob. Judged acceptable:
  such links cannot realistically install multi-MB packages anyway.
- **Probe cost**: ≤ 1.5 s × unique hosts, once per process, only on
  multi-candidate downloads. Single-URL downloads pay nothing.
- **Mirror-first with sha256**: bytes are pinned by checksum; worst case
  a poisoned proxy causes a checksum mismatch → next candidate.
- **No e2e for the stall path** (needs a throttled HTTP server): covered
  by unit tests on the pure detector + reorder logic; the existing
  download e2e suite guards against regressions in the happy path.
