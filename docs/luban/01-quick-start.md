# Quick Start

**English** | [简体中文](../zh/luban/01-quick-start.md)

`luban` is a package of its own: `xlings install luban` (the window:
`xlings install luban-os`). Its version follows xlings' releases; `xlings update
luban` updates it. On the host, in every Luban environment and on every Luban
machine it is the same program. This page is one path through it: installing,
an environment, work in it, go back a step, an agent's workspace, an upgrade,
the environment as a machine, a desktop, using it offline, and cleaning up.
Each step says what you should see.

## 0. Install

```bash
xlings install luban luban-os    # the luban command and the Luban OS window
luban status                     # this host: what it can run, and the parts' versions
```

`luban status` says in one line which versions of xlings, luban, Luban OS and
the index you have; where they disagree, each with what to do.

## 1. An environment

```bash
luban new box                           # Luban Core, the default: shells, editors, git, compilers
luban new small --edition tiny          # or an edition: nano, tiny, tiny-musl, core, agent-workspace, desktop
luban new desk --edition desktop --desktop lxqt   # a desktop: xdesk (its own), labwc, compositor, lxqt, kde
luban new mine --xpkg myindex:myos      # any index's system (02)
luban search                            # what the index offers: distributions, desktops by what they bring
```

One is enough. You see two lines while it runs — `▸ making box (core)  58%
fetch ✓ · unpack ✓ · configure ● · verify`, and under it the stage's own
progress, what it is doing and how long it has taken — then `✓ box is ready`,
what it is (`box  core@… · N packages · generation N`) and the next step
(`enter  luban enter box`). The first `luban new` on a host may ask for a
one-time setup first (below). Luban Core is the default: a whole working
system with compilers and build tools; `tiny` is the small one, `desktop` has a
desktop. If it stops halfway, `luban new` says what it left (`luban rm box`
clears it, then make it again); a name being made by another `luban new` is
said, not taken twice.
`nano` has no shell: it says so, and its next step is `luban run box -- …`.

## 2. Work in it

```bash
luban enter box               # a shell in it; the prompt says where: [luban] root@box:~#
luban run box -- uname -a     # one command in it, from outside
luban run box -- xlings install -y cmake
```

A console says first what it is -- the system and its edition, how it runs
(sharing this host's kernel, or a machine with its own), what it keeps, who
logs in, what xlings manages there, the time -- and how to leave (in Chinese
where your language is), then its prompt:

```
 Luban OS 2026.10.15.1 · [luban] box
 ──────────────────────────────────────────────────────────
 system    tiny 2026.10.11.1 · generation 4 · 4 packages
 runs as   sandbox: shares this host's kernel (Linux 6.8.0)
 keeps     everything
 user      root · /root
 xlings    manages this system only (R)
 time      2026-10-15 09:00 UTC
 ──────────────────────────────────────────────────────────
 exit to leave · luban status box
```

The prompt's mark says where you are: `[luban]` a system that shares this
host's kernel, `<luban>` one with its own (`--mode machine`), `:temp` or
`:home` when it keeps less than everything (`[luban:temp] root@box:~#`). Its
host name is its name. A view of your host made with `xlings subos` is marked
`(name)` instead.

A Luban system is its own userland: its xlings manages only it (`/xlings`
inside), it shares no package files with your host's SubOS, and your user
name appears nowhere in it. Its users are its own (below).

A regular user installs without root. In a machine, their `xlings` uses their
own `~/.xlings`, with the system's packages (`/xlings`) as the layer below: what
the system has is used, what they install is theirs alone. In a sandbox, an
install goes through the system's broker into the system itself.

Installing software is still xlings: `xlings install <package>` — inside an
environment it installs into that environment. `exit` leaves it.

### Who logs in

`luban enter` logs in as root unless the system names another user. A desktop
always has one: `luban new d --edition desktop` asks for a name (`lubanoser`
by default) and a password, typed hidden (empty: none, it logs in without
one); `-y` takes the defaults. Any edition can have one:

```bash
luban new box --edition core --user alice --no-password
luban enter box --user root   # root this once
luban config box --login root # and from now on
```

The password is stored only as its hash, in the system's own `/etc/shadow`.
To act as root inside, enter as root: you own the system from your host.

## 3. Go back a step

```bash
luban history box             # its generations: one per change
luban rollback box            # back one: cmake is gone again
```

A generation is the packages as well as the files they make: going back to it
stays when you enter again.

## 4. An agent's workspace

```bash
luban new agent --edition agent-workspace
luban run agent -- claude
luban status agent            # what it may do: its policy, network and grants
```

An agent's workspace is isolated by its policy; its only network can be a proxy
(`luban config agent --proxy socks5h://127.0.0.1:7897`). See
[Agent workspace](04-agent-workspace.md).

## 5. Upgrade

```bash
luban upgrade box --dry-run   # the plan: the edition, packages added, upgraded and kept
luban upgrade box             # one new generation; rollback undoes it
```

`--dry-run` moves nothing: it says `box is up to date`, or lists what would
move and names `luban upgrade box`. Nothing is upgraded for you: a system
stays as it was made until you upgrade it, and `luban status box` says when an
upgrade has something for it. An upgrade that moves a system into its own
userland keeps what it was before; going back to that is one command:

```bash
luban rollback box --to relayout -y   # the system as it was before its move
```

## 6. Run it as a machine

```bash
xlings install -y qemu-system            # the virtual machine monitor, from the index
luban enter box --mode machine           # the environment itself, with a kernel of its own
luban enter box --mode machine --temp    # the same, and nothing it writes stays
luban config box --mode machine          # from now on, luban enter starts it this way
```

The machine boots the environment itself, not a copy: what it writes is the
environment's. You get its login shell in this terminal in a few seconds.
`exit` asks whether it keeps running; Ctrl-] then `d` leaves it running, Ctrl-]
then `p` powers it off. `luban stop box` powers it off from anywhere.

## 7. A desktop

```bash
luban new desk --edition desktop         # xdesk: tile, stack or a canvas, a panel, a launcher, a terminal
xlings install luban-os                  # the Luban OS window
luban enter desk --display window
```

The window shows the desktop at its own size and holds the keyboard while it
does: Ctrl+Alt+K lets it go (and takes it back); the Host key (Ctrl+Alt alone)
opens its menu. Super+m changes how the windows are arranged, Super+/ lists the
keys. See [The desktop and the Luban OS window](08-desktop.md).

## Offline

A system's home starts from a copy of your host's index, so it reads the index
without the network. An exported system carries what it runs on:

```bash
luban export box box.luban      # the system, its packages and its index
luban import box.luban          # elsewhere: made again with no network
```

## 8. A boot drive (optional)

```bash
luban write box.iso /dev/sdb  # every byte of the drive is replaced; it asks first
```

## 9. Clean up

```bash
luban rm box                  # asks first, and says how much its home holds
xlings remove gcc --older-than 16 --dry-run
```

`xlings remove <package> --older-than <version>` removes the versions of a
package nothing uses any more; `--dry-run` lists them first.

## The first time on a host

On Ubuntu 23.10 and later, AppArmor keeps programs without a profile from making
the sandbox an environment runs in. The first `luban new` checks the host before
anything is downloaded and asks for a one-time setup: a root-owned bwrap and one
narrow AppArmor profile, for every user of the machine, with one `sudo`.

```bash
luban setup                   # the same setup, whenever you like
```

A host with no bwrap at all gets one from the index first, in the same step.
A session that keeps less than everything (`--temp`, `--persist home`) needs a
bwrap of 0.10 or later; on a host whose own is older (Ubuntu 24.04 ships 0.9)
the index's is installed the first time. An agent (`--agent`) is never asked:
the command exits 2 and names this setup.

## More

```bash
luban                         # one screen: where you are, the environments, what next
luban ls                      # the environments
luban ls b                    # those whose name matches
luban help --all              # every command
luban <command> --help        # one command, and the xlings command it is
```

[Editions](02-editions.md) · [Environments](03-environments.md) · [Images](05-images.md) · [Desktop](08-desktop.md)
