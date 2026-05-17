# Build vs Runtime Deps Split

**Date**: 2026-05-01
**Status**: in implementation
**Branch**: `feat/build-runtime-deps`
**Cross-repo**: requires changes in both `mcpplibs/libxpkg` (schema/parser) and `d2learn/xlings` (consumer)

## Goal

Allow xlings packages to declare two distinct kinds of deps:

- **`runtime`** — what the consumer **needs at run-time** after install. Activated in subos workspace, exposed via shim/PATH (program/script/config) or linked at consumer build (lib).
- **`build`** — what the package needs **only while installing/building itself**. Filesystem-installed but not activated; the install hook accesses them via injected env / API and uses absolute paths.

Why this matters:

- Today, every dep declared by a package becomes part of the active workspace, polluting the user's tool versions even if the dep was only needed at install time.
- Build-time tools (compilers, patchelf, code generators) are per-consumer concerns — different consumers can require different versions without conflict if treated as build-only.
- This unblocks several real conflict scenarios (see `2026-05-01-semver-constraint-design.md` if/when written) where two packages want incompatible versions of the same tool *just for their own builds*.

## Core design

### Lua schema (in xim-pkgindex packages)

New format:

```lua
package = {
    name = "fancyapp",
    type = "program",
    deps = {
        linux = {
            runtime = { "openssl@^3.0", "ncurses" },
            build   = { "gcc@13.5", "patchelf@0.18" }
        }
    }
}
```

Backward-compatible legacy format (continues to work, fan-out to both kinds):

```lua
package = {
    deps = {
        linux = { "node", "npm" }   -- treated as runtime AND build
    }
}
```

**Fan-out rule**: when `deps[platform]` is a flat array, every entry is implicitly added to *both* `runtime` and `build`. This is a conservative default that preserves today's behavior. Packages that want the new ergonomics opt in by switching to the table form.

### Lua install-hook API

```lua
function install()
    -- absolute path to a build dep payload
    local gcc = pkginfo.build_dep("gcc")
    -- gcc.path    = "<xpkgs>/xim-x-gcc/13.5.0"
    -- gcc.bin     = "<xpkgs>/xim-x-gcc/13.5.0/bin"
    -- gcc.version = "13.5.0"

    os.exec(gcc.bin .. "/gcc -O2 src/foo.c -o foo")
end
```

Plus a convenience: build deps' bin/ directories are auto-prepended to `PATH` for the duration of the install hook, so legacy code like `os.exec("gcc ...")` Just Works and finds the right version.

### xlings consumer behaviour

| Phase | Build deps | Runtime deps |
|-------|-----------|---------------|
| Resolver | walked into plan with `kind=build` | walked into plan with `kind=runtime` |
| Installer | downloaded + extracted to `xpkgs` only | downloaded + extracted + registered in xvm DB + workspace activated |
| Workspace | NOT touched | active version updated |
| Visible to user shell | ❌ | ✅ |
| GC | refcount via consumer's `.build_deps_used.json`; cleaned by `xlings self clean` when no consumer references them | normal workspace removal |
| Conflict resolution (see semver-constraint plan) | per-consumer private — never cross-conflict | per-package convergence required (subos isolation if not) |

## Schema changes — `mcpplibs/libxpkg`

### `src/xpkg.cppm`

`XpmEntry` (or equivalent platform record) gains two vectors. Keep the existing `deps` field for backward serialization compat.

```cpp
struct XpmEntry {
    // ... existing fields
    std::vector<std::string> deps;          // legacy/effective-union (kept)
    std::vector<std::string> runtime_deps;  // NEW
    std::vector<std::string> build_deps;    // NEW
};
```

Semantics:
- Loader populates `runtime_deps`/`build_deps` from the lua source (table form), or fans out from the legacy array.
- `deps` is set to the union (`runtime ∪ build`) so existing consumers reading `deps` keep working.

### `src/xpkg-loader.cppm`

Detect whether `deps[platform]` is array or table, dispatch to two helper paths. Roughly:

```cpp
if (lua_is_table_array(...)) {
    auto v = parse_string_array(...);
    entry.runtime_deps = v;
    entry.build_deps = v;
    entry.deps = v;
} else {
    entry.runtime_deps = parse_string_array(deps_table["runtime"]);
    entry.build_deps   = parse_string_array(deps_table["build"]);
    entry.deps = union(entry.runtime_deps, entry.build_deps);
}
```

### `src/lua-stdlib/xim/libxpkg/pkginfo.lua`

Add `pkginfo.build_dep(name)` returning `{path, bin, version}`. Implementation reads from `XLINGS_BUILDDEP_<NAME>_PATH` env vars that the C++ installer injects.

## Consumer changes — `d2learn/xlings`

### `src/core/xim/libxpkg/types/type.cppm`

`PlanNode` gains `kind` discriminator + dep lists:

```cpp
enum class DepKind { Runtime, Build };

struct PlanNode {
    // ... existing fields
    DepKind kind = DepKind::Runtime;        // how this node was reached in the dep graph
    std::vector<std::string> runtime_deps;
    std::vector<std::string> build_deps;
};
```

### `src/core/xim/resolver.cppm`

When walking dependencies of a node, partition them:

```cpp
// previous: for (auto& dep : pkg->xpm.deps[plat]) plan_add(dep, ...);
for (auto& dep : pkg->xpm.runtime_deps[plat]) plan_add(dep, DepKind::Runtime);
for (auto& dep : pkg->xpm.build_deps[plat])   plan_add(dep, DepKind::Build);
```

Build deps inherit `kind=build` even through transitive dependencies — once you're in the build subtree, you stay there.

### `src/core/xim/installer.cppm`

Topological install order honours `kind`:

```cpp
// install build_deps for X
for (auto& bd : x.build_deps_resolved) {
    install_payload(bd);            // unpack to xpkgs/, no DB / workspace updates
    bump_build_dep_refcount(bd, x); // x is now an "owner"
}

// run X's install hook with build deps on PATH + env
auto env = inherit_current_env();
for (auto& bd : x.build_deps_resolved) {
    env["PATH"]                                       = bd.bin + ":" + env["PATH"];
    env["XLINGS_BUILDDEP_" + upper(bd.name) + "_PATH"] = bd.path;
}
exec_install_hook(x, env);

// register X in xvm DB + workspace (runtime side only)
xvm::add_version(x.name, x.version, ...);
update_workspace(...);
```

### `src/core/xim/commands.cppm`

`xlings info <pkg>` displays:

```
Runtime deps: openssl@^3.0, ncurses
Build deps:   gcc@13.5, patchelf@0.18
```

### `src/core/xvm/db.cppm`

Refcount tracking for build deps. Each consumer's install dir grows a `build_deps_used.json`:

```json
{ "gcc": "13.5.0", "patchelf": "0.18.0" }
```

`xlings self clean` walks this to determine GC eligibility.

## Local joint debugging via xmake.lua

Cross-repo iteration is awkward — every libxpkg change normally requires a release. Workaround: xlings's `xmake.lua` accepts a flag to point at a local libxpkg checkout instead of fetching from xrepo.

```lua
-- xmake.lua
option("local_libxpkg")
    set_default("")
    set_description("Path to a local mcpplibs/libxpkg checkout for dev")
option_end()

if has_config("local_libxpkg") and get_config("local_libxpkg") ~= "" then
    -- Use a private xrepo source pointing at local path
    add_requireconfs("mcpplibs-xpkg",
        { override = true,
          version  = "dev",
          configs  = { sourcedir = get_config("local_libxpkg") } })
else
    add_requires("mcpplibs-xpkg 0.0.32")
end
```

Dev workflow:

```bash
# Iterate on libxpkg
cd ~/workspace/github/mcpplibs/libxpkg && vim src/xpkg.cppm

# Build xlings against local libxpkg
cd ~/workspace/github/openxlings/xlings
xmake f --local_libxpkg=/home/speak/workspace/github/mcpplibs/libxpkg ...
xmake build
```

(Exact xmake API may need tweaking — see implementation phase.)

## Test plan

### Unit (libxpkg)

- Parse legacy array form → both vectors populated (fan-out)
- Parse new table form (only `runtime`) → `build_deps` empty
- Parse new table form (only `build`) → `runtime_deps` empty
- Parse mixed → both populated correctly

### Unit (xlings)

- Resolver: `kind=Runtime` and `kind=Build` distinction propagates
- Installer: hook gets `XLINGS_BUILDDEP_*_PATH` env vars and modified PATH
- xvm DB: build deps don't enter workspace

### E2E (in isolated XLINGS_HOME)

Fixture package `bdfix` with:
- `runtime = { "needed-at-runtime@1.0" }`
- `build = { "compiler-mock@2.0" }`

After install:
1. ✅ `<xpkgs>/needed-at-runtime/1.0/` exists
2. ✅ `<xpkgs>/compiler-mock/2.0/` exists
3. ✅ `<subos>/bin/needed-at-runtime` shim exists (active)
4. ❌ `<subos>/bin/compiler-mock` shim does NOT exist (build-only)
5. ✅ `bdfix` install hook successfully invoked compiler-mock via injected PATH/env

## Files

```
NEW:
  docs/plans/2026-05-01-build-runtime-deps-split.md   (this file)
  tests/e2e/build_deps_split_test.sh                   (e2e fixture-driven)

MODIFIED (mcpplibs/libxpkg, separate repo):
  src/xpkg.cppm                                        (~10 LOC)
  src/xpkg-loader.cppm                                 (~30 LOC)
  src/lua-stdlib/xim/libxpkg/pkginfo.lua               (~25 LOC)
  tests/...                                            (~40 LOC)

MODIFIED (d2learn/xlings):
  src/core/xim/libxpkg/types/type.cppm                 (~5 LOC)
  src/core/xim/resolver.cppm                           (~25 LOC)
  src/core/xim/installer.cppm                          (~60 LOC)
  src/core/xim/commands.cppm                           (~15 LOC)
  src/core/xvm/db.cppm                                 (~15 LOC)
  xmake.lua                                            (~10 LOC, local-override)
  tests/unit/test_main.cpp                             (~40 LOC, new asserts)
```

## Phases

### Phase A — libxpkg: schema + parser + lua API
1. Edit `xpkg.cppm` (add fields)
2. Edit `xpkg-loader.cppm` (legacy fan-out + table form)
3. Edit `pkginfo.lua` (build_dep API)
4. Tests
5. ⚠️ Don't release yet — wait for Phase B integration

### Phase B — xlings: integration (parallel-able with A's later half)
1. Add `local_libxpkg` xmake option
2. Edit `type.cppm`, `resolver.cppm`, `installer.cppm`, `commands.cppm`, `db.cppm`
3. Tests (unit + e2e)
4. Build against local libxpkg

### Phase C — xim-pkgindex migration (optional, post-merge)
- musl-gcc: patchelf as `build`
- llvm: ninja, cmake as `build`
- fromsource packages: compilers as `build`
- One PR per package, optional, low priority.

### Out of scope (this PR)
- Lockfile (`xlings.lock`)
- `--override-constraint`
- semver constraint full grammar (`^`, `~` etc) — that's a separate PR

## Risks & mitigations

| Risk | Mitigation |
|------|-----------|
| Legacy `deps` array gets installed twice (once as runtime, once as build) | Identical artifacts, idempotent install — no actual problem; just a small redundancy log. Documented |
| Build deps' bin on PATH leaks beyond install hook | Scope env mutation to the hook subprocess; restore after exit |
| User unintentionally relies on a build dep being on PATH (because it used to be) | Migration of pkgindex packages is opt-in; legacy fan-out preserves old behaviour for unmigrated packages |
| `xlings remove` of a build-dep leaves dangling consumer references | `self clean` reads `build_deps_used.json` to detect; if user manually removes, refcount layer reports orphans |
| Cross-repo sync (libxpkg version bump) | `local_libxpkg` xmake option enables dev-time iteration without round-tripping through release |

## Removal / cleanup notes

This is permanent feature, not transitional. No `COMPAT` markers needed. The legacy fan-out stays indefinitely as backward-compat with old pkgindex entries.
