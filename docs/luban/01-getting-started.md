# Getting Started

**English** | [简体中文](../zh/luban/01-getting-started.md)

`luban` is installed with xlings, at the same version; on the host, in every
Luban environment and on every Luban machine it is the same program.

## Common commands

```bash
luban                         # one screen: where you are, the environments, what next
luban new box                 # an environment (Luban Core by default)
luban new box tiny            # an edition: nano, tiny, core, agent-workspace, or ns:name
luban enter box               # go into it
luban run box -- make -j8     # one command in it
luban ls                      # the environments
luban status box              # one environment; without a name, this host
```

Installing software is still xlings: `xlings install <package>` — inside an
environment it installs into that environment.

```bash
luban run box -- xlings install -y cmake
```

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
luban help --all              # every command
luban <command> --help        # one command, and the xlings command it is
```

[Editions](02-editions.md) · [Environments](03-environments.md) · [Images](05-images.md)
