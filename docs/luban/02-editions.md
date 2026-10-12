# Editions

**English** | [简体中文](../zh/luban/02-editions.md)

An edition is a template for an environment: the packages it brings, the policy
it is made under, how it boots. Editions are packages of the index
(`type = "subos"`, namespace `subos`).

## The official editions

| Edition | `luban new box …` | What it adds | x86_64 | aarch64 |
|---|---|---|---|---|
| `luban-nano` | `nano` | xlings and luban — nothing else, not even a shell | yes | yes |
| `luban-tiny` | `tiny` | BusyBox, glibc, CA certificates, BusyBox init | yes | yes |
| `luban-tiny-musl` | `luban-tiny-musl` | tiny with musl instead of glibc | yes | yes |
| `luban-core` | `core` (the default) | bash, fish, vim, nvim, git, mcpp, ninja, coreutils, gcc, binutils, make, openssl | yes | yes |
| `luban-agent-workspace` | `agent-workspace` | core, an agent (claude), and the `agent-private` policy | yes | yes |
| `luban-desktop` | `desktop` | core and a desktop (labwc, a panel, a launcher, a terminal), run as a machine | yes | yes |

Each starts `from` the one below it (`core` from `tiny`, `tiny` from `nano`);
an edition built for another architecture than this machine's is refused before
anything is made.

## Versions

- An edition is versioned by the date it was published (`2026.10.11.2`), and a
  published version's output never changes.
- `luban new box --edition core` takes the edition's `latest`; `subos:luban-core@2026.10.11.1`
  takes that version.
- An edition that needs a newer client than yours says so (`min_client`) and
  names the upgrade, instead of half-working.
- An environment remembers which edition and version it came from, and what that
  brought; `luban upgrade` moves it to the newest ([03](03-environments.md)).

## What is pinned

The edition's packages are pinned to exact versions, so an environment made
today and one made next month from the same version are the same. The agent of
`agent-workspace` is the exception: it changes weekly, so the newest at creation
is installed and recorded, and `luban upgrade` moves it.

## An edition of your own

An edition is a manifest:

```json
{ "subos_kind": "rootfs", "from": "subos:luban-nano",
  "abi": { "kernel": "linux", "libc": "gnu" },
  "packages": [ "xim:glibc@2.44.3", "xim:busybox@1.35.0" ],
  "boot": { "init": "/sbin/init", "profile": "xim:luban-boot-generic" } }
```

| Key | |
|---|---|
| `subos_kind` | `rootfs`: an environment that is a root |
| `from` | the edition below it |
| `abi` | the kernel and the libc it needs (an architecture-neutral edition names no machine) |
| `packages` | what it brings, pinned |
| `policy` | the policy it is made under (`xim:agent-private@…`) |
| `boot` | its init, and the boot profile an image of it uses |
| `min_client` | the oldest xlings that understands it |

Two files of the root say how it runs, and neither is code:

- `usr/share/luban/settings.json` — the edition's defaults for `luban config`
  (`mode`, `persist`, `desktop`, `on_close`); what a user sets wins, and they
  follow an upgrade. The desktop edition's is `{ "mode": "machine", "desktop": "labwc-desktop" }`.
- `share/wayland-sessions/<id>.desktop` — a desktop session, from a package
  ([The desktop](08-desktop.md)).

Make an environment from it, adjust it, and publish it:

```bash
xlings subos new mydistro --rootfs --from subos:luban-nano
xlings subos pack mydistro --as myns:mydistro@2026.10.12.1
```

Then anyone with your index can `luban new box --edition myns:mydistro` and export it.
The index's [Luban packages](https://github.com/openxlings/xim-pkgindex/blob/main/docs/12-luban-packages.md)
chapter covers editions, policies and boot profiles as recipes.
