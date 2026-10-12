# Quick Start

**English** | [简体中文](../zh/luban/01-quick-start.md)

`luban` is installed with xlings, at the same version; on the host, in every
Luban environment and on every Luban machine it is the same program. This page
is one path through it: an environment, work in it, go back a step, an agent's
workspace, an upgrade, the environment booted as a machine, and cleaning up.
Each step says what you should see.

## 1. An environment

```bash
luban new box tiny            # an edition: nano, tiny, core, agent-workspace, or ns:name
```

You see the edition once (`'box' from subos:luban-tiny@… <- …: N package(s)`),
the download, and `subos created: box`. The first `luban new` on a host may ask
for a one-time setup first (below). Made without an edition name it is Luban
Core, which adds compilers and build tools. If it stops halfway, the same
command again finishes it.

## 2. Work in it

```bash
luban enter box               # a shell in it; the prompt says where: [luban:box] ~ #
luban run box -- uname -a     # one command in it, from outside
luban run box -- xlings install -y cmake
```

Installing software is still xlings: `xlings install <package>` — inside an
environment it installs into that environment. `exit` leaves it.

## 3. Go back a step

```bash
luban history box             # its generations: one per change
luban rollback box            # back one: cmake is gone again
```

## 4. An agent's workspace

```bash
luban new agent agent-workspace
luban run agent -- claude
luban status agent            # what it may do: its policy, network and grants
```

An agent's workspace is isolated by its policy; its only network can be a proxy
(`luban config agent proxy socks5h://127.0.0.1:7897`). See
[Agent workspace](04-agent-workspace.md).

## 5. Upgrade

```bash
luban upgrade box --dry-run   # what the edition's newest version changes
luban upgrade box             # one new generation; rollback undoes it
```

## 6. Boot it as a machine

```bash
luban export box box.iso      # a live ISO: BIOS and UEFI, x86_64 and aarch64
xlings install -y qemu-system # the VM luban try boots it in
luban try box                 # export it and boot it in a local VM
```

`luban try` boots the image in `qemu-system` (one package, both architectures,
with its firmware); you see the boot and a login. Without it, `luban try` names
the command that installs it. `--nographic` puts its console in this terminal.

## 7. A boot drive (optional)

```bash
luban write box.iso /dev/sdb  # every byte of the drive is replaced; it asks first
```

## 8. Clean up

```bash
luban rm box                  # asks first, and says how much its home holds
xlings remove gcc --older-than 16 --dry-run
```

`xlings remove <package> --older-than <version>` removes the versions of a
package nothing uses any more; `--dry-run` lists them first.

## The first time on a host

On Ubuntu 23.10 and later, AppArmor keeps programs without a profile from making
the sandbox an environment runs in. The first `luban new` checks the host before
anything is downloaded and asks for a one-time setup: a root-owned bwrap and one
narrow AppArmor profile, for every user of the machine, with one `sudo`.

```bash
luban setup                   # the same setup, whenever you like
```

An agent (`--agent`) is never asked: the command exits 2 and names this setup.

## More

```bash
luban                         # one screen: where you are, the environments, what next
luban ls                      # the environments
luban help --all              # every command
luban <command> --help        # one command, and the xlings command it is
```

[Editions](02-editions.md) · [Environments](03-environments.md) · [Images](05-images.md)
