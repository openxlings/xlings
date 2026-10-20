# Getting Started

**English** | [简体中文](zh/01-getting-started.md)

## Install

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
```

The installer puts the home in `~/.xlings` (`%USERPROFILE%\.xlings` on
Windows) and adds its `bin/` to your shell's PATH. Nothing else on the machine
changes.

From China, the mirror makes downloads come from GitCode instead of GitHub:

```bash
xlings config --mirror CN
```

## A first package

```bash
xlings install mcpp luban node    # the latest
xlings install node@20            # an older version, beside it
xlings use node@20                # which version is active
xlings search gcc                 # what the indexes offer
xlings list                       # what is installed here
xlings remove node@20             # uninstall
```

A name without a version installs the newest the index offers. The commands
are the same on every platform; a package that is not built for your machine
is refused before anything is downloaded.

## Typical tasks

| Task | How |
|---|---|
| Several versions of one tool | `xlings install gcc@16 gcc@11`, then `xlings use` ([03](03-packages-and-versions.md)) |
| A project's own toolchain | a `.xlings.json` in the project, then `xlings install` ([04](04-projects.md)) |
| An isolated environment for an agent | `xlings subos new agent-ws --sandbox=private` ([10](10-subos.md)) |
| A command inside it | `xlings subos exec agent-ws -- python run.py` |
| A whole Linux environment, image or machine | `luban new box` ([Luban](luban/README.md)) |
| Upgrade xlings | `xlings self update` ([06](06-self-management.md)) |
| Something misbehaves | `xlings self doctor`, then `xlings self doctor --fix` |

## Where to go next

- [Scenarios](02-scenarios.md) — what each kind of work uses
- [Packages and versions](03-packages-and-versions.md)
- [SubOS](10-subos.md) and [Luban](luban/README.md)
- [The command reference](generated/command-reference.md)
