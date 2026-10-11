# Architecture

**English** | [简体中文](../zh/luban/91-architecture.md)

For developers: how Luban is put together. What luban adds over xlings is
small on purpose.

## Where the code is

| Package | What |
|---|---|
| `luban/src/cli` | the `luban` command: one declared tree (levels common, more, expert); each environment command builds the xlings command it is and runs it on the same terminal |
| `luban/src/image` | images, in-process: GPT, FAT16 with long names, ISO 9660 El Torito entries and the hybrid GPT, qcow2; the table of what differs between machines (UEFI loader, BIOS or not, qemu machine, firmware) |
| `luban/src/boot` | boot entries (`boot.json`: default, fallback, a trial) |
| `luban/src/stage0` | the first process; reads nothing before `/proc` is mounted |
| `luban/src/machine` | the machine's `/etc` (factory files) and system users |
| `apps/luban` | the binary: luban, or stage-0 when started as `luban-init` |
| `src/core/subos/root` | the xlings side of a root: projection, generations, rollback, boot, export |

`luban/` imports `modules/` and never the xlings frontend: what is on stage-0's
path cannot read Config. A build rule (`tools/lint_layer_deps.sh`) checks it.

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
