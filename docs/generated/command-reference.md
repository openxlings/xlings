# Generated Command Reference

<!-- Generated from `xlings --command-reference-json`; do not edit by hand.
     Regenerate with:
       python3 tests/scripts/test_generated_command_reference.py \
         --xlings <path-to-xlings> --write
-->

## `xlings`

Universal package management and SubOS environments

Options: `-h, --help` — Show help for the selected command; `--version` — Show version; `-y, --yes` — Skip confirmation prompts; `--agent` — Use stable plain-text output; `-v, --verbose` — Enable verbose output; `-q, --quiet` — Suppress non-essential output; `--ui-mode <MODE>` — Frontend for this run (cli/tui/auto)

## `xlings install [packages]...`

Install packages

Options: `-g, --global` — Use global scope; `-u, --use` — Activate installed version; `--reconfig` — Run the configuration step again, even where it already ran

## `xlings remove <package> [version]`

Remove a package

Options: `-g, --global` — Use global scope; `--force` — Remove even if packages depend on it, the recipe is gone, or its uninstall hook fails; `--all` — Remove every installed version, not just the active one; `--all-subos` — Remove from every subos that has it installed; `--subos <NAME>` — Act on this subos only, instead of the current one

## `xlings update [package] [version]`

Update package index or package

## `xlings search <keyword>`

Search for packages

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

Options: `--lang <LANG>` — Set language; `--mirror <MIRROR>` — Set mirror; `--ui-mode <MODE>` — Set UI mode (cli/tui/auto); `--theme <THEME>` — Set colour theme (name, path, or list); `--interactive <BOOL>` — Inline prompts in tui mode; `--add-xpkg <FILE>` — Add package recipe; `--list-xpkg` — List local recipes and how they relate to the synced index; `--remove-xpkg <NAME>` — Remove one local recipe; `--clear-xpkg <all|stale>` — Remove local recipes (all, or stale = identical/behind the synced index); `--index-repo <NS:URL>` — Add index repository; `--rm-index-repo <NAME>` — Remove index repository

## `xlings subos`

Manage SubOS environments

## `xlings subos new <name>`

Create a SubOS

Options: `--storage <MODE>` — shared, tmpfs or image; `--image-size <SIZE>` — Image size; `--from <SOURCE>` — Fork source; `--runtime <SPEC>` — Runtime binding, e.g. glibc@2.44

## `xlings subos use [name]`

Enter a SubOS

Options: `--global` — Persist the active SubOS; `--shell [KIND]` — Emit shell activation code; `--sandbox [BACKEND]` — Enable sandbox (bwrap or proot on Linux); --sandbox=dev|private|locked picks a preset; `--net <MODE>` — This call only: host, nat, none or proxy (may only tighten); `--fetch <ACTION>` — This call only: auto, ask, layer or deny (may only tighten); `--allow <GRANT>` — This call only: grant from the policy's grants_allowed; `--no-degrade` — Refuse to enter when anything asked for is missing; `--cmd <COMMAND>` — Run one command; `--keep` — Keep the session after the shell exits; `--no-keep` — End the session with the shell; `--ttl <SECONDS>` — Session idle timeout; `--gpu` — Expose GPU devices (bwrap only)

## `xlings subos list`

List SubOS environments

## `xlings subos remove <name>`

Remove a SubOS

## `xlings subos info [name]`

Show SubOS details

## `xlings subos stop <name>`

Stop a SubOS's running session

## `xlings subos exec [name] [command]...`

Run a command in a SubOS from outside it

Options: `--sandbox [BACKEND]` — Run in the SubOS's sandbox (bwrap or proot on Linux); --sandbox=dev|private|locked picks a preset; `--net <MODE>` — This call only: host, nat, none or proxy (may only tighten); `--fetch <ACTION>` — This call only: auto, ask, layer or deny (may only tighten); `--allow <GRANT>` — This call only: grant from the policy's grants_allowed; `--no-degrade` — Refuse to run when anything asked for is missing; `--cwd <DIR>` — Working directory inside; `--env <K=V>` — Set a variable; repeatable; `--timeout <DURATION>` — End the command after DURATION (90, 30s, 10m, 2h); exits 124; `--json` — Print the result as JSON on stderr when the command ends; `--temp` — Use a throwaway SubOS, removed afterwards (its audit is kept); `--from <SOURCE>` — With --temp: fork it from this SubOS or package

## `xlings subos start <name>`

Start a SubOS session that runs without a terminal

Options: `--sandbox [BACKEND]` — Sandbox backend (bwrap or proot); --sandbox=dev|private|locked picks a preset; `--net <MODE>` — host, nat, none or proxy (may only tighten); `--allow <GRANT>` — Grant from the policy's grants_allowed; `--no-degrade` — Refuse to start when anything asked for is missing; `--ttl <DURATION>` — End after DURATION idle (90, 30s, 10m, 2h); default: until stop

## `xlings subos cp <src> <dst>`

Copy files into or out of a SubOS

## `xlings subos config <name>`

Show or change what a SubOS may do (its policy)

Options: `--sandbox <PRESET>` — Start from a preset: dev, private or locked; `--net <MODE>` — host, nat, none or proxy; `--fetch <ACTION>` — Installing a missing package from inside: auto, ask, layer or deny; `--index-update <ACTION>` — Updating the index from inside: auto, ask or deny; `--observe <LEVEL>` — off, basic, standard or full; `--allow <GRANT>` — Grant display, audio, camera, gpu, ssh-agent, dbus or host-loopback; `--disallow <GRANT>` — Withdraw a grant; `--grants-allowed <LIST>` — Grants a single call may add; `--env-pass <NAME>` — Let this host variable in; NAME* for a prefix; `--no-degrade` — Refuse to enter when anything asked for is missing; `--degrade` — Enter and report what is missing; `--reset` — Remove the policy file; `--json` — Machine-readable output

## `xlings subos status [name]`

Show what a SubOS asks for and what this host gives it

Options: `--json` — Machine-readable output

## `xlings subos ps`

List running SubOS sessions

Options: `--json` — One JSON object per session

## `xlings subos log [name]`

Show a SubOS's audit events

Options: `--kind <KIND>` — Only this kind (ops, lifecycle, perm, exec, net, fs); repeatable; `--session <ID>` — Only this session; `-n, --lines <N>` — Show the last N events (default 50); `-f, --follow` — Keep printing new events; `--json` — One JSON object per event

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

## `xlings self update`

Update xlings

## `xlings self config`

Show configuration

## `xlings self clean`

Clean cache

Options: `--dry-run` — Preview

## `xlings self migrate`

Migrate old layout

## `xlings self doctor`

Verify installation

Options: `--deep` — Audit package payloads and runtime functionality; `--scope <PACKAGE[@VERSION]>` — Limit deep payload/runtime audit to one local package coordinate; `--subos <NAME>` — Check/repair one specific subos instead of the active one; `--fix` — Repair (implies --deep; walks every subos that owns a finding); `--dry-run` — Preview repairs without changing detection depth; `--show-ok` — Show all findings, including non-defects; `--all` — Deprecated alias for --show-ok; `--reset-metadata` — Discard unreadable metadata

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
