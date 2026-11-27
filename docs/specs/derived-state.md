# Derived state

> Status: in force since xlings 2026.11.23.1. Implemented in
> `src/core/xim/host_facts.cpp` (host facts), `src/core/xim/install_state.cpp`
> (the configured verdict), `src/core/xim/installer.cpp` (removal),
> `src/core/xself/doctor.cpp` and `src/core/xself/repair.cpp` (repair), and
> `src/core/profile.cpp` (the store's references, [store roots](store-roots.md)).

Much of a home is derived: payload directories, shims, sysroot links, the
records of which package was configured where, the link farms a recipe
generates. Two rules already govern it. Derived state may be rebuilt by any
flow ([upgrade and compatibility](upgrade-and-compatibility.md)), and a part
that could not be read is never read as empty. This specification states the
third, and the places it applies.

## 1. The rule

**Derived state carries the fact it was derived from; the place that uses it
checks that fact.** When the fact still holds, the state is used. When it does
not, the state is generated again, or the use is refused with the reason and
the command that repairs it. It is never used silently as though the fact still
held.

| Derived state | The fact it depends on | Where it is checked | § |
|---|---|---|---|
| The store's payloads | who references each one | `self clean`, before anything is deleted | [store roots](store-roots.md) |
| "Removed", "repaired" | what is on disk after the action | the removal and every repair step | §2 |
| A configuration generated from the host | the host files its `config()` read | install, activation, entering a SubOS, `self doctor` | §3 |

## 2. Removal and repair report what the disk says

### 2.1 The installer owns the install directory

An install directory (`<data>/xpkgs/<store>/<version>`) is created by the
installer, and its deletion belongs to the installer too. A removal runs the
package's `uninstall()` hook (which withdraws what the package registered), then
deletes the directory itself, then reports the package removed. The hook is not
asked to delete it: a hook that reclaims its payload through `xvm.remove`
deletes nothing for a payload the version database has no record of. A
versionless `xvm.remove(name)` for a name with no record asks for what already
holds and removes nothing; it does not fail the removal.

The directory is kept only where something else holds it, and the removal says
so: another SubOS still uses the version (the removal detaches this SubOS), a
retained root generation links into it (the retained ledger), or a file in it
cannot be deleted (the rest is deleted and the directory is marked incomplete,
so a later install rebuilds it).

### 2.2 A repair checks after each action

`xlings self doctor --fix` judges a repair by the finding, not by the exit code
of the command it ran. After each action the finding is detected again: a swept
payload's fingerprint after its removal and after its reinstall, a stale
configuration's record after it is configured again, every finding once more
at the end of each phase. A finding that outlives its repair is reported as a
failure, with what was run and what is still there; it is never reported as
repaired, and it reaches the exit code.

## 3. Host facts

A package states the host files its configuration depends on in its recipe
(`host_facts`, [the xpkg format](xpkg.md) §3.5).

### 3.1 The record

When a package's `config()` succeeds in a scope, the installer records, for each
path the recipe declares for the host's platform, the state it reads at that
moment:

| State | Meaning |
|---|---|
| `sha256:<hex>` | the SHA-256 of the file's contents, as a read returns them |
| `absent` | there is no such file |
| `unreadable` | the file exists and cannot be read |

The record is kept in the scope's state file (the SubOS's `.xlings.json`, or a
project's state file), as a top-level object beside `configured`, keyed like it
by `<namespace>:<name>@<version>`:

```json
{
  "configured": { "xim:nvidia-gl-host-link@0.1.4": 0 },
  "host_facts": {
    "xim:nvidia-gl-host-link@0.1.4": {
      "/sys/module/nvidia/version": "sha256:3b4c..."
    }
  }
}
```

It is written and erased with the `configured` record: an uninstall or an unbind
in the scope removes both. A client before 2026.11.23.1 keeps the key when it
writes the file and does not read it.

### 3.2 The check

A configuration is current only when every recorded path reads as recorded, and
the recipe declares no path the record lacks. The check reads the recorded
files and nothing else; no recipe is loaded and no recipe code runs.

| Where | What happens when it is not current |
|---|---|
| `xlings install` of the package | `config()` runs again (the configured verdict, beside the recorded revision) |
| activation: `xlings use <target>` | `config()` of that package runs again in the current scope |
| entering a SubOS: `subos use`, `subos exec`, `subos start` | `config()` of each such package runs again in that SubOS's scope, before the entry |
| `xlings self doctor` | reported as `host facts changed`, with the file and the command; `--fix` runs the command and checks the record again |

When configuring again fails, or leaves the record different from the host, the
client says so, names the file that changed and the command that configures the
package (`xlings install <coordinate> --reconfig`), and goes on: the entry is
not refused. The check is not made on a shim's launch.

### 3.3 Recipes

A recipe that states `host_facts` generates what depends on them in `config()`,
from the facts as they read then -- a glob over the host's directory rather than
a list of file names -- so that running `config()` again is the whole repair.
