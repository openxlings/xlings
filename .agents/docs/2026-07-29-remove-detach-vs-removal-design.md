# The upgrade loop: `remove` that kept it, `install` that refused it

Date: 2026-07-29
Issues: [#443](https://github.com/openxlings/xlings/issues/443) ·
[#422](https://github.com/openxlings/xlings/issues/422) — the two ends of one
loop, fixed together because neither exit works while the other is shut
Code: `src/core/xvm/registration.cppm`, `src/core/xim/installer.cppm`,
`src/core/xim/commands.cppm`,
`src/core/profile.cppm`, `src/ui/info_panel.cppm`,
`src/agent/text_renderer.cppm`, `tests/unit/test_runtime.cpp`,
`tests/e2e/subos_install_remove_isolation_test.sh`

## The report

After upgrading, `xlings remove glibc -y` printed `✓ xim:glibc@2.39 removed`,
but `~/.xlings/.xlings.json` still carried the glibc entry and
`~/.xlings/data/xpkgs/xim-x-glibc/` was still on disk. The reinstall that
follows then refused with `xvm-legacy-payload-mismatch`, whose hint is
*"uninstall it before reinstalling"* — advice the user had just taken.

## What was actually happening

Reproduced on 2026.7.28.3 against a real home's state, and pinned with a
differential — same binary, same command, one variable:

| other subos pinning `glibc@2.39` | version DB entry | payload | printed |
| --- | --- | --- | --- |
| 20 | **kept** | **kept, 112M** | `✓ … removed` |
| 0 | deleted | deleted | `✓ … removed` |

`uninstall()` returns early when any *other* subos still references the exact
version (`installer.cppm`, the `stillReferenced` branch): it detaches the
current subos and keeps both the registration and the payload.

**That behaviour is correct.** Deleting a payload that twenty other subos are
using would break every one of them, and the user's documented workaround —
`rm -rf ~/.xlings/data/xpkgs/xim-x-glibc` — does exactly that damage.

The defect is that the two outcomes were **indistinguishable to the user**.
The detach path logged one line at `debug` and then fell through to the same
success summary as a real removal. Same wording, opposite state.

Not a regression: 2026.7.28.1 behaves identically. The `.2` change to
`workspace_config_paths_for_scope_` widened the set of files that can pin a
payload (its own comment: *"Erring toward the union can only over-retain"*),
which makes the branch easier to hit but did not create it.

## The design

**Say which of the two things happened.** No new command, no new flag, no
change to what `remove` deletes.

1. `Installer::uninstall()` returns `UninstallOutcome { detachedOnly, target,
   version }` instead of `void`. The two exits of a function that does two
   different things now report which one ran — the caller could not previously
   tell them apart at all.
2. `profile::find_subos_pinning_version(home, target, version)` names who is
   holding it. Version-exact, and not the existing name-only
   `find_subos_referencing()`: on a multi-version home the name-only answer
   names subos pinning a *different* version and cannot explain why *this*
   payload was kept. Namespace stripping mirrors
   `is_version_referenced_anywhere_`, or the list would omit a real holder.
3. Both renderers (`ui/info_panel`, `agent/text_renderer`) distinguish
   `detached` from `removed` and state that the payload was kept.

```
✓ xim:glibc@2.39 detached  (subos: default)
  payload kept — 20 other subos still use it
  remove it there too to delete it for good
```

The closing line is the remedy, and it needs no new machinery: removing the
package from each referencing subos in turn leaves the last one with nothing
pinning the version, and that removal deletes the registration and the payload
for real. That is the differential's second row.

### Why the count, and why names only sometimes

ftxui clips at the screen edge. Naming twenty subos rendered four names and a
half-word — which reads as a complete list. That is the same *looks-finished,
isn't* failure this change exists to remove, so the cap is applied where the
count is known instead of being left to the terminal. The **count** is the
load-bearing fact (it tells the user this is not one more `remove` away) and
always fits; names are printed only when there are at most two of them.

### Not done here

- **No `--purge`.** The per-subos path above already reaches "gone for good"
  with existing commands. A flag that deletes a payload other subos are using
  is the user's dangerous workaround with a blessing on it.
- **`xvm-legacy-payload-mismatch` itself is untouched.** Reaching that error
  needs a registration written by 0.4.69; on a home whose entry is current the
  reinstall simply re-attaches. #422 tracks that half — this change removes the
  false "removed" that sends users into it, not the refusal at the other end.

## Tests

`tests/unit/test_runtime.cpp` — four cases on
`find_subos_pinning_version`: names only the matching version, counts
`installed[]` and not just `active`, matches namespaced stored values, and is
empty when nobody pins it.

`tests/e2e/subos_install_remove_isolation_test.sh` **S2** — the regression.
The file already covered the *real* removal (two subos, two different
versions); S2 adds two subos on the **same** version, removes from one, and
asserts the output says `detached` / `payload kept` while the payload, the
version DB and the other subos are all still intact — the exact combination
the old wording contradicted.

Falsifiability checked by mutation: forcing the renderer back onto the
`removed` branch makes S2 fail with the old output in the failure message.


---

# Part 2 — `install` refused to take over an entry nobody owned (#422)

## The loop

The two issues are one circuit. A user upgrading from 0.4.69 who wants a
package re-registered is told:

```
install → owner-less legacy payload field 'path' is incompatible
          hint: uninstall it before reinstalling
remove  → ✓ removed          (and the entry is still there — Part 1)
```

Part 1 stops `remove` from lying. That alone does not open the door: the user
now correctly learns the entry was kept, and `install` still refuses it. The
only remaining exit was hand-editing `~/.xlings/.xlings.json`.

## What "owner-less" means, and why refusing it protected nothing

Since binding groups landed, **every** registration gets an owner — a recipe
that registers several independent targets makes each ungrouped node its own
singleton group. So an entry with no `bindingGroup` can only have been written
by a client that predates ownership.

`apply_registration_batch` treated the two cases oppositely:

| existing entry | same provider re-registers it |
| --- | --- |
| **owned** | allowed — overwritten, group integrity checked |
| **owner-less** | `LegacyPayloadMismatch` unless every field matched exactly |

Nothing owns an owner-less entry, so there is no claim to protect. The refusal
was guarding *contents*, not ownership — and the contents it guarded can be
wrong: the measured case is an `llvm@20.1.7` whose recorded path used Windows
backslashes **on Linux**, written by an old client on the wrong platform table.
Refusing to replace that preserves the damage.

## The change

Adopt the entry in place, and say so.

- The refusal becomes a recorded adoption: `{target, version, field}` collected
  during validation, applied by the write pass that already overwrites every
  node's fields.
- `RegisteredMember` gains `adoptedLegacy` / `adoptedLegacyField`, so the
  installer can print
  `[xvm] adopted a pre-ownership registration: ninja@1.12.1 ('path' changed)`.
  Silent adoption would be its own defect — a payload changed behind a name.
- **Adoption is a repair, not a hole.** The entry comes out owned, so the next
  provider that tries to take the same `name@version` meets
  `OwnershipConflict` — a protection an owner-less entry could never raise.
- Group-integrity checks are untouched. `IncompleteLegacyComponent` still
  rejects a batch that would rewrite part of a bound group, and
  `OwnershipConflict` still rejects a foreign provider.

`RegistrationErrorKind::LegacyPayloadMismatch` is now unreachable. It is kept
so old logs stay decodable, with a comment not to reintroduce it as a refusal
without an exit the user can take.

## Verified against real state

A slice of the real home (266 of its 517 entries are owner-less), with
`ninja@1.12.1`'s recorded path corrupted the way #422 measured:

| binary | result |
| --- | --- |
| 2026.7.28.3 | `owner-less legacy payload field 'path' is incompatible` → `[ninja] failed: config hook failed` |
| 2026.7.28.4 | `[xvm] adopted a pre-ownership registration: ninja@1.12.1 ('path' changed)` → installed |

Post-state on the fixed binary: the corrupt path is replaced with the correct
one **and** the entry is now owned.

## Tests

`tests/unit/test_xvm_bindings.cpp` — the refusal test
(`RejectsIncompatibleLegacyPayloadWithoutMutation`) is replaced by four:
adoption happens in place; the adopted entry becomes owned; the batch reports
which members it adopted and on which field; an unchanged re-registration is
**not** reported as an adoption.

No e2e: the fixture index installs through the current client, so every entry
it produces is already owned. Manufacturing an owner-less one means writing the
state file by hand, which is what the unit tests do — more precisely, and
without a network.
