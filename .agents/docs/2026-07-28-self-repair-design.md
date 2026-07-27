# `xlings self doctor --fix` as a repair engine

2026-07-28 · follows `2026-07-28-seamless-upgrade-design.md`

## 1. Why this cannot live in `self update`

`self update` runs the **old** binary. It replaces that binary underneath
itself and exits. Anything it did after the swap would either still be the old
code, or a subprocess whose failure it cannot meaningfully recover from —
while holding a home that is half-migrated.

So the repair is **user-triggered**, in the new binary, through the command
that already owns "is this home healthy":

```
xlings self doctor --fix
```

`self update` and a small number of other places **tell the user to run it**.
They do not run it.

## 2. What has to be repaired, and why it is not hypothetical

Measured on the upgrade simulation (PR #434), a home built by 0.4.69 and then
upgraded:

```
             targets   bindingGroup   members
before          1          no            0      ← written by 0.4.69
after          12          yes          12      ← after the new client re-ran config
```

That is `linux-headers@5.11.1`. Under 0.4.69 it is a single owner-less entry
with a payload path, typed `program` by default, and **no executable** —
because it is a headers-only package. The new client cannot tell that apart
from a program whose binary went missing, so `self doctor` reports

```
✗ broken payload [active]  linux-headers@5.11.1 executable 'linux-headers'
                           not found in …/xim-x-linux-headers/5.11.1
  → run  xlings install linux-headers@5.11.1
```

and exits 1 on a machine where nothing is actually wrong.

The important part: **the printed remedy is the correct one.** Re-running the
install re-runs `config()`, which registers the package in the current form —
12 targets, a binding group, file assets — and the finding disappears, because
the entry is now recognisable as a release anchor.

So the finding is not a false positive to suppress. It is a **stale
registration**, the cure is re-registration, and today the user has to notice
the line, read it, and run the command by hand for every package the old
client installed.

## 3. The repair ladder

One ladder, applied per (target, version), each rung tried only if the
previous failed:

| rung | action | touches network | loses data |
|---|---|---|---|
| **R1 local** | recreate a missing shim, delete an orphan shim | no | no |
| **R2 re-register** | `xlings install <pkg>@<ver> -y` | only if the payload is absent | no |
| **R3 reinstall** | `xlings remove <pkg>@<ver> -y` then `xlings install <pkg>@<ver> -y` | yes | the payload, briefly |
| **stop** | report with the reason the ladder ran out | — | — |

R2 is the workhorse and is cheap: the installer's xvm-DB shortcut checks the
payload on disk, so when the payload is intact this re-runs `config()` only —
no download — and that is exactly what converts a 0.4.69 record into the
current format. R3 exists for the case R2 cannot reach: a record the
registration layer **refuses** to overwrite (`xvm-legacy-payload-mismatch`),
which is the field dead end from 2026-07-28.

R3 is the fallback the whole design hangs on: *taking something out and putting
it back is always available*. It is also the rung that can leave the user worse
off if it half-completes, so:

- **R3 is never entered silently.** Without `--yes` it asks, listing exactly
  what will be removed and reinstalled.
- **R3 is skipped for anything not in the index.** If the package cannot be
  reinstalled, removing it is destruction, not repair.
- **R3 runs one package at a time**, verifying reinstall before moving on, so a
  failure leaves one package down rather than all of them.

### What is deliberately NOT on the ladder

- **Version upgrades.** "Old package" here means *recorded in an old format*,
  not *an older version than the index offers*. Silently moving a user's gcc
  from 15.1.0 to 16.1.0 during a repair would change their toolchain without
  being asked. Moving forward stays an explicit `xlings upgrade` (D5 in the
  companion doc).
- **`--reset-metadata` repairs.** Unchanged: they discard a release's member
  and header information, so they stay opt-in and are not inherited by `--fix`.
- **Alias warnings.** An unresolved alias may legitimately be a system command.

## 4. How the user finds out they should run it

The home config already records which xlings set it up (`.xlings.json:version`).
Today `self update` never updates it, which makes it useless — and that defect
becomes the feature:

- **`self doctor --fix`, on success, stamps the field with the running
  version.** That is the migration marker.
- **Any command may notice the mismatch** — recorded version ≠ running version
  — and print, at most once per version, one line:

  ```
  this home was set up by 0.4.69; packages installed then may need migrating
    run  xlings self doctor --fix
  ```

- **`self update` prints the same line** as its last output, since it is the
  moment the mismatch is created.

Properties this has to keep, because axis 2 of 无感 currently holds and must
not regress:

- **no network**, ever, on this path — it is a string comparison against a
  compiled-in constant
- **no extra file reads** — the home config is already loaded by every command
- **once per version**, not once per invocation
- **never when stdout is not a TTY**, so scripts and CI are unaffected

## 5. Module shape

`src/core/xself/repair.cppm` — new, and separate from `doctor.cppm` on purpose.
Detection and repair are different concerns with different risk profiles, and
today they are tangled: `cmd_doctor` is 585 lines in which the `--fix` branches
are interleaved with the reporting.

```cpp
enum class RepairKind { MissingShim, OrphanShim, StaleRegistration, BrokenPayload };

struct RepairTask {            // what doctor found, in a form repair can act on
    RepairKind  kind;
    std::string target;
    std::string version;
    std::string detail;
};

struct RepairResult {
    bool        healed;
    std::string rung;          // "local" | "re-register" | "reinstall" | "none"
    std::string note;          // why it stopped, when it did
};

struct RepairPolicy {
    bool allowNetwork  { true };   // R2/R3 off for a purely local pass
    bool allowReinstall{ true };   // R3 off
    bool assumeYes     { false };
};

RepairResult repair_one(const RepairTask&, const RepairPolicy&);
```

`cmd_doctor` keeps ownership of **what is wrong** and emits `RepairTask`s;
`repair.cppm` owns **what to do about it**. That split is what makes the ladder
unit-testable without a home, a network, or a payload: `repair_one` against a
fake executor is a pure decision table.

## 6. Idempotence and termination

The ladder must not be able to loop, and running `--fix` twice must be a no-op
the second time. Three rules:

1. **One ladder per (target, version) per run.** A task that reaches "stop" is
   not retried in the same invocation.
2. **Re-detect once, at the end.** After the repair pass, findings are
   recomputed and reported. If anything remains, it is reported as *remaining*,
   not repaired again. A second pass is the user's decision.
3. **A rung that reports success must change something observable**, and the
   final re-detect is what checks it. R2 "succeeding" while the finding
   persists is the silent-success shape this codebase keeps producing; here it
   is caught by construction, because the report is computed after the fact
   rather than accumulated from return codes.

## 7. Acceptance

Not "the command exits 0" — that is the failure mode, not the test. The
simulation (PR #434) provides the measurement:

| assertion | before | after |
|---|---|---|
| owner-less legacy entries in a 0.4.69 home after `--fix` | many | **0** |
| `self doctor` exit code on a healthy upgraded home | 1 | **0** |
| `.xlings.json:version` after `--fix` | `v0.4.69` | running version |
| packages still usable (`use` both directions, shim `--version`) | ✓ | ✓ (unchanged) |
| second consecutive `--fix` repairs anything | — | **nothing** |

The last row is the idempotence check and is the one most likely to catch a
mistake in §6.

## 8. Steps

| # | step | gate |
|---|---|---|
| S1 | `repair.cppm` with the ladder + policy, unit-tested against a fake executor | unit tests |
| S2 | `cmd_doctor` emits `RepairTask`s; `--fix` drives the ladder | existing doctor tests stay green |
| S3 | stamp `.xlings.json:version` on successful `--fix` | simulation assertion flips |
| S4 | one-line migration hint on version mismatch (TTY only, once per version) | e2e: hint appears once, then not |
| S5 | `self update` prints the same hint last | e2e |
| S6 | re-detect pass + "remaining" reporting | idempotence assertion |
