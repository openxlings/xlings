# Supported Platforms

**English** | [简体中文](zh/51-supported-platforms.md)

## Platforms

| Platform | Release | Packages | SubOS sandbox | Luban roots and images |
|---|---|---|---|---|
| Linux x86_64 | static (musl) | yes | bwrap, landlock, proot | yes; images boot on BIOS and UEFI |
| Linux aarch64 | static (musl) | yes | bwrap, landlock, proot | yes; images boot on UEFI |
| macOS 14+ arm64 | yes | yes | the home redirected (advisory) | in the Luban host machine (`vz`, Apple silicon) |
| Windows x86_64 | yes | yes | `%USERPROFILE%` redirected (advisory) | through the `wsl2` carrier |

An aarch64 image boots on a virtual machine (`virt`); booting aarch64 hardware
needs a kernel for it, which is not in the index yet.

## What a machine must provide

Three layers decide where everything xlings runs comes from:

| Layer | What | Rule |
|---|---|---|
| **T0 — the operating system** | the kernel and its features (user namespaces, Landlock, seccomp, KVM); `sudo` for the one-time setup and `--system`; `apparmor_parser`; `mount` and `chown` through the elevation door; on macOS and Windows their own tools (`pbcopy`, `clip`, `wsl.exe`, `msiexec`) | the only layer that may come from the machine; xlings names what it is and why |
| **T1 — the index** | every other program and data file: bwrap, pasta, proot, curl, e2fsprogs, limine, qemu, UEFI firmware, the clipboard tools, WSL | a package of the index, for every architecture; a machine's own copy may be used, but what a missing one is told is `xlings install <package>`, never the machine's package manager |
| **T2 — in-process** | GPT, FAT, ISO 9660 and El Torito, the hybrid partition table, qcow2, tarballs, file clones | no program at all |

So a clean Linux machine needs `curl` (or a browser) to install xlings, and
nothing else for any of its features. `xlings self doctor --isolation` shows what
this machine gives and where each tool comes from.

## Versions

Releases are dated: `YYYY.M.D.N`, `N` from 1 (`.0` is reserved for a formal
release). `luban` is released with the xlings beside it, at the same version.

| What | How it is versioned |
|---|---|
| xlings and luban | the release date |
| Luban editions, policies, boot profiles | their own date; a published version's output never changes |
| packages | upstream's version; `revision` on a version when its payload or configuration changes |
| an index snapshot | its own version; it names the clients it supports ([specs/index-version-contract](specs/index-version-contract.md)) |

### Compatibility

- **`min_client`**: an edition or policy that uses a newer client's fields names
  the oldest client that understands them; an older one refuses with the upgrade
  command instead of misreading it.
- A field an older client ignores is designed so that ignoring it is safe.
- A home upgraded by `xlings self update` keeps working with what it has; repairs
  of older records are `xlings self doctor --fix`.
- Never removed in an upgrade: a SubOS's home and anything the user owns.
