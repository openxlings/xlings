# What Luban Is

**English** | [简体中文](../zh/luban/00-what-luban-is.md)

## The smallest system that holds together

A Luban system is a kernel, xlings, and luban. xlings installs packages into a
root whose `/usr` is a generation of links into payloads; luban is the system
side — what boots, which environment is the machine's, what an image is.
Everything else is a package, so a system is a list of packages, and a list of
packages is a template that anyone can publish.

## Three ways to run the same environment

| As | How | Isolation |
|---|---|---|
| a sandbox on your machine | `luban new box`, `luban run box -- …` | the SubOS sandbox (bwrap on Linux) |
| an image | `luban export box box.iso` (or `.img`, `.qcow2`, `.tar.zst`) | its own kernel when booted |
| a machine | `luban write box /dev/sdX`, or boot an image | the hardware |

The environment is the same in all three: the packages, its `/etc`, its policy.

## What it is for

- **Development environments** that do not touch the host: a toolchain per
  project, its own shell and editors (`core`).
- **Agents**: a workspace that is private from the moment it exists — its only
  network a proxy, an identity of its own, nothing of your home
  ([04](04-agent-workspace.md)).
- **Images and appliances**: a live ISO or a drive from what you made, booting
  on BIOS and UEFI ([05](05-images.md)).
- **Your own distribution**: a template of your own in your own index
  ([02](02-editions.md)).

## What it is not

- Not a second package manager: packages are xlings'.
- Not a container runtime: an image for docker or WSL is `luban export box box.tar.zst`;
  luban does not run containers.
- Not a hiding place on a shared kernel: what a shared kernel cannot hide (its
  version, the CPU, the host paths bound in) is listed by `luban status`; a kernel
  of its own is `luban enter --mode machine`.
