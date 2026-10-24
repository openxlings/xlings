# Sandbox and Policy

**English** | [简体中文](zh/11-sandbox-and-policy.md)

How much a SubOS is isolated is its **policy**, declared on the instance; what
implements it on this machine is the **backend**. On Linux the sandbox is a real
boundary (bwrap: filesystem, processes, network, identity); on macOS and Windows
it redirects the home only and is **not** a security boundary — use an operating
system sandbox or a virtual machine for code you do not trust there.

## Presets

| Preset | Filesystem | Processes | Network | Identity | Installing |
|---|---|---|---|---|---|
| `dev` | the xlings home read-only; only this instance's directory writable | own pid / ipc / uts | the host's | the host's | automatic |
| `private` | the same | the same; no nested user namespaces | `nat` | neutral | asks |
| `locked` | the same; `--mount` read-only by default | the same | `none` (only lo) | neutral | refused |

Neither preset sees the host's home, the other instances, or the audit logs.
The environment is emptied except a base set and the proxy variables;
`--env-pass NAME` lets one more through.

## An agent in a sandbox

An agent edits code, installs dependencies and runs tests, and should not reach
your home, your SSH keys or other projects:

```bash
xlings subos new agent-ws --sandbox=private
xlings subos config agent-ws --mount ~/code/myproj:/work    # only this project, writable
xlings subos exec agent-ws -- sh -c 'cd /work && claude --task "fix the failing tests"'
```

An agent declares itself, so that no command ever waits for an answer
([50](50-agents-and-machine-interface.md)):

```bash
export XLINGS_AGENT_MODE=1
xlings subos use agent-ws        # an interactive entry is refused (exit 2): use subos exec
```

## Code you do not trust

No network, the code read-only, and a record of what it did:

```bash
xlings subos new quarantine --sandbox=locked
xlings subos config quarantine --observe full
xlings subos exec quarantine --mount ~/Downloads/suspicious-repo:/src:ro --timeout 5m -- sh -c 'cd /src && ./build.sh'
xlings subos report quarantine        # every program it ran, every file it changed
xlings subos exec --temp --sandbox=locked -- ./untrusted-binary   # an instance just for this; its audit stays
```

A call can only tighten the instance's declaration: `--net host` on a `locked`
instance is refused (125). To loosen it, change the declaration from outside:
`xlings subos config quarantine --net nat`.

## Settings

| Setting | Values | |
|---|---|---|
| `--net` | `host`, `nat`, `none`, `proxy` | the network (below) |
| `--proxy` | `socks5h://host:port` | the only way out, with `--net proxy` |
| `--tz` | `utc`, `proxy`, a zone name | the time zone a neutral identity shows |
| `--fetch` | `auto`, `ask`, `deny` | installing from inside |
| `--index-update` | `auto`, `ask`, `deny` | updating the index from inside |
| `--observe` | `basic`, `standard`, `full` | what is recorded (below) |
| `--allow` / `--disallow` | a grant | a single thing opened (below) |
| `--grants-allowed` | grants | which grants a single call may add |
| `--mount` / `--unmount` | `host:inside[:ro]` | a host directory inside |
| `--env-pass` | `NAME` or `NAME*` | a variable let through |

On `subos config` they are the declaration; on a single call they can only
tighten it, or add a grant within `grants_allowed`.

### Rules

Fine-grained rules live in the policy file
(`<home>/config/subos/<name>/policy.json`), matched in order:

```json
{
  "extends": "private",
  "permissions": {
    "fetch": {
      "default": "ask",
      "rules": [
        { "match": "xim:*", "index": "official", "action": "auto" },
        { "match": "*", "size_gt": "2GB", "action": "deny" }
      ]
    }
  }
}
```

Only the owner, outside the sandbox, changes a policy; a process inside can read
it. A policy with a value this client does not know (`"net": "vpn"`) is refused,
not ignored ("fail closed"). A policy names the oldest client that enforces it
(`min_client`); an older one refuses to enter.

### Policy packages

A team's policy can be a package (`type = "subos-policy"`, a `policy.json` at
its root):

```bash
xlings subos config ci --sandbox xim:agent-confined     # shows what it changes against its preset
xlings subos config ci --policy-upgrade                 # after the package moved: explicit, with the diff
```

The policy is copied into the instance and pinned (`resolved.from`, `sha256`): a
newer package never changes an existing instance by itself. An administrator can
limit the sources in `/etc/xlings/config.json`
(`{"subos_policy_sources": ["xim:*"]}`).

## Installing from inside

xlings works inside the sandbox: reading commands run there; `install`, `remove`
and `update` are handed to a broker outside, decided by the instance's policy,
and run there:

```bash
xlings subos config dev --fetch ask      # auto (default) / ask / deny
# inside:   xlings install jq            -> exit 75: queued for approval
xlings subos requests dev                # outside: what waits
xlings subos approve dev r1a2b3c         # or deny
```

What only the owner can do (`self update`, changing a policy, another instance,
switching the xlings version) exits 13 inside, with the command to run outside.

## Network and identity

| `--net` | | the host's own services | out |
|---|---|---|---|
| `host` | the host's network | reachable | the host |
| `nat` | its own network namespace, joined by pasta; `--publish 8080:80` | not reachable (`--allow host-loopback` to allow) | the host's address |
| `none` | only lo | not reachable | none |
| `proxy` | only lo; a SOCKS5h entry at `127.0.0.1:1080` hands each connection to the declared proxy | not reachable | the proxy |

```bash
xlings subos config box --sandbox private --proxy socks5h://127.0.0.1:1080
xlings subos exec box -- curl https://example.com
```

With `proxy`, names are resolved by the proxy (socks5h), never by the host; a
client that cannot use SOCKS5h, direct IP and UDP do not get through, and there
is no fallback to another network. A neutral identity is the user `user`, a host
name and a machine-id of the instance's own (made once, kept), `LANG=C.UTF-8`,
and a time zone that is UTC, a zone you chose, or the proxy's exit (asked
through the proxy) — never the host's.

## Grants

Off by default; each opens one thing (a socket, a set of device nodes), never
the host's whole runtime directory:

```bash
xlings subos config gui-app --allow display      # X11 / Wayland
xlings subos config gui-app --allow audio        # PulseAudio / PipeWire
xlings subos config ci --allow ssh-agent         # the agent socket, not ~/.ssh
xlings subos config ml --allow gpu               # NVIDIA nodes and /dev/dri
xlings subos config work --allow clipboard       # copy into the host's clipboard
xlings subos config work --allow clipboard-paste # read the host's clipboard
```

The grants are `display`, `audio`, `camera`, `gpu`, `ssh-agent`, `dbus`,
`host-loopback`, `clipboard`, `clipboard-paste`, `desktop-control` (an agent
may drive the system's Luban desktop on the bus: its input, its displays).

The clipboard is not the display. Granted, the programs a sandbox already calls —
`wl-copy`, `wl-paste`, `xclip`, `xsel` — reach the host's clipboard through the
broker, which runs the host's tool (from the index: `wl-clipboard`, `xsel`).
Without the grant, copying goes through the terminal (OSC 52) and lands on the
clipboard of the machine you sit at. `xlings subos status <name>` says which.

## Audit

Written outside, where the sandbox cannot see it:
`<home>/logs/subos/<name>/events.ndjson`, and `sessions/<id>.ndjson` per session.

```bash
xlings subos log dev                  # every event
xlings subos log dev --kind perm      # permission decisions
xlings subos log dev --kind exec -f   # programs run, followed
xlings subos report dev --json        # per-session summary
```

| `--observe` | records |
|---|---|
| `basic` (default) | sessions starting and ending, policy changes, permission decisions, every `exec` |
| `standard` | + files changed in writable mounts |
| `full` | + every program run inside |

Program names and argument counts, not values; variable names, never values.
`locked` refuses to start or ends a session when the audit cannot be written; the
others warn.

## Backends

| Backend | How | Gives | Does not give | Chosen |
|---|---|---|---|---|
| **bwrap** | namespaces | every preset | — | by default: a root-owned `/usr/lib/xlings/bwrap`, the distribution's, then the index's |
| **landlock** | the Landlock LSM (Linux ≥ 5.13), no namespaces | a write fence: only the instance, `/dev`, `/proc` and writable mounts | hiding files, pid / net / identity; host sockets stay reachable — not a boundary for untrusted code | only with `--sandbox landlock` |
| **proot** | ptrace | a filesystem view | a security boundary | `--sandbox proot`, or when there is no bwrap |
| **home redirect** | environment variables | the home | the rest (advisory) | macOS, Windows |

What a backend cannot give is degraded with a reason under `dev`, and refused
(125) under `private` and `locked`, item by item with the fix.

A view of the host (bwrap, proot) presents `/usr`, and `/bin`, `/sbin` and
`/lib*` where the host's resolve: a merged `/usr`'s links into `/usr`, a
non-merged host's own directories (Alpine's `/lib`, openSUSE Leap 15's `/lib64`).

### Recipes of a home layered on a system home

A home that shares the store of the system home below it (`/xlings`, or
`XLINGS_SYSTEM_LAYER`) evaluates recipes -- its index build, the configuration
of a payload it borrows -- in a worker that cannot write the layer below.
bwrap runs that worker where it starts. Inside a Luban system nested user
namespaces are forbidden and bwrap cannot start; there the worker puts up a
Landlock write fence before it reads a recipe (its scratch, the devices, its
hook logs and the payload being installed are writable, nothing else), and
the system's own pid namespace separates it from the host. On a host without
bwrap the boundary is refused, with the fix. A home that declares `upstreams`
borrows through them -- link entries in its own store -- and has no system
layer.

### The one-time setup (Ubuntu 23.10 and later)

Ubuntu restricts unprivileged user namespaces with AppArmor; bwrap then fails with
`setting up uid map: Permission denied`. No sysctl changes:

```bash
xlings self doctor --isolation          # why
xlings self doctor --isolation --fix    # one sudo: a root-owned bwrap and one AppArmor profile for it
```

`subos new` asks for it when a sandbox needs it; in agent mode it exits 2 with
this command.
