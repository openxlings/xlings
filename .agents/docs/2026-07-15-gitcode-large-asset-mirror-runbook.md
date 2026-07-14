# Runbook: GitCode large-asset mirror (manual post-release step)

**Status:** active procedure · **Date:** 2026-07-15 · **Owner:** release maintainer

## TL;DR

After every `xlings` release, run this once from **any environment with a
healthy GitCode (CN) network route** — a CN shell/box/cloud terminal, not tied
to any specific machine:

```bash
# needs: tools/gtc + a GitCode token (env GITCODE_TOKEN, or
#        ~/.config/gitcode-tool/config.json). GitHub side is auto-skipped.
tools/mirror-latest.sh xlings
```

It uploads only the GitCode assets that release CI could not, verifies all
platforms on both hosts, and is a ~40 s no-op when nothing is missing. Done.

## Why this step exists

The release pipeline mirrors every binary to two hosts so `XLINGS_RES`
downloads resolve for all regions:

- **GitHub** (`github.com/xlings-res/xlings`) — GLOBAL.
- **GitCode** (`gitcode.com/xlings-res/xlings`) — CN acceleration.

**A GitHub-hosted CI runner cannot upload large (>~8 MiB) assets to GitCode.**
The runner sits in Azure-US; GitCode's asset store is Huawei Cloud OBS in CN.
The cross-border upload path sustains only ~15–30 KB/s and every method times
out, so a 27 MiB asset never completes.

This was not a guess — probe PR **openxlings/xlings#371** measured it from a
GitHub runner against the real 27 MiB asset:

| method | result | speed | uploaded before timeout |
|---|---|---|---|
| gtc (urllib PUT) | timeout | — | — |
| curl default | timeout | 31 KB/s | 6.2 / 27 MB |
| curl --http1.1 | timeout | 15 KB/s | 3.0 MB |
| curl --limit-rate 300k | timeout | 15 KB/s | 4.2 MB |
| curl --limit-rate 1M | timeout | 24 KB/s | 4.8 MB |

The same files upload in ~10 s from a CN host via the same `gtc`. So this is a
network-route fact, not a script/protocol/rate-limit issue, and not a GitCode
fault. Protocol, timeout, and rate-limit tuning were all tried and rejected.

## Why it is a manual step and not automated

- **Correctness does not depend on it.** CN clients already fall back to the
  GitHub asset URLs (`build_xlings_res_fallback_urls_` in
  `src/core/xim/installer.cppm`), so a missing GitCode copy only costs CN
  download speed, never availability.
- **It is low-volume and low-frequency** — ~2–4 big files, once per release.
- Standing infra to do it automatically (CN cron / VM / self-hosted runner)
  is possible but was judged not worth the cost/maintenance/attack-surface for
  a per-release convenience. Revisit if releases become frequent.

## What the CI does on its own

- `release.yml` → `mirror-binaries` job: mirrors everything to GitHub + the
  small GitCode assets; fail-fast (non-blocking) on the big GitCode ones. It
  is bounded (`timeout-minutes: 15`, per-call caps) after the v0.4.65 incident
  where an unbounded stalled upload pinned the job for 40+ min (#369, #370).
- `mirror-binaries.yml` (manual dispatch): same script, for re-mirroring the
  GitHub side / small assets on demand. Also cannot do the big GitCode assets.

## The manual procedure in detail

1. A release has published (GitHub assets exist on `openxlings/xlings`).
2. From a CN-routed environment with `tools/gtc` and a GitCode token:
   ```bash
   tools/mirror-latest.sh xlings
   ```
   - Resolves the latest release version automatically.
   - Skips assets already mirrored (no download for those).
   - Downloads only the missing ones from the public GitHub release URL and
     uploads them to GitCode, then verifies 200 + size on both hosts.
3. Confirm the final `verify:` block shows `OK` for all 16 URLs (8 assets ×
   2 hosts). `all platforms mirrored OK` means done.

If a GitCode asset is reported "registered but not downloadable after
retries", it is a broken/phantom attachment: GitCode has **no** asset/release
delete via API (v5 returns 405, exposes no release id), so delete it in the
GitCode release **web UI**, then re-run the command.

## Validation

The tooling was verified end-to-end from a CN host before adoption:

- **Steady state** (nothing missing, v0.4.65): 16 assets skipped, 0 downloads,
  all 16 URLs (8 assets × 2 hosts) `OK`, ~40 s.
- **Gap-fill** (a real GitHub-only gap constructed on a throwaway tag): the
  script detected the gap, downloaded from the public GitHub URL, uploaded to
  GitCode, and verified — the GitCode copy was `sha256`-identical to the
  source, and a second run was a clean idempotent skip.

## Related

- Probe: openxlings/xlings#371 (throwaway; closed).
- Mirror hardening: openxlings/xlings#369, #370.
- Tooling: `tools/mirror-latest.sh`, `tools/mirror_res.sh`.
