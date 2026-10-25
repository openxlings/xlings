# Generated Command Reference

<!-- Generated from `xlings --command-reference-json`; do not edit by hand.
     Regenerate with:
       python3 tests/scripts/test_generated_command_reference.py \
         --xlings <path-to-xlings> --write
-->

## `xlings`

Universal package management and SubOS environments

Options: `-h, --help` — Show help for the selected command; `--version` — Show version; `-y, --yes` — Skip confirmation prompts; `--agent` — Use stable plain-text output; `-v, --verbose` — Enable verbose output; `-q, --quiet` — Suppress non-essential output; `--ui-mode <MODE>` — Frontend for this run (cli/tui/auto); `--home <DIR>` — Use this home for this run only, as XLINGS_HOME does

## `xlings install [packages]...`

Install packages

Options: `-g, --global` — Use global scope; `-u, --use` — Activate installed version; `--reconfig` — Run the configuration step again, even where it already ran; `--subos <NAME>` — Install into this subos instead of the current one; `--system` — Install into the system layer (/xlings): every user of this machine gets it

## `xlings remove <package> [version]`

Remove a package

Options: `-g, --global` — Use global scope; `--force` — Remove even if packages depend on it, the recipe is gone, or its uninstall hook fails; `--all` — Remove every installed version, not just the active one; `--all-subos` — Remove from every subos that has it installed; `--subos <NAME>` — Act on this subos only, instead of the current one; `--older-than <VERSION>` — Remove every installed version older than VERSION that nothing uses; `--dry-run` — With --older-than: list what would be removed, and change nothing

## `xlings update [package] [version]`

Update package index or package

## `xlings search [keyword]`

Search for packages

Options: `--category <NAME>` — Only packages in this category (luban-desktop: the desktops)

## `xlings list [filter]`

List installed packages

Options: `-a, --all` — Show every subos

## `xlings info <package> [version]`

Show package information

Options: `--all-versions` — Show every available version

## `xlings why <package> [dep]`

Show why a dependency resolved to the version it did

## `xlings use <target> [version]`

Switch tool version

Options: `-a, --all` — Show every subos; `--strict` — Require a coherent release

## `xlings config`

Show or modify configuration

Options: `--lang <LANG>` — Set language; `--mirror <MIRROR>` — Set mirror; `--runtime <LIBC@VERSION>` — Pin this home's default runtime (glibc@2.44.3), or follow the newest installed (glibc@latest); `--ui-mode <MODE>` — Set UI mode (cli/tui/auto); `--theme <THEME>` — Set colour theme (name, path, or list); `--interactive <BOOL>` — Inline prompts in tui mode; `--add-xpkg <FILE>` — Add package recipe; `--list-xpkg` — List local recipes and how they relate to the synced index; `--remove-xpkg <NAME>` — Remove one local recipe; `--clear-xpkg <all|stale>` — Remove local recipes (all, or stale = identical/behind the synced index); `--index-repo <NS:URL>` — Add index repository; `--rm-index-repo <NAME>` — Remove index repository

## `xlings subos`

Manage SubOS environments

## `xlings subos new <name>`

Create a SubOS

Options: `--sandbox [PRESET]` — Declare its isolation once: dev (default), private or locked; `--storage <MODE>` — shared, tmpfs or image; `--image-size <SIZE>` — Image size; `--from <SOURCE>` — Fork source; `--runtime <SPEC>` — Runtime binding, e.g. glibc@2.44; `--rootfs` — Make it a root: entered, exported or booted as /; `--proxy <URL>` — Its network goes only through this SOCKS5h proxy, from the first entry; `--domain <HOME>` — Build a rootfs at this logical home prefix in an owned namespace; `--domain-if-possible <HOME>` — As --domain where this host can build in a private namespace; otherwise in this home's prefix, said once; `--carrier <NAME>` — Where it runs: local, wsl2 (Windows) or vz (macOS); default chosen by what it is; `--abi <ABI>` — native (this machine's programs) or linux; `--dry-run` — With --from: what it would be made of (each layer, its index, its manifest), nothing made; `--offline-from <DIR>` — Payloads and an index to make it from first (DIR/payloads, DIR/index): no network for what they hold

## `xlings subos use [name]`

Enter a SubOS

Options: `--global` — Persist the active SubOS; `--shell [KIND]` — Emit shell activation code; `--sandbox [BACKEND]` — Enable sandbox (bwrap, proot or landlock on Linux); --sandbox=dev|private|locked picks a preset; `--net <MODE>` — This call only: host, nat, none or proxy (may only tighten); `--proxy <URL>` — SOCKS5h endpoint for net=proxy; `--observe <LEVEL>` — This call only: off, basic, standard or full; `--fetch <ACTION>` — This call only: auto, ask or deny (may only tighten); `--allow <GRANT>` — This call only: grant from the policy's grants_allowed; `--no-degrade` — Refuse to enter when anything asked for is missing; `--mount <HOST[:INSIDE][:ro|rw]>` — This call only: map a host path into the SubOS; repeatable; `--cmd <COMMAND>` — Run one command; `--keep` — Keep the session after the shell exits; `--no-keep` — End the session with the shell; `--ttl <SECONDS>` — Session idle timeout; `--gpu` — Expose GPU devices (bwrap only); `--persist <MODE>` — A root's state after this session: all, home (only /home stays) or none; `--user <NAME>` — A root: log in as this user this once (root, or one of its users)

## `xlings subos list`

List SubOS environments

Options: `--all` — Also list the Luban systems (luban ls lists them as systems)

## `xlings subos remove <name>`

Remove a SubOS

## `xlings subos info [name]`

Show SubOS details

## `xlings subos stop <name>`

Stop a SubOS's running session

## `xlings subos exec [name] [command]...`

Run a command in a SubOS from outside it

Options: `--sandbox [BACKEND]` — Run in the SubOS's sandbox (bwrap, proot or landlock on Linux); --sandbox=dev|private|locked picks a preset; `--net <MODE>` — This call only: host, nat, none or proxy (may only tighten); `--proxy <URL>` — SOCKS5h endpoint for net=proxy; `--observe <LEVEL>` — This call only: off, basic, standard or full; `--fetch <ACTION>` — This call only: auto, ask or deny (may only tighten); `--allow <GRANT>` — This call only: grant from the policy's grants_allowed; `--no-degrade` — Refuse to run when anything asked for is missing; `--publish <HOST:SANDBOX>` — With net=nat: publish a TCP port; repeatable; `--mount <HOST[:INSIDE][:ro|rw]>` — This call only: map a host path into the SubOS; repeatable; `--cwd <DIR>` — Working directory inside; `--user <NAME>` — A root: run as this user (root, or one of its users); `--env <K=V>` — Set a variable; repeatable; `--timeout <DURATION>` — End the command after DURATION (90, 30s, 10m, 2h); exits 124; `--json` — Print the result as JSON on stderr when the command ends; `--temp` — Use a throwaway SubOS, removed afterwards (its audit is kept); `--from <SOURCE>` — With --temp: fork it from this SubOS or package

## `xlings subos start <name>`

Start a SubOS session that runs without a terminal

Options: `--sandbox [BACKEND]` — Sandbox backend (bwrap, proot or landlock); --sandbox=dev|private|locked picks a preset; `--net <MODE>` — host, nat, none or proxy (may only tighten); `--proxy <URL>` — SOCKS5h endpoint for net=proxy; `--observe <LEVEL>` — This call only: off, basic, standard or full; `--allow <GRANT>` — Grant from the policy's grants_allowed; `--no-degrade` — Refuse to start when anything asked for is missing; `--publish <HOST:SANDBOX>` — With net=nat: publish a TCP port; repeatable; `--mount <HOST[:INSIDE][:ro|rw]>` — This call only: map a host path into the SubOS; repeatable; `--ttl <DURATION>` — End after DURATION idle (90, 30s, 10m, 2h); default: until stop

## `xlings subos cp <src> <dst>`

Copy files into or out of a SubOS

## `xlings subos config <name>`

Show or change what a SubOS may do (its policy)

Options: `--sandbox <PRESET>` — Start from a preset (dev, private, locked) or a policy package (ns:name[@version]); `--policy-upgrade` — Move to the newest version of the selected policy package; `--net <MODE>` — host, nat, none or proxy; `--proxy <URL>` — SOCKS5h endpoint for net=proxy; `--tz <ZONE>` — A neutral identity's time zone: utc, proxy (the proxy's exit) or a name such as Asia/Tokyo; `--fetch <ACTION>` — Installing a missing package from inside: auto, ask or deny; `--index-update <ACTION>` — Updating the index from inside: auto, ask or deny; `--observe <LEVEL>` — off, basic, standard or full; `--allow <GRANT>` — Grant display, audio, camera, gpu, ssh-agent, dbus or host-loopback; `--disallow <GRANT>` — Withdraw a grant; `--grants-allowed <LIST>` — Grants a single call may add; `--env-pass <NAME>` — Let this host variable in; NAME* for a prefix; `--mount <HOST[:INSIDE][:ro|rw]>` — Map a host path into the SubOS, every time it is entered; `--unmount <PATH>` — Stop mapping it; `--no-degrade` — Refuse to enter when anything asked for is missing; `--degrade` — Enter and report what is missing; `--reset` — Remove the policy file; `--json` — Machine-readable output

## `xlings subos status [name]`

Show what a SubOS asks for and what this host gives it

Options: `--json` — Machine-readable output

## `xlings subos doctor [name]`

Check each SubOS: policy, entry on this host, policy package, sessions

Options: `--json` — Machine-readable output; `--fix` — Re-install a selected policy package whose payload is missing

## `xlings subos requests <name>`

List what a SubOS's sandbox asked for and is waiting on

Options: `--json` — One JSON object per request

## `xlings subos approve <name> <id>`

Run a request a SubOS's sandbox is waiting on

## `xlings subos deny <name> <id>`

Refuse a request a SubOS's sandbox is waiting on

## `xlings subos report [name]`

Summarise what happened in a SubOS: sessions, programs, permissions, files

Options: `--session <ID>` — Only this session; `--json` — Machine-readable output

## `xlings subos ps [name]`

List running SubOS sessions

Options: `--json` — One JSON object per session

## `xlings subos log [name]`

Show a SubOS's audit events

Options: `--kind <KIND>` — Only this kind (ops, lifecycle, perm, exec, net, fs); repeatable; `--session <ID>` — Only this session; `-n, --lines <N>` — Show the last N events (default 50); `-f, --follow` — Keep printing new events; `--json` — One JSON object per event

## `xlings subos upgrade <name>`

Move a SubOS made from an edition to its newest version (your packages stay yours)

Options: `--to <VERSION>` — This version of the edition, or the package that replaces it (ns:name[@version]); `--dry-run` — Show the plan only

## `xlings subos rollback <name>`

Move a root SubOS back to an earlier generation

Options: `--to <N>` — This generation (default: the one before); `--list` — List the generations

## `xlings subos export <name>`

Export a root SubOS as a directory or a tarball (an image a machine boots: luban export)

Options: `--rootfs <DIR>` — A root directory (chroot, bwrap, nspawn); `--tar <FILE>` — A root tarball (docker import, podman import, wsl --import); `--disk <FILE>` — An ext4 filesystem image; `--size <SIZE>` — Disk size (default 4G); `--with-data` — Include /root, /home, /var, /srv, /opt

## `xlings subos diff <a> <b>`

Compare the packages of two SubOS

## `xlings subos pack <name>`

Pack a SubOS's declaration as a subos-type xpkg

Options: `--as <NS:NAME@VERSION>` — The package it becomes; `--out <DIR>` — Where the tarball goes

## `xlings subos runtime <binding> [name]`

Rebind a SubOS to another runtime

## `xlings self`

Manage xlings itself

## `xlings self install`

Install xlings

## `xlings self uninstall`

Uninstall xlings

Options: `-y, --yes` — Skip confirmation; `--keep-data` — Keep data; `--dry-run` — Preview

## `xlings self init`

Initialize directories

Options: `--home <DIR>` — Initialize DIR and select it as this user's home; `--force` — Replace a home already selected (nothing is moved or deleted)

## `xlings self update`

Update xlings

Options: `--user` — Install into this home even when xlings is a system package's

## `xlings self config`

Show configuration

## `xlings self clean`

Clean cache

Options: `--dry-run` — Preview

## `xlings self migrate`

Migrate old layout

## `xlings self log [trace]`

What an operation did: the latest ones, or one by its trace

Options: `--bundle [FILE]` — The operation and its side files in one file, the home's paths and the user's name taken out; `--json` — The journal's lines as they are; `--subos <NAME>` — The latest operation on this subos (a system's own record followed where it links)

## `xlings self doctor`

Verify installation

Options: `--deep` — Audit package payloads and runtime functionality; `--scope <PACKAGE[@VERSION]>` — Limit deep payload/runtime audit to one local package coordinate; `--subos <NAME>` — Check/repair one specific subos instead of the active one; `--fix` — Repair (implies --deep; walks every subos that owns a finding); `--dry-run` — Preview repairs without changing detection depth; `--show-ok` — Show all findings, including non-defects; `--all` — Deprecated alias for --show-ok; `--reset-metadata` — Discard unreadable metadata; `--isolation` — Check what this host can isolate SubOS sandboxes with; with --fix, install a root-owned bwrap and its AppArmor profile; `--json` — Machine-readable output (with --isolation)

## `xlings script <script-file> [args]...`

Run an xlings script

## `xlings interface [capability]`

Use the NDJSON interface

Options: `--args <JSON>` — Capability arguments; `--args-file <PATH>` — Read capability arguments from a file; `--list` — List capabilities; `--version` — Show protocol version

## `xlings index`

Inspect and select package index snapshots

## `xlings index list [name]`

List published index snapshots

Options: `--json` — Machine-readable output

## `xlings index use <name> <version>`

Pin an index source to a snapshot

## `xlings agent`

Agent integration

## `xlings agent skills [name]`

List or show built-in skills

## `xlings profile`

Manage profile configuration

## `xlings profile list`

List recorded generations

## `xlings profile commit [reason]`

Record the active generation

## `xlings profile rollback <generation>`

Restore a recorded generation
