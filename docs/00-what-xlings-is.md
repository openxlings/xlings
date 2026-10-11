# What xlings Is

**English** | [简体中文](zh/00-what-xlings-is.md)

xlings is a package manager and an environment manager in one static binary,
on Linux, macOS and Windows. It installs any version of anything without root,
keeps every version side by side, and gives environments of their own — from a
PATH switch to a rootless sandbox with a private network, up to a whole
operating system (Luban).

## What it does

| Area | What you get |
|---|---|
| **Packages** | binaries, scripts, configurations and SubOS templates, all in one format (xpkg), from the official index, third-party indexes or your own |
| **Versions** | any number side by side; N environments share one copy of each payload |
| **SubOS** | named environments with their own tools, home and policy — a PATH overlay, a sandbox, or a root that is a whole system |
| **Agents** | agent mode that never waits for input, exit codes with one meaning each, `--json`, and an NDJSON interface (`xlings interface`) |
| **Repair** | `xlings self doctor --fix` checks its own state and repairs it in one run |

## The guarantee

- **One binary.** The Linux release is a static build: nothing from the machine
  is needed to run it, and a home can be moved to a machine with no libraries.
- **Rootless.** Installing, switching and sandboxing need no administrator.
  The few things only an administrator can do on a given machine (the one-time
  sandbox setup on Ubuntu 23.10+, a system-wide layer) are asked for once, by
  the command that needs them.
- **Nothing is installed twice.** A payload lives once in the home's store;
  each environment holds a view of the versions it uses.
- **What it runs comes from an index.** Every external program xlings uses is
  a package of the index, except what the operating system itself is
  ([51](51-supported-platforms.md)).

## Compared with other tools

| | apt / brew | nix | docker | **xlings** |
|---|:---:|:---:|:---:|:---:|
| Versions side by side | no | yes | yes | yes |
| No root | no | partly | partly | yes |
| No daemon | yes | yes | no | yes |
| One command set on Linux, macOS, Windows | no | partly | yes | yes |
| Isolation | none | filesystem | container | PATH overlay, sandbox, root |
| Storage shared between environments | — | store | layers | version views over one store |
| Decentralized indexes | no | no | no | official, third-party, self-hosted |
| Agent / JSON interface | no | no | API | `xlings interface` (NDJSON) |
| As an operating system's package manager | apt is | NixOS | no | Luban |

## Where to go next

[Getting started](01-getting-started.md), then the [scenarios](02-scenarios.md).
