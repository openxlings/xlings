# NDJSON Interface Protocol v1.7

> Status: in force; protocol 1.7 since xlings 2026.10.10.2. Implemented in
> `src/interface.cpp`; `xlings interface --list` is the authoritative schema.

## 1. Overview

`xlings interface` is the structured API for programs: IDE plugins, CI scripts
and AI agents talk to xlings over standard IO. The protocol is **1.7**, over NDJSON
(newline-delimited JSON).

| Protocol | xlings | Change |
|---|---|---|
| 1.0 | 0.4.36 | first version |
| 1.1 | 2026.9.27.1 | added: the `install_targets` event; the third element of an `install_plan` entry, `revision` (§6.3.1) |
| 1.2 | 2026.9.28.1 | added: `update_packages`'s `progress` events (`index_sync`, `index_rebuild`, §6.1.1) and the `download_progress` data event (§6.3.2); no capability writes anything but NDJSON to stdout (§5) |
| 1.3 | 2026.9.28.2 | added: `download_progress`'s `stream` field and a rate limit (§6.3.2); `prevLines` deprecated, always 0 |
| 1.4 | 2026.9.29.1 | added: `install_packages`'s `reconfig` field (§7.3) and the `configure` progress event (§6.1.2); a package already configured in this scope at the current revision does not run config again |
| 1.5 | 2026.9.30.1 | added: `install_packages`'s `hook` progress event (§6.1.2); output of commands a hook runs goes to the hook log, no longer forwarded to stderr as `[stray stdout]` (§5); a download failing on a local write (disk full, no permission) is `E_DISK_FULL` (§6.1.3) |
| 1.6 | 2026.10.9.1 | `subos_exec_output` sends separate stdout / stderr byte chunks while the command runs; a chunk that is not valid UTF-8 is `encoding: base64`; a cancel reaps the child |
| 1.7 | 2026.10.10.2 | added: `install_packages`'s `extract` progress event; `hook` events first after 10 s, then every 30 s (§6.1.2); an unpacking write failure is `E_DISK_FULL` or `E_PERMISSION` by cause; a node whose dependency failed reports that cause and runs no hook (§6.1.4) |

A minor version only adds: a 1.0 client reads 1.1 output unchanged. A client
**probes for a capability** (for example, whether `install_targets` appears)
rather than comparing version numbers.

## 2. Transport

| Direction | Channel | Format |
|---|---|---|
| requests / control | stdin | one JSON object per line |
| responses / events | stdout | one JSON object per line |

- Each line ends with `\n` and has no embedded newline.
- stderr is for debug logs; clients do not parse it.
- The encoding is UTF-8.

## 3. Starting a session

```
xlings interface [--version] [--list] [<capability> --args '<json>']
```

### 3.1 The protocol version

```bash
xlings interface --version
```

prints one line and exits:

```json
{"protocol_version":"1.7"}
```

### 3.2 The capabilities

```bash
xlings interface --list
```

prints the capability list and exits:

```json
{"protocol_version":"1.7","capabilities":[{"name":"install_packages","description":"...","destructive":true,"inputSchema":{},"outputSchema":{}}]}
```

### 3.3 Running a capability

```bash
xlings interface install_packages --args '{"targets":["gcc@14"],"yes":true}'
```

The server streams event lines to stdout until it writes a `result` line, then
exits.

`--args-file <path>` reads the JSON arguments from a file instead of `--args`
(for Windows quoting).

#### 3.3.1 Argument validation (2026.9.20.1+)

Before a capability is dispatched, the server checks:

- **for every capability**: `--args` is valid JSON and a JSON object;
- **for a capability declaring `required`**: no field in `inputSchema.required`
  is **missing** or `null`.

The first applies to every capability because one with no required field **still
reads its optional fields**, and params that failed to parse would silently become
the defaults — `update_packages` would update the whole index instead of the
requested package, `list_packages` would list everything instead of filtering by
`filter`. The request would not have happened, and the answer would look like a
valid one.

`--args ""` (an explicit empty string) is the same as no `--args`, i.e. `{}`;
undeclared extra fields are still accepted (this is `required` plus a top-level
type check, not full JSON Schema validation).

On a failure the server writes one `error` line (`code` `E_INVALID_INPUT`,
`message` listing the missing fields, `hint` giving the capability's whole
`required` set), then `result` (`exitCode` 1), and exits. **The capability is
not run.**

```jsonc
// xlings interface plan_install --args '{"packages":["cpp"]}'
{"kind":"error","code":"E_INVALID_INPUT","message":"missing required field(s): targets","recoverable":false,"hint":"this capability requires: targets — run `xlings interface --list` for the full schema"}
{"kind":"result","exitCode":1}
```

Before 2026.9.20.1 `required` was **published** and never **enforced**: a request
with a misspelt field got `{"exitCode":0,"kind":"result"}`, identical to the valid
and common answer "nothing needed installing", and a client could not tell them
apart (openxlings/xlings#464).

The check covers **only** `required` and the top-level type: `required` was a
published promise, now kept; turning other shapes accepted today into hard errors
is another decision with its own impact. Capabilities without `required` are
unaffected.

## 4. Control requests on stdin

While a capability runs, a client may send:

```json
{"action":"cancel"}
{"action":"pause"}
{"action":"resume"}
{"action":"prompt-reply","id":"<prompt-id>","value":"<answer>"}
```

| action | Meaning |
|---|---|
| `cancel` | cancel the run; the capability throws CancelledException |
| `pause` | pause |
| `resume` | resume |
| `prompt-reply` | answer a prompt the server sent |

An unknown action produces a `kind: log` event at warn level.

## 5. Events on stdout

Each line is a JSON object with a `"kind"` field naming its type. Nothing but
these lines reaches stdout — including output of code running in-process that
xlings does not control (for example an index build script downloaded with the
index and run in the library's Lua sandbox when the cache is invalidated). Such
code writing to the process's stdout is intercepted: output recognisable as
progress becomes a `progress` event, the rest goes to stderr; neither reaches
stdout. (Before, an index rebuild triggered by `update_packages` /
`install_packages` mixed such scripts' raw terminal text — `\r`, ANSI line
clearing — into the NDJSON stream, and clients failed on that line.)

## 6. Event types

### 6.1 `progress`

```json
{"kind":"progress","phase":"downloading","percent":42,"message":"gcc-14.2.0.tar.xz"}
```

| Field | Type | Meaning |
|---|---|---|
| phase | string | the current phase |
| percent | number | an integer 0–100; -1 when unknown |
| message | string | a human-readable description |

#### 6.1.1 `update_packages`'s progress events

While refreshing indexes (without `target`, or before upgrading a `target`),
`update_packages` reports these phases. Each may not appear at all: a local or
offline source has no network bytes, and a valid cache is not rebuilt.

| phase | When | i/n |
|---|---|---|
| `index_sync` | per configured index repository synchronised | its position in its group (top-level `index_repos`, or a batch of discovered sub-indexes) / the group's size |
| `index_rebuild` | when the cache is invalid and `pkgs/` is parsed again | package files done / total |

`percent` is `i/n` (0 when the group is empty); `message` names the repository or
package file, so a client without a dedicated UI can still print a meaningful
line. Byte-level progress of an index artefact download is `download_progress`
(§6.3.2), not this.

`index_rebuild`'s steps come from the index's own `pkgindex-build.lua`. Since
2026.9.29.1 their output reaches xlings through libxpkg's `BuildOutput`, not the
process's stdout; the events' phase and message are unchanged.

#### 6.1.2 `install_packages`'s progress events (1.4+)

During an install, each node that **did something** reports once when done:

| phase | When | message |
|---|---|---|
| `configure` | a node was installed (its payload written now) or configured in this scope (the payload already in the store, mapped in now) | `installed <ns:name@version>` or `configured <ns:name@version>` |

`percent` is nodes done / nodes expected to do something. A node already
configured in this scope at the current revision does nothing and reports nothing
(§7.3). The CLI prints the same as `  [i/n] installed …`.

Since 1.5 a long-running install / config hook adds `hook` events; since 1.7
unpacking an archive adds `extract` events:

| phase | When | message |
|---|---|---|
| `hook` | once the hook has run 10 s, then every 30 s until it ends (15 s and 60 s in 1.5 and 1.6) | `<ns:name@version> <hook> hook running <elapsed>[: <last log line>]` |
| `extract` | every further 1% of the archive read (compressed bytes) | `<ns:name@version> extracting <percent>%` |

The CLI prints `extract` as `  … <ns:name@version> extracting <percent>% (<s>s)`,
with the hook heartbeat's rhythm: first after 10 s of unpacking, then every 30 s;
an unpack finishing within 10 s prints nothing.

`percent` is as for `configure`. Output of commands a hook runs is written to
`<XLINGS_HOME>/logs/hooks/<ns>-<name>@<version>.<hook>.log` (rewritten each run);
before, it was forwarded to stderr by §5's interception (prefixed
`[stray stdout]`), and the hook's stderr arrived as is. When a hook fails, the
`error` event's message carries the last 20 lines and the log's path.

#### 6.1.3 Download failure codes (1.5+)

When a package's download fails, there is exactly one `error` event, its message
the cause (before, there was also an event with an empty message, and a
`download artifact missing`). When the cause is local — a failed write, a file on
disk shorter than what was received, or a disk the server's size shows cannot
hold it — the `code` is `E_DISK_FULL` and `hint` names the download directory;
no other mirror is tried (they would write to the same disk).

#### 6.1.4 Unpacking and dependency failures (1.7+)

When unpacking an archive fails, the `error` event's `code` follows the cause:

| Cause | code | hint |
|---|---|---|
| a corrupt archive or an unsupported entry | `E_INVALID_INPUT` | the cache was deleted; a retry downloads again |
| no space while writing (`ENOSPC`, `EDQUOT`) | `E_DISK_FULL` | free space on the xlings home's file system |
| writing refused (`EACCES`, `EPERM`, `EROFS`) | `E_PERMISSION` | check the owner and permissions of the directory of the file in the message |
| another write failure | `E_INTERNAL` | the system's reason is in the message's parentheses |

Up to 1.6 the last three were all `E_DISK_FULL`. A file system refusing hard
links (Android's app sandbox, FAT, exFAT) is no longer a failure: an archive's
hard-link entry becomes a copy of the file it names, with the same permissions and
modification time.

When a node fails, every node of the plan that depends on it (directly or not)
reports one failure, message `not installed: its dependency <ns:name@version>
failed`, with that dependency's `code`; their hooks do not run, and they no
longer report "installed but registered none of its declared programs".

### 6.2 `log`

```json
{"kind":"log","level":"info","message":"extracting archive..."}
```

| Field | Type | Meaning |
|---|---|---|
| level | string | `debug` / `info` / `warn` / `error` |
| message | string | the message |

### 6.3 `data`

A structured payload, for query results and the like.

```json
{"kind":"data","dataKind":"env","payload":{"xlingsHome":"/home/user/.xlings"}}
```

| Field | Type | Meaning |
|---|---|---|
| dataKind | string | the kind of data |
| payload | object / string | the data; a string when the original JSON failed to parse |

#### 6.3.1 Install data events

`install_packages` (and the CLI's `xlings install`) emits three data events, in
this order.

**`install_plan`**: the nodes to install (dependencies expanded; installed nodes
whose payload is current are not listed). Not emitted when every target is
installed. `plan_install` (dry-run) ends after it.

```json
{"kind":"data","dataKind":"install_plan","payload":{"packages":[["xim:glibc@2.44.3","reinstall: recipe revision 1, installed revision 0",1],["xim:node@22.4.0","",0]]}}
```

Each `packages` entry is an array `[coordinate, note, revision]`:

| Position | Type | Meaning |
|---|---|---|
| 0 | string | `<namespace>:<name>@<version>` |
| 1 | string | the note: the reinstall reason when that version is on disk at another revision, otherwise empty |
| 2 | integer | the packaging revision the recipe declares for that version (1.1+; [xpkg manifest](xpkg-manifest-v1.md), §5.1.1) |

**`install_summary`**: this run's counts. Not emitted when every target is
installed.

```json
{"kind":"data","dataKind":"install_summary","payload":{"success":1,"failed":0}}
```

| Field | Type | Meaning |
|---|---|---|
| success | integer | nodes this run actually installed; a node whose current payload was present does not count |
| failed | integer | nodes that failed |

**`install_targets`** (1.1+): the version and payload each requested target
resolved to. A top-level install emits it once on **every path** — all installed,
a resolution failure, an install failure; a nested install a recipe starts with
`pkgmanager.install()` does not. The install `update_packages` runs to upgrade to
a new version emits it too; `plan_install` (dry-run) does not.

```json
{"kind":"data","dataKind":"install_targets","payload":{"targets":[{"request":"glibc@2.44","namespace":"xim","name":"glibc","version":"2.44.3","revision":1,"status":"installed","payload_dir":"/home/u/.xlings/data/xpkgs/xim-x-glibc/2.44.3"}]}}
```

`targets` matches the request's `targets` one to one, in order. `status` says
whether this run satisfied that request: when the run ends before a plan is made
(for example, another target cannot be resolved), every entry is `failed`, and the
resolved ones still carry `namespace`, `name`, `version`.

| Field | Type | Meaning |
|---|---|---|
| request | string | the caller's original string |
| namespace | string | the package's namespace; empty when unresolved |
| name | string | the package's name; empty when unresolved |
| version | string | the version resolved; empty when unresolved |
| revision | integer | the packaging revision the recipe declares for it |
| status | string | `installed` (by this run) / `already_present` (its current payload was there) / `failed` |
| payload_dir | string | the payload directory, absolute; empty when `failed` |

The three cases:

```jsonc
// a fresh install
{"kind":"data","dataKind":"install_targets","payload":{"targets":[{"request":"xim:node@22","namespace":"xim","name":"node","version":"22.4.0","revision":0,"status":"installed","payload_dir":"/home/u/.xlings/data/xpkgs/xim-x-node/22.4.0"}]}}
// all installed (install_plan and install_summary are not emitted)
{"kind":"data","dataKind":"install_targets","payload":{"targets":[{"request":"xim:node@22","namespace":"xim","name":"node","version":"22.4.0","revision":0,"status":"already_present","payload_dir":"/home/u/.xlings/data/xpkgs/xim-x-node/22.4.0"}]}}
// a failure (here an unknown target; when resolution succeeds and the install fails, namespace/name/version are filled)
{"kind":"data","dataKind":"install_targets","payload":{"targets":[{"request":"xim:nosuchpkg","namespace":"","name":"","version":"","revision":0,"status":"failed","payload_dir":""}]}}
```

#### 6.3.2 `download_progress`

Byte-level download progress, with one shape for `install_packages` and
`update_packages` — the latter reports index artefact downloads with it (when the
index comes as an artefact and the size is known; git and local sources have no
such layer), so a client writes one renderer for both.

```json
{"kind":"data","dataKind":"download_progress","payload":{"stream":"index:xim","files":[{"name":"xim","totalBytes":204800,"downloadedBytes":102400,"started":true,"finished":false,"success":false}],"nameWidth":3,"elapsedSec":1.2,"sizesReady":true,"prevLines":0}}
```

An event carries data, not rendering state (1.3+). The client decides how to draw,
and keeps per `stream`: the previous frame's line count and time, and which tasks
have reported start and end.

The rate is limited: at most one event per `stream` every 100 ms, nothing
repeated while the state is unchanged, and the last event (every task
`finished`) is always sent. An index fetched as an artefact sends a `finished`
event when the fetch completes, whether or not the size was known during it.

| Field | Type | Meaning |
|---|---|---|
| stream | string | (1.3+) the stream the event belongs to: `index:<repository>` for one index fetch, `install:<n>` for one install's batch of downloads; clients keep rendering state per stream. A 1.2 server does not send it; its events are one stream |
| files | array | every download task in this event |
| files[].name | string | the task's label — the repository name for `update_packages`, the file name for `install_packages` |
| files[].totalBytes | number | the total; meaningless while `sizesReady` is false |
| files[].downloadedBytes | number | bytes downloaded |
| files[].started / finished / success | boolean | the task's lifecycle |
| nameWidth | number | the label column width for alignment; structured clients may ignore it |
| elapsedSec | number | seconds since this refresh / install began |
| sizesReady | boolean | whether totals are known (not before the server's response headers) |
| prevLines | number | deprecated (always 0 since 1.3, removed in 2.0). In 1.2 it was the previous frame's line count — renderer state; 1.3 clients ignore it |

### 6.4 `prompt`

The server asks the client for the user's input.

```json
{"kind":"prompt","id":"confirm-remove","question":"Remove gcc@14?","options":["yes","no"],"defaultValue":"no"}
```

| Field | Type | Meaning |
|---|---|---|
| id | string | the prompt's id, quoted in the reply |
| question | string | the question |
| options | array | the choices (may be empty) |
| defaultValue | string | the default |

The client answers on stdin with
`{"action":"prompt-reply","id":"<id>","value":"<answer>"}`.

### 6.5 `error`

Not necessarily terminal — `recoverable` says whether the capability is still
running.

```json
{"kind":"error","code":"E_NOT_FOUND","message":"unknown capability: foo","recoverable":false,"hint":"run `xlings interface --list`"}
```

| Field | Type | Meaning |
|---|---|---|
| code | string | the error code, below |
| message | string | the description |
| recoverable | boolean | true: the run continues |
| hint | string? | an optional fix |

| code | Meaning |
|---|---|
| `E_INVALID_INPUT` | invalid arguments |
| `E_NOT_FOUND` | the target does not exist |
| `E_NETWORK` | a network error |
| `E_DISK_FULL` | not enough disk space |
| `E_PERMISSION` | not permitted |
| `E_CANCELLED` | cancelled by the user |
| `E_INTERNAL` | an internal error |

### 6.6 `result`

The terminal line: exactly one per run.

```json
{"kind":"result","exitCode":0}
```

| Field | Type | Meaning |
|---|---|---|
| exitCode | integer | 0 success, non-zero failure, 130 cancelled |
| data | object? | the capability's return fields other than `exitCode` (omitted when there are none) |

Install results are not in `result` but in `install_targets` (§6.3.1).

### 6.7 `heartbeat`

Sent after 5 s of silence: the process is alive.

```json
{"kind":"heartbeat","ts":"2026-05-17T08:30:00Z"}
```

## 7. Capabilities

| Name | Does | Destructive |
|---|---|---|
| `search_packages` | fuzzy search by keyword | no |
| `install_packages` | install one or more packages | yes |
| `plan_install` | an install dry-run | no |
| `remove_package` | remove a package | yes |
| `update_packages` | refresh indexes or upgrade a package | yes |
| `list_packages` | list installed packages | no |
| `package_info` | a package's details | no |
| `list_installed_versions` | list installed versions | no |
| `use_version` | switch the active version | yes |
| `system_status` | system status | no |
| `list_subos` | list SubOSes | no |
| `list_subos_shims` | the active SubOS's shims | no |
| `create_subos` | create a SubOS | yes |
| `switch_subos` | switch the active SubOS | yes |
| `remove_subos` | remove a SubOS | yes |
| `subos_exec` | run one command in a SubOS from outside it, as argv (§10) | yes |
| `subos_start` | start a SubOS session without a terminal, for a series of `subos_exec` calls (`ttl` ends it after that long idle) | no |
| `subos_stop` | end a SubOS's running session; succeeds when none is running | no |
| `subos_events` | a SubOS's audit events (sessions, commands, permission decisions), recorded outside its sandbox; variable values are never recorded, only names | no |
| `env` | the current environment | no |
| `list_repos` | list index repositories | no |
| `list_index_versions` | published index snapshots and the one in use | no |
| `add_repo` | add or update an index repository | yes |
| `remove_repo` | remove an index repository | yes |

Each capability's `inputSchema` / `outputSchema` comes from
`xlings interface --list`; its `required` is enforced since 2026.9.20.1 (§3.3.1).

### 7.1 Capabilities that need the user's confirmation (2026.9.26.1+)

A SubOS's home is user data; deleted, it cannot be recovered. These two
capabilities act only when the caller passes `"yes": true` (meaning **the user
has confirmed**); without it nothing changes, the answer is `E_INVALID_INPUT` with
exitCode 2, the `message` names the directory that would be deleted or taken over
and how much it holds, and the `hint` says this is the user's decision:

| Capability | When it needs `yes` |
|---|---|
| `remove_subos` | always: it deletes the SubOS's home and every file in it |
| `create_subos` | when the target directory exists and is not empty (an unregistered SubOS directory, or an existing one passed as `dir`): `yes` means the user agrees to take it over as it is |

What an agent does: tell the user what the `message` says, and call again with
`"yes": true` only after they agree. Never add `yes` to make the call succeed.

```jsonc
// xlings interface remove_subos --args '{"name":"dev"}'
{"kind":"error","code":"E_INVALID_INPUT","recoverable":true,
 "message":"removing subos 'dev' deletes /home/u/.xlings/subos/dev (home 2.5 GB in 30256 file(s)); nothing was removed",
 "hint":"this needs the user's confirmation. Tell the user what would be deleted; if they ask for it, call remove_subos again with \"yes\": true"}
{"kind":"result","exitCode":2}
```

Every deletion that happens appends a line to
`<XLINGS_HOME>/logs/destructive.ndjson`: the path, the size, how it was confirmed
(`terminal` / `-y` / `yes:true`), the command line and the parent process.

### 7.2 Removed fields

`install_packages` / `plan_install`'s `noDeps` said "Skip dependency installation"
and never did (dependencies were always installed). Since 2026.9.26.1 it is gone
from the schema; `"noDeps": true` is refused (`E_INVALID_INPUT`, exitCode 2), and
`false` or absent behaves as before.

### 7.3 `install_packages`'s `reconfig` (1.4+)

A payload is shared by the whole home, while "configured in a scope (a SubOS or
a project)" is that scope's fact. Since 2026.9.29.1 each scope records
`configured` (`"<ns>:<name>@<version>": <revision>`) in its own `.xlings.json`
([`.xlings.json`](xlings-json.md)). A node's config is not run again when both
hold:

1. the record's revision equals the recipe's current revision;
2. every ledger entry the payload owns is in this scope's `installed[]`.

When the whole closure satisfies both, `install_packages` only activates and
rebuilds the routing table once, and `install_targets` reports
`already_present` as usual.

| Field | Type | Meaning |
|---|---|---|
| reconfig | boolean | `true`: every node in the plan runs config again (the behaviour before 2026.9.29.1). Default `false` |

No record means "not configured": config runs and writes the record, so older
homes migrate on their own. A recipe that changes what its config does raises
`revision`; each scope reconfigures the next time an install touches the package.

## 8. Errors

- **At start-up** (e.g. an unknown capability): an `error` line, then `result`
  (exitCode 1), then exit.
- **While running**: `error` events in the stream; `recoverable=true` means the
  run continues; `recoverable=false` is usually followed by the `result` line.
- **Arguments missing `inputSchema.required`**: `E_INVALID_INPUT`, exitCode 1, the
  capability not run (§3.3.1).
- **A confirmation needed and not given** (2026.9.26.1+): `E_INVALID_INPUT`,
  exitCode 2, nothing changed (§7.1).
- **A dependency that cannot be resolved** (2026.9.26.1+): the `message` starts
  with the dependency chain, e.g. `xim:xmake@3.1.1 -> ncurses: ...`; one failure is
  reported once.
- **An internal exception**: `E_INTERNAL`, exitCode 1.
- **An unpacking failure** (2026.9.20.1+): no longer always `E_INTERNAL` — a
  corrupt archive or an unsupported entry is `E_INVALID_INPUT` (the `hint` says the
  cache was cleared and a retry downloads again); write failures follow §6.1.4.
  Before, `ExtractError`'s kind was lost on the way, and a client could not tell
  "retry" from "free disk space" (openxlings/xlings#376).
- **Cancelled**: exitCode 130.
- **Refused by policy**: exitCode 2. The command was valid and nothing went wrong,
  but xlings will not do it. It is separate from 1 because a client does different
  things: 1 is "something went wrong", 2 is "I will not do this unless you
  explicitly override".

### 8.1 `remove_package` refusing for reverse dependencies (2026.8.8.1+)

When an installed package in the active SubOS **directly depends** on the target,
`remove_package` refuses: it sends `remove_blocked`, then ends with exitCode 2:

```json
{"kind":"data","dataKind":"remove_blocked","payload":{
  "subos":"default","name":"glibc","version":"2.39",
  "required_by":[{"name":"xim:binutils","version":"2.42"},
                 {"name":"xim:llvm","version":"22.1.8"}]}}
{"kind":"result","exitCode":2}
```

`{"force": true}` skips the check (`remove_plan` / `remove_summary` as usual).

Only **direct** dependants count: a dependant's libdirs enter its payload's RPATH
closure only when declared directly, so a package one hop away has this payload on
no search path, and removing it cannot affect that package through the loader.

## 9. A complete session

```
$ xlings interface install_packages --args '{"targets":["node@22"],"yes":true}'
```

stdout (one JSON per line):

```json
{"kind":"progress","phase":"resolving","percent":0,"message":"resolving node@22"}
{"kind":"progress","phase":"downloading","percent":25,"message":"node-v22.4.0-linux-x64.tar.xz"}
{"kind":"progress","phase":"downloading","percent":80,"message":"node-v22.4.0-linux-x64.tar.xz"}
{"kind":"log","level":"info","message":"extracting node-v22.4.0-linux-x64.tar.xz"}
{"kind":"progress","phase":"installing","percent":90,"message":"linking shims"}
{"kind":"data","dataKind":"install_summary","payload":{"success":1,"failed":0}}
{"kind":"data","dataKind":"install_targets","payload":{"targets":[{"request":"node@22","namespace":"xim","name":"node","version":"22.4.0","revision":0,"status":"installed","payload_dir":"/home/u/.xlings/data/xpkgs/xim-x-node/22.4.0"}]}}
{"kind":"result","exitCode":0}
```

After `kind: result` the client may close stdin and exit.

## 10. Running a command in a SubOS

`subos_exec` takes `argv` (no shell quoting) and joins the SubOS's running session
when there is one (`subos_start`). Output is sent as `dataKind: subos_exec_output` chunks.
`payload.stream` is `stdout` or `stderr`, `payload.data` the chunk, which may not
end with a newline. An ordinary chunk is UTF-8; a chunk with
`payload.encoding: base64` is decoded first, then its bytes joined to its stream.
A UTF-8 character split across chunks is decoded after joining. The final
`result.exitCode` is the command's own; 125 when it never started, 126 / 127 when
it cannot run / is not found, 124 on a timeout, 128+n for signal n; stdin is not given to the command,
and a `cancel` on the control channel ends the call with `E_CANCELLED` and 130.

```json
{"kind":"data","dataKind":"subos_exec_output","payload":{"stream":"stdout","data":"ready"}}
{"kind":"data","dataKind":"subos_exec_output","payload":{"stream":"stderr","encoding":"base64","data":"/w=="}}
{"kind":"result","exitCode":0}
```
