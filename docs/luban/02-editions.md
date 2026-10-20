# Editions and distributions

**English** | [简体中文](../zh/luban/02-editions.md)

A Luban system is made from a **system package** of an index: a template for a
root (the packages it brings, the policy it is made under, how it boots, what
it presets). Luban's own are its **editions**; anyone's are distributions
based on Luban. Desktops and other components are packages that go into a
system. Everything is an xpkg (`type = "subos"` for a system).

## The official editions

Luban's editions are in the `luban:` namespace of the official index; a short
name in luban is a name there (`--edition core` is `luban:core`).

| Edition | What it adds | Presets | x86_64 | aarch64 |
|---|---|---|---|---|
| `nano` | xlings and luban — nothing else, not even a shell | console | yes | yes |
| `tiny` | BusyBox, glibc, CA certificates, BusyBox init | console, `/bin/sh` | yes | yes |
| `tiny-musl` | tiny with musl instead of glibc | console, `/bin/sh` | yes | yes |
| `core` (the default) | bash, fish, vim, nvim, git, gh, mcpp, ninja, coreutils, gcc, binutils, make, openssl | console, fish | yes | yes |
| `agent-workspace` | core, the development and agent tool sets, the `agent-private` policy | console, fish | yes | yes |
| `desktop` | core, the xdesk desktop (labwc on aarch64), foot, Xwayland, the cli, development and agent tool sets | machine, desktop `xdesk-classic`, fish | yes | yes |

Each starts `from` the one below it (`core` from `tiny`, `tiny` from `nano`);
an edition built for another architecture than this machine's is refused before
anything is made. A desktop is not an edition: an edition presets one, and
`--desktop` chooses another ([The desktop](08-desktop.md)).

The editions published before 2026.10.18.1 (`subos:luban-core`,
`subos:luban-desktop-xdesk`, …) are frozen: kept, resolved, never updated. Each
`luban:` edition says which it replaces, and `luban upgrade` moves a system made
from one to its successor, keeping what it showed (a desktop-xdesk system starts
in the WuXian view).

## Any index's system: `--xpkg`

```bash
luban new box                                   # luban:core
luban new box --edition desktop --desktop lxqt  # Luban's desktop edition, with LXQt
luban new box --xpkg myindex:myos               # a distribution from an index you added
luban new box --xpkg myindex:myos --xpkg luban:kde --xpkg xim:htop   # and components into it
```

Before anything is made, luban reads the system's chain (`xlings subos new
--dry-run`): what it is, what it is based on, what it presets, what it needs.
A system or component from another index than the official one is shown with
where it comes from and is agreed to once (`--yes` agrees; an agent must pass it);
the agreement is kept per package and version.

```
 MyOS 1.0.0                                   from myindex
 Based on     Luban core 2026.10.18.1         official
 It will be   sandbox · login shell /usr/bin/fish · 12 packages
```

A system whose chain reaches no official edition is refused ("not built on
Luban"); one that needs what this luban lacks says what (`luban.requires`,
below) instead of half-working.

## Versions

- An edition is versioned by the date it was published (`2026.10.18.1`), and a
  published version's output never changes.
- `luban new box --edition core` takes the edition's `latest`;
  `--xpkg luban:core@2026.10.18.1` takes that version.
- An edition that needs a newer client than yours says so (`min_client`) and
  names the upgrade, instead of half-working; with no version named, the newest
  this client can use is taken and the one passed over is said.
- A system remembers what it was made from — every layer of its chain and the
  index of each — and `luban upgrade` moves it to the newest ([03](03-environments.md)).

## What is pinned

An edition's packages are pinned to exact versions, so a system made today and
one made next month from the same version are the same. The agents of the agent
tool set are the exception: they change weekly, so the newest at creation is
installed and recorded, and `luban upgrade` moves them.

## A distribution of your own

A system package is a manifest; a distribution based on Luban starts `from` an
official edition:

```json
{ "subos_kind": "rootfs", "from": "luban:core@2026.10.18.1",
  "abi": { "kernel": "linux", "libc": "gnu" },
  "packages": [ "xim:htop@3.5.3", "luban:lxqt@2.4.0-r2" ],
  "luban": { "os": { "name": "MyOS", "version": "1.0.0" },
             "settings": { "desktop": "lxqt", "enter": "desktop" },
             "requires": [ "root-layout>=2" ] } }
```

| Key | |
|---|---|
| `subos_kind` | `rootfs`: a system that is a root |
| `from` | the system below it (an official edition, for a distribution based on Luban) |
| `abi` | the kernel and the libc it needs (an architecture-neutral system names no machine) |
| `packages` | what it brings, pinned |
| `policy` | the policy it is made under (`xim:agent-private@…`) |
| `boot` | its init, and the boot profile an image of it uses |
| `min_client` | the oldest xlings that understands it |
| `replaces` | the package it succeeds (honoured within one index) |
| `luban.os` | how it names itself: `luban status` says "MyOS 1.0.0 (Luban core)", and its `/etc/os-release` is its own |
| `luban.settings` | what it presets for `luban config` (`mode`, `enter`, `desktop`, `login_shell`, `persist`, `on_close`): below what the person sets |
| `luban.requires` | capabilities it needs of whoever reads or runs it, by name and level ([opencaps](https://github.com/openxlings/xlings/tree/main/luban/specs/opencaps)): an unknown name is unmet |

The root's `usr/share/luban/settings.json` holds the same presets for an older
luban, and `share/wayland-sessions/<id>.desktop` the desktop sessions its
packages bring ([The desktop](08-desktop.md)).

Make a system from it, adjust it, and publish it in your index:

```bash
xlings subos new mydistro --rootfs --from luban:core
xlings subos pack mydistro --as myns:mydistro@2026.10.18.1
```

Then anyone with your index can `luban new box --xpkg myns:mydistro`. The
official index reserves `luban`, `xim`, `subos` and `config`: a package in them
from another index is refused. The index's
[Luban packages](https://github.com/openxlings/xim-pkgindex/blob/main/docs/12-luban-packages.md)
chapter covers editions, desktop components, policies and boot profiles as recipes.
