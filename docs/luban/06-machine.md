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

## Its files

A machine's tree and the home it reads are shared with it, not copied:
over virtio-fs, one unprivileged `virtiofsd` for each share (the first machine
installs `xim:virtiofsd`), and over 9p where that cannot be -- a machine
without hardware virtualization, a host it is not built for, a daemon that
does not start. `luban status <name>` says which,
and why:

```bash
luban status box                  # files     virtiofs
luban config box --files 9p       # always 9p, from the next start
```

Listing and reading many files is about ten times faster over virtio-fs; what
the system may do with its files is the same on both.

Both keep every file the host user's. A machine with users of its own besides
the one it was made with (`adduser alice` inside) has its `/home` over 9p with
`mapped-xattr` from its next start: each user's owner and modes are kept in the
host file's extended attributes, and a home made before is given to its user
at boot. Its own user notices nothing; `luban status` says
`/home over 9p (users of their own)`.

## Where a machine's home is

| Layout | The system home | |
|---|---|---|
| single | the path of the home that built it | one user, equivalent to root |
| multi | `/xlings`, owned by root | several users, each with a home of their own |

`/etc/xlings/root.json` is how stage-0 finds the system home.
