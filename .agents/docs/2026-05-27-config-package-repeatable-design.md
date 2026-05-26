# Config Package Repeatable Design

> Date: 2026-05-27
> Status: design note for PR #306 and follow-up libxpkg/spec work

## Summary

`type = "config"` should mean a repeatable configuration procedure, not an installed package.

A config package may depend on normal packages and may execute configuration logic after those dependencies are available. It should not own install artifacts, should not create an implicit installed state, and should generally avoid registering itself through xvm. Its core value is to provide a reentrant configuration code path that can be run more than once safely.

This definition intentionally narrows `config` so package authors do not need to reason about install markers, versioned payload directories, uninstall snapshots, or xvm deregistration for a package that is only meant to configure an environment.

## Definition

A `config` package is:

- a repeatable configuration procedure;
- dependency-aware;
- reentrant and expected to tolerate repeated execution;
- usually implemented with `config()`;
- normally not uninstallable, because it has no owned install payload.

A `config` package is not:

- a package with its own installed payload;
- a package that owns package payload files under `pkginfo.install_dir()`;
- a package that should become installed because xlings created metadata;
- an xvm registration unit;
- a good place for irreversible or non-idempotent system mutation.

## Lifecycle Contract

Recommended shape:

```lua
package = {
    spec = "1",
    name = "example-config",
    type = "config",
    xpm = {
        linux = {
            deps = { "some-tool" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {},
        },
    },
}

function config()
    -- Reentrant configuration logic only.
    return true
end
```

Lifecycle constraints:

- `install()` is discouraged for `type = "config"`.
- `config()` is the primary entrypoint.
- `installed()` is discouraged because config packages should not rely on a persistent installed state.
- `uninstall()` is optional and should only exist when the config procedure creates a deliberate, reversible external side effect.
- Hooks must be idempotent. Running `xlings install <config>` multiple times should not corrupt user or system state.
- Hooks should use explicit paths such as user config paths, dependency paths, or `system.rundir()` when needed. They should not rely on `pkginfo.install_dir()` as durable package-owned storage.

## Dependency Semantics

Config packages may have dependencies.

Those dependencies are normal packages and keep their own install state, xvm state, payloads, and uninstall semantics. The config package itself does not inherit or proxy those states.

This allows patterns such as:

- install normal toolchain packages as dependencies;
- run a config procedure that wires editor settings or project state;
- rerun the config procedure after user changes without reinstalling the toolchain.

## XVM Semantics

Config packages should avoid `xvm.add()` as part of their own definition.

Reason: `xvm.add()` creates durable registration state. Durable registration implies a corresponding `xvm.remove()` and therefore an uninstall contract. That contradicts the narrow config definition: a repeatable configuration procedure without its own install/uninstall lifecycle.

If a package needs xvm registration, it should usually be one of these instead:

- a normal `package` that owns an installed payload and registers its programs;
- a future explicit registration-oriented package type or policy;
- a dependency whose own `config()` registers itself.

Short-term compatibility note: existing packages may still use `xvm.add()` from config hooks. The stricter rule should first be documented and linted in libxpkg/index tooling before being made a hard runtime error.

## Install State Semantics

For `type = "config"` in the current soft-constraint PR:

- xlings should not create `.xim-installed`;
- xlings should not copy `.xpkg.lua` into the package install directory;
- xlings should not treat xlings-owned metadata as evidence that the config package is installed.
- an empty install directory is allowed by the existing hook flow and must not count as installed.

If a config hook deliberately writes files somewhere, those files are external configuration state, not package payload. The hook author is responsible for making that write idempotent.

If the package truly needs owned files, backups, snapshots, or versioned removal, it should not be modeled as a pure config package.

## Current PR #306 Evaluation

PR #306 already moves in the right direction:

- it skips the `.xim-installed` auto-stamp for config packages;
- it skips the `.xpkg.lua` install-dir snapshot for config packages;
- it tests that a no-payload config package runs repeatedly instead of being mistaken for installed.

Current PR #306 soft policy:

- keep existing hook and cwd semantics unchanged;
- allow the generic flow to leave an empty install directory;
- treat only non-empty author-created content as installed state;
- avoid xlings-owned stamp/snapshot files for config packages;
- keep xvm compatibility for now, but document that new config packages should avoid `xvm.add()`.

This keeps the code change small while leaving room to refine the config contract in libxpkg/spec.

## Discussion Options

Option A: soft runtime policy, current PR scope.

- Keep hook semantics unchanged.
- Allow an empty install directory.
- Skip only xlings-owned install markers and snapshots for `type = "config"`.
- Add TODO comments and tests.

Option B: stricter no-install-dir runtime policy.

- Do not precreate `install_dir` for config packages.
- Do not run config hooks from `install_dir`.
- This is cleaner theoretically, but it changes hook runtime behavior and may break existing recipes.

Option C: spec/lint first.

- Keep runtime compatibility.
- Update libxpkg/spec and index checks to discourage `install()`, `installed()`, `pkginfo.install_dir()`, and `xvm.add()` in config packages.
- Migrate existing packages gradually before any hard runtime behavior change.

Recommended now: Option A in PR #306, then Option C as follow-up. Option B should wait until existing package usage has been audited.

## Follow-Up Work

Spec and libxpkg should later make the contract explicit:

- update `docs/spec/xpkg-manifest-v1.md` with the narrow config definition;
- add libxpkg/index lint checks:
  - warn when `type = "config"` defines `install()`;
  - warn when it calls `xvm.add()`;
  - warn when it uses `pkginfo.install_dir()`;
  - warn when it defines `installed()` as persistent install-state logic;
- add migration guidance for existing config packages that are actually xvm registration or managed-state packages;
- consider a future explicit package type or policy for registration/managed external state.

## Recommended Author Rule

Use `type = "config"` only when this sentence is true:

> This package is just a repeatable configuration procedure over its dependencies and environment; it has no owned install payload and no durable registration state of its own.

If that sentence is not true, use a normal package or introduce a more specific type/policy.
