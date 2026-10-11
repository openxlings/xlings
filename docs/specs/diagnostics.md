# Diagnostics

> Status: in force since 2026.8.22.1 (§2.1, §2.1b and §3 revised in 2026.8.22.2).
> Implemented in `src/core/diag.cppm`, `src/core/uimode.cppm`, `modules/ui/src/theme.cppm`;
> asserted by `tests/unit/test_diag.cpp`.

## 1. One problem, one severity marker

Every problem a user sees carries **one** `[error]` / `[warn]` marker. Its
evidence and its way out are its **continuation lines**, not messages of their own.

```
[error] llvm is not installed in this subos (default)
          from                  ./.xlings.json  ->  workspace.llvm
          installed elsewhere   22.1.8, 20.1.7
          install it here       xlings install llvm@22.1.8
          see every subos       xlings use llvm --all
```

The rule exists because it used to be broken: the four `log::` levels all
answered "how bad", none answered "this continues the previous line", so a call
site could only print another line at the same level. Of 313 error/warn texts in
the tree, 74 (24%) were such continuations, each paying for a red marker.

**How**: build a `diag::Diagnostic` and call `diag::emit()`. Never print several
`log::error` lines in a row.

## 2. The fields of a `Diagnostic`

| Field | Meaning | Required |
|---|---|---|
| `level` | `Note` / `Warn` / `Error` | yes |
| `code` | stable, searchable, documentable, e.g. `xvm.not_in_subos` | yes |
| `summary` | one sentence, lower-case start, no full stop, no line break, no `[prefix]` of its own | yes |
| `source` | **who set this constraint** — a file path and a field | when there is one |
| `facts` | the evidence, rendered in two aligned columns | no |
| `actions` | the ways out; `label` is plain words, `command` runs as written | for `Warn` / `Error` |
| `nothingChanged` | a **promise**, set only when no state was written | no |

`validate()` rejects an object that breaks any of these; `tests/unit/test_diag.cpp`
asserts each one.

### 2.1 Why `source` is a first-class field

A project's `.xlings.json` holding `"mcpp": "2026.99.9.9"` makes every `mcpp`
call fail, and the user never typed that version. The old message said only
`version '2026.99.9.9' not found for 'mcpp'` and named no file — the only way to
find the cause was to grep the disk.

`Config::version_origin(target)` gives the answer; every new failure that
configuration decides fills it in.

It returns **two halves**: `source` (the file and field, for display) and
`fromProjectManifest` (which layer won). They are one return value not for
convenience: asked separately, callers rebuild the second half from a proxy, and
the proxy is wrong (§2.1b).

### 2.1b An action names the command the user should run, not its manual equivalent

When the version a project pins is not installed, the action is `xlings install`
(no arguments: it reads the project's declaration), **not**
`xlings install <name>@<ver>`. The latter makes the user redo what the project
file already says, and it is wrong when the project declares several
dependencies.

Likewise the candidate list in `facts` keeps only the newest two in this case: the
reader is about to run the action, not to choose a version, and a long list
pushes the action off the screen.

**Which command is right depends on which layer pinned the version, not on
whether there is a project.** `xlings install` (no arguments) reads only the
`workspace` of the `.xlings.json` found upwards; the global subos file and a
project-subos file pin versions too and render a `from` line too, and that
command reads neither. So the test is `version_origin().fromProjectManifest`.
Using `has_project_config()` says three wrong things at once when the directory
happens to hold a project file but the package was pinned globally: the project
did not ask for it, `from` does not name the project manifest, and the command
given installs something else and exits 0. E2E-61 S2c holds this.

### 2.2 Why `actions` is required

An error with no way out is not finished. The rule used to live in a comment in
`xvm/errors.cppm` ("No hint means the error is not ready to be shown to a
user"); it is now a global invariant.

`Note` is the exception: it is a reminder, and there may be nothing to do.

## 3. Candidate lists

Every "you could have chosen these" list goes through `diag::candidates()`:

- **descending by version order**, not by `std::map`'s lexical order;
- **cut to the first N**, saying **how many were dropped** and where to see all.

Three places used to concatenate directly: measured, one line of 877 characters,
94 versions, `0.0.100` before `0.0.24`.

## 4. Severity

| Level | For | Exit code |
|---|---|---|
| `Error` | the command failed and xlings cannot fix it | non-zero |
| `Warn` | it degraded and went on; **or** it failed, and one known command fixes it | usually 0, see below |
| `Note` | a remark about something that **succeeded** | 0 |

**The severity says how bad it is; the exit code says what happened — they may
differ.**

Specifically: when a project's `.xlings.json` declares a version that is not
installed, the shim reports `Warn` and **still exits 1**, because:

- the command did fail (the program did not run), and a script must see that →
  non-zero;
- but it is a **project not yet set up**, one `xlings install` from working, and
  xlings knows the command → a bold red `[error]` would read "your project needs
  setting up" as "the tool is broken".

The test is **who can fix it**: when xlings knows the exact way out, `Warn`; when
the user has to investigate, think, or fix it elsewhere, `Error`.

`Note` was added later. Of 123 `log::warn` call sites, 120 were on paths that
finally succeeded — an amber `[warn]` meant both "something is wrong" and "by the
way", and users learnt to skip it.

**A successful `xlings use` reporting which names did not follow is a `Note`; a
subos ignoring the runtime you asked for is a `Warn`.**

## 5. Prefixes

A message **never** carries its own `[xlings]` — `log::info` prints it. A module
prefix (`[xim]` / `[xvm]` / `[index]`) is used only when it adds information.

## 6. Interaction

### 6.1 A confirmation with nobody to ask

The caller declares the **direction** of the default:

| Policy | When | Behaviour |
|---|---|---|
| `Proceed` | the default is "go on" (install / update) | carries on — the existing behaviour E2E-48 asserts |
| `Refuse` | the default is "do not" (remove) | reports `Diagnostic{Error}`, exit 2, **never pretends to succeed** |

This came from a real defect: `--agent` answered every prompt with its
`defaultValue`, and two confirmations had opposite defaults —
`xlings remove foo --agent` printed `cancelled` and exited 0 with the package
still there. The agent calling it read that as a successful removal.

### 6.2 Interactive selection is opt-in

`tui.interactive` is **false** by default.

The reason is in E2E-48 (`non_interactive_contract_test.sh` N3): **a pty is not
evidence that someone is at the keyboard**. Agents and automation often allocate
one, so "is it a terminal" handed the blocking branch to exactly the callers least
able to answer. 2026.7.30 removed the picker and its `--pick` switch for this
reason.

`xlings config --interactive true` is a different kind of signal: a **stored
promise**, made by the person who will be at the keyboard.

## 7. Front ends and modes

`--ui-mode` has two values:

| Value | Meaning |
|---|---|
| `cli` | plain text: pipes, CI, `--agent` |
| `tui` | ftxui, rendered **inline**. **Never full screen** |

Two entry points are not `--ui-mode` values: `xlings interface` (NDJSON, another
entry point) and `xlings-gui` (another binary).

**A downgrade is said out loud.** `tui` requested with stdout not a terminal →
`cli`, with a `Note` saying why. A silent downgrade makes the user think the
setting did not take.

## 8. Configuration

New keys sit **flat beside** `mirror` / `lang` and take the same path (CLI option
→ written to `~/.xlings.json` under the lock → the project overrides the global
one → echoed by `xlings config`):

```json
{
  "mirror": "CN",
  "lang":   "auto",
  "uiMode": "auto",
  "theme":  "config/themes/mono.json",
  "tui":    { "interactive": false }
}
```

`tui.*` is a sub-object because it is that front end's own setting; under `cli`
it means nothing.

```bash
xlings config --lang        en|zh|auto
xlings config --ui-mode     cli|tui|auto
xlings config --theme       <name>|<path>|default|list
xlings config --interactive true|false
```

### 8.1 `lang`

`auto` by default, following the system language (POSIX reads `LC_ALL` >
`LC_MESSAGES` > `LANG`; Windows reads `GetUserDefaultLocaleName`).

**Never `std::locale("")`.** Measured: musl static (the release build) supports
only `C` / `POSIX` in `newlocale`, and libstdc++ throws for any real locale name;
on glibc it depends on whether the target machine ran `locale-gen`. Both were
caught as `"en"`, so "the system really is English" and "detection failed
completely" printed the same thing.

Tests always pass `--lang en`.

## 9. Themes

See [themes](themes.md). In short:

- the slots are **closed**: nine roles (`accent / alt / success / warn / error /
  text / muted / border / surface`);
- colours are always taken **by role**, never by colour name: a call such as
  `theme::cyan()` in `ui/` is a regression;
- a theme file is a **partial override**: a slot it does not name falls back to
  the `default` compiled into the binary;
- a missing path, broken JSON, a misspelt slot — **all three are reported**,
  never a silent fallback.

## 10. Stable codes

| code | Meaning |
|---|---|
| `xvm.not_in_subos` | the target is not installed in the current subos |
| `xvm.no_active_version` | installed in this subos, with no active version |
| `xvm.unknown_target` | this home does not know the name |
| `xvm.pinned_version_missing` | the pinned version is not installed |
| `xim.remove_absent` | what is to be removed is not here |
| `cli.needs_confirmation` | it needs a confirmation and nobody can be asked |
| `cli.bad_ui_mode` / `cli.bad_interactive` | an invalid configuration value |
| `ui.mode_degraded` | the front end was downgraded |
| `ui.interactive_first_run` | the first-run interactive hint (once only) |
| `theme.not_found` / `theme.unreadable` / `theme.bad_json` / `theme.unknown_slot` / `theme.bad_color` | a theme failed to load |

A new diagnostic adds its code to this table.
