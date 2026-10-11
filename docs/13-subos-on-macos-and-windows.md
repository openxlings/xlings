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

When Windows asks for a restart, restart once, then create the SubOS again. In
agent mode nothing is asked: the command exits 2 and names this setup.

## A Linux SubOS on macOS

The `vz` carrier runs a Linux SubOS in a virtual machine through
Virtualization.framework, driven by a signed helper (`xlings-vm`). The helper is
not published yet; until it is, the carrier is refused with that reason, and a
native SubOS is unaffected.

## Choosing the carrier

```bash
xlings subos new box --carrier local     # this machine (the default where the payloads are native)
xlings subos new box --carrier wsl2      # Windows
xlings subos new box --abi linux         # whatever runs Linux payloads here
```

A payload's target decides: Linux programs on Windows need `wsl2`; on Linux the
carrier is `local`.
