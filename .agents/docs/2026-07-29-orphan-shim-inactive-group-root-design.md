# The orphan-shim loop: a file install writes, `--fix` deletes, install writes again

Date: 2026-07-29
Issue: [#452](https://github.com/openxlings/xlings/issues/452)
Code: `src/core/xim/installer.cppm`, `src/core/xself/doctor.cppm`,
`src/core/xvm/registration.cppm` (unchanged, but it is layer 2),
`tests/unit/test_xvm_bindings.cpp`, `tests/e2e/self_doctor_orphan_shim_test.sh`
Index: `openxlings/xim-pkgindex` — `pkgs/g/gcc.lua`, `pkgs/m/musl-gcc.lua`,
`pkgs/a/aarch64-linux-musl-gcc.lua`

## The report

`xlings self doctor` on a real home, immediately after a successful install:

```
$ xlings install musl-gcc@15
  ✓ 2 package(s) installed
$ xlings self doctor
  ✗ orphan shim  @xlings/subos/default/bin/xim-musl-gnu-gcc exists but
                 workspace has no active version for xim-musl-gnu-gcc
```

The user's reading was "the reinstall did not clean up". It is the opposite:
the install *created* the thing doctor is complaining about, and it does so
every time.

## What was actually happening

The state, read straight off the home:

```
bin/xim-musl-gnu-gcc -> ../../../bin/xlings          # mtime == the install
subos/default/.xlings.json:
  xim-musl-gnu-gcc          {"installed": ["15.1.0-musl"]}         ← no active
  xim-aarch64-musl-gnu-gcc  {"installed": ["15.1.0-aarch64-musl"]} ← no active
  gcc                       {"active": "16.1.0", ...}
```

The shim is dead on arrival — it has nothing to dispatch to:

```
$ bin/xim-musl-gnu-gcc --version
[error] xlings: no active version of 'xim-musl-gnu-gcc' in current subos
[error]   available: 15.1.0-musl
```

`xim-aarch64-musl-gnu-gcc` is the same shape with the file already gone. That
is the tell: an earlier `--fix` reaped it, and nothing has reinstalled that
package since. The shell history closes the case:

| time | command | doctor verdict |
| --- | --- | --- |
| 19:55:30 | `xlings self doctor --fix` | orphan shim **deleted** |
| 19:57, 19:58 | `xlings self doctor` | clean |
| 19:58:46 | `xlings remove musl-gcc@15` | clean |
| 19:58:57 | `xlings install musl-gcc@15` | — |
| 19:59:45 | `xlings self doctor` | orphan shim **back** |

So `remove` is not the variable and neither is a dirty uninstall. `--fix` is
why the home looked clean before; **`--fix` and `install` undo each other.**

## Four layers, each locally correct

**1 — the recipe registers a virtual anchor as a program.**
`musl-gcc.lua:224` anchors the gcc-flavor subtree:

```lua
xvm.add(root_name, { version = flavor_ver })   -- "xim-musl-gnu-gcc"
```

No `bindir`, no `alias`. It exists only so `gcc`/`g++`/`cc`/`cpp`/`c++` at
`15.1.0-musl` have a root to bind to. But `type` defaults to `"program"`, so
every layer below treats it as an executable the user might run.

**2 — registration withholds activation from the whole group.**
`registration.cppm:921-925`:

```cpp
const bool anyMemberActive = std::ranges::any_of(
    group.members, [&](const auto& member) {
        return candidateWorkspace.contains(member.first);
    });
const bool activateGroup = batch.useAfterInstall || !anyMemberActive;
```

`gcc` is already in the workspace at glibc 16.1.0, so `activateGroup` is false
and no member gets an active pointer — `installed[]` is written unconditionally
(:952-966), `workspace` is not (:967-969).

**This is deliberate and stays.** `InstallDoesNotSplitTheWorkspaceAcrossReleases`
(`test_xvm_bindings.cpp:3044`) guards it: installing gcc must not take `cc`
away from an active llvm, and a release must not be half-activated. The
collateral is that a *private* root — contested by nobody — is swept into the
same all-or-nothing decision.

**3 — `ProgramShim` writes the file without asking whether it is active.**
`installer.cppm`, the effects loop:

```
:1590  InstallHeaders → :1591  if (!resolved->active) { … continue; }   ← guarded
:1611  ProgramShim    →        create_shim(...)                        ← NOT guarded
:1657  FileAsset      → :1658  if (!resolved->active) { … continue; }   ← guarded
```

`resolved->active` is computed for every effect (:184-187) and `ProgramShim`
consults it only at :1627, for the self-replace decision. So a name that
registration deliberately left inactive still gets a file in `bin/`.

**4 — doctor Check 2 lacks the exemption Check 3 already has.**
Check 2 (`doctor.cppm:301-328`) is "shim file present + workspace has no active
version" → **Error**. Check 3 (:477) faces the same entries and already knows
better:

```cpp
if (xvm::is_binding_root(st.db, name, version) || !payload_has_any_executable_(expanded)) {
    add({ .kind = FindingKind::ReleaseAnchor, .level = FindingLevel::Notice, … });
```

Those are the `release anchor` notices in every report. doctor already models
"this name exists only to anchor a release"; Check 2 was never taught it.

## Blast radius

Not musl-specific, and not new. Any release that introduces a program name the
previous release lacked, while the previous release stays active, ends up with
a shim for a name that has no active version. `doctor.cppm:1430-1435` records
the symptom and works around it:

> Measured: one llvm re-registration produced 29 orphan shims and 29
> incoherent-release findings that a single-pass repair reported as unfixed.

The response then was a phase-3 re-run inside `--fix`. That makes one `--fix`
converge; it does not stop the next `install` from re-creating the state. This
change goes at the source.

Three index recipes register a purely virtual root: `gcc.lua` (`xim-gnu-gcc`),
`musl-gcc.lua` (`xim-musl-gnu-gcc`), `aarch64-linux-musl-gcc.lua`
(`xim-aarch64-musl-gnu-gcc`). `xim-gnu-gcc`'s shim is equally dead — running it
gives `executable 'xim-gnu-gcc' not found` — it just happens to hold an active
pointer, so Check 2 never looked at it.

This is the [silent-success](../../MEMORY.md) family inverted: three modules
whose local judgements are each defensible produce a state only the
cross-module view can call wrong.

## The design

Three layers, independently shippable, deliberately overlapping.

### P1 — `ProgramShim` respects activation (installer)

A shim is meaningful only if its name has an active version. That is exactly
the invariant doctor Check 1 and Check 2 already encode, and the runtime proves
it — an inactive shim can only print `no active version`. Install now obeys the
same invariant `remove` already does (`remove_target_shims_` has been
active-aware since 0.4.19).

The condition is about the **name**, not this version:

```cpp
const auto activeIt = scopedWorkspace.find(resolved->target);
const bool nameHasActiveVersion =
    activeIt != scopedWorkspace.end() && !activeIt->second.empty();
if (!nameHasActiveVersion) continue;
```

`!resolved->active` would be wrong. Installing a second version of an active
program leaves `resolved->active` false for the new version while the name
still needs the shim its active sibling dispatches through — and since the
branch only ever *creates*, skipping there would be a silent no-op today and a
missing shim the moment the file did not already exist.

**Not scoped to binding roots**, though that was the first draft. Two reasons
the general rule is the right one:

- it covers the whole class, including the non-root members that produced the
  29 orphan shims doctor's comment records — a root-only guard would leave
  that case exactly as it is;
- anything activated later still gets its file. `activate_requested_targets`
  (`commands.cppm:365`) runs after the effects loop and calls `cmd_use`, which
  resolves the *whole* release (`plan_use_switch`) and creates a shim for every
  member (`commands.cppm:433`). Traced through llvm, whose group root is its
  own package name: install while gcc owns `cc` leaves the group inactive, the
  effects loop now writes nothing, and `cmd_use` immediately writes all of it.
  Same end state as before.

What is lost: a name that has *never* had an active version no longer gets a
shim that would have printed

```
[error] xlings: no active version of 'gcc-ar' in current subos
[error]   hint: xlings use gcc-ar <version>
```

and now gets the shell's `command not found` instead. That only reaches a
package whose own name is not a registered target *and* whose group lost the
vote — nothing in the index today — and in that state the command never worked
either way.

### P2 — Check 2 exempts binding roots (doctor)

Existing homes already carry the file, and it is not the user's fault nor
theirs to fix. Report it the way Check 3 reports the same entries — a
`ReleaseAnchor` notice — instead of an Error that sets exit 1.

Check 2 has no version in hand (that is the whole point: nothing is active), so
the predicate is "**every** registered version of this target is a binding
root". A target with one real program version and one anchor version stays an
Error.

`--fix` still deletes the file (it is genuinely useless); it is just no longer
an error when it is there.

### P0 — virtual roots declare `type = "group"` (xim-pkgindex)

The real fix at the source. `group` produces no `ProgramShim` effect at all
(`installer.cppm:421` only emits for `program`/`lib`/`files`) and doctor skips
it in Checks 1, 2 and 3 (`type != "program"`). Six recipes already use this
idiom, with the reason written down (`ripgrep.lua:78`):

> `group` is the only kind that both avoids a bogus shim under the package name
> and lets `xlings remove` find the package

Verified: `apply_registration_batch` puts **no** kind constraint on a binding
root (:440 checks only that the root is an exact node in the batch), so a
`group` node is a valid root.

Compatibility: an xlings older than 2026.7.x refuses to remove a group root and
leaks the member shims — the same floor `ripgrep.lua` documents. P1+P2 ship
first so the client that meets a group-rooted recipe is already the fixed one.

## Why all three and not just P0

- **P0 alone** fixes these three recipes and leaves the mechanism armed for the
  next recipe that anchors a subtree.
- **P1 alone** fixes every existing recipe with no index release, but leaves the
  file already on disk in existing homes.
- **P2 alone** silences the report without changing the state.

P1 is the invariant, P2 is the existing-home migration, P0 is the intent
declared where it belongs.

## Tests

| level | test | what it pins |
| --- | --- | --- |
| unit | `test_xvm_bindings.cpp::AnUncontestedRootStillLosesWithItsGroup` | the precondition the installer guard is built on — a private root is registered, not activated, and *is* a binding root |
| e2e | `self_doctor_anchor_shim_test.sh` (E2E-45) | S1 no shim for an inactive root · S2 doctor clean · S3 anchor shim is a notice **and a genuine orphan is still an error** · S4 `--fix` removes it · S5 **convergence** |

doctor's `detect_` is not exported (only `cmd_doctor` is), so the Check 2 half
is pinned at e2e level rather than in `test_xvm_doctor.cpp`.

S5 is the property that actually matters. Every previous fix in this area made
a single `--fix` converge; none checked that the install after it stays clean.

Both halves were confirmed load-bearing by reverting them one at a time and
rebuilding — pre-fix fails S1, installer-only fails S3, both pass.

## Verified on the reported home

Built with `.agents/tools/slice-real-home.sh`, same slice, two binaries:

| | released 2026.7.29.1 | this change |
| --- | --- | --- |
| `self doctor` on the reported state | `orphan shims 1`, **exit 1**, "run --fix to repair" | `1 anchor shim` in the nothing-to-do line, **exit 0** |
| `--fix` | removes the file | removes the file |
| `remove musl-gcc@15` + `install musl-gcc@15` after that | shim back, exit 1 | **no shim, exit 0** |
| `musl-gcc t.c -o t.out` after the reinstall | compiles | compiles |

`verify-untouched` confirms the real `~/.xlings/data/xpkgs` was not written to.

One pre-existing defect surfaced and was filed rather than folded in:
`xlings use gcc 15.1.0-musl` leaves `gcc-ar`/`gcc-nm`/`gcc-ranlib` on the
glibc release, because the musl flavor publishes only the five frontends —
`xvm-active-group-incoherent` ×5, on both binaries
(openxlings/xim-pkgindex#451).
