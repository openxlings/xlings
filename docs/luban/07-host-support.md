# Host Support

**English** | [简体中文](../zh/luban/07-host-support.md)

## Hosts it is checked on

Every release runs the same scenarios — the one-time setup, every policy preset,
proxy-only networking, the clipboard, nano, tiny, tiny-musl, the agent workspace,
upgrade, a terminal-attached run — as an ordinary user without sudo, on:

| Host | Kernel and LSM |
|---|---|
| Ubuntu 24.04, AppArmor restriction as shipped / turned off | the runner's |
| Ubuntu 22.04 | the runner's |
| Ubuntu 24.04 and 22.04 on arm (aarch64) | the runner's |
| Fedora (SELinux enforcing), Debian 12, Arch | their own kernels, in virtual machines |
| Fedora, Arch, openSUSE Tumbleweed, Debian testing and 12, Ubuntu 20.04 | containers: their userland |

## What the host must have

Its kernel (user namespaces), and nothing installed from its package manager.
Everything an environment, an image or a VM needs comes from the index: bwrap,
pasta, e2fsprogs, limine, the kernels, qemu, UEFI firmware, the clipboard tools.
The only things taken from the host are the operating system's own — the kernel,
`sudo` for the one-time setup, AppArmor's loader
([Supported platforms](../51-supported-platforms.md)).

## The one-time setup

Ubuntu 23.10 and later restrict unprivileged user namespaces with AppArmor. The
setup installs a root-owned bwrap and one AppArmor profile that allows it exactly
that — once, for every user; it does not turn the restriction off.

```bash
luban setup                              # = xlings self doctor --isolation --fix
xlings self doctor --isolation           # what this host can isolate with, and why not
```

`luban new` checks the host before anything is downloaded and asks for it
there; in agent mode it exits 2 with the command. Unattended (CI, a script),
`luban setup -y` answers its one question.

`luban status` on macOS and Windows answers for the carrier a Luban system
runs in there (vz, wsl2): whether it is available, why not, and the way
forward. Ready exits 0, not ready 1, the same on every platform.

## When something fails

```bash
luban status                     # this host: what it gives, what is missing
luban status box                 # an environment: what it asks for and gets, and what a shared kernel cannot hide
xlings subos doctor              # every environment, with a real entry
xlings self doctor               # the home itself
```

| Seen | Meaning | What to do |
|---|---|---|
| `setting up uid map: Permission denied` | AppArmor's restriction | `luban setup` |
| a private environment refuses to enter: the bwrap cannot forbid nested user namespaces | a setuid bwrap | `luban setup` |
| `--temp` or `--persist home` says no bwrap here has overlays | the host's bwrap is older than 0.10, or setuid | `xlings install bwrap` (it is installed the first time when it can be) |
| exit 125 with a list | the policy asks for what this host cannot give | the list names each item and its fix |
| an edition is refused before anything is made | it is not built for this machine's architecture | another edition ([02](02-editions.md)) |
| exit 2 in agent mode | a question an agent cannot answer | the error names the option that answers it |
