# Scenarios

**English** | [简体中文](zh/02-scenarios.md)

What xlings is used for, and which features each kind of work uses.

## Several toolchains, no conflicts

```bash
xlings install gcc@16 gcc@11 node@24
xlings use gcc@11        # switch back any time; both stay installed
```

Every version lives once in the home's store; switching changes which one the
environment sees. [03](03-packages-and-versions.md)

## One environment for a whole team

```json
{ "workspace": { "xmake": "3.0.7", "gcc": { "linux": "16.1.0" }, "llvm": { "macosx": "20.1.7" } } }
```

```bash
cd my-project/ && xlings install     # the project's own SubOS, the declared versions
```

The `.xlings.json` goes into version control; everyone who clones the project
gets the same versions, and CI uses the same file. [04](04-projects.md)

## An agent, or code you do not trust, in a sandbox

```bash
xlings subos new agent-ws --sandbox=private      # isolation is declared once, here
xlings subos exec agent-ws -- python run.py      # every entry is that sandbox; its own exit code
xlings subos use agent-ws                        # or a shell inside -- no flag needed
```

On Linux the sandbox separates the filesystem, processes, network and
identity; on macOS and Windows it redirects only the home and is not a
security boundary. [10](10-subos.md), [11](11-sandbox-and-policy.md)

## A private workspace for an agent

```bash
luban new agent agent-workspace --proxy socks5h://127.0.0.1:7897
luban run agent -- claude
```

Private from the moment it exists: its only network is the proxy, it has an
identity of its own, it sees nothing of your home. [Luban: agent
workspace](luban/04-agent-workspace.md)

## A Linux environment, an image, a machine

```bash
luban new box tiny                  # a small Linux userland of its own
luban export box box.iso            # a live image; or box.img, a drive
luban try box                       # boot it in a local virtual machine
```

[Luban](luban/README.md)

## Your own packages and index

A recipe in a repository of your own, listed in `.xlings.json`'s
`index_repos`, installs like an official package. [30](30-authoring-a-package.md),
[31](31-hosting-an-index.md)

## Upgrade and repair

```bash
xlings self update
xlings self doctor --fix
```

[06](06-self-management.md)
