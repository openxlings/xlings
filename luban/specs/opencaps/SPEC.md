# opencaps Specification, version 0.1

## 1. Scope

opencaps names what one part of a system provides and another needs. The parts
are released apart -- a command-line tool, a window, an agent inside a machine,
a system made last year, a component a community published -- and each must
tell, before it acts, whether the others can do what it is about to rely on,
and if not, which one is behind and how to bring it forward. Versions cannot
say that: parts have versions of their own, and a version says when, not what.

## 2. Capabilities

2.1 A capability is a name and a level. A name is lowercase letters, digits and
`-`, starting with a letter. A level is an integer from 1.

2.2 A level only rises, and each level includes every lower one: level 2 of a
capability is level 1 and what level 2 adds.

2.3 A name, once registered, is never reused for something else, and a level,
once registered, never changes its meaning.

2.4 A part **provides** a set of capabilities, each at one level (the highest it
has). A part **needs** a list of capabilities, each at a least level.

## 3. Text forms

3.1 A need is `<name>>=<level>`, or `<name>` for level 1.
3.2 A provision is `<name>=<level>`, or `<name>` for level 1.
3.3 Whitespace around the parts is not significant; anything else is malformed,
and a malformed need is an error, not an unmet need.

## 4. Meeting a need

4.1 A need is met when the providing side provides its name at its level or
higher.

4.2 A need whose name the checking side does not know is **not met**, and the
answer says that the name is unknown here: the side that wrote the need is
newer than the one checking it. ("This luban does not know the capability X:
update it.")

4.3 A need whose name is known and not provided is not met; the answer says
what is provided (nothing, or a lower level), so the reader can tell which side
is behind.

## 5. The registry

| Name | Level | What it adds | Provided by | Package | Since |
|---|---|---|---|---|---|
| `root-layout` | 1 | A system's root has `/usr` as a link to its generation | a system (its tree); a reader of roots (luban, the Luban OS app) | | Luban 2026.10.11 |
| `root-layout` | 2 | `/usr` is a mount point, the generation mounted on it and recorded in `etc/xlings/usr` | as level 1 | | Luban 2026.10.16.1 |
| `luban-manifest` | 1 | A system package's manifest carries a `luban` object: `settings`, `os`, `requires` | luban | | Luban 2026.10.19 |
| `agent-display` | 1 | A machine's agent opens a desktop session on a display channel and bridges it (opensal.wire) | a machine's agent | | Luban 2026.10.13 |
| `desktop-cursor` | 1 | A system has a default cursor theme (`/usr/share/icons/default`) | a system | `luban:cursors` | Luban 2026.10.16.1 |
| `host-gpu` | 1 | The window that shows a system draws on the host's GPU | the Luban OS app | | Luban 2026.10.19 |
| `app-capabilities` | 1 | The app answers `luban-os __capabilities --json` (version, provides, needs) before it opens a display | the Luban OS app | | Luban 2026.10.19.2 |
| `tray-protocol` | 1 | One tray process per home; its windows register, report jobs and are raised over its socket (NDJSON) | the Luban OS tray and its windows | | Luban 2026.10.19.2 |
| `virtio-fs` | 1 | The kernel mounts virtio-fs shares (`FUSE_FS`, `VIRTIO_FS`) | a machine's kernel | `xim:linux-kernel-virt` | Luban 2026.10.24.2 |
| `virtio-9p` | 1 | The kernel mounts 9p shares over virtio (`NET_9P_VIRTIO`, `9P_FS`) | a machine's kernel | `xim:linux-kernel-virt` | Luban 2026.10.24.2 |
| `virtio-gpu` | 1 | The kernel drives a virtio GPU (`DRM_VIRTIO_GPU`) | a machine's kernel | `xim:linux-kernel-virt` | Luban 2026.10.24.2 |
| `udmabuf` | 1 | The kernel makes dma-bufs of memory (`UDMABUF`) | a machine's kernel | `xim:linux-kernel-virt` | Luban 2026.10.24.2 |
| `pci-sysfs-mmap` | 1 | A PCI device's memory is mapped through sysfs (`PCI`, `SYSFS`): a desktop's frames by reference through ivshmem | a machine's kernel | `xim:linux-kernel-virt` | Luban 2026.10.24.2 |
| `virtio-vsock` | 1 | The kernel speaks vsock to its host (`VIRTIO_VSOCKETS`) | a machine's kernel | `xim:linux-kernel-virt` | Luban 2026.10.24.2 |
| `display-virtual` | 1 | A desktop runs on a virtual display, and displays come and go on the bus (openhal 0.3) | luban | | Luban 2026.10.24.2 |
| `frames-region` | 1 | A desktop's frames by reference, in memory both sides map (opensal 0.7 Region) | xdesk, opensal-wlr; the Luban OS app, luban | | Luban 2026.10.24.2 |
| `desktop-control` | 1 | An agent drives a system's desktop on the bus only with the system's `desktop-control` grant (openbus 0.2 §9.4) | luban (the host's router) | | Luban 2026.10.24.2 |

A machine's kernel provides its capabilities by its build configuration (the
`config` file beside it, `CONFIG_<X>=y`); a machine that needs one its kernel
lacks says which, and the kernel that has it (`luban config <n> --kernel
<version>`), before it starts.

**Package**: for a capability a system has, the package that brings it to a
system that lacks it -- what a reader offers besides upgrading the whole
system (design 2026-10-19 §12.2).

A new capability, or a new level of one, is a row here and a provider that
provides it; nothing else changes.

## 6. Conformance

6.1 A part's provisions name registered capabilities at registered levels.
6.2 A part checks a need against the provisions as §4 says, and reports each
unmet need with its name, the level needed, the level provided (0: none) and
whether the name is known.
