# The Machine

**English** | [简体中文](../zh/luban/06-machine.md)

A Luban machine is one whose userland is a SubOS: the kernel starts stage-0,
stage-0 picks an environment, and that environment is the system.

## Booting

The kernel is started with `init=<home>/boot/luban-init` (images set this). Stage-0
is the first process: it reads no configuration before `/proc` exists, mounts
the kernel filesystems, reads `boot.json`, points `/usr` at the chosen
environment's generation, and executes its init. An init that does not come up
falls back to the next entry.

## Boot entries

```bash
luban boot                        # this machine's entries
luban boot box2 --once            # try box2 on the next boot only
luban boot --mark-good            # this boot worked: keep it
luban boot box2                   # make box2 the default
luban boot box2 --now             # switch now, without restarting the kernel (when the init supports it)
```

A trial (`--once`) that is not confirmed returns to the default on the next
boot; an entry whose boots keep failing gives way to its `--fallback`.

## Updating and rolling back a machine

Inside a machine, xlings is the package manager: `xlings install` installs into
the running system as a new generation; `luban rollback` returns to the previous
one. A broken library cannot prevent the rollback — xlings is static.

## Where a machine's home is

| Layout | The system home | |
|---|---|---|
| single | the path of the home that built it | one user, equivalent to root |
| multi | `/xlings`, owned by root | several users, each with a home of their own |

`/etc/xlings/root.json` is how stage-0 finds the system home.
