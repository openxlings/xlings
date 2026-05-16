# Mirror Fallback Step 1 — Implementation Plan

**Date**: 2026-05-01
**Status**: in implementation
**Branch**: `feat/mirror-fallback`

## Goal

Ship a minimum viable "GitHub URL fallback" mechanism that covers all three resource classes xlings downloads:

1. **HTTP downloads** — release tarballs, raw files, archives
2. **Git clone** — source-build packages (`fromsource.lua`, project templates)
3. **Index repos** — `xim-pkgindex` and its sub-indexes

The mechanism is **failure-driven**, not preemptive: the original URL is always tried first, and mirror URLs are appended as a fallback queue. This keeps the happy path zero-overhead for users who can reach GitHub directly, while giving users on restricted networks a chance to recover automatically.

## Scope (in)

- A new top-level module `xlings.core.mirror` that exposes `expand(url, opts) -> [url]`.
- Three URL-rewriting "forms": `prefix` (ghproxy-style), `host-replace` (kkgithub-style), `jsdelivr` (raw-only CDN).
- A built-in default mirror list (compiled into the binary as a raw string literal).
- An optional user override at `~/.xlings/data/github-mirrors.json`.
- Three integration points in existing code (HTTP downloader, git clone, index repo sync).
- Configuration via `XLINGS_MIRROR_FALLBACK={off|auto|force}` env var and `mirror_fallback` field in `~/.xlings.json`.
- Unit tests for classification, rewriting, expand semantics.
- An e2e test that mocks a 503 GitHub and verifies fallback succeeds with sha256 still validated.

## Scope (out — explicitly deferred)

| Deferred | Reason |
|----------|--------|
| Mid-flight throughput monitoring + speculative race | Step 2 — needs more invasive plumbing into the download loop |
| Cross-process mirror health stats | Step 3 — only worth doing if Step 1 reveals patterns |
| `xlings mirror {list,test,use,report}` CLI subcommand | Step 3 — env var + config covers all current needs |
| Custom proxy URL templates (corporate intranet) | Wait for user feedback |
| Remote hot-update of mirror list | Chicken-and-egg: can't fetch the list if GitHub is unreachable |
| First-party `gitee.com/d2learn/xim-pkgindex` mirror | Independent ops task; tracked separately |

## Open questions (decided)

| # | Question | Decision | Rationale |
|---|----------|----------|-----------|
| 1 | gitee one-party index mirror status | Independent ops task; not in this PR | Step 1 covers the "GitHub fails" case regardless |
| 2 | Where does git clone happen? | `xim/downloader.cppm:25-127` (source pkgs) and `xim/repo.cppm:160-177` (index repos) | Confirmed by code scan |
| 3 | Trigger fallback when `mirror=CN` too? | Yes — fallback is failure-driven, not config-driven | Don't let mirror config become a single point of failure |
| 4 | Failure criteria | HTTP: connect timeout / 5xx / 429 / 403; Git: subprocess exit code ≠ 0. NOT 404/401 | 404/401 are real failures; stderr parsing is fragile |
| 5 | Default JSON embedding | Raw string literal in `.cppm` | No xmake changes; constexpr-evaluable |
| 6 | Same-priority shuffle? | Yes, PID-seeded stable shuffle | Distribute load across mirrors; reproducible per-process |

## Module structure

```
src/core/mirror.cppm           — umbrella, re-exports types + expand
src/core/mirror/types.cppm     — ResourceType / Form / Mode enums + Mirror struct
src/core/mirror/forms.cppm     — three URL-rewriting strategies
src/core/mirror/registry.cppm  — embedded default JSON + user override loader
src/core/mirror/expand.cppm    — classify() + expand() core logic
```

Mirrors the `xself.cppm` umbrella + partition pattern used for the 0.4.8 alias migration. Future Step 2 race code goes in `src/core/mirror/race.cppm` without touching the existing files.

## Public API

```cpp
namespace xlings::mirror {

enum class ResourceType { Release, Raw, Archive, Git, Unknown };
enum class Form         { Prefix, HostReplace, JsDelivr };
enum class Mode         { Auto, Off, Force };

struct Mirror {
    std::string name;
    std::string host;
    Form form;
    std::vector<ResourceType> supports;
    int priority = 100;
    std::optional<std::size_t> limit_bytes;
};

struct ExpandOptions {
    std::optional<ResourceType> type;
    Mode mode = Mode::Auto;
    std::size_t expected_size = 0;
};

ResourceType classify(std::string_view url);
bool is_github_url(std::string_view url);

std::vector<std::string> expand(std::string_view url,
                                const ExpandOptions& opts = {});

Mode current_mode();
void set_mode(Mode mode);  // testing only

} // namespace xlings::mirror
```

## URL-rewriting forms

```
form=prefix
  https://github.com/x/y/releases/download/v1/asset.tar.gz
  → https://ghfast.top/https://github.com/x/y/releases/download/v1/asset.tar.gz

form=host-replace
  https://github.com/x/y/releases/download/v1/asset.tar.gz
  → https://kkgithub.com/x/y/releases/download/v1/asset.tar.gz
  https://raw.githubusercontent.com/x/y/main/file.txt
  → https://kkgithub.com/x/y/raw/main/file.txt

form=jsdelivr (raw only)
  https://raw.githubusercontent.com/x/y/<ref>/path/file
  → https://cdn.jsdelivr.net/gh/x/y@<ref>/path/file
```

## Default mirror list (built-in)

```json
{
  "version": 1,
  "mirrors": [
    { "name": "jsdelivr",     "form": "jsdelivr",     "host": "cdn.jsdelivr.net",
      "supports": ["raw"], "limit_bytes": 52428800, "priority": 5 },
    { "name": "ghfast",       "form": "prefix",       "host": "ghfast.top",
      "supports": ["release", "raw", "archive", "git"], "priority": 10 },
    { "name": "ghproxy-net",  "form": "prefix",       "host": "ghproxy.net",
      "supports": ["release", "raw", "archive", "git"], "priority": 20 },
    { "name": "kkgithub",     "form": "host-replace", "host": "kkgithub.com",
      "supports": ["release", "raw", "archive", "git"], "priority": 30 }
  ]
}
```

## Config resolution flow

```
At first use of mirror::expand() in a process:
  1. Resolve mode: env XLINGS_MIRROR_FALLBACK > .xlings.json mirror_fallback > Auto
  2. Resolve list: ~/.xlings/data/github-mirrors.json (if exists, full override)
                   else compiled-in DEFAULT_MIRRORS_JSON
  3. Cache both for the process lifetime
```

## Integration points

### A. `src/core/xim/downloader.cppm:157-160` (HTTP)

Append `mirror::expand(url)` results to the existing `urls` vector after package-author fallbacks, deduped against existing entries.

### B. `src/core/xim/downloader.cppm:25-127` (git clone for source pkgs)

Refactor `git_clone_one` to iterate a fallback list. Merge `task.fallbackUrls` (insert after primary) with `mirror::expand(url, {type=Git})` (append to tail). Try sequentially; on subprocess failure, clean partial clone dir and retry next URL. Honor cancellation between attempts.

### C. `src/core/xim/repo.cppm:160-177` (index repo sync)

Same pattern as B: in the clone branch, replace single-URL clone with a fallback loop using `mirror::expand`. Pull-then-fail-then-reclone path automatically benefits because the reclone goes through the new code.

## Configuration knobs

```
Env (highest priority):
  XLINGS_MIRROR_FALLBACK=auto    # default
  XLINGS_MIRROR_FALLBACK=off     # strict: original URL only (CI tests)
  XLINGS_MIRROR_FALLBACK=force   # skip original, go direct to mirrors

Config file (~/.xlings.json or project .xlings.json):
  "mirror_fallback": "auto" | "off" | "force"
```

No CLI flags in Step 1.

## Test plan

### Unit tests (`tests/unit/test_mirror.cpp`)

- `classify()` — release / raw / archive / git / unknown
- `rewrite()` × 3 forms × representative URLs
- `expand()` — Auto / Off / Force, GitHub vs non-GitHub URL, size limit filtering
- registry — user-override JSON fully replaces default
- shuffle — same priority bucket rotates between calls (within same process is stable)

### E2E test (`tests/e2e/mirror_fallback_test.sh`)

Six scenarios using Python's stdlib `http.server` for mock origins:

- S1: original URL serves 200 → no fallback path entered (verify by log)
- S2: original URL 503 + mirror 200 → fallback succeeds + sha256 verified
- S3: `XLINGS_MIRROR_FALLBACK=off` + original URL 503 → hard fail (no fallback)
- S4: gitee URL + 503 → no expansion (output list is exactly `[gitee_url]`)
- S5: user-supplied `~/.xlings/data/github-mirrors.json` → replaces defaults
- S6: 404 on original URL → does NOT trigger fallback (real not-found)

## Files

```
NEW:
  docs/plans/2026-05-01-mirror-fallback-step1.md   (this file)
  src/core/mirror.cppm                             (~10 LOC, umbrella)
  src/core/mirror/types.cppm                       (~80 LOC)
  src/core/mirror/forms.cppm                       (~120 LOC)
  src/core/mirror/registry.cppm                    (~150 LOC)
  src/core/mirror/expand.cppm                      (~150 LOC)
  tests/unit/test_mirror.cpp                       (~250 LOC)
  tests/e2e/mirror_fallback_test.sh                (~150 LOC)

MODIFIED:
  src/core/xim/downloader.cppm                     (~40 LOC added)
  src/core/xim/repo.cppm                           (~20 LOC added)
  src/core/config.cppm                             (~10 LOC, mirror_fallback reader)
  .github/workflows/xlings-ci-linux.yml            (~3 LOC, new e2e step)
```

## Commit & PR plan

- **Commit 1** — `feat(mirror): add core mirror module + unit tests`
  Adds the 5 mirror/*.cppm files + unit tests. Zero behavior change in main binary.
- **Commit 2** — `feat(mirror): wire fallback into HTTP/git/index download paths`
  Three integration points + config field + e2e test + CI step.

Single PR `feat: github mirror fallback for resilient downloads`.

## Risks & mitigations

| Risk | Mitigation |
|------|-----------|
| Mirror serves wrong/tampered content | sha256 (HTTP) and ref hash (git) checks already enforced; not relaxed for mirrors |
| ghproxy-style mirror dies | List is data-driven; remove via config or release patch |
| Fallback adds latency on every download | Only attempted after primary fails; happy path zero-cost |
| Same-priority mirrors get hammered | PID-seeded shuffle distributes load |
| User has corporate proxy that needs different config | Existing HTTP_PROXY env vars still respected (tinyhttps layer below) |
| Step 2 (race) requires module restructure | Module is already partition-based; race goes in `mirror/race.cppm` without touching others |

## Removal / future-evolution markers

This is **not** transitional code — mirror fallback is a permanent feature. No `COMPAT` markers needed. The module is structured for extension (race, stats, custom proxies) without rewrite.
