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

Options: `--json` — Machine-readable output: the events and the result; `--agent` — For an agent: plain output, never a question; `-y, --yes` — Answer yes to confirmations; `-v, --verbose` — Each step as well; `-q, --quiet` — Only what failed; `-h, --help` — This help; `--version` — The luban version

## `luban new <name>` (common)

Make an environment

Options: `--edition <EDITION>` — nano, tiny, core (default), desktop, desktop-xdesk, agent-workspace, or ns:name[@version]; `--proxy <URL>` — Its network goes only through this SOCKS5h proxy; `--carrier <CARRIER>` — Where it runs: local, wsl2, vz

## `luban enter <name>` (common)

Go into an environment, or boot an image

Options: `--mode <MODE>` — sandbox (this machine's kernel) or machine (its own); default: its setting; `--temp` — Nothing it writes stays; `--display <DISPLAY>` — terminal or window; default: its desktop decides

## `luban run <name> <command>...` (common)

Run one command in an environment

## `luban ls` (common)

List the environments

## `luban status [name]` (common)

This host, or one environment

## `luban config <name>` (more)

Show or change an environment's settings

Options: `--mode <MODE>` — sandbox or machine: how `luban enter` starts it; `--persist <WHAT>` — all, home (only /home stays) or none: what a session keeps; `--shell <SHELL>` — Its login shell (fish, bash, sh, or a path); `--desktop <SESSION>` — The desktop it starts, or none; `--on-close <WHAT>` — ask, background or poweroff: closing its window; `--policy <PRESET>` — dev, private, locked, or ns:name; `--proxy <URL>` — Its only network is this SOCKS5h proxy; off for none; `--tz <ZONE>` — utc, proxy, or Area/City; `--allow <GRANT>` — Grant a capability (clipboard, gpu, audio, ...); `--disallow <GRANT>` — Take a grant back; `--mount <HOST[:INSIDE][:ro|rw]>` — Map a host directory into it; `--unmount <INSIDE>` — Remove a mapping; `--fetch <ACTION>` — auto, ask or deny: installing from inside; `--observe <LEVEL>` — off, basic, standard or full

## `luban stop <name>` (more)

Stop a running environment or machine

## `luban history [name]` (more)

An environment's generations

## `luban upgrade <name>` (more)

Move an environment to its edition's newest version

Options: `--to <VERSION>` — This version of the edition; `--dry-run` — Show the plan only

## `luban rollback [name]` (more)

Go back a generation

Options: `--to <N>` — This generation

## `luban export <name> <file>` (more)

Write it out: .iso, .img, .qcow2, .tar(.zst), .luban, or a directory

Options: `--format <FORMAT>` — iso, img, qcow2, tar, luban or dir, whatever the name says; `--size <SIZE>` — Disk size (img, qcow2); `--boot <PROFILE>` — How an image boots: generic (machines), virt (VMs), or ns:name

## `luban import <file>` (more)

Make an environment from a .luban someone exported

Options: `--name <NAME>` — Its name here (default: the one it had)

## `luban write <source> <device>` (more)

Make a boot drive from an image or an environment

Options: `--serial <SERIAL>` — The drive's serial (required with --agent)

## `luban rm <name>` (more)

Remove an environment (asks first)

## `luban setup` (more)

This host's one-time setup (isolation)

## `luban help` (more)

Help; --all for every command

Options: `--all` — Every command; `--expert` — And the expert ones

## `luban boot [name]` (expert)

This machine's boot entries

Options: `--once` — Only the next boot (a trial); `--fallback` — When the default fails; `--mark-good` — This boot worked; `--now` — Switch to it now
