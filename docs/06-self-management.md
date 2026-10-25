# Self-Management

**English** | [简体中文](zh/06-self-management.md)

`xlings self` manages xlings itself: upgrading the client, checking and
repairing the home, cleaning caches. Rarely needed — after an upgrade, or when
a command starts failing strangely, start here.

## Upgrade

```bash
xlings self update
```

It refreshes the indexes and installs the newest xlings as a package. The first
command of the new version that changes the home then upgrades it by itself,
once that command has succeeded: it checks the
home config, rebuilds the command routes and checks the home the way
`self doctor` does. When all of that worked, it says nothing. What it could not
do by itself it says once: how many problems (one per cause -- a payload that
is gone is one problem, however many names it registered), the one command that
repairs them, and where the details are:

```text
[warn] xlings 2026.10.13.1 (from 2026.10.12.3): 1 problem(s) in this home it could not fix by itself
         fix          xlings self doctor --fix
         details      xlings self doctor   (log: @xlings/logs/upgrade.ndjson)
         or go back   xlings use xlings 2026.10.12.3
```

Each item, with its own command, is in the log and in `xlings self doctor`;
`-v` on the command prints them there too. A home that has never been through an
upgrade record says the problems were already there before this version.

Going back is one command while the previous version is installed:

```bash
xlings use xlings 2026.10.11.1
```

When `self update` itself cannot work, it says what to try: the other mirror
(`xlings config --mirror CN`), or the install script, which installs over the
home and keeps everything in it. The rules are in
[Upgrade and compatibility](specs/upgrade-and-compatibility.md).

## Check

```bash
xlings self doctor
```

Read-only. It checks that these agree:

| Layer | What |
|---|---|
| workspace | the active versions the current SubOS declares |
| version database | each `package@version` registration |
| shims | the dispatch links in `bin/` |
| payloads | the directories the records point at, and their programs |
| sysroot | the links under `lib/`, `lib64/`, `usr/`, `etc/`, `share/` |

Exit 0 means the current SubOS is healthy. Warnings alone do not fail it.

```bash
xlings self doctor --show-ok          # every check, the good ones too
xlings self doctor --subos work       # another SubOS
xlings self doctor --isolation        # what this machine can sandbox with (see 11)
```

## Repair

```bash
xlings self doctor --fix
```

One run repairs what can be repaired:

- missing shims are made, stray ones removed;
- bindings to versions that do not exist are dropped;
- an active entry pointing at an unregistered version is deactivated, in every SubOS;
- a package whose payload is broken is registered again, or reinstalled;
- a broken sysroot link is **pointed back** where the version database says it belongs, and removed only when nothing says;
- a link that should be there and is not is put back;
- a home that was moved is re-pointed at where it is now (below);
- a record nothing can restore and no index provides is removed.

It checks again at the end: the report is the state **after** the repair. A
repair is judged by whether the finding is gone, not by its command's exit code;
one that outlives a reinstall is reported as the check's fault.

A repair that runs another SubOS's `self doctor --fix` runs it with no terminal:
nothing it starts can wait for a password, Ctrl-C ends it, and its output is in
`logs/doctor/<subos>.log`. Steps a package declared need administrator rights
(`system.admin_step`) and nobody could approve are listed by `self doctor`
every time, and taken by `--fix` at a terminal.

### The runtime

```bash
xlings config --runtime glibc@2.44.3   # pin this home's default runtime
xlings config --runtime glibc@latest   # follow the newest installed again
```

Every package bound to glibc in this home runs on one runtime: the pinned
version, else the newest installed. A change switches links (`<home>/abi`),
never a package; `abi.json` says what it is and `logs/abi.ndjson` when and
why it changed.

```bash
xlings self doctor --fix --dry-run    # what it would do, changing nothing
```

### Removing a record

`--fix` removes a registration only when the payload is gone, registering and
reinstalling failed, no index provides it, and no other SubOS uses it. That
loses a fact ("this was installed"), so such a run does not end with `OK`; it
says how many it removed. Removing a sysroot link counts the same way. The exit
code stays 0: the question it answers is whether the home still needs a person.

### A moved home

Moving a home with `mv` is not supported: programs in payloads carry the old
path in their `PT_INTERP` and `RPATH`. `self doctor` says so:

```text
✗ home relocated   this home is at <new path>, but its records were written for <old path>
  → run            xlings self doctor --fix
  • note           re-points the versions database and the sysroot links;
                   binaries that baked the old path into PT_INTERP/RPATH are
                   NOT repaired — re-provision the home if the toolchain fails
```

`--fix` re-points xlings' own records and every SubOS's sysroot links at the
current root. It does not claim the toolchain works afterwards.

### A suggested command always runs

What follows `→ run` in a report is a complete command to paste. It names a
**package**, never a program (`nm` comes from llvm; there is no package `nm`).
When nothing can provide an entry, it says so instead of giving a command that
cannot work.

### Resetting metadata

```bash
xlings self doctor --reset-metadata
```

Separate from `--fix` because it loses information: unreadable release metadata
is dropped and the entry becomes a version switched on its own. Only when the
metadata is damaged and reinstalling does not help.

## Clean

```bash
xlings self clean            # caches, and payloads nothing references
xlings self clean --dry-run  # what it would delete
```

A SubOS's home is user data and is never cleaned: only a deletion the user
started and confirmed removes it (`subos remove`).

## Other commands

```bash
xlings self config     # the home, the current SubOS, the mirror
xlings self install    # install from a release tarball (what the installer runs)
xlings self init       # create the home's directories
xlings self migrate    # move an old layout into subos/default
xlings self uninstall  # remove xlings, asking first
```

## Choosing the home

The home is chosen in two steps, the same ones mcpp uses (mcpp SPEC-010). The
raw home is `XLINGS_HOME` when it is set, a self-contained installation's root,
or `~/.xlings`. A raw home may hold `home.json`, which names the actual home;
it is followed once, never twice.

```bash
xlings self init --home /data/xlings           # initialize /data/xlings and select it
xlings self init --force --home /data/other    # replace that selection
xlings self init --home ~/.xlings              # back to the raw home; home.json is removed
xlings --home /tmp/h install gcc               # this run only, as XLINGS_HOME=/tmp/h
```

Selecting moves and deletes nothing. A locator that names no directory stops
every command with the repair command rather than falling back to another
home. `xlings self config` shows the actual home, the locator and the previous
home. A Luban system's home is the administrator's and reads no locator.

## What a command did

Every command writes one operation journal, `logs/ops/<date>/<trace>.ndjson`,
and every process it runs (a hook, a repair, a command inside a SubOS) joins the
same file under the trace it inherits. Phases, their times and failures are
there, and a tool's own output (limine's, mkfs's) is a side file next to it.

```bash
xlings self log                      # the latest operations
xlings self log 4ed6cd2b             # one of them (a prefix of its trace)
xlings self log 4ed6cd2b --bundle    # one file for a bug report, without your paths or name
```

The journal stays on this machine; old days are removed after two weeks. The
format is in [Operation journal](specs/operation-journal.md).

## When something is wrong

```bash
xlings self doctor                   # what is wrong
xlings self doctor --fix --dry-run   # what it would do
xlings self doctor --fix             # repair
xlings self doctor                   # exit 0: clean
```

If something remains, the report says why for each item. A payload problem that
belongs to another SubOS is not repaired from here — that would pull the
package into this environment — and the report names the SubOS.
