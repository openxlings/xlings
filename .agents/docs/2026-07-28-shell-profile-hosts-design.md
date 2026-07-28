# Shell profile hooks: one host was hardcoded, Windows has two

Date: 2026-07-28
Issue: [#387](https://github.com/openxlings/xlings/issues/387) ·
Origin PR: [#388](https://github.com/openxlings/xlings/pull/388) (diagnosis by @ZheFeng7110)
Code: `src/core/xself/shell_profile.cppm`, `src/core/xself/install.cppm`,
`src/core/xself/uninstall.cppm`, `tests/unit/test_shell_profile.cpp`,
`tests/e2e/release_self_install_test.ps1`

## The report

On Windows with PowerShell 7 installed, `xlings subos use <name>` came up in a
shell where xlings was not configured: no prompt marker, no `XLINGS_BIN`, the
subos's `bin/` not on `PATH`.

## Why

Windows has two PowerShell hosts and they read **different** startup files:

| host         | product                | `$PROFILE`                                             |
| ------------ | ---------------------- | ------------------------------------------------------ |
| `powershell` | Windows PowerShell 5.1 | `Documents\WindowsPowerShell\Microsoft.PowerShell_profile.ps1` |
| `pwsh`       | PowerShell 7+          | `Documents\PowerShell\Microsoft.PowerShell_profile.ps1`        |

`setup_shell_profiles()` hooked `powershell` only. `use_spawn_shell()`
(`src/core/subos.cppm`) tries `pwsh.exe` **first**. So the one host xlings
never hooked was the one xlings itself launches — on any box with pwsh 7, the
feature hooked the wrong shell 100% of the time.

Neither path is guessable, either: OneDrive Known Folder Redirection moves
`Documents` out from under `%USERPROFILE%`, so a hardcoded
`Documents\PowerShell\` is wrong on a large share of real machines.

## What was actually wrong with the old code

The missing host was the symptom. The shape was the cause:

```cpp
std::string psCmd = "powershell -NoProfile -Command \""
    "$prof=$PROFILE;"
    ...
    "Add-Content $prof \\\"`n# xlings`nif(Test-Path '" + profilePs1.string() + "'){...}\\\""
    ...;
platform::exec(psCmd);
```

- **One host, spelled inline.** Adding a second meant copy-pasting the block.
- **A file edit written as a shell script inside a C++ string literal inside a
  `-Command` string inside `std::system()`.** Three escaping layers. A `'` in
  the home path closed the PowerShell literal early and left every future
  shell starting with a parse error.
- **The exit code was discarded.** A profile that was never written and one
  that was produced identical output — the recurring failure shape in this
  codebase.
- **Untestable.** No unit test could reach it and no e2e asserted it, which is
  why a whole missing host survived.

## The design

A new module, `xlings.core.xself.shell_profile`, owns *which shells xlings
hooks and how*. Four separable pieces:

1. **Hosts are data.** `kPowerShellHosts = { "powershell", "pwsh" }`. Adding a
   host is one array entry, not a second copy of a code block.
2. **Location is asked, not computed.** `probe_command(host)` builds
   `<host> -NoProfile -NonInteractive -Command 'XLINGS_PROFILE='+$PROFILE`.
   Each host reports its own startup file, which is correct under OneDrive
   redirection by construction, and the same call doubles as the
   is-it-installed probe — a host that is not installed cannot answer.
   The answer is **tagged** because `run_command_capture()` merges stderr into
   stdout: untagged, a warning banner would be adopted as a path and written to.

   The exact shape of that script cost two CI rounds and is worth recording,
   because the two hosts disagree about more than their profile paths:

   | script passed to `-Command` | pwsh 7 | Windows PowerShell 5.1 |
   | --- | --- | --- |
   | `"Write-Output ('XLINGS_PROFILE=' + $PROFILE)"` | answers | **no answer** |
   | `Write-Output XLINGS_PROFILE=$PROFILE` (bare word) | answers | **`XLINGS_PROFILE=`**, path missing |
   | `'XLINGS_PROFILE='+$PROFILE` | answers | answers |

   Two independent rules fall out, and the unit test
   `ProbeCommandIsASingleUnquotedExpression` holds both:

   - **No double quote.** The command travels `_popen` → `cmd.exe` →
     `powershell.exe`, and 5.1 does not use argv for `-Command` — it takes the
     rest of the line and strips double quotes itself. Single quotes pass all
     three layers untouched, so the script can still quote its own literal.
   - **An expression, not a command with arguments.** The bare-word form
     relies on argument-mode expansion, which pwsh 7 does and 5.1 does not.
     5.1's reply was the tag with an empty value — indistinguishable from a
     host that has no profile, which is exactly why round two still missed it
     and why `Unusable` (below) exists.
3. **Every host gets a verdict.** `probe_hosts()` returns one `Probe` per host
   with `Answered` / `NotInstalled` / `Unusable`, and the raw reply. A host
   that quietly drops out of the result cannot be reported on, and an
   unreported host is exactly how #387 looked from outside: `self install`
   printed success and hooked nothing. `NotInstalled` is the ordinary shape of
   a 5.1-only machine and stays at debug; `Unusable` — it started and said
   something we cannot hook — is a defect and is warned about, with what it
   said. `self install` prints the manual `$PROFILE` hint when *nothing* got
   hooked, counted from the hooks rather than from the probe list.
4. **The edit is ordinary C++.** `hook()` does the same read → marker → append
   the POSIX branch has always done, and returns
   `Added` / `AlreadyHooked` / `Failed` so the caller can report the truth.
   Nothing passes through a shell, so `powershell_snippet()` can escape `'` by
   doubling it and be *tested* for that.

`install.cppm` keeps only what genuinely belongs to it: spawning the probe
process and printing the result — including a manual fallback hint when no
host answered at all.

### Deliberate non-decisions

- **No dedup by path.** `hook()` is idempotent on the `xlings-profile` marker,
  so two hosts reporting the same file converge on their own. A dedup pass
  would be a second mechanism for a problem the first one already solves.
- **Idempotency by marker, not by exact line.** A user who reformatted the
  line, or an older xlings that wrote a different one, must not get a
  second copy.
- **The `Test-Path` guard stays.** `self uninstall` does not edit the user's
  profile back; without the guard, every shell after an uninstall would start
  with a "file not found".
- **pwsh on Linux/macOS is not hooked.** It is reachable with the same module
  (the module is platform-neutral), but `subos use` on POSIX follows `$SHELL`,
  so there is no reported failure to fix. Left for when there is one.
- **The two `powershell` calls above the profile block stay single-host.**
  `setup_shell_profiles()` also sets `XLINGS_HOME` and prepends to `PATH` via
  `[System.Environment]::SetEnvironmentVariable(..., 'User')`. Those write
  **per-user registry** values, which every host reads — unlike `$PROFILE`,
  one host genuinely suffices there, so routing them through the registry
  would add a second process for no behavioural gain. They do still discard
  their exit code (a `powershell` that is not on `PATH` makes them no-ops that
  report success), which is a real but separate gap: it is not what #387
  reports, and no supported Windows ships without Windows PowerShell 5.1.
- **`tools/other/quick_install.ps1` is unchanged.** PR #388 also added a
  ~40-line PowerShell reimplementation of the same policy there as a safety
  net. `quick_install.ps1` runs `xlings self install` from the release it just
  downloaded, so with the fix in xlings the net is dead weight — a second copy
  of the policy, in a second language, free to drift from the first.

## Fallout fixed on the way

`emit_shell_advisory_()` in `uninstall.cppm` listed the POSIX rc files by
relative name. On Windows that set is empty, so uninstall reported *nothing*
while leaving a source line behind in one or two PowerShell profiles. It now
builds its candidate list from the same host registry, and matches the home
path in both its generic and native spellings (PowerShell profiles carry
backslashes; `generic_string()` alone never matched them).

## Tests

`tests/unit/test_shell_profile.cpp` — 22 cases, platform-neutral, run on every
CI runner including Linux. The probe is injected, so the Windows-only policy is
testable off Windows; every case asserts the observable effect rather than the
verdict alone. `TheShippedHostListCoversBothWindowsPowerShellHosts` asserts
against the shipped `kPowerShellHosts` array — dropping `pwsh` from it, i.e.
reintroducing #387, fails that test.

`tests/e2e/release_self_install_test.ps1` — after `self install`, every
PowerShell host reachable on the runner must have a profile that sources the
installed home's `xlings-profile.ps1`. Five details make it real rather than
decorative:

- The install runs with the host directories explicitly on `PATH`
  (`Get-MinimalSystemPath` omits both `System32\WindowsPowerShell\v1.0` and
  `C:\Program Files\PowerShell\7`) — a host xlings cannot start is one it is
  *right* to skip, which would make the assertion vacuous.
- Each host's `$PROFILE` is resolved **inside** the redirected environment,
  because it is not environment-independent: pwsh 7 derives it from
  `USERPROFILE` and lands inside the sandbox, while 5.1 may answer with the
  runner's real Documents folder. Resolving before the redirect measured the
  install's work against the wrong path for whichever host follows
  `USERPROFILE`.
- The sandbox home is given a `Documents` folder before anything is probed.
  Windows PowerShell 5.1 answers an **empty** `$PROFILE` when that folder is
  missing — .NET Framework's `GetFolderPath` returns `""` for a folder that
  does not exist, while .NET Core (pwsh 7) returns the path regardless. A real
  user profile always has one, so without this the sandbox stops being a
  stand-in for a user's home and 5.1 drops out of the test for a reason that
  cannot happen on a real machine.
- The profiles are **deleted** before the install: the CI job installs a
  bootstrap xlings into the runner's own home first, and hooking is correctly
  idempotent, so an already-hooked profile would be left alone and the
  assertion would measure nothing.
- Anything found is snapshotted and restored in a `finally`, so a profile that
  did land outside the sandbox does not leave later CI steps sourcing a
  throwaway home.

The test resolves `$PROFILE` with `-Command '$PROFILE'` rather than reusing
`probe_command()`'s spelling. That is deliberate: a test that asks the question
the same way the production code does agrees with it when it is wrong — with
the bare-word form, both would have read an empty answer and the test would
have passed against an install that hooked nothing.

Idempotency is *not* re-asserted in the e2e: a second `self install` of the
same version stops at an interactive confirmation that CI answers "no", so the
assertion would pass without running the code it claims to cover. It lives in
the unit test instead.
