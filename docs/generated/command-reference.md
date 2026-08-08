# Generated Command Reference

<!-- Generated from `xlings --command-reference-json`; do not edit by hand.
     Regenerate with:
       python3 tests/scripts/test_generated_command_reference.py \
         --xlings <path-to-xlings> --write
-->

## `xlings`

Universal package management and SubOS environments

Options: `-h, --help` — Show help for the selected command; `--version` — Show version; `-y, --yes` — Skip confirmation prompts; `--agent` — Use stable plain-text output; `-v, --verbose` — Enable verbose output; `-q, --quiet` — Suppress non-essential output

## `xlings install [packages]...`

Install packages

Options: `-g, --global` — Use global scope; `-u, --use` — Activate installed version

## `xlings remove <package> [version]`

Remove a package

Options: `-g, --global` — Use global scope; `--force` — Remove even if installed packages depend on it

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

Options: `--lang <LANG>` — Set language; `--mirror <MIRROR>` — Set mirror; `--add-xpkg <FILE>` — Add package recipe; `--index-repo <NS:URL>` — Add index repository

## `xlings subos`

Manage SubOS environments

## `xlings subos new <name>`

Create a SubOS

Options: `--storage <MODE>` — shared, tmpfs or image; `--image-size <SIZE>` — Image size; `--from <SOURCE>` — Fork source; `--runtime <SPEC>` — Runtime binding, e.g. glibc@2.44

## `xlings subos use <name>`

Enter a SubOS

Options: `--global` — Persist the active SubOS; `--shell [KIND]` — Emit shell activation code; `--sandbox [BACKEND]` — Enable sandbox (bwrap or proot on Linux); `--cmd <COMMAND>` — Run one command; `--keep` — Keep the namespace keeper; `--no-keep` — Disable the namespace keeper; `--ttl <SECONDS>` — Keeper idle timeout; `--gpu` — Expose GPU devices (bwrap only)

## `xlings subos list`

List SubOS environments

## `xlings subos remove <name>`

Remove a SubOS

## `xlings subos info [name]`

Show SubOS details

## `xlings subos stop <name>`

Stop a SubOS keeper

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

Options: `--fix` — Repair; `--dry-run` — Preview; `--all` — Show all findings; `--reset-metadata` — Discard unreadable metadata

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
