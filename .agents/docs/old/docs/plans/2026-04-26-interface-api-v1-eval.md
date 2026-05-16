# xlings interface v1 — Universality Evaluation

Date: 2026-04-26
Branch: `feat/interface-protocol-v1` @ `d8abeb3`
Reviewer scope: protocol layer + 9 implemented capabilities, against xstore's actual consumption and the v1 design doc (`2026-04-25-interface-api-v1.md`).

## TL;DR

| Axis | Grade | Note |
|---|---|---|
| Protocol design | A | NDJSON envelope, 7 event kinds, control channel, heartbeat, version field. Idiomatic and reusable. |
| Capability coverage today | C+ | 9 / ~17 designed verbs. Read+install paths solid; SubOS, repo, self-update, run/exec all missing. |
| Universality stance (P1–P9) | A− | Strong: protocol-first, primitives-not-endpoints, no client-specialization. Roadmap lines up. |
| xstore readiness | B− | Read paths and install/remove/use covered. SubOS, shim listing, env derivation still bypass interface (filesystem + raw subcommands). |
| 3rd-party client readiness | C | Until Phase 3 (subos), Phase 4 (dataKind cleanup) and Phase 5 (reference client) land, adopters will reverse-engineer xlings home layout. |

Conclusion: protocol is the right shape. Capability set is half-built. The single highest-leverage fix is the SubOS capability bundle in Phase 3 — it's also the only thing forcing xstore to keep filesystem-level coupling.

---

## 1. Protocol layer (what's already good)

`src/interface.cppm`, `src/runtime/eventstream*.cppm`, `src/runtime/capability.cppm`.

- **Envelope**: every line is one JSON object with `kind ∈ {data, progress, log, prompt, error, heartbeat, result}`. `result` is the terminator. Clean and parseable line-by-line.
- **Versioning**: `protocol_version = "1.0"` is exposed via `xlings interface --version`. Clients can gate behavior. Minor-bump for additive, major-bump for breaking — documented (P5).
- **Streaming-first** (P4): long ops (install, search, list) emit incrementally. Combined with `heartbeat` every 5 s of idle (`src/interface.cppm:109–125`), clients reliably detect liveness.
- **Control channel**: stdin accepts `cancel | pause | resume | prompt-reply{id, value}` (`src/interface.cppm:147–161`). Avoids signal/PID coupling (P6).
- **Errors first-class** (P7): `kind=error` is its own event with `code, message, recoverable, hint?`. Independent from `result.exitCode`, so a capability can stream non-fatal errors and still finish 0.
- **Cancellation token** plumbed through `Capability::execute(params, stream, CancellationToken*)` — `install_packages` already uses it.

What's still rough at the protocol layer:

- **dataKind values are UI-bound** (`info_panel`, `styled_list`). Phase 4 plans to rename to `package_listed`, `system_info`, `repo_updated`, `shim_rebuilt`. Until then, every external client has to know that "info_panel" means "field/value table" — a P3 violation.
- **ErrorEvent.code is an int**. Clients can't distinguish E_NETWORK from E_DISK_FULL. Phase 3 switches to enum strings.
- **No bidirectional content stream**. `run` / `exec` (PTY) deferred to v2 — discussed in §5.4 of the design doc, not yet RFC'd.
- **`result.data` overlaps with mid-stream `data` events**. Two channels for the same kind of payload increases parser complexity. Recommend: pick one — either `data` events accumulate and `result` carries summary only, or `result.data` is canonical and `data` events are progress. xstore currently merges them (`backends/electron/xlings.ts:54–67`), which works but is a non-obvious convention.

---

## 2. Capability coverage today

9 capabilities are registered (`src/capabilities.cppm:209–222`):

| # | Name | Streams | Cancel | Side effects | Status |
|---|---|---|---|---|---|
| 1 | `search_packages` | yes | no | read-only | ✅ shipped |
| 2 | `list_packages` | yes | no | read-only | ✅ shipped |
| 3 | `package_info` | no | no | read-only | ✅ shipped |
| 4 | `list_installed_versions` | no | no | read-only | ✅ shipped |
| 5 | `system_status` | no | no | read-only | ✅ shipped |
| 6 | `install_packages` | yes (4 dataKinds) | yes | writes XLINGS_HOME, network | ✅ shipped |
| 7 | `remove_package` | no | no | deletes from disk | ✅ shipped |
| 8 | `update_packages` | partial | no | git pull | ⚠️ design doc §5.2: emit coverage incomplete |
| 9 | `use_version` | no | no | rebuilds shims | ✅ shipped |

Designed but not yet implemented (per design doc §5):

| # | Name | Phase | Notes |
|---|---|---|---|
| 10 | `list_subos` | 3 | xstore today reads `$XLINGS_HOME/subos/` directly |
| 11 | `create_subos` | 3 | xstore today shells `xlings subos new <name>` |
| 12 | `switch_subos` | 3 | xstore today shells `xlings subos use <name>` |
| 13 | `remove_subos` | 3 | not yet exposed |
| 14 | `list_subos_shims` | 3 | xstore today reads `$XLINGS_HOME/subos/current/bin/` directly |
| 15 | `list_repos` | 4+ | not yet exposed |
| 16 | `add_repo` | 4+ | not yet exposed |
| 17 | `remove_repo` | 4+ | not yet exposed |
| 18 | `self_update` | 4+ | xself logic exists in core, no capability wrapper |
| 19 | `env` | 3 | recommended: return `{XLINGS_HOME, PATH, current_subos, …}` so clients stop deriving env from filesystem |

8 capabilities missing out of ~17 designed. **Phase 3 closes the SubOS gap**, which is the only one blocking xstore from being 100 % interface-driven.

---

## 3. xstore consumption — what's covered, what bypasses

`backends/electron/xlings.ts`, `backends/electron/pty.ts`, `backends/electron/config.ts`, `src/components/*`.

### Through the interface (good path)

| UI feature | Capability used |
|---|---|
| Discover / search | `search_packages` |
| Installed list | `list_packages` |
| Package detail page | `package_info` |
| Version panel + active marker | `list_installed_versions` |
| Get / Install button | `install_packages` (streams to Downloads page) |
| Use button | `use_version` |
| Remove button | `remove_package` |
| Settings → Update Index | `update_packages` |
| Settings → System Info | `system_status` |

That's all 9 capabilities used, and all the package-level UI is interface-driven. Good.

### Bypassing the interface (gaps)

| xstore call site | What it does | Why it has to bypass |
|---|---|---|
| `xlings.ts:20–34 buildXlingsEnv()` | reads `$XLINGS_HOME/subos/current` symlink to know active subos; assembles PATH | no `env` capability |
| `xlings.ts:253–266 list_subos handler` | `fs.readdirSync($XLINGS_HOME/subos/)` | no `list_subos` capability |
| `xlings.ts:268–274 list_subos_shims handler` | `fs.readdirSync($XLINGS_HOME/subos/current/bin/)` | no `list_subos_shims` capability |
| `xlings.ts:276–308` | raw `spawn(xlingsPath, ["subos", "use" \| "new", name])` — non-interface subcommands | no subos write capabilities |
| `xlings.ts:49–54 findXlingsBinary()` | `fs.existsSync` + `which xlings` to locate binary | clients always need a bootstrapping path; acceptable |
| `pty.ts:12–18` | `pty.spawn(shell, args, { env: buildXlingsEnv() })` | env still derived from filesystem; would benefit from `env` capability |

The **only two non-interface xlings invocations** in the whole xstore codebase are `xlings subos use` and `xlings subos new` (xlings.ts:280, 297). Everything else either uses a capability or reads files. That's a tight blast radius — Phase 3 SubOS capabilities + `env` would eliminate it entirely.

### Notes on PTY (xsh)

xsh is correctly out of v1 scope. xstore spawns PTY directly with the env it built; xlings is just on PATH. This is fine — wrapping a PTY into NDJSON is a v2 conversation (run/exec RFC §5.4) and would force base64-encoded stdout chunks etc. xstore today is the right abstraction.

---

## 4. Are the operations actually atomic + composable?

The design doc declares P8 (primitives not endpoints) and P9 (no client specialization). Spot-checking against the implemented set:

- **`install_packages` accepts `targets[]`** — composable (multi-pkg install in one call), not specialized.
- **`use_version`** is a single (target, version) tuple — true atom; clients compose `install_packages` + `use_version` themselves. Good.
- **`search_packages`** returns flat hits without ranking pre-applied; clients can re-rank. Good.
- **`package_info`** returns one package's full record without UI hints. ✓ (modulo the `info_panel` dataKind name that needs renaming).

Counter-examples:

- **`install_packages` mixes plan + download + install + complete events**. Arguably four concerns. Reasonable for now (one user-visible action) but separating into `plan_install` (read-only resolver, returns plan + size estimate) + `apply_install` (executes a plan) would make the API more honest about pre-flight vs apply, and gives clients a "dry-run" without reinventing it.
- **No `list_repos`** means clients can't enumerate index sources before `update_packages` — they'd have to use `search_packages` and parse the namespace prefix ("xim:", "awesome:"). Brittle.
- **`update_packages` with no target updates everything**, no way to query "which repo is stale". A `list_repos` returning per-repo timestamps would close this.

---

## 5. Concrete recommendations (priority-ordered)

**P0 — finish v1 / unblock xstore from filesystem coupling**

1. Implement Phase 3 SubOS capability bundle: `list_subos`, `create_subos`, `switch_subos`, `remove_subos`, `list_subos_shims`. xstore can drop 5 file-system call sites and 2 raw-subcommand spawns.
2. Add `env` capability: returns `{xlingsHome, currentSubos, path, mirrors, lang}` as a structured object. xstore's `buildXlingsEnv()` becomes a one-liner; the same dict can drive PTY env, install env, and any future client.
3. Rename UI-bound dataKinds (`info_panel` → `system_info` / `version_list`; `styled_list` → `package_listed` / `search_results`). Document deprecation; emit both for one minor release. (Phase 4 — bring forward.)

**P1 — make 3rd-party adoption realistic**

4. Migrate `ErrorEvent.code` from int to enum strings (E_NETWORK, E_INVALID_INPUT, E_DISK_FULL, E_NOT_FOUND, E_PERMISSION, E_CANCELLED, E_INTERNAL). Currently every error is `code:1`, opaque to clients.
5. Implement `list_repos` / `add_repo` / `remove_repo`. Even read-only `list_repos` first — most clients only need to know what's installed.
6. Self-contained reference clients (Phase 5): bash-jq one-liner, Python script, Node script. Living documentation that catches protocol drift.

**P2 — design work for v2**

7. Split `install_packages` into `plan_install` + `apply_install`. Enables dry-run, conflict pre-flight, package-manager-style "would do X". Backwards-compat: keep `install_packages` as a convenience wrapper.
8. RFC for `run` / `exec` (PTY over interface). Use case: agents that want to run pkg-installed binaries inside the same env without managing the PATH themselves. Specify byte stream framing (one option: `data.dataKind=stdout/stderr` with base64 chunks).
9. Decide `result.data` vs mid-stream `data` events. Recommend: `data` events are the source of truth (clients accumulate), `result.data` is reserved for summary stats only (counts, durations). Document this clearly.

**P3 — nice to have**

10. `cache_info` / `cache_gc` capabilities once the cache layout stabilizes.
11. Workspace `.xlings.json` operations as a capability set (`workspace_info`, `workspace_install`).
12. JSON Schema files published alongside protocol_version, so clients can validate / generate types.

---

## 6. Verdict on the original question

> 我们目前实现能不能够作为一个通用的一个接口，让外部的其他工具去使用？

**Yes for the package-management hot path** (search/install/use/remove/info) — protocol is solid, semantics are clean, schemas exist. xstore proves it round-trips end-to-end.

**Not yet for a full xlings replacement** — SubOS, env, and repo management are still filesystem/raw-subcommand territory. A 3rd party building a Web/CLI/agent UI today would have to either (a) avoid those features, (b) reimplement xstore's filesystem coupling, or (c) wait for Phase 3.

The single most important next move is shipping Phase 3 SubOS + `env`. That alone takes the API from "good for the happy path" to "complete enough to use as the only contract."
