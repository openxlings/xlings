# Commands by Scenario

**English** | [简体中文](zh/09-commands-by-scenario.md)

The lookup, once the nouns are known. Every command and option is in the
[generated reference](generated/command-reference.md); luban's are in
[its own](generated/luban-command-reference.md).

## The command tree

The first level is a noun or a verb that applies to every noun; what acts on a
noun is under it; options are the cheapest addition and the most numerous.

| First level | What it names |
|---|---|
| `install`, `remove`, `update`, `use`, `list`, `search`, `info`, `why` | packages and versions |
| `config`, `index` | where packages come from, and how xlings behaves |
| `subos …` | environments |
| `self …` | xlings itself |
| `interface`, `agent` | programs that drive xlings |
| `profile`, `script` | the profile's history; xlings scripts |

`luban` is a second front door for environments: each of its environment
commands is the `xlings subos` command it names ([Luban](luban/README.md)).

## Packages

| I want to | Command |
|---|---|
| install, at a version | `xlings install gcc@16.1.0` |
| install into another SubOS | `xlings install gcc --subos work` |
| install for every user (root) | `sudo xlings install --system gcc` |
| switch the active version | `xlings use gcc 16.1.0` |
| see what is installed | `xlings list` |
| see a package's versions | `xlings info gcc --all-versions` |
| know why a dependency resolved so | `xlings why glibc` |
| remove | `xlings remove gcc` (`--all`, `--all-subos`, `--subos <n>`) |
| refresh the indexes | `xlings update` |

## Environments

| I want to | Command |
|---|---|
| make one | `xlings subos new work` |
| make one that is a sandbox | `xlings subos new work --sandbox=private` |
| make a root (a Linux system) | `xlings subos new box --rootfs --from subos:luban-core` |
| enter it | `xlings subos use work` |
| run one command in it | `xlings subos exec work -- make -j8` |
| keep it running | `xlings subos start work --ttl 30m` |
| copy files in or out | `xlings subos cp ./data.csv work:/tmp/` |
| change its policy | `xlings subos config work --net none` |
| see what it asks and gets | `xlings subos status work` |
| see what happened in it | `xlings subos log work`, `xlings subos report work` |
| move it to its edition's newest | `xlings subos upgrade work` |
| roll a root back | `xlings subos rollback box` |
| make an image of it | `luban export box box.iso` |
| remove it | `xlings subos remove work` |

## xlings itself

| I want to | Command |
|---|---|
| upgrade | `xlings self update` |
| check and repair | `xlings self doctor`, `xlings self doctor --fix` |
| see what this machine can isolate with | `xlings self doctor --isolation` |
| use the China mirror | `xlings config --mirror CN` |
| add an index | `xlings config --index-repo myteam:https://example.com/idx.git` |
| clean caches | `xlings self clean` |
| uninstall | `xlings self uninstall` |

## Programs

| I want to | Command |
|---|---|
| never be asked a question | `--agent`, or `XLINGS_AGENT_MODE=1` |
| machine-readable output | `--json` where a command has it |
| a stable programmatic interface | `xlings interface --list` |
