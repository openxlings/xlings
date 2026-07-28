# `remove` said "removed" for a run that removed nothing

Date: 2026-07-29
Issue: [#443](https://github.com/openxlings/xlings/issues/443) · related:
[#422](https://github.com/openxlings/xlings/issues/422)
Code: `src/core/xim/installer.cppm`, `src/core/xim/commands.cppm`,
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
