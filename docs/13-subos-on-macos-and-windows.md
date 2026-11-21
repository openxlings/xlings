# SubOS on macOS and Windows

**English** | [简体中文](zh/13-subos-on-macos-and-windows.md)

A SubOS's content is the same everywhere; what a platform can enforce, and
where a SubOS runs, differ.

## What each platform enforces

| Platform | Sandbox | Kernel-enforced | Sessions |
|---|---|---|---|
| Linux | bwrap, landlock, proot | yes (proot excepted) | yes |
| macOS 14+ | the home redirected | no (advisory) | no |
| Windows | `%USERPROFILE%` redirected | no (advisory) | no |

On macOS and Windows `--sandbox` keeps tools from writing into your real home;
it does not contain code that tries to escape. `xlings subos status <name>` and
`xlings subos doctor` show what this machine actually gives — the table they
print comes from the backends' own probes.

## A Linux SubOS on Windows

A SubOS whose payloads are Linux programs runs in a **carrier**: on Windows, a
WSL2 distribution of the home's own, which you never enter. `xlings subos …` on
Windows forwards to the xlings inside it.

```powershell
xlings subos new box --carrier wsl2 --rootfs --from subos:luban-tiny
xlings subos exec box -- uname -a
```

Isolation from Windows is the distribution's configuration: no automount of the
Windows drives, no interop (running a `.exe` from inside). A Windows directory
reaches a SubOS only as a grant, one mount of exactly that directory.

### The one-time setup

The first Linux SubOS on a Windows machine needs WSL2. xlings brings it: WSL from
the index (`xim:wsl-installer`, Microsoft's installer, pinned by its digest) and
the Virtual Machine Platform it runs on, each through the elevation prompt (UAC).
`subos new` asks for it where it is needed, or run it yourself:

```powershell
xlings self doctor --isolation          # is WSL2 ready here
xlings self doctor --isolation --fix    # install WSL and turn on the Virtual Machine Platform
```

When Windows asks for a restart, `subos new` exits 3: restart once, then create
the SubOS again. In agent mode nothing is asked: the command exits 2 and names
this setup.

## A Linux SubOS on macOS

The `vz` carrier runs a Linux SubOS in the Luban host machine: one small Linux
machine per home, run by Virtualization.framework through the release's luban
(`luban __vm`, signed with the virtualization entitlement). Its files are on a
case-sensitive APFS volume under `<home>/carriers/vz` (made and attached with
the system's `hdiutil`), shared with the machine by virtio-fs; the first use
fetches the Linux build of this release and the machine kernel. A Mac that
cannot run a virtual machine (an Intel Mac, or one that is itself a virtual
machine) is refused with the reason, and a native SubOS is unaffected.

## What the host machine is, and what you see from here

The Luban host machine (WSL2's distribution, the Mac's machine) is not a Linux
distribution of its own: it holds a static xlings and luban, busybox, bwrap and the
home's runtime (`/xlings/abi`; `/lib64` there points at it) -- none of this
computer's tools. A package's hooks run there, so a recipe that assumes a
distribution's tools behaves differently than on a Linux desktop.

- **Errors come back here.** A command asked through the interface (luban, an
  agent) runs there as `xlings interface`, and what it says reaches you as its
  own events: a failed hook's message and the end of its output, not "no error
  detail". The log it names is fetched into this home,
  `<home>/logs/carriers/<carrier>/`, and the message names that copy; what the
  host machine printed outside its events is kept in
  `<home>/logs/carriers/<carrier>/stderr.log`.
- **It stays at this release.** Before a call, a host machine that reports
  another release is given this computer's Linux build of it (fetched with the
  release, checked by its digest; granted read-only, its two programs replaced by
  rename) -- no network in the host machine. It also brings what it holds besides
  xlings to this release's revision (busybox's applets, the `/lib64` link). If that
  fails, the call is refused with the reason and the log
  (`<home>/carriers/<carrier>/update.log`), and it is tried again on the next call;
  `luban status` says the host machine's xlings and an update that did not take. A
  host machine newer than this client is left as it is.

## Choosing the carrier

```bash
xlings subos new box --carrier local     # this machine (the default where the payloads are native)
xlings subos new box --carrier wsl2      # Windows
xlings subos new box --abi linux         # whatever runs Linux payloads here
```

A payload's target decides: Linux programs on Windows need `wsl2`; on Linux the
carrier is `local`.
