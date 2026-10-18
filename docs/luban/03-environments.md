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

An edition may set their defaults; what you set wins.

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

An upgrade is one new generation, so it can be rolled back.

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
certificate authorities, a UTF-8 locale, the terminal's description, and, for
a desktop, its applications and a cursor theme -- and what the edition
declares. An edition declares its own in `/usr/share/luban/checks.d/*.json`:

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
