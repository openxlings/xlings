# Architecture

**English** | [简体中文](../zh/luban/91-architecture.md)

For developers: how Luban is put together. What luban adds over xlings is
small on purpose.

## Where the code is

`luban/` holds three kinds of package (`luban/README.md`):

| Package | What |
|---|---|
| `luban/specs/opensal` | a surface: given pixels, gives back events; `opensal.wire` across an address space |
| `luban/specs/openhal` | devices: which GPUs, displays, audio endpoints and input devices a machine has |
| `luban/specs/openas` | actors and what they do: the actor and its delegation, effect classes, the envelope of a run |
| `luban/kits/openhal-host` | openhal's source on Linux, macOS and Windows |
| `luban/kits/opensal-wlr`, `xdesk-base`, `xdesk-layout`, `xdesk-compositor` | a wlroots compositor as an opensal surface; the xdesk desktop |
| `luban/os/system` | stage-0 (the first process), boot entries, a machine's `/etc`, archives |
| `luban/os/service` | environments and machines as operations with events; what this computer can do (`luban status`) |
| `luban/os/vm` | machines: supervisor, terminal relay, guest sessions, hypervisors |
| `luban/os/cli` | the `luban` command: one declared tree; each command's effect; `luban interface` for programs |
| `luban/os/desktop` | the Luban OS app's window (SDL3) and Dear ImGui |
| `modules/image` | images, in-process: GPT, FAT, ISO 9660 with El Torito, qcow2 |
| `apps/luban` | the binary: luban, or stage-0 when started as `luban-init` |
| `src/core/subos/root` | the xlings side of a root: projection, generations, rollback, export |

A specification imports only the standard library and itself; a kit never
imports xlings or Luban; the OS service imports no presentation; nothing in
`luban/` imports the xlings frontend (what is on stage-0's path cannot read
Config). `tools/lint_layer_deps.sh` checks each rule.

## An environment from an edition

1. The edition's template is read; its ABI and architectures are checked against
   this machine, and the host is checked (the one-time setup asked) — before any
   download.
2. The root is made: the edition's `from` chain, its packages installed as one
   plan (in-process, one terminal), its policy selected and locked.
3. `instance.json` records the edition: its reference, the chain, each package as
   installed, the policy. `luban upgrade` plans from that record: what the
   edition brought and you left alone moves, what you moved stays.

## An image

1. The boot profile is installed into the root (its `share/luban/boot.json` names
   the kernel release and the command line).
2. The root is staged: the instance becomes the image's default, with its closure.
3. The image is written in-process by `luban.image`: for a drive, GPT with an EFI
   system partition (FAT, written here) and an ext4 root by PARTUUID; for an ISO,
   ISO 9660 with El Torito entries (BIOS where the machine has one, UEFI always)
   and a hybrid GPT whose EFI system partition is the El Torito image.
4. On x86_64, `limine bios-install` makes it boot on BIOS from a drive.

## Versions

luban is released with the xlings beside it, at the same version; editions,
policies and boot profiles are dated on their own, and a published version's
output never changes (the index checks every published version's files).
