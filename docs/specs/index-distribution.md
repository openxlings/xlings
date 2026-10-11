# Index Distribution

> Status: in force since 0.4.52 (artefacts), 0.4.54 (combined pointers),
> 2026.9.16.1 (region chains, bounded network). Implemented in
> `src/core/xim/indexfetch.cppm`, `src/core/xim/repo.cppm` (`sync_all_repos`),
> `src/core/config.cppm` (resource servers); built and published by
> `tools/build_xim_index_artifact.sh` and `tools/publish_xim_index.sh`.

How an index's content reaches a user's machine. What an index contains is
[xpkg manifest v1](xpkg-manifest-v1.md); which snapshot a client may use is the
[index version contract](index-version-contract.md).

## 1. An index is a versioned artefact, not a clone

An index is downloaded over HTTP as an **artefact with a version and a sha256**,
through the same download path as any resource — with its latency ordering and
stall watchdog — instead of `git clone` at run time. git remains the fallback.

Why not git: a git transfer cannot resume, gets no CDN, had no stall detection (a
handshake that completes and then goes silent waited hundreds of seconds), and
cloning 1.7 MB of history to read a 13 KB tree is waste. No mainstream package
manager clones an index from GitHub at run time.

| | git clone (≤ 0.4.51) | artefact |
|---|---|---|
| transport | git packfile: no resume, no CDN | HTTP tar.gz: resumable, CDN-able |
| resilience | proxies in a fixed order, no stall detection | latency ordering, stall watchdog, fall through each candidate |
| integrity | trusts the git origin | **pinned by the pointer's sha256**: any mirror may be untrusted, a lagging one is refused |
| size | 1.7 MB and history for a 13 KB tree | ~95 KB (main), 6–8 KB (sub-indexes) |
| offline / first start | needs the network | can be bundled |

## 2. The artefact contract

```
xim-index[-<sub>]-<ver>.tar.gz        # the index tree (pkgs/, .xpkgindex.json, xim-indexrepos.lua, ...; no .git)
xim-index-pointers.json               # the combined pointer file for every index
```

- **Pointers = one combined file** (a repository file, overwritten by a git push),
  holding every index:
  `{"format_version": 2, "indexes": {"xim": {...}, "awesome": {...}, ...}, "client_latest": {...}}`.
  One fetch returns every pointer (GitCode rate-limits raw requests made in quick
  succession), and a repository file can be overwritten where a GitCode release
  asset cannot. Its fields and history are the
  [index version contract](index-version-contract.md), §4.
- **An artefact is a release asset with a unique version**: created once, never
  overwritten. An index artefact is published once its content is final — a
  second publication under the same name cannot replace the first on GitCode.
- Published to `xlings-res/xim-index` on GitHub and GitCode.
- A custom source's base serves `<last path segment of the base>-pointers.json`.

An index directory managed as an artefact carries `.xlings-index-version` and is
never cloned again.

## 3. Fetching

```
the candidates for the region (the chosen server first, GitHub always appended)
  -> ordered by measured latency
  -> each candidate in turn (any failure, HTTP 404 included, tries the next)
  -> the pointer -> the artefact it names, verified by sha256
  -> unpacked and swapped in atomically (data/xim-pkgindex)
  -> on failure: git
```

Index candidates use **GitCode and GitHub directly**, never GitHub proxies: those
often accept the connection and do not serve the resource, sort first on latency,
and cost a timeout each. A host that times out or does not answer is penalised for
the session (a 404 is not).

The sha256 comes from the pointer, so a mirror serving an older tarball is
refused and the next candidate used: **correctness never depends on a mirror
being current**.

## 4. Which source an index uses

`XLINGS_INDEX_SOURCE=git|artifact|auto` (default `auto`):

| Index | `auto` |
|---|---|
| the official main index (remote, the official repository URL), new or already an artefact | **artefact** |
| an official default sub-index (in the main index's `xim-indexrepos.lua`, URL not overridden, not local) | **artefact** |
| an existing **git** main index (an old checkout, a fixture) | stays git |
| a sub-index the user added, overrode, or points at `file://` | git / a local link |

A default sub-index is recognised by "**in the Lua defaults, with the same URL**",
not by "absent from the JSON" — every sync writes the defaults back to
`xim-indexrepos.json`.

## 5. A region is an order of preference, not a choice

Every region object in configuration, `{"GLOBAL": ..., "CN": ...}`, reads as an
**ordered chain of candidates: the mirror decides the order, not the set**.

```
mirror=CN       ->  CN -> GLOBAL -> the rest (in declared order)
mirror=""       ->  GLOBAL -> the rest
unknown mirror  ->  GLOBAL -> the rest
```

| Key | Read as | Note |
|---|---|---|
| `index_repos[].artifact` | **the whole chain** | a failed pointer fetch at the first base (403, 429, offline) tries the next region; git only when all fail |
| `xim.index-base` | **the whole chain** | a self-hosted or offline base gets regional fallback too; once the overrides are exhausted it **never** silently returns to the official servers |
| `["artifact"]` in `xim-indexrepos.lua` | **the whole chain** | a sub-index declares its own artefact source |
| `xim.mirrors.index-repo`, git URLs in `xim-indexrepos.lua` | **the first only** | these name a **git remote**; a clone has one origin |

- Each base derives **its own pointer file name**: two mirror repositories with
  different names would otherwise ask each other for the wrong file.
- A region object **round-trips**: `xim-indexrepos.json` writes back the object,
  not the entry chosen this time.
- `xlings index list` (and `artifact_bases` in `--json`, `list_repos` in the
  interface) shows the order that will be tried.

**The artefact is the mechanism and git the fallback**: the region chain decides
when git's turn comes, never whether there is one.

## 6. Every network step is bounded

| Step | Bound | Override |
|---|---|---|
| HTTP connect / one attempt | 10 s connect, 120 s per attempt (index files are small, and there is always another candidate) | `XLINGS_INDEX_HTTP_TIMEOUT=<connect>:<max>`, `off` for the download defaults |
| an HTTP stall | an average below 10 KB/s for 15 s moves to the next candidate | `XLINGS_DOWNLOAD_LOW_SPEED=off\|<bytes>:<secs>` |
| a git transfer | `GIT_HTTP_LOW_SPEED_LIMIT=1000`, `GIT_HTTP_LOW_SPEED_TIME=60`, `GIT_TERMINAL_PROMPT=0` | `XLINGS_GIT_NETWORK_TIMEOUT=<secs>\|off`; **values the operator set are kept** |
| one `xlings update` | 300 s in total; past it the remaining sources are **skipped** and named (their local copies stay) | `XLINGS_UPDATE_TIMEOUT=<secs>\|off` |

The git bound is needed because a TCP reachability probe passes for a connection
whose handshake completes and which then goes silent; git's own low-speed pair
aborts a transfer averaging zero bytes.

## 7. Self-hosting and offline

`XLINGS_INDEX_BASE_URL` (environment) or `xim.index-base` in `.xlings.json` (a
string or `{GLOBAL, CN}`) sets the base for pointers and artefacts. A self-hosted
server serves two kinds of static file under it: `xim-index-pointers.json` and
`xim-index[-<sub>]-<ver>.tar.gz`. A local directory or `file://` base serves
offline installs, private mirrors and tests.

A release package bundles `data/xim-pkgindex` as an artefact (no `.git`, with
`.xlings-index-version`), so a fresh install starts on artefacts.

## 8. Compatibility with older clients

Clients up to 0.4.53 read a release-asset pointer, `xim-index[-<sub>]-latest.json`,
not the combined repository file. Publishing still updates that asset on GitHub
(GitCode cannot overwrite; those clients fall back to GitHub), so an old client's
`xlings self update` sees the newest release; after the upgrade it reads the
combined pointer.
