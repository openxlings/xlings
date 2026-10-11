# Generated Command Reference: luban

<!-- Generated from `luban --command-reference-json`; do not edit by hand.
     Regenerate with:
       python3 tests/scripts/test_generated_command_reference.py \
         --xlings <path-to-xlings> --write
-->

Levels: `common` is what `luban --help` shows, `more` what `luban help --all` adds,
`expert` what `luban help --expert` adds.

## `luban`

Luban: environments and the system they run on

Options: `--json` — Machine-readable output; `--agent` — For an agent: plain output, never a question; `-y, --yes` — Answer yes to confirmations; `-v, --verbose` — More detail; `-q, --quiet` — Less output; `-h, --help` — This help; `--version` — The luban version

## `luban new <name> [edition]` (common)

Make an environment (edition: core by default)

Options: `--from <EDITION>` — The edition, as an option; `--proxy <URL>` — Its network goes only through this SOCKS5h proxy; `--carrier <CARRIER>` — Where it runs: local, wsl2, vz

## `luban enter <name>` (common)

Go into an environment

## `luban run <name> <command>...` (common)

Run one command in an environment

## `luban ls` (common)

List the environments

## `luban status [name]` (common)

This host, or one environment

## `luban config <name> [key] [value]` (more)

Show or set an environment's settings (proxy, tz, policy, allow, mount)

## `luban history [name]` (more)

An environment's generations

## `luban upgrade <name>` (more)

Move an environment to its edition's newest version

Options: `--to <VERSION>` — This version of the edition; `--dry-run` — Show the plan only; `-y, --yes` — Do not ask

## `luban rollback [name]` (more)

Go back a generation

Options: `--to <N>` — This generation

## `luban export <name> <file>` (more)

Make an image: .iso, .img, .qcow2, .tar(.zst|.gz), or a directory

Options: `--format <FORMAT>` — iso, img, qcow2, tar or dir, whatever the name says; `--size <SIZE>` — Disk size (img, qcow2); `--boot <PROFILE>` — How an image boots: generic (machines), virt (VMs), or ns:name

## `luban write <source> <device>` (more)

Make a boot drive from an image or an environment

Options: `--serial <SERIAL>` — The drive's serial (required with --agent)

## `luban try <source>` (more)

Boot an environment or an image in a local virtual machine

Options: `--proxy <URL>` — Its only network is this proxy (a private machine); `--boot <PROFILE>` — How it boots (default virt; the root's own kernel when there is none); `--memory <MB>` — Its memory (default 2048); `--nographic` — Its console in this terminal

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
