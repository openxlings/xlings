# Generated Command Reference: luban

<!-- Generated from `luban --command-reference-json`; do not edit by hand.
     Regenerate with:
       python3 tests/scripts/test_generated_command_reference.py \
         --xlings <path-to-xlings> --write
-->

Levels: `common` is what `luban --help` shows, `more` what `luban help --all` adds,
`expert` what `luban help --expert` adds.

## `luban`

Luban OS: environments, and the machines they run as

Options: `--json` — Machine-readable output: the events and the result; `--agent` — For an agent: plain output, never a question; `-y, --yes` — Answer yes to confirmations; `-v, --verbose` — Each step as well; `-q, --quiet` — Only what failed; `--as <ACTOR>` — Who this is (agent:<name>[;via=person:<name>]): recorded and inherited by what it runs; `-h, --help` — This help; `--version` — The luban version

## `luban new <name>` (common)

Make an environment

Options: `--edition <EDITION>` — An official system by its short name: core (default), desktop, tiny, nano, ...; `--xpkg <COORD>` — Any package, from any index: a system (one), desktops, packages; repeat it; `--desktop <SESSION>` — Its desktop (an official one is installed when it has none); `--enter <WHAT>` — console or desktop: what going in shows first; `--mode <MODE>` — sandbox or machine; `--persist <WHAT>` — all, home or none; `--shell <SHELL>` — Its login shell; `--login <NAME>` — Who luban enter logs in as; `--on-close <WHAT>` — ask, background or poweroff; `--policy <PRESET>` — dev, private, locked, or ns:name; `--tz <ZONE>` — utc, proxy, or Area/City; `--allow <GRANT>` — Grant a capability (clipboard, gpu, audio, ...); `--runtime <LIBC@VERSION>` — Its default runtime; `--dry-run` — What it would be made of, and nothing made; `--user <NAME>` — Its login user (a desktop needs one; default lubanoser); `--password-file <FILE>` — Its user's password, the file's first line; `--no-password` — Its user logs in without a password, and its sudo asks nothing (otherwise: the default password lubanoser); `--proxy <URL>` — Its network goes only through this SOCKS5h proxy; `--carrier <CARRIER>` — Where it runs: local, wsl2, vz

## `luban enter <name>` (common)

Go into an environment, or boot an image

Options: `--mode <MODE>` — sandbox (this machine's kernel) or machine (its own); default: its setting; `--temp` — Nothing it writes stays; `--user <NAME>` — Log in as this user this once (root, or one of its users); `--display <DISPLAY>` — terminal, window or virtual[=WxH@S] (its desktop with no window); default: its desktop decides; `--desktop <SESSION>` — This desktop, this once, in a window

## `luban logs <name>` (more)

What a system's desktop, its programs and its machine said, gathered

Options: `--app <ID>` — Only this program's (its desktop entry's id, as falkon); `--since <TIME>` — Only what changed in this time back: 30s, 10m, 2h, 1d; `--bundle <FILE>` — All of it, with its status, in one tar to share; `--with-screenshot` — The bundle also holds a picture of its desktop

## `luban display <name> [action] [args]...` (more)

The displays of a running desktop: list, capture, record, attach, detach, mode, input, end

Options: `--display <N>` — This display (its number in the list); default: the virtual one; `--as <ROLE>` — attach: primary (default) or mirror

## `luban run <name> <command>...` (common)

Run one command in an environment

Options: `--user <NAME>` — As this user (root, or one of its users); `--display <DISPLAY>` — terminal (here) or window: a graphical program alone in a window of its own

## `luban ls [pattern]` (common)

List the environments, or those whose name matches

Options: `--all` — And what cancelled makings set aside

## `luban search [keyword]` (common)

What the index offers: systems to make, desktops to give one

Options: `--distributions` — Only the systems that can be made; `--desktops` — Only the desktops a system can have

## `luban status [name]` (common)

This host, or one environment

Options: `--check` — Check that it is usable: its /usr, /etc, runtime closure, and programs; `--log` — What its latest operation did, its own record of it too; `--bus` — What is on its bus: what its programs offer; the host's: the systems linked under it

## `luban config <name>` (more)

Show or change an environment's settings

Options: `--mode <MODE>` — sandbox or machine: how `luban enter` starts it; `--persist <WHAT>` — all, home (only /home stays) or none: what a session keeps; `--shell <SHELL>` — Its login shell (fish, bash, sh, or a path); `--login <NAME>` — Who luban enter logs in as: root, or one of its users; `--runtime <LIBC@VERSION>` — Its default runtime (glibc@2.44.3): a new generation runs on it; `--desktop <SESSION>` — The desktop it starts, or none; `--on-close <WHAT>` — ask, background or poweroff: closing its window; `--enter <WHAT>` — console or desktop: what going in shows first; `--files <HOW>` — virtiofs (9p where it cannot) or 9p: how a machine shares its files; `--kernel <VERSION>` — The kernel it boots as a machine (a linux-kernel-virt version), or default: its own; `--policy <PRESET>` — dev, private, locked, or ns:name; `--proxy <URL>` — Its only network is this SOCKS5h proxy; off for none; `--tz <ZONE>` — utc, proxy, or Area/City; `--allow <GRANT>` — Grant a capability (clipboard, gpu, audio, ...); `--disallow <GRANT>` — Take a grant back; `--mount <HOST[:INSIDE][:ro|rw]>` — Map a host directory into it; `--unmount <INSIDE>` — Remove a mapping; `--fetch <ACTION>` — auto, ask or deny: installing from inside; `--observe <LEVEL>` — off, basic, standard or full

## `luban stop <name>` (more)

Stop a running environment or machine

## `luban history [name]` (more)

An environment's generations

## `luban upgrade <name>` (more)

Move an environment to its edition's newest version

Options: `--to <VERSION>` — This version of the edition; `--dry-run` — Show the plan only

## `luban rollback [name]` (more)

Go back a generation

Options: `--to <N>` — This generation, or `relayout`: what it was before its move into its own userland

## `luban export <name> <file>` (more)

Write it out: .iso, .img, .qcow2, .tar(.zst), .luban, or a directory

Options: `--format <FORMAT>` — iso, img, qcow2, tar, luban or dir, whatever the name says; `--size <SIZE>` — Disk size (img, qcow2); `--boot <PROFILE>` — How an image boots: generic (machines), virt (VMs), or ns:name

## `luban import <file>` (more)

Make an environment from a .luban someone exported

Options: `--name <NAME>` — Its name here (default: the one it had)

## `luban write <source> <device>` (more)

Make a boot drive from an image or an environment

Options: `--serial <SERIAL>` — The drive's serial (required with --agent)

## `luban rm [name]` (more)

Remove an environment (asks first)

Options: `--unfinished` — What cancelled or failed makings set aside

## `luban setup` (more)

This host's one-time setup (isolation)

## `luban help` (more)

Help; --all for every command

Options: `--all` — Every command; `--expert` — And the expert ones

## `luban boot [name]` (expert)

This machine's boot entries

Options: `--once` — Only the next boot (a trial); `--fallback` — When the default fails; `--mark-good` — This boot worked; `--now` — Switch to it now

## `luban interface [verb]` (expert)

For programs: the verbs described (openas), or one run with JSON arguments, reported as NDJSON

Options: `--describe` — Describe every verb: its arguments, options, effect and what answers it; `--args <JSON>` — The verb's arguments and options, by name; `--args-file <FILE>` — The same, read from a file
