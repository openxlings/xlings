# Environments

**English** | [简体中文](../zh/luban/03-environments.md)

An environment is made from an edition and then is yours: settings, packages,
generations.

## Settings

```bash
luban config box                                  # its settings
luban config box --proxy socks5h://127.0.0.1:7897   # its only network is this proxy
luban config box --tz utc                           # utc, proxy (the proxy's exit), or a zone
luban config box --policy xim:agent-confined        # another policy
luban config box --allow clipboard                  # a grant
luban config box --mount ~/code/proj:/work          # a host directory inside
```

| Key | |
|---|---|
| `proxy` | `socks5h://host:port`: the environment's network is that proxy and nothing else |
| `tz` | the time zone it shows: `utc`, `proxy`, or a zone name — never the host's unless you choose it |
| `policy` | a policy package (`xim:agent-private`, `xim:agent-confined`, your own) |
| `allow` | a grant: `display`, `audio`, `gpu`, `ssh-agent`, `clipboard`, `clipboard-paste`, … |
| `mount` | a host directory inside the environment |

Each is the `xlings subos config` setting of the same name
([Sandbox and policy](../11-sandbox-and-policy.md)). These are luban's own:

```bash
luban config box --mode machine       # luban enter starts it with a kernel of its own
luban config box --persist home       # each session starts from its baseline; /home stays
luban config box --shell fish         # its login shell
luban config box --desktop none       # its console, not its desktop, in the window
luban config box --on-close background
```

| Key | |
|---|---|
| `mode` | `sandbox` (this machine's kernel) or `machine` (a local VM booting the environment itself) |
| `persist` | `all` (default), `home` (the system part is restored each time, `/home` stays) or `none` (nothing stays) |
| `shell` | its login shell, from its own `/etc/passwd` |
| `desktop` | the desktop session the window shows, or `none` ([The desktop](08-desktop.md)) |
| `on-close` | closing the window of a running machine: `ask`, `background` or `poweroff` |
| `files` | how a machine shares its files: `virtiofs` (default; 9p where it cannot) or `9p` |

An edition may set their defaults; what you set wins.

A system that starts as a machine runs `luban run` there too, on its own
kernel: a machine that was off starts for the command and powers off when the
last command in it ends (commands run at once each run to their end); one that
is running keeps running. `--user` names one of its users -- a name it does not
have is refused. `poweroff` inside a machine ends its session and it is off —
nothing is asked.

## Upgrading

```bash
luban upgrade box --dry-run      # what would change
luban upgrade box                # the edition's newest version
luban upgrade box --to 2026.10.11.2
```

An environment knows what its edition brought. An upgrade:

- moves each package the edition brought **and you left alone** to the new
  edition's version;
- leaves alone a package **you** installed or moved to another version;
- installs what the new edition adds;
- leaves installed what the new edition dropped, and says so;
- applies the new edition's policy only if it **allows nothing more** than the
  current one; otherwise it keeps the current policy and lists what would have
  been loosened — switching is your explicit `luban config box --policy …`.

An upgrade is one new generation, so it can be rolled back. Nothing is
upgraded for you: a system stays as it was made, and `luban status box` says
when an upgrade has something for it, or when its desktop lacks something a
newer edition brings (with both ways: install that one package into it, or
upgrade).

An upgrade that moves an older system into its own userland keeps what it was
before (its archive, named by `luban history box` and `luban status box`);
going back to that is one command, asked first:

```bash
luban rollback box --to relayout -y   # the system as it was before its move
```

## Generations and rollback

```bash
luban history box                # its generations
luban rollback box               # back one
luban rollback box --to 3
```

Every change to an environment's packages is a generation; rolling back moves a
pointer and touches no data. A generation records its packages too, so the
environment stays at the one you went back to when it is entered again (one
made before 2026.10.13.2 has no record: its files come back, and the next entry
rebuilds them from the packages it has now -- the rollback says so).

## Its network

`luban status box` says it in one line, the same whatever it runs as: `like the
host` -- the host's DNS and its proxy (`HTTPS_PROXY`, `HTTP_PROXY`,
`ALL_PROXY`, `NO_PROXY`, of the environment that starts it) -- `only its proxy`
(`luban config box --proxy …`) or `none` (`--net none`).

A machine is on its own NAT: its address and name servers come from the
kernel's DHCP, not from the system, and a host proxy on loopback
(`127.0.0.1:7890`) is the host's address on that NAT inside
(`10.0.2.2:7890`): `network  like the host · proxy http://10.0.2.2:7890 (the
host's http://127.0.0.1:7890)`. It is in `/run/luban/network.env` for that
boot, in every session and every login shell, and never written into the
system. A host proxy that changed is the next start's; `luban status` says so.

## Offline

A system's home starts from a copy of the host's index: what it installs is
read from it without the network, and `xlings update` inside updates its own
copy. An exported system carries its packages and that index, so it is made
again with no network:

```bash
luban export box box.luban
luban import box.luban
```

## Users, sudo and packages

A system's user is made with it (`--user`, or the default `lubanoser` for a
desktop). Given no password, it has the default one, `lubanoser`: `luban new`
says so, and `luban status` keeps saying so until `passwd` changes it.
`--no-password` is a choice: no password at all, and a `sudo` that asks
nothing. Otherwise `sudo` works as on Ubuntu or Fedora: the user is in
`wheel`, sudo asks for their password and not again for 15 minutes, finds
`xlings`, and keeps the host's proxy (`/etc/sudoers.d/luban`; edit with
`visudo`). The core, desktop and agent-workspace editions carry `sudo`; a
sandbox needs none (its writes go through the system's broker).

```bash
luban new box --user alice                  # asks for a password; empty: lubanoser
luban new box --user alice --no-password    # no password, a sudo that asks nothing
luban run box --user alice -- passwd        # change it
```

Inside a system there is one store of packages, the system's (`/xlings`).
A user's `~/.xlings` is a view of it: their own choices of versions, their
shims and logs -- no index, no packages of their own. What they do not
choose themselves is the system's choice. `xlings list`, `info` and `use`
show the system's packages too, marked `system`.

| As a user | What happens |
|---|---|
| `xlings install codex` (the system has it) | nothing to install: "codex 2.1 is provided by the system" |
| `xlings use node 22` (in the store) | your choice; nobody else's changes |
| `xlings install node@22` (not in the store) | one line, one question: it goes into the system's store with `sudo`, then it is your choice |
| `xlings remove node` | your choice goes; the store keeps it (root removes it for everyone) |
| `sudo xlings ...` | the system's own: its default for every user, and its store |

With nobody to answer (an agent, a script) nothing is asked: the command
fails with `E_NEEDS_ROOT` and says the exact `sudo xlings install ...` to run.
Root removing a version a user has chosen keeps it, and says whose choice
keeps it.

## Removing

```bash
luban rm box                     # asks first, and says how much its home holds
```

An environment's home is your data: only a removal you started and confirmed
deletes it.

## Checking it

A system checks itself when it is made, upgraded, booted and entered, and when
asked:

```bash
luban status box --check          # every check, and what failed
luban status box --check --json   # {"ok": ..., "failed": [...], "items": [...]}
```

A check is critical or not. A critical failure stops what was under way: a new
system is not ready, an upgrade is not switched to, a machine does not boot, an
entry does not start, and each says which check failed. Any other failure is
reported and the system is used. The checks are what any Linux userland needs
-- `/usr` is the system's generation, every program in `/usr/bin` has its
loader and finds the libraries it names, each package's programs start, its
users, its login shell, its host name and machine id, a time zone, the
certificate authorities, a UTF-8 locale, the terminal's description, the
package index its xlings reads (its users' too), and, for a desktop, its
applications and a cursor theme -- and what the edition declares. An edition declares its own in `/usr/share/luban/checks.d/*.json`:

```json
{"checks": [{"id": "desk-session", "what": "its desktop session starts", "run": "xdesk --version", "critical": false, "timeout": 30}]}
```

`run` is a `/bin/sh -c` command inside the system; it passes when it exits 0
within `timeout` seconds (120 by default). Entering runs the fast checks only,
which read files and start no program.

## Using the clipboard

The clipboard is not the display:

- by default, copying goes through the terminal (OSC 52) and lands on the
  clipboard of the machine you sit at; pasting is the terminal's paste;
- `allow clipboard` (copy to the host) and `allow clipboard-paste` (read the
  host's) are separate grants: `wl-copy`, `wl-paste`, `xclip` and `xsel` inside
  then reach the host's clipboard through the broker;
- `luban status box` says which is in effect.

A command that joins a running environment from another terminal (`luban enter`,
`luban run`) gets its own terminal: job control, signals, and the window size
follow yours. Terminals in one environment are peers: whichever came first can
leave while the others stay, Ctrl-C in one reaches only its own program, and
the environment's session ends a few seconds after the last terminal leaves.
