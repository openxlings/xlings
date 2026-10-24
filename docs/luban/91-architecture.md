# Architecture

**English** | [简体中文](../zh/luban/91-architecture.md)

For developers: how Luban is put together. What luban adds over xlings is
small on purpose.

## Where the code is

`luban/` holds four kinds of package (`luban/README.md`): specifications, kits (libraries), services and the OS:

| Package | What |
|---|---|
| `luban/specs/opensal` | a surface: given pixels, gives back events; `opensal.wire` across an address space |
| `luban/specs/openhal` | devices: which GPUs, displays, audio endpoints and input devices a machine has |
| `luban/specs/openas` | actors and what they do: the actor and its delegation, effect classes, the envelope of a run |
| `luban/specs/openbus` | the bus: objects by path and interface, call, publish, stream, event, region, links between routers |
| `luban/kits/openhal-host` | openhal's source on Linux, macOS and Windows |
| `luban/kits/openhal-emu` | devices that are not there: a virtual display (an opensal provider nobody sees, its picture as PNG), input as steps |
| `luban/kits/opensal-wlr`, `imkit`, `tray` | a wlroots compositor as an opensal surface; widgets; a status-area icon |
| `luban/kits/telemetry` | what a program says of itself: metrics, a flight recorder, a structured log, profiles, dumps |
| `luban/services/xbus` | XBus, openbus's implementation: the router, the node library, `xbus` (a service: a process with an endpoint, reached by its library) |
| `luban/xdesk/{base,layout,compositor,app}` | the xdesk desktop: its libraries and its program |
| `luban/os/system` | stage-0 (the first process), boot entries, a machine's `/etc`, archives |
| `luban/os/service` | environments and machines as operations with events; what this computer can do (`luban status`) |
| `luban/os/vm` | machines: supervisor, terminal relay, guest sessions, hypervisors |
| `luban/os/cli` | the `luban` command: one declared tree; each command's effect; `luban interface` for programs |
| `luban/os/window` | the Luban OS app's window (SDL3) and Dear ImGui |
| `luban/os/compat` | what is kept only for a released older part, with when it goes |
| `modules/image` | images, in-process: GPT, FAT, ISO 9660 with El Torito, qcow2 |
| `luban/host/luban` | the binary: luban, or stage-0 when started as `luban-init` |
| `luban/host/luban-os` | the Luban OS app: its windows, and its tray (`luban-os __tray`) |
| `src/core/subos/root` | the xlings side of a root: projection, generations, rollback, export |

A specification imports only the standard library and itself; a kit never
imports xlings, Luban or a service; the OS service imports no presentation; nothing in
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

## A desktop's displays

A running desktop has one supervisor, `luban __desktop-session <home> <name>`,
detached like a machine's. It offers the system's virtual display on the bus
(`host/<host>/system/<name>/device/display/<n>`, openhal 0.3) and the
desktop's own service (`.../service/desktop`: status, attach, detach, end),
and starts the desktop in the system as `luban __desktop <session> --display
<path>`: through the system's socket on the host's router (a sandbox) or the
machine's agent (a machine), that command opens the display's stream and gives
it to the session as a window's stdin and stdout would be (opensal.wire).
Every display comes the same way -- a Luban OS window asks to be the primary
(the desktop takes its size), another virtual display mirrors or extends it (an
output of xdesk's own beside the primary). Frames go by reference where both
sides map one region (opensal 0.7): a sandbox's frames file bound into it, a
machine's ivshmem memory (the guest maps the PCI device's BAR); only where the
rows are crosses the stream. What drives a desktop -- input, displays,
xdesk's control -- is an agent's only with the system's `desktop-control`
grant, decided by the host's router and recorded.

## The app's processes

Every Luban OS window is a process of its own; one more, `luban-os __tray`,
holds the status-area icon for all of them and draws nothing else. The tray is
one per home: whoever holds `<home>/run/luban-os/tray.lock` is it. Windows
reach it over `tray.sock` (a local stream socket, owner-only; on Windows an
AF_UNIX one), in lines of JSON: a window says who it is and what it shows
(`hello`, `register`), what it is doing (`job`), what to tell the person
(`notify`); the tray asks a window to come forward on a view (`raise`), to ask
whether to quit (`confirm-quit`), to end (`close`). Liveness is a lock -- each
window holds `windows/<pid>.lock` -- never a pid. A window that finds no tray
starts one; the lock keeps it to one. The tray ends when no window, no running
system and no *Always in the tray* need it, and becomes a newer app as soon as
one is installed.

## Versions

luban is released with the xlings beside it, at the same version; editions,
policies and boot profiles are dated on their own, and a published version's
output never changes (the index checks every published version's files).
