# `xlings self uninstall` — Design

**Date**: 2026-05-04
**Status**: design (pre-implementation)
**Owner**: TBD
**Related**: existing `xself.cppm` dispatch + `cmd_install` (counterpart)

## Goal

Implement a single command that **completely removes the active xlings installation**, including the running binary, all subos, all installed packages, all global state, and (optionally) all data caches — usable both interactively and from scripts/CI. Closes the long-standing gap where `xlings remove xim:xlings` refuses single-version self-removal and routes the user to `self uninstall`, but `self uninstall` doesn't actually exist.

## Non-goals

- Removing **project-local** xlings state (`<project>/.xlings/`). Those belong to user projects, not to xlings's own install. (Out of scope; user can `rm -rf <project>/.xlings/` themselves.)
- Modifying user shell rc files (`~/.bashrc`, `~/.zshrc`, `~/.config/fish/config.fish`). xlings's installer appends `source <profile>` lines; we'll **detect and warn** but not auto-edit, because rc-file mutation is risky and platform-dependent.
- Uninstalling other XLINGS_HOMEs the user might have (only the active `$XLINGS_HOME`).

## Semantics vs `xlings remove xim:xlings`

| | `xlings remove xim:xlings@VER` | `xlings self uninstall` |
|---|---|---|
| Scope | one xlings version | entire `$XLINGS_HOME` |
| When only one version exists | **refuses** (single-version guard, points user here) | **proceeds** (this is the escape hatch) |
| Touches non-xlings packages | no | yes — removes the whole xpkgs store |
| Removes home directory | no | yes (after content cleanup) |
| Modifies shell profile lines | no | warns, doesn't auto-edit |

## CLI

```
xlings self uninstall [-y | --yes] [--keep-data] [--dry-run]
```

| flag | default | effect |
|---|---|---|
| `-y` / `--yes` | off | skip the interactive `[y/N]` confirmation |
| `--keep-data` | off | don't remove `$XLINGS_HOME/data/` (keeps installed packages, indexes, downloaded artifacts — user can re-bootstrap with `quick_install` later and reuse) |
| `--dry-run` | off | print what would be removed; do nothing |

Exit codes:
- `0` — success (or dry-run completed)
- `1` — user cancelled at prompt, or removal failed mid-way (state may be partial — error message tells user to manually `rm -rf $XLINGS_HOME`)
- `2` — invalid invocation (e.g. `--dry-run --yes` together is fine; bad flag combos that exist would surface here)

## What gets removed (default)

| path | rationale |
|---|---|
| `$XLINGS_HOME/bin/` (incl. xlings shim binary) | the binary itself |
| `$XLINGS_HOME/subos/` (all subos: default + named) | runtime / shim layer |
| `$XLINGS_HOME/data/` | installed packages, index repos, runtime cache, downloaded tarballs |
| `$XLINGS_HOME/config/` | shell init scripts (xlings-profile.sh / .fish / .ps1) |
| `$XLINGS_HOME/.xlings.json` | global versions DB / config |
| any other top-level files in `$XLINGS_HOME/` | cleanup artifacts |
| `$XLINGS_HOME/` itself | empty parent — `rmdir` after content drain |

## What gets PRESERVED (default)

- `~/.bashrc` / `~/.zshrc` / `~/.profile` / `~/.config/fish/config.fish` — user content. On removal we **detect** lines that source `$XLINGS_HOME/config/shell/*` and **emit a one-line advisory** for each, e.g.:
  ```
  [info] you may want to remove this line from ~/.bashrc:
    source /home/user/.xlings/config/shell/xlings-profile.sh
  ```
- Project-local `<project>/.xlings/` directories outside `$XLINGS_HOME` — user-owned project state.
- `~/.xmake/` — xmake cache, not xlings-owned.

## What `--keep-data` preserves

Only `$XLINGS_HOME/data/` survives. Everything else (bin, subos, config, .xlings.json) still gets removed. Use case: "I want to reinstall xlings but skip re-downloading 8 GB of glibc/gcc/llvm." After re-running `quick_install`, re-`xlings install` of any same-version package is a no-op (it's already in `data/xpkgs`).

## Cross-platform self-deletion

The xlings binary running the uninstall is **inside the directory it's about to delete**. Per-platform handling:

### Linux / macOS

- Kernel decouples opened file from directory entry: deleting the running binary is fine. The process keeps executing from the in-memory inode; the file is reclaimed when the process exits.
- `unlink("$XLINGS_HOME/bin/xlings")` works while xlings is mid-execution.
- After deleting `$XLINGS_HOME/`, we attempt `rmdir` of any leftover empty parent — but `cwd` may still be inside it; we explicitly `chdir("/")` before any deletion so the process doesn't have a dangling cwd.
- Order: `chdir` → delete content top-down → `rmdir` $XLINGS_HOME.

### Windows

- A running `.exe` cannot be deleted (`ERROR_SHARING_VIOLATION`). Standard workarounds:
  1. **`MoveFileExW(.., NULL, MOVEFILE_DELAY_UNTIL_REBOOT)`** — schedules deletion at next reboot. User-visible: "xlings.exe will be removed on next restart."
  2. **Spawn helper batch + exit parent** — write a `.bat` to `%TEMP%` that loops `del xlings.exe` until success, then `rmdir`. Parent xlings exits, batch wins. (Same trick `Chocolatey` uses.)
  3. **Move first, schedule on reboot for the moved copy** — `MoveFileW` to `%TEMP%\xlings-pending-delete.exe` (works even on running exe if same volume), then `MOVEFILE_DELAY_UNTIL_REBOOT` on the temp copy. The original `$XLINGS_HOME/bin/xlings.exe` is gone immediately; only a leftover stays in `%TEMP%` until reboot.

We'll use **strategy 3** (cleanest user experience: home dir is fully gone immediately, only one cleanup file in `%TEMP%`).

Order on Windows:
1. `chdir` to `%TEMP%` (away from XLINGS_HOME)
2. Move `xlings.exe` to `%TEMP%\xlings-pending-delete-<pid>.exe`
3. Schedule that temp file for delete-on-reboot
4. Recursively delete the rest of XLINGS_HOME content
5. `rmdir` XLINGS_HOME
6. Print: "xlings uninstalled. The launcher binary will be cleaned up on next system restart."

### `xself init` shim binaries

xlings creates per-shim copies under `$XLINGS_HOME/subos/<name>/bin/` (small wrapper exes). Same Windows treatment for any of those that may be locked by other running processes — but in practice they're not running at uninstall time. If `unlink` fails on a Windows shim, log warning and continue (best-effort) — user can mop up after reboot.

## Interactive UX

```
$ xlings self uninstall
This will permanently remove your xlings installation:

  XLINGS_HOME:    /home/user/.xlings
  installed pkgs: 50  (~12.3 GB)
  config:         /home/user/.xlings/config/shell/xlings-profile.sh
  active subos:   default

The following will NOT be touched:
  - shell init lines in ~/.bashrc / ~/.zshrc (you can clean those manually)
  - project-local .xlings/ directories under your projects

Proceed with full uninstall? [y/N]
```

With `-y`: prints the same summary, then proceeds without prompt. Useful for CI / scripts.

With `--keep-data`: summary shows `data: KEEP (12.0 GB) — pkgs preserved`.

With `--dry-run`: summary shows `[DRY RUN]` markers, does nothing, exits 0.

## Safety invariants

1. **Refuse to operate on root-like paths.** If `$XLINGS_HOME` resolves to `/`, `/usr`, `/home`, `$HOME` (without a trailing `.xlings`), refuse with an error. Defense against misconfigured `XLINGS_HOME=/` or similar.

2. **Refuse if `$XLINGS_HOME` is a symlink target shared with system dirs.** Resolve via `weakly_canonical` first; reject if canonicalized path matches any system blacklist.

3. **Lock-file check.** If `$XLINGS_HOME/.lock` (install in progress) exists, refuse with "another xlings operation is running; finish or remove `.lock` first".

4. **Show summary before deletion** — even with `-y`, the summary still prints (so script logs show what was removed). Just no confirmation prompt.

5. **Best-effort, not all-or-nothing.** Once content deletion starts, continue past individual file failures (log them); after the loop, print summary of what was/wasn't removed. The user gets actionable info instead of half-deleted state with no log.

## Implementation plan

### File layout

New file: `src/core/xself/uninstall.cppm` (parallel to `install.cppm`).

### Code structure

```cpp
export module xlings.core.xself.uninstall;
import std;
import xlings.core.config;
import xlings.platform;
import xlings.core.log;

export namespace xlings::xself {

struct UninstallOpts {
    bool yes      = false;
    bool keepData = false;
    bool dryRun   = false;
};

// Returns 0 on success / cancelled-cleanly, 1 on partial failure.
export int cmd_uninstall(UninstallOpts opts);

} // namespace xlings::xself
```

### Wiring

`xself.cppm` (router):

```cpp
export import xlings.core.xself.uninstall;
// ...
if (action == "uninstall") {
    UninstallOpts opts;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-y" || a == "--yes")     opts.yes = true;
        else if (a == "--keep-data")        opts.keepData = true;
        else if (a == "--dry-run")          opts.dryRun = true;
        else { /* unknown flag → exit 2 */ }
    }
    return cmd_uninstall(opts);
}
```

Plus add to `cmd_help`'s opts list:
```cpp
{{"name", "uninstall"}, {"desc", "Completely remove this xlings installation (-y / --keep-data / --dry-run)"}},
```

### Bonus fix (out of scope but trivial — same PR)

Make unknown actions return non-zero instead of silent `cmd_help` + exit 0:

```cpp
// at end of run()
if (action != "help" && action != "" && action != "-h" && action != "--help") {
    log::error("unknown 'self' action: {}", action);
    cmd_help(stream);
    return 2;
}
return cmd_help(stream);
```

This catches `xlings self uninstal` (typo) / `xlings self bogus` etc.

## Tests

New: `tests/e2e/self_uninstall_test.sh`:

| scenario | expected |
|---|---|
| fresh isolated home → `self uninstall -y` | home dir gone, exit 0 |
| with installed pkgs → `self uninstall -y` | home dir gone (incl. data/), exit 0 |
| `--dry-run -y` | home unchanged, dry-run summary printed, exit 0 |
| `--keep-data -y` | bin/subos/config gone; data/ remains; exit 0 |
| invalid flag (`--bogus`) | exit 2, error message |
| safety: `XLINGS_HOME=/tmp/test/` (no `.xlings` suffix) | refuse, exit 1 |
| safety: cwd inside `$XLINGS_HOME` before invoke | works, cwd auto-changed to `/` |
| running shim that's NOT xlings.exe (Linux/macOS) | shim removed cleanly |

Cross-platform: parallel `.ps1` for Windows-specific checks (delete-on-reboot scheduling, `%TEMP%` move).

## Open questions

1. **Should `--keep-data` also keep `config/`?** Rationale for keeping config: shell profile entries reference `$XLINGS_HOME/config/shell/xlings-profile.sh`; if we keep config, user's shell rc lines don't dangle. Decision: NO — `--keep-data` is about "reuse package payloads on reinstall", not "preserve shell config". User who wants to skip the full uninstall should probably not be running `self uninstall` at all.

2. **Should we record an "uninstalled" marker for telemetry / next-install awareness?** Decision: NO — would conflict with the "leave nothing behind" intent.

3. **What about XLINGS_HOME inside a portable/USB drive?** Same path, same logic. The "refuse if `$XLINGS_HOME == $HOME`" check might reject `/media/usb/xlings/.xlings` — actually it won't, because `$HOME` is checked literally, not "any user home". OK.

4. **Should we offer to also remove rc-file lines automatically with `--purge`?** Phase 2. Not in MVP. Risk of corrupting non-xlings content on shared rc files; manual is safer for now.

## Migration / impact

- Doesn't change any existing behavior (all current `self <action>` commands keep working as-is).
- Closes the broken loop where `xlings remove xim:xlings` (single-version) hints at `self uninstall` which silently no-ops.
- `tests/e2e/release_self_install_test.sh:124`'s `xlings self uninstall -y || rm -rf "$HOME/.xlings"` becomes meaningful — `self uninstall -y` will actually do it; `||` fallback still safety-nets.

## Rollout

- Single PR: new `xself/uninstall.cppm` + dispatch + help + test + `unknown-action returns non-zero` fixup.
- Ships in next xlings release (e.g. 0.4.14 or 0.5.0).
- Doc update: `xself --help` will list `uninstall` automatically (driven by `cmd_help`).
- README/install docs: add a note that `xlings self uninstall` is the canonical removal path.
