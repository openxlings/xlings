# SubOS

**English** | [简体中文](zh/10-subos.md)

A SubOS is a named environment: its own tool versions, its own home, its own
policy. It can be no more than a PATH switch, a rootless sandbox with a private
network, or a whole Linux root. Three things make one:

| | what it is | chapter |
|---|---|---|
| **content** | the workspace (which versions), the home, the policy | this one |
| **view** | how a process sees it: a PATH overlay, a sandbox, a root, a machine | [11](11-sandbox-and-policy.md), [12](12-roots-and-generations.md) |
| **carrier** | where it runs: this machine, a WSL2 distribution, a VM | [13](13-subos-on-macos-and-windows.md) |

## At a glance

```bash
xlings subos new work --sandbox=private        # create it, and declare its isolation
xlings subos use work                          # enter: that sandbox, no flag needed
xlings subos exec work -- make -j8             # one command; its exit code
xlings subos status work                       # what it asks for vs what this machine gives
xlings subos list                              # every SubOS
xlings subos remove work                       # delete it (with its home; asks first)
```

**Isolation belongs to the instance and is declared once**, when it is made
(`--sandbox=dev|private|locked`) or later with `subos config`. Every way in —
`use`, `exec`, `start`, the interface — enters that sandbox. Options on one call
(`--net none`, `--sandbox=locked`) can only tighten it. An instance that declared
nothing behaves as a plain environment: it is a sandbox only when `--sandbox`
is given. One made `--from` another inherits its declaration.

## A toolchain apart from the host

No sandbox, only versions kept apart:

```bash
xlings subos new py312
xlings subos use py312                     # a shell whose PATH is py312's tools
xlings install python@3.12 poetry          # into py312; the host is unchanged
exit
xlings install ninja --subos py312         # install without entering
xlings subos use py312 --cmd "python -V"   # one command inside
```

`--from` derives one from another, at almost no cost (payloads are shared):

```bash
xlings subos new py312-exp --from py312
xlings subos new rust-env --from subos:rust-env@1.80   # from a SubOS package
```

## Sessions: one instance, many commands

Every `exec` starts the sandbox anew. To let several commands share one `/tmp`
and one set of processes (a server and its tests), start a session:

```bash
xlings subos start dev --ttl 30m                    # in the background; ends after 30 idle minutes
xlings subos exec dev -- sh -c 'redis-server --daemonize yes'
xlings subos exec dev -- make integration-test      # joins the session in milliseconds
xlings subos ps                                     # running sessions
xlings subos stop dev
```

`xlings subos use dev --sandbox --keep` also starts one; it outlives the shell
until `subos stop`. A command that joins from another terminal gets a terminal
of its own: job control, Ctrl-C, and the window size follow the caller's.

A service in the sandbox is reachable from outside with `--publish`:

```bash
xlings subos exec dev --publish 8080:80 -- python -m http.server 80
```

## Files in and out

```bash
xlings subos cp ./data.csv dev:/tmp/           # in
xlings subos cp dev:~/results ./results        # out (~ is the instance's home)
```

Only the instance's home and `/tmp` are reachable. Links inside the instance are
not followed: it cannot make your copy write somewhere on the host.

## Storage

| `--storage` | the home | notes |
|---|---|---|
| `shared` (default) | `subos/<name>/home/<user>` | the same home for the shell and the sandbox |
| `tmpfs` | in memory; gone on exit | needs bwrap |
| `image` | `home.img`, an ext4 sparse file, loop-mounted | needs bwrap and one administrator step |

Storage decides where the home is; it adds no boundary.

## Removing

```bash
xlings subos remove dev        # asks, and says how much data its home holds
xlings subos remove dev -y     # only when the user asked for the deletion
```

A SubOS's home is user data: only a deletion the user started and confirmed
removes it (the interface's `remove_subos` needs `"yes": true`). Every deletion is
recorded in `<home>/logs/destructive.ndjson`. Upgrades, repairs and cleaning
never delete it.

## Exit codes

| Code | Meaning |
|---|---|
| the command's own | the command ran and ended |
| 125 | failed before entering (including a policy this machine cannot give) |
| 126 / 127 | the command cannot run / does not exist |
| 124 | `--timeout` ran out |
| 128 + n | ended by signal n |
| 13 | `E_PERMISSION`: only the owner can do it, or the policy refuses |
| 75 | a request is queued for approval |
| 2 | agent mode needs a confirmation, or an interactive entry was refused |

## When it does not work

```bash
xlings subos status dev        # asked vs given; why it cannot enter, item by item, with the fix
xlings subos doctor            # every instance: its policy, a real entry, its policy package, its sessions
```

`XLINGS_TRACE=caps,provider,session` prints diagnostics for one command.

## Commands

| Command | What |
|---|---|
| `subos new <name> [--sandbox=preset] [--from <src>] [--storage …]` | create, declaring its isolation |
| `subos use <name> [--sandbox] [--cmd "…"] [--keep\|--ttl N]` | enter; `--cmd` runs one command |
| `subos exec <name>\|--temp [--sandbox[=preset]] [--timeout D] -- argv` | one command, its exit code |
| `subos start / stop / ps` | sessions |
| `subos cp <src> <dst>` | copy in or out (one side `<name>:<path>`) |
| `subos config / status / doctor` | policy, what is in effect, health ([11](11-sandbox-and-policy.md)) |
| `subos log / report` | audit ([11](11-sandbox-and-policy.md)) |
| `subos upgrade <name>` | move to the edition's newest ([Luban](luban/03-environments.md)) |
| `subos rollback / export / boot / diff / pack` | roots ([12](12-roots-and-generations.md)) |
| `subos list / info / remove` | list, details, delete |
