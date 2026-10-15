# Upgrade and compatibility

> Status: in force since xlings 2026.10.12.2. Implemented in
> `src/core/xself/upgrade.cpp` (the upgrade steps), `src/core/home_health.cpp`
> (the home config's health), `src/main.cpp` (the home layout) and the
> specifications each surface below links to.

How a new xlings takes over a home an older one wrote, what an older xlings
does with state a newer one wrote, and how a failure stays small. The rules
follow what long-lived package managers and runtimes converge on: migrations
that come with the new version and run by themselves, readers that tolerate
what they do not know, writers that declare the reader they need, failures
confined to the part that failed, and a way back.

## 1. Failure domains

The state of a home is divided into domains. A domain that cannot be read or
used fails on its own: the commands that do not need it run, and every command
that touches it says which file, what falls back while it is broken, and the
command that repairs it. **A domain that cannot be read is never treated as
empty, and never silently.**

| Domain | When it is broken | Repair |
|---|---|---|
| the home config `<home>/.xlings.json` (not readable JSON, or a field of the wrong shape) | every command runs and prints one line naming it; settings, the SubOS registry and the version records fall back to defaults while it is | `xlings self doctor --fix` keeps it as `.xlings.json.corrupt-<time>` and rebuilds the version records from `data/versions.json` and the SubOS registry from `subos/` |
| one SubOS's state | that SubOS is unusable and `self doctor` names it | `xlings self doctor --fix` keeps the file and writes a fresh one; the SubOS's home is never touched |
| one payload | that package is unusable | `xlings install <package>` |
| one policy file | that instance refuses entry and says why | `xlings subos config <name> --reset` |
| a theme | the default theme, said once | `xlings config --theme default` |
| an upgrade step (§2) | it becomes a pending item | the item's command |
| the index | the previous snapshot, or the one this client is routed to | `xlings update` |

## 2. The upgrade

After the first command a client runs on a home that changes it and succeeds
(a read-only or a refused command leaves the home exactly as it was), when the
home's last upgrade was by another version (`upgrade.to` in the home config;
`verifiedBy` or `version` when that is absent):

| The recorded version is | What happens |
|---|---|
| this one | nothing |
| absent (a new home) | this version is recorded |
| newer (a downgrade) | nothing is changed; said once: the newer version, and `xlings use xlings <it>` when it is installed, `xlings self update` otherwise |
| older | the steps below |

The steps run in order. Each is a failure domain: an exception or a refusal
becomes a pending item with its command and stops neither the next step nor
the command the user ran.

1. The home config's health (§1). When it is broken, the steps that read it are skipped.
2. Derived state is rebuilt from the records: the command routes, a root's
   generation. These are idempotent, so they are redone rather than migrated.
   Under the home's lock; when another xlings holds it, nothing is recorded and
   the next command runs the steps.
3. A read-only check by the same detector `xlings self doctor` runs: each error
   or warning that has a repairing command becomes a pending item.

The result is recorded as `upgrade: {from, to, at, pending: [{what, command}]}`
in the home config and appended to `<home>/logs/upgrade.ndjson`. When nothing is
pending the upgrade says nothing. Otherwise it says once: each item (the first
five), each distinct command, and `xlings use xlings <from>` when that version
is still installed.

The steps do not run in a command another xlings started while holding the
home's lock (a hook's, `self update`'s own steps: the outermost command does
it), inside a SubOS session (the session's home is its owner's,
upgraded by the owner's next command), after a session's command (`subos use`,
`exec`, `start`, `stop`: their exit code is the program's), nor for `xlings self
...` and `xlings interface`, which report the same facts as part of their own
work.

## 3. Compatibility

| Surface | Version | A client that is too old |
|---|---|---|
| the home layout (`.xlings-home`) | an integer, raised when a layout is added | reads it, never writes it, and names the home's own client |
| a policy file | `min_client` | refuses to enter, with `xlings self update` |
| an edition (SubOS template) | `min_client` | refuses before anything is fetched, with `xlings self update` |
| an index snapshot | the [index version contract](index-version-contract.md) | is routed to a snapshot it can read |
| the NDJSON interface | `protocol_version`, additions only within 1.x ([interface](interface-ndjson-v1.md)) | sees fields it ignores; a client probes capabilities, never versions |
| a recipe | `revision` per version ([xpkg manifest](xpkg-manifest-v1.md)) | — a changed revision reconfigures every scope on its next install |
| the version records | `data/versions.json` stamped against the home config | falls back to the home config's field (the source every client writes) |
| the runtime indirection (2026.10.15) | `<home>/abi` -> `abi.gen/<n>`, `abi.json`, the record's `source: "scope"` | a payload bound through it runs on any client (its loader is an ordinary path); an older client rebinds such a package by path when it reinstalls it, which is still correct for that home |
| a Luban system in the system domain (2026.10.15) | its control record's `prefix_domain` | an older `luban` lists it without its edition; `xlings subos` routes it through the domain as before |
| administrator steps (2026.10.15) | `state/admin-steps.json` | ignored: the steps stay until a client that knows them runs `self doctor --fix` |

Rules:

- **Readers tolerate.** A field a client does not know is kept and passed
  through; a value it does not know refuses only the thing that holds it.
- **Writers declare.** State that an older client would misread carries the
  version that reads it (`min_client`, a layout), so the older client refuses
  with the command that fixes it instead of guessing.
- **Additions only within a major version** of a protocol or a format.
- **Compatibility code says when it goes**: `COMPAT(<since> … drop in <YYYY.M>)`,
  kept at least six months after the release that made it unnecessary.

What moves forward on its own, without a layout change (2026.10.15):

- **Payloads already bound by path** keep running: their glibc is still where
  they name it. New installs are bound through `<home>/abi`; nothing rewrites
  an installed payload.
- **A Luban system made before** stays where it is and keeps working;
  `luban upgrade <n> -y` moves it into the system domain (an archive of its
  `/etc`, `/home`, `/root`, `/var`, its policy, settings and own packages,
  kept until the new one stands, named if it does not).
- **`/etc/hostname = luban`**, the editions' placeholder, is read as no name:
  the system's own is used. `etc/profile.d/luban.sh` is written when missing,
  never rewritten.

## 4. Releases

- A release is `YYYY.M.D.N`, published through the index's `latest` ref, its
  assets on GitHub and mirrored to GitCode, with a cross-region fallback.
- Before it is published: the pull request's CI (Linux, macOS, Windows,
  aarch64, the static performance lane), the release candidate's own install,
  and the host matrix (smoke on every pull request; every leg nightly and on
  demand), whose `oldhome` scenario upgrades a home an older release filled.
- After: the fresh-install suite installs whatever `latest` resolves to; it
  never pins the version under test.

## 5. When an upgrade cannot be done in place

| Situation | The command |
|---|---|
| a pending item | the item's command, usually `xlings self doctor --fix` |
| the new version misbehaves | `xlings use xlings <previous>` |
| `self update` cannot reach the index | `xlings config --mirror CN` or `GLOBAL`, then `xlings self update` |
| `self update` cannot install | the install script, which installs over the home and keeps everything in it: `curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh \| bash` (Windows: `irm …/quick_install.ps1 \| iex`) |
