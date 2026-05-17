# Subos-as-XPKG Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement subos-as-xpkg system per `.agents/docs/subos-as-xpkg-design-2026-05-16.md` rev4 — allow distributing subos environments as standard xpkg packages with `type = "subos"`, with 0s fork via `--from`, single-command exec via `--cmd`, and auto-keeper for high-frequency exec.

**Architecture:** Reuse existing xpkg infrastructure 100%. Add `Subos` as a new `PackageType` enum value in upstream `mcpplibs/libxpkg`; add xlings-side dispatch in installer + new `xpkg::libxpkg::types::subos` module providing default install/config/uninstall hooks (author can override). Add `--from <spec>` fork to `subos.cppm` (cross-platform reflink/clonefile/copy). Extend `use_spawn_shell` with `--cmd <string>` for non-interactive execution. Add auto-keeper (Linux only) that auto-spawns when storage=image/tmpfs + sandbox, with 5min idle TTL.

**Tech Stack:** C++23 (modules), xmake build, xmake's xrepo deps, Lua hooks (xim.libxpkg), bwrap/proot (Linux sandbox), nsenter (Linux mount-namespace reuse), xpkg packaging.

**Parallelization map:**
- Phase 0 (sequential, blocking): Upstream library support
- Phase 1 (parallel): Track A = Task 2-3 (xim subos type handler); Track B = Task 4-5 (`subos use --cmd`)
- Phase 2 (sequential): Task 6-7 (`subos new --from`)
- Phase 3 (sequential): Task 8-9 (auto-keeper)
- Phase 4 (sequential): Task 10 (keeper flag overrides)

---

## File Structure

### Create

| Path | Responsibility |
|---|---|
| `src/core/xim/libxpkg/types/subos.cppm` | Default install/config/uninstall handlers for `type="subos"` packages — analogous to existing `script.cppm` |
| `src/core/subos/keeper.cppm` | Auto-keeper logic: spawn keeper process, manage `.keeper.pid` + `.last_used`, idle TTL self-kill, nsenter for cmd execution |
| `tests/e2e/subos_xpkg_install_test.sh` | e2e: install a `type="subos"` package, verify xpkgs/ layout and xvm registration |
| `tests/e2e/subos_xpkg_fork_test.sh` | e2e: fork from base pkg, verify workspace inheritance, deps not re-installed |
| `tests/e2e/subos_xpkg_use_cmd_test.sh` | e2e: `subos use --cmd` returns expected output + exit code |
| `tests/e2e/subos_xpkg_keeper_test.sh` | e2e (Linux): auto-keeper spawn, TTL behavior, `subos stop` cleanup |
| `tests/e2e/fixtures/subos_xpkg_demo/` | Test fixture — a sample `type="subos"` package's `.lua` + tarball |

### Modify

| Path | Change |
|---|---|
| `/home/speak/workspace/github/mcpplibs/libxpkg/src/xpkg.cppm` | Add `Subos` to `PackageType` enum (line 12) |
| `/home/speak/workspace/github/mcpplibs/libxpkg/src/xpkg-loader.cppm` | Map `"subos"` string → `PackageType::Subos` (line 153-156) |
| `src/core/xim/index.cppm` | Add `case 4 ↔ PackageType::Subos` to `int_to_type` and `type_to_int` (line 19-28) |
| `src/core/xim/libxpkg/types/type.cppm` | Update comment `// 0=Package, 1=Script, ..., 4=Subos` (line 79) |
| `src/core/xim/installer.cppm` | Add `else if pkgType == 4` dispatch to `subos::default_install`/`subos::default_config`/`subos::default_uninstall` (lines 1355, 1440 area, and uninstall section) |
| `src/core/subos.cppm` | Add `new_from_spec()` export (Task 6); extend `use_spawn_shell` to accept `cmd` arg (Task 4); integrate keeper hooks (Task 8); add `subos stop` (Task 8); add `--no-keep`/`--ttl`/`--keep` flag plumbing (Task 10) |
| `src/cli.cppm` | Add argparse for `subos new --from <spec>`, `subos use --cmd <str>`, `subos use --no-keep/--ttl/--keep`, `subos stop` (each at relevant Task) |
| `xmake.lua` | Add `src/core/subos/keeper.cppm` to module list (Task 8) |

---

## Phase 0: Upstream `PackageType::Subos` support

**Blocks everything.** All later phases assume `type = "subos"` resolves to a distinct pkgType.

### Task 1: Add `Subos` to mcpplibs/libxpkg + xlings type mapping

**Files:**
- Modify: `/home/speak/workspace/github/mcpplibs/libxpkg/src/xpkg.cppm:12`
- Modify: `/home/speak/workspace/github/mcpplibs/libxpkg/src/xpkg-loader.cppm:153-156`
- Modify: `src/core/xim/index.cppm:19-28`
- Modify: `src/core/xim/libxpkg/types/type.cppm:79`

- [ ] **Step 1: Explore — Read the upstream PackageType definition and the loader's `type_from_string` logic to confirm naming pattern**

Read: `/home/speak/workspace/github/mcpplibs/libxpkg/src/xpkg.cppm` (lines 1-50), and `xpkg-loader.cppm` (lines 140-170).

Expected understanding: enum is `enum class PackageType { Package, Script, Template, Config };` — append `Subos` as 5th value.

- [ ] **Step 2: Add `Subos` to enum**

Modify `xpkg.cppm:12`:

```cpp
enum class PackageType   { Package, Script, Template, Config, Subos };
```

- [ ] **Step 3: Map `"subos"` string to enum in loader**

Modify `xpkg-loader.cppm` (after line 155):

```cpp
if (s == "subos")    return PackageType::Subos;
return PackageType::Package;
```

- [ ] **Step 4: Add int mappings in xlings `index.cppm`**

Modify `src/core/xim/index.cppm` around line 19-28:

```cpp
int type_to_int(xpkg::PackageType t) {
    switch (t) {
        case xpkg::PackageType::Script:   return 1;
        case xpkg::PackageType::Template: return 2;
        case xpkg::PackageType::Config:   return 3;
        case xpkg::PackageType::Subos:    return 4;
        default: return 0;  // Package
    }
}
xpkg::PackageType int_to_type(int v) {
    switch (v) {
        case 1: return xpkg::PackageType::Script;
        case 2: return xpkg::PackageType::Template;
        case 3: return xpkg::PackageType::Config;
        case 4: return xpkg::PackageType::Subos;
        default: return xpkg::PackageType::Package;
    }
}
```

- [ ] **Step 5: Update comment in `type.cppm`**

Modify `src/core/xim/libxpkg/types/type.cppm:79`:

```cpp
int pkgType { 0 };  // 0=Package, 1=Script, 2=Template, 3=Config, 4=Subos
```

- [ ] **Step 6: Rebuild mcpplibs/libxpkg and verify xlings picks up new enum**

Run from `/home/speak/workspace/github/mcpplibs/libxpkg/`:
```bash
xmake -y
```

Then from xlings repo:
```bash
xmake clean
xmake -y
```

Expected: clean build, no errors. `xmake build` succeeds.

- [ ] **Step 7: Commit**

```bash
# In mcpplibs/libxpkg:
cd /home/speak/workspace/github/mcpplibs/libxpkg
git add src/xpkg.cppm src/xpkg-loader.cppm
git commit -m "feat(xpkg): add PackageType::Subos for subos-as-xpkg support

Adds 5th enum value and string mapping in loader. Consumed by xlings
to dispatch type='subos' packages through a dedicated install handler."

# In xlings:
cd -
git add src/core/xim/index.cppm src/core/xim/libxpkg/types/type.cppm
git commit -m "feat(xim): map PackageType::Subos to pkgType=4

Mirrors upstream mcpplibs/libxpkg enum addition. Foundation for
type=subos package dispatch (see .agents/docs/subos-as-xpkg-design-2026-05-16.md)."
```

---

## Phase 1: Parallel — Track A (M1) + Track B (M3)

**Both tracks operate on independent code paths and can be done by separate agents in parallel.**

### Track A — M1: type="subos" installer dispatch + default hooks

#### Task 2: Create `subos.cppm` libxpkg type module with default hooks

**Files:**
- Create: `src/core/xim/libxpkg/types/subos.cppm`
- Test: `tests/e2e/subos_xpkg_install_test.sh`
- Test fixture: `tests/e2e/fixtures/subos_xpkg_demo/py-demo.lua`

- [ ] **Step 1: Explore — Read `src/core/xim/libxpkg/types/script.cppm` to learn the default-handler pattern**

Read the file fully. Note function signatures:
- `bool default_install(const PlanNode&, ExecutionContext&)`
- `bool default_config(const PlanNode&, const std::filesystem::path& dataDir)`
- (May need `default_uninstall` too — check existing uninstall flow)

- [ ] **Step 2: Write failing e2e test fixture**

Create `tests/e2e/fixtures/subos_xpkg_demo/py-demo.lua`:

```lua
-- Fixture: minimal type="subos" package for e2e tests
package = {
    spec = "1",
    name = "py-demo",
    namespace = "subos",
    description = "Demo subos base",
    licenses = {"MIT"},
    type = "subos",
    archs = {"x86_64"},

    xpm = {
        linux = {
            deps = {},  -- no deps for the simplest test
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {},  -- no URL; default install just creates skeleton
        }
    }
}
-- No install/config/uninstall hooks — xim defaults handle everything
```

Create `tests/e2e/subos_xpkg_install_test.sh`:

```bash
#!/usr/bin/env bash
# e2e: install a type="subos" package, verify xpkgs/ layout
set -euo pipefail

XLINGS_HOME="$(mktemp -d)/xlings"
export XLINGS_HOME
export XLINGS_NON_INTERACTIVE=1

# Set up a local pkgindex with our fixture
mkdir -p "$XLINGS_HOME/data/xim-pkgindex/pkgs/p"
cp "$(dirname "$0")/fixtures/subos_xpkg_demo/py-demo.lua" \
   "$XLINGS_HOME/data/xim-pkgindex/pkgs/p/py-demo.lua"

# Install
xlings install subos:py-demo@1.0.0

# Verify install dir exists at standard xpkgs path
test -d "$XLINGS_HOME/data/xpkgs/py-demo/1.0.0" || {
    echo "FAIL: xpkgs/py-demo/1.0.0/ not created"
    exit 1
}

# Verify .xlings.json was written by default hook
test -f "$XLINGS_HOME/data/xpkgs/py-demo/1.0.0/.xlings.json" || {
    echo "FAIL: .xlings.json not created by default hook"
    exit 1
}

# Verify xvm registration
xlings list | grep -q "py-demo" || {
    echo "FAIL: py-demo not registered in xvm"
    exit 1
}

echo "PASS"
```

- [ ] **Step 3: Run test to verify it fails**

```bash
chmod +x tests/e2e/subos_xpkg_install_test.sh
./tests/e2e/subos_xpkg_install_test.sh
```

Expected: FAIL — `xlings install subos:py-demo` will error or hang because installer has no dispatch for pkgType=4 (after Phase 0, it routes to default Package payload extraction which expects URL/tarball).

- [ ] **Step 4: Implement `subos::default_install`**

Create `src/core/xim/libxpkg/types/subos.cppm`:

```cpp
export module xlings.core.xim.libxpkg.types.subos;

import std;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.catalog;
import xlings.core.common;
import xlings.core.config;
import xlings.core.log;
import xlings.core.xself;
import xlings.core.xvm.db;
import xlings.libs.json;
import mcpplibs.xpkg.executor;

export namespace xlings::xim::subos {

// Default install for type="subos" packages:
// - install_dir comes from extracted tarball OR is empty (creates skeleton)
// - ensures .xlings.json exists (auto-generates workspace from deps if missing)
// - subos baseline structure: bin/ (empty, shims minted by xvm)
bool default_install(const PlanNode& node,
                     mcpplibs::xpkg::ExecutionContext& ctx) {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::create_directories(ctx.install_dir, ec);
    if (ec) {
        log::error("subos: failed to create install dir {}: {}",
                   ctx.install_dir.string(), ec.message());
        return false;
    }

    fs::create_directories(ctx.install_dir / "bin", ec);

    // If tarball already laid down .xlings.json, leave it; else synthesize
    // from deps. Deps are visible via the resolved node's deps list.
    auto xlingsJson = ctx.install_dir / ".xlings.json";
    if (!fs::exists(xlingsJson)) {
        nlohmann::json j;
        j["workspace"] = nlohmann::json::object();
        for (const auto& dep : node.deps) {
            // dep format: "name@version" or just "name"
            auto at = dep.find('@');
            if (at != std::string::npos) {
                j["workspace"][dep.substr(0, at)] = dep.substr(at + 1);
            } else {
                j["workspace"][dep] = "latest";
            }
        }
        std::ofstream ofs(xlingsJson);
        ofs << j.dump(2);
    }

    log::debug("subos installed: {}", ctx.install_dir.string());
    return true;
}

// Default config: register in xvm so package is queryable
bool default_config(const PlanNode& node,
                    const std::filesystem::path& dataDir) {
    auto storeName = package_store_name(node.namespaceName, node.name);
    auto installDir = (node.storeRoot.empty() ? (dataDir / "xpkgs") : node.storeRoot)
        / storeName
        / node.version;
    auto bindir = (installDir / "bin").string();

    xvm::add_version(Config::versions_mut(),
                     node.name, node.version, bindir, "program", "", "");

    auto ver_key = xvm::make_ns_version("", node.version);
    Config::workspace_mut()[node.name] = ver_key;

    Config::save_versions();
    Config::save_workspace();
    return true;
}

// Default uninstall: xpkg removal handled by xim's standard path; this
// just needs to not break it (default = no-op since xim removes install_dir)
bool default_uninstall(const PlanNode& node) {
    log::debug("subos uninstalling: {}", node.name);
    return true;
}

} // namespace xlings::xim::subos
```

- [ ] **Step 5: Add dispatch in `installer.cppm`**

Modify `src/core/xim/installer.cppm`:

Add import at top (around line 22):
```cpp
import xlings.core.xim.libxpkg.types.subos;
```

Add dispatch in install loop (after line 1364, where Script dispatch ends):

```cpp
} else if (!payloadInstalled && node.pkgType == 4 /* Subos */) {
    log::debug("installing subos base {}...", node.name);
    if (!subos::default_install(node, ctx)) {
        if (onStatus) {
            onStatus({ node.name, InstallPhase::Failed, 0.0f,
                       "default subos install failed" });
        }
        continue;
    }
}
```

Add config dispatch (after line 1447, where Script config dispatch ends):

```cpp
} else if (!executor.has_hook(mcpplibs::xpkg::HookType::Config) && node.pkgType == 4 /* Subos */) {
    if (!subos::default_config(node, dataDir)) {
        if (onStatus) {
            onStatus({ node.name, InstallPhase::Failed, 0.0f,
                       "default subos config failed" });
        }
        continue;
    }
}
```

- [ ] **Step 6: Register module in `xmake.lua`**

Modify `xmake.lua`, find the modules section, add `src/core/xim/libxpkg/types/subos.cppm` next to `script.cppm`.

- [ ] **Step 7: Rebuild and run test**

```bash
xmake -y
./tests/e2e/subos_xpkg_install_test.sh
```

Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add src/core/xim/libxpkg/types/subos.cppm src/core/xim/installer.cppm xmake.lua \
        tests/e2e/subos_xpkg_install_test.sh tests/e2e/fixtures/subos_xpkg_demo/
git commit -m "feat(xim): dispatch type='subos' through default hooks

Adds xim::subos::default_install/config/uninstall mirroring script.cppm
pattern. Default install ensures .xlings.json + bin/ skeleton; default
config registers the package via xvm.add. Authors can override any of
the three hooks.

Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M1)"
```

#### Task 3: type="subos" with tarball URL support + author override

**Files:**
- Modify: `tests/e2e/fixtures/subos_xpkg_demo/py-demo.lua` (add deps to test workspace synthesis)
- Test: extend `tests/e2e/subos_xpkg_install_test.sh`

- [ ] **Step 1: Extend test fixture to declare deps**

```lua
-- Update py-demo.lua: add deps and a custom install hook
-- ...
xpm = {
    linux = {
        deps = {"hello"},  -- assume xim-pkgindex has 'hello' package for testing
        ...
    }
}

import("xim.libxpkg.pkginfo")

function install()
    -- Custom hook: write a marker file to show override works,
    -- then call back to default behavior (write .xlings.json)
    local dir = pkginfo.install_dir()
    io.writefile(path.join(dir, ".author-touched"), "1")
    -- The default install would still need to run; for now, hook
    -- replaces default entirely, so we manually do what defaults do:
    os.mkdir(path.join(dir, "bin"))
    return true
end
```

- [ ] **Step 2: Extend e2e test for override case**

Add to `subos_xpkg_install_test.sh`:

```bash
# After install verification, also check author hook ran
test -f "$XLINGS_HOME/data/xpkgs/py-demo/1.0.0/.author-touched" || {
    echo "FAIL: author install hook didn't run (override broken)"
    exit 1
}
```

- [ ] **Step 3: Run test, verify pass**

```bash
xmake -y && ./tests/e2e/subos_xpkg_install_test.sh
```

Expected: PASS (override behavior works because if hook is present, xim runs it instead of default — this is the existing executor logic, no extra code needed in our part)

- [ ] **Step 4: Commit**

```bash
git add tests/e2e/fixtures/subos_xpkg_demo/py-demo.lua tests/e2e/subos_xpkg_install_test.sh
git commit -m "test(subos-xpkg): cover author install-hook override"
```

---

### Track B — M3: `subos use --cmd`

#### Task 4: Add `--cmd <string>` argparse + thread through to use_spawn_shell

**Files:**
- Modify: `src/cli.cppm` (subos use argparse)
- Modify: `src/core/subos.cppm` (use_spawn_shell signature)
- Test: `tests/e2e/subos_xpkg_use_cmd_test.sh`

- [ ] **Step 1: Explore — Read current subos use argparse in `src/cli.cppm`**

```
grep -n "subos.*use\|--sandbox\|--shell\|--global" src/cli.cppm | head -30
```

Read the surrounding context (lines ±20) to understand how the existing flags are parsed and passed to `subos::use_spawn_shell`. Note the function call signature.

- [ ] **Step 2: Write failing e2e test**

Create `tests/e2e/subos_xpkg_use_cmd_test.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

XLINGS_HOME="$(mktemp -d)/xlings"
export XLINGS_HOME
export XLINGS_NON_INTERACTIVE=1

# Create a subos (any storage mode)
xlings subos new test-cmd --storage shared

# Single command, no sandbox
output=$(xlings subos use test-cmd --cmd "echo hello-from-subos")
[[ "$output" == *"hello-from-subos"* ]] || {
    echo "FAIL: --cmd didn't run the command"
    echo "Got: $output"
    exit 1
}

# Exit code propagates
set +e
xlings subos use test-cmd --cmd "exit 42"
rc=$?
set -e
[[ "$rc" -eq 42 ]] || {
    echo "FAIL: exit code not propagated (got $rc, want 42)"
    exit 1
}

echo "PASS"
```

- [ ] **Step 3: Run test, expect fail**

```bash
chmod +x tests/e2e/subos_xpkg_use_cmd_test.sh
./tests/e2e/subos_xpkg_use_cmd_test.sh
```

Expected: FAIL with "unknown flag --cmd" or similar.

- [ ] **Step 4: Add `--cmd <string>` to cli.cppm subos use argparse**

Modify the subos-use argparse section in `src/cli.cppm` to accept `--cmd <string>`:

```cpp
// In subos use subparser:
auto cmd_arg = sub_use.add_argument("--cmd")
    .help("Run a single command in the subos and exit (non-interactive)")
    .default_value(std::string{});

// In the dispatch:
auto cmd = sub_use.get<std::string>("--cmd");
return subos::use_spawn_shell(name, stream, sandbox, sandbox_backend, cmd);
```

- [ ] **Step 5: Extend `use_spawn_shell` to accept cmd parameter**

Modify `src/core/subos.cppm` `use_spawn_shell` (around line 1390):

```cpp
int use_spawn_shell(const std::string& name, EventStream& stream,
                    bool sandbox = false,
                    const std::string& sandbox_backend = "",
                    const std::string& cmd = "")
{
    if (sandbox) return use_sandbox_mode_(name, stream, sandbox_backend, cmd);

    // ... existing pre-exec logic ...

    // POSIX exec branch (around line 1488):
    auto shell = utils::get_env_or_default("SHELL");
    if (shell.empty()) shell = "/bin/sh";

    if (!cmd.empty()) {
        // Non-interactive single-command path
        ::execl(shell.c_str(), shell.c_str(), "-c", cmd.c_str(),
                static_cast<char*>(nullptr));
    } else {
        // Interactive default
        ::execl(shell.c_str(), shell.c_str(), "-i", static_cast<char*>(nullptr));
    }
    log::error("failed to exec shell '{}': {}", shell, std::strerror(errno));
    return 127;
}
```

For Windows branch, route to `pwsh -Command <cmd>` / `cmd /c <cmd>` when cmd non-empty:

```cpp
#if defined(_WIN32)
    // ... in the shell selection loop, modify cmdline:
    std::string cmdline = exe;
    if (!cmd.empty()) {
        // pwsh / powershell support "-Command", cmd.exe uses "/c"
        if (std::string(exe).find("cmd.exe") != std::string::npos) {
            cmdline += " /c " + cmd;
        } else {
            cmdline += " -Command \"" + cmd + "\"";
        }
    }
    // ... rest of CreateProcessA logic
#endif
```

- [ ] **Step 6: Update `use_sandbox_mode_` to accept + thread cmd**

Read sandbox mode function (likely `use_sandbox_mode_` around line 1100-1200 in subos.cppm). Modify signature to accept `const std::string& cmd = ""`, and:

- When `cmd.empty()`: existing behavior (interactive shell in sandbox)
- When `cmd` non-empty: bwrap/proot command line ends with `-- sh -c <cmd>` instead of default shell

(Engineer: read the existing bwrap/proot command construction, append `"-c", cmd` to the argv after the shell binary.)

- [ ] **Step 7: Update back-compat `use()` wrapper**

```cpp
export int use(const std::string& name, EventStream& stream) {
    return use_global(name, stream);  // unchanged
}
```

The existing `use` wrapper does not need the cmd parameter; CLI dispatch goes through `use_spawn_shell` directly.

- [ ] **Step 8: Rebuild and run test**

```bash
xmake -y && ./tests/e2e/subos_xpkg_use_cmd_test.sh
```

Expected: PASS

- [ ] **Step 9: Commit**

```bash
git add src/cli.cppm src/core/subos.cppm tests/e2e/subos_xpkg_use_cmd_test.sh
git commit -m "feat(subos): subos use --cmd <string> for non-interactive exec

Adds a --cmd flag that runs a single command in the subos via sh -c and
exits with the command's exit code. Works in both shell-level and sandbox
mode. Backwards compatible: --cmd absent => interactive shell as before.

Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M3)"
```

#### Task 5: --cmd in sandbox mode (image + tmpfs storage)

**Files:**
- Test: extend `tests/e2e/subos_xpkg_use_cmd_test.sh`

- [ ] **Step 1: Extend test to cover sandbox + tmpfs**

Append to `subos_xpkg_use_cmd_test.sh`:

```bash
# Sandbox + tmpfs storage
xlings subos new test-cmd-sb --storage tmpfs
output=$(xlings subos use test-cmd-sb --sandbox --cmd "echo sandbox-ok")
[[ "$output" == *"sandbox-ok"* ]] || {
    echo "FAIL: --cmd in sandbox mode failed"
    exit 1
}
```

- [ ] **Step 2: Run, verify pass; if fail, fix sandbox cmd plumbing per Task 4 Step 6**

```bash
./tests/e2e/subos_xpkg_use_cmd_test.sh
```

Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add tests/e2e/subos_xpkg_use_cmd_test.sh
git commit -m "test(subos): cover --cmd in sandbox+tmpfs mode"
```

---

## Phase 2: M2 — `subos new --from <spec>`

(Requires Task 2 from Track A to have base packages available for testing.)

### Task 6: `subos new --from <local-subos>` (local fork only, simpler case first)

**Files:**
- Modify: `src/cli.cppm` (subos new argparse)
- Modify: `src/core/subos.cppm` (new export `new_from`)
- Test: `tests/e2e/subos_xpkg_fork_test.sh`

- [ ] **Step 1: Write failing e2e test**

Create `tests/e2e/subos_xpkg_fork_test.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

XLINGS_HOME="$(mktemp -d)/xlings"
export XLINGS_HOME
export XLINGS_NON_INTERACTIVE=1

# Create a base subos
xlings subos new base-env --storage shared
echo "hello from base" > "$XLINGS_HOME/subos/base-env/marker.txt"

# Fork it
xlings subos new fork-env --from base-env

# Verify fork inherited content
test -f "$XLINGS_HOME/subos/fork-env/marker.txt" || {
    echo "FAIL: fork didn't copy marker"
    exit 1
}
content=$(cat "$XLINGS_HOME/subos/fork-env/marker.txt")
[[ "$content" == "hello from base" ]] || {
    echo "FAIL: fork content mismatch"
    exit 1
}

# Modifying fork shouldn't affect base
echo "modified" > "$XLINGS_HOME/subos/fork-env/marker.txt"
base_content=$(cat "$XLINGS_HOME/subos/base-env/marker.txt")
[[ "$base_content" == "hello from base" ]] || {
    echo "FAIL: fork modification leaked to base"
    exit 1
}

echo "PASS"
```

- [ ] **Step 2: Run, expect fail**

```bash
chmod +x tests/e2e/subos_xpkg_fork_test.sh
./tests/e2e/subos_xpkg_fork_test.sh
```

Expected: FAIL — `--from` unknown flag.

- [ ] **Step 3: Add `--from <spec>` to argparse**

In `src/cli.cppm` subos new subparser:

```cpp
auto from_arg = sub_new.add_argument("--from")
    .help("Fork from another subos (local name) or install from a subos xpkg (spec like 'subos:py-ds@1.0.0')")
    .default_value(std::string{});

// Dispatch:
auto from = sub_new.get<std::string>("--from");
if (!from.empty()) {
    return subos::new_from(name, customDir, storage, imageSize, from, stream);
} else {
    return subos::create(name, customDir, storage, imageSize, stream);
}
```

- [ ] **Step 4: Implement `new_from` in subos.cppm**

Add to `src/core/subos.cppm`:

```cpp
namespace new_from_detail_ {

// Detect whether spec is a pkg-spec (contains `:` or `@`) or a local subos name
bool is_pkg_spec_(const std::string& spec) {
    return spec.find(':') != std::string::npos || spec.find('@') != std::string::npos;
}

// Cross-platform copy with reflink preferred (silent fallback to full copy).
int copy_dir_(const fs::path& src, const fs::path& dst, EventStream& stream) {
    std::error_code ec;
    fs::create_directories(dst, ec);
#if defined(__linux__)
    // Use cp --reflink=auto for COW where supported
    auto cmd = "cp -a --reflink=auto " + src.string() + "/. " + dst.string() + "/";
    auto rc = std::system(cmd.c_str());
    if (rc != 0) {
        log::warn("reflink copy failed (rc={}), falling back to full copy", rc);
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
#elif defined(__APPLE__)
    // macOS APFS clonefile via /bin/cp -c
    auto cmd = "cp -ac " + src.string() + "/. " + dst.string() + "/";
    std::system(cmd.c_str());
#else
    // Windows / generic
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
#endif
    if (ec) {
        log::error("copy failed: {}", ec.message());
        return 1;
    }
    return 0;
}

} // namespace new_from_detail_

export int new_from(const std::string& name, const fs::path& customDir,
                    StorageMode storage, const std::string& imageSize,
                    const std::string& fromSpec, EventStream& stream) {
    auto& p = Config::paths();

    if (new_from_detail_::is_pkg_spec_(fromSpec)) {
        // pkg-spec path: handled in Task 7
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "--from <pkg-spec> not yet implemented in this task",
            .recoverable = false,
        });
        return 1;
    }

    // Local subos fork
    auto srcDir = p.homeDir / "subos" / fromSpec;
    if (!fs::exists(srcDir)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::NotFound,
            .message = "source subos '" + fromSpec + "' not found",
            .recoverable = true,
        });
        return 1;
    }

    // Create empty target subos with correct storage
    if (auto rc = create(name, customDir, storage, imageSize, stream); rc != 0) {
        return rc;
    }

    auto dstDir = customDir.empty() ? (p.homeDir / "subos" / name) : customDir;

    // Copy content (but NOT .xlings.json — already created by create(); we'll merge)
    // Safer: copy everything, then rewrite name in .xlings.json
    if (auto rc = new_from_detail_::copy_dir_(srcDir, dstDir, stream); rc != 0) {
        return rc;
    }

    // Rewrite name field in .xlings.json (if it has a name field)
    // (Most subos .xlings.json doesn't carry name; this is defensive)

    nlohmann::json payload;
    payload["name"] = name;
    payload["from"] = fromSpec;
    payload["mode"] = "local-fork";
    stream.emit(DataEvent{"subos_forked", payload.dump()});
    return 0;
}
```

- [ ] **Step 5: Build and run test**

```bash
xmake -y && ./tests/e2e/subos_xpkg_fork_test.sh
```

Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/cli.cppm src/core/subos.cppm tests/e2e/subos_xpkg_fork_test.sh
git commit -m "feat(subos): subos new --from <local-subos> for local fork

Adds a local-fork path: cp -a (reflink where supported) the source
subos dir into the new one. macOS uses APFS clonefile via cp -c.

Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M2 - local fork)"
```

### Task 7: `subos new --from <pkg-spec>` with auto-install

**Files:**
- Modify: `src/core/subos.cppm` (new_from pkg-spec branch)
- Test: extend `tests/e2e/subos_xpkg_fork_test.sh`

- [ ] **Step 1: Extend test fixture to cover pkg-spec fork**

Append to `subos_xpkg_fork_test.sh`:

```bash
# pkg-spec fork — uses fixture from Task 2
mkdir -p "$XLINGS_HOME/data/xim-pkgindex/pkgs/p"
cp "$(dirname "$0")/fixtures/subos_xpkg_demo/py-demo.lua" \
   "$XLINGS_HOME/data/xim-pkgindex/pkgs/p/py-demo.lua"

# This should auto-install subos:py-demo if missing, then fork
xlings subos new from-pkg --from subos:py-demo@1.0.0

test -d "$XLINGS_HOME/subos/from-pkg" || {
    echo "FAIL: pkg-spec fork didn't create subos"
    exit 1
}

# Base should be installed under xpkgs/
test -d "$XLINGS_HOME/data/xpkgs/py-demo/1.0.0" || {
    echo "FAIL: base pkg not auto-installed"
    exit 1
}

# .xlings.json should have been copied
test -f "$XLINGS_HOME/subos/from-pkg/.xlings.json" || {
    echo "FAIL: .xlings.json not copied from base"
    exit 1
}
```

- [ ] **Step 2: Implement pkg-spec branch**

Modify `new_from` in `subos.cppm`:

```cpp
if (new_from_detail_::is_pkg_spec_(fromSpec)) {
    // Strip namespace prefix and parse version
    std::string pkgName = fromSpec;
    std::string version;
    if (auto colon = pkgName.find(':'); colon != std::string::npos) {
        pkgName = pkgName.substr(colon + 1);
    }
    if (auto at = pkgName.find('@'); at != std::string::npos) {
        version = pkgName.substr(at + 1);
        pkgName = pkgName.substr(0, at);
    }

    // Locate base in xpkgs/<name>/<ver>/
    auto baseDir = Config::global_data_dir() / "xpkgs" / pkgName / version;
    if (!fs::exists(baseDir)) {
        // Auto-install base
        log::info("base {}:{} not installed, installing...", pkgName, version);
        auto cmd = "xlings install " + fromSpec;
        auto rc = std::system(cmd.c_str());
        if (rc != 0 || !fs::exists(baseDir)) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "failed to install base " + fromSpec,
                .recoverable = true,
            });
            return 1;
        }
    }

    // Validate type=subos
    // (Read xvm registry to check; or simpler: check .xlings.json existence)
    if (!fs::exists(baseDir / ".xlings.json")) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = fromSpec + " is not a type='subos' package",
            .recoverable = false,
        });
        return 1;
    }

    // Create empty target subos
    if (auto rc = create(name, customDir, storage, imageSize, stream); rc != 0) {
        return rc;
    }

    // Copy base .xlings.json + any extra files (templates/, etc.)
    auto dstDir = customDir.empty() ? (Config::paths().homeDir / "subos" / name) : customDir;
    if (auto rc = new_from_detail_::copy_dir_(baseDir, dstDir, stream); rc != 0) {
        return rc;
    }

    nlohmann::json payload;
    payload["name"] = name;
    payload["from"] = fromSpec;
    payload["mode"] = "pkg-fork";
    stream.emit(DataEvent{"subos_forked", payload.dump()});
    return 0;
}
```

- [ ] **Step 3: Build, run test, verify pass**

```bash
xmake -y && ./tests/e2e/subos_xpkg_fork_test.sh
```

Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add src/core/subos.cppm tests/e2e/subos_xpkg_fork_test.sh
git commit -m "feat(subos): subos new --from <pkg-spec> with auto-install

When --from refers to a subos: pkg-spec (contains : or @), locate
xpkgs/<name>/<ver>/. If absent, auto-invoke 'xlings install <spec>'.
Then fork from the materialized base via the same cp -a path as
local fork.

Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M2 - pkg fork, E5)"
```

---

## Phase 3: M4 — Auto-keeper

(Requires M3 since keeper extends `use_spawn_shell --cmd` execution.)

### Task 8: Create keeper module — spawn, register, nsenter

**Files:**
- Create: `src/core/subos/keeper.cppm`
- Modify: `xmake.lua` (add module)
- Modify: `src/core/subos.cppm` (integrate `use_sandbox_mode_` with keeper)
- Test: `tests/e2e/subos_xpkg_keeper_test.sh` (Linux only)

- [ ] **Step 1: Explore — Read `use_sandbox_mode_` in subos.cppm**

```
grep -n "use_sandbox_mode_\|bwrap\|proot" src/core/subos.cppm | head -20
```

Read function body. Understand how bwrap argv is constructed and how command exec happens.

- [ ] **Step 2: Write failing e2e test**

Create `tests/e2e/subos_xpkg_keeper_test.sh`:

```bash
#!/usr/bin/env bash
# Linux-only test
[[ "$(uname)" != "Linux" ]] && { echo "SKIP (non-Linux)"; exit 0; }

set -euo pipefail
XLINGS_HOME="$(mktemp -d)/xlings"
export XLINGS_HOME
export XLINGS_NON_INTERACTIVE=1

# Create tmpfs subos (auto-keeper trigger condition)
xlings subos new k-test --storage tmpfs

# First cmd: should spawn keeper
time_first=$(date +%s%3N)
xlings subos use k-test --sandbox --cmd "echo first" > /dev/null
time_first_end=$(date +%s%3N)
duration_first=$((time_first_end - time_first))

# Keeper PID file should exist
test -f "$XLINGS_HOME/subos/k-test/.keeper.pid" || {
    echo "FAIL: keeper PID file not created"
    exit 1
}

# Second cmd: should reuse keeper (much faster)
time_second=$(date +%s%3N)
xlings subos use k-test --sandbox --cmd "echo second" > /dev/null
time_second_end=$(date +%s%3N)
duration_second=$((time_second_end - time_second))

echo "first=${duration_first}ms second=${duration_second}ms"
[[ "$duration_second" -lt "$duration_first" ]] || {
    echo "WARN: second exec not faster than first (keeper may not be working)"
}

# Stop keeper explicitly
xlings subos stop k-test

# PID file gone
test ! -f "$XLINGS_HOME/subos/k-test/.keeper.pid" || {
    echo "FAIL: PID file not cleaned up after stop"
    exit 1
}

echo "PASS"
```

- [ ] **Step 3: Run, expect fail**

Expected: FAIL (no `subos stop` command; no keeper logic).

- [ ] **Step 4: Implement keeper module**

Create `src/core/subos/keeper.cppm`:

```cpp
export module xlings.core.subos.keeper;

import std;
import xlings.core.config;
import xlings.core.log;

#if defined(__linux__)
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <sched.h>
#endif

export namespace xlings::subos::keeper {

namespace fs = std::filesystem;

constexpr int DEFAULT_TTL_SEC = 300;  // 5 min idle

struct KeeperState {
    fs::path pidFile;
    fs::path lastUsedFile;
};

KeeperState state_for(const std::string& subosName) {
    auto& p = Config::paths();
    auto dir = p.homeDir / "subos" / subosName;
    return { dir / ".keeper.pid", dir / ".last_used" };
}

// Check if keeper for this subos exists and is alive
bool is_alive(const std::string& subosName) {
#if !defined(__linux__)
    return false;
#else
    auto s = state_for(subosName);
    if (!fs::exists(s.pidFile)) return false;
    std::ifstream ifs(s.pidFile);
    pid_t pid;
    ifs >> pid;
    if (pid <= 0) return false;
    return ::kill(pid, 0) == 0;
#endif
}

// Touch .last_used (called by every cmd exec)
void touch_activity(const std::string& subosName) {
    auto s = state_for(subosName);
    std::ofstream ofs(s.lastUsedFile);
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    ofs << now;
}

// Spawn keeper for the given subos. Returns the keeper PID, or -1 on failure.
// Should be called after bwrap mount is established; the keeper sits in
// the mount namespace and self-exits when idle TTL expires.
pid_t spawn_keeper(const std::string& subosName, int ttlSec = DEFAULT_TTL_SEC) {
#if !defined(__linux__)
    return -1;
#else
    auto s = state_for(subosName);

    pid_t pid = ::fork();
    if (pid < 0) {
        log::error("fork failed: {}", std::strerror(errno));
        return -1;
    }
    if (pid == 0) {
        // Child: keeper loop
        // (Caller is responsible for having entered the right mount NS before fork)
        for (;;) {
            ::sleep(10);  // wake every 10s
            // Check idle
            std::ifstream ifs(s.lastUsedFile);
            long long lastUsed = 0;
            if (ifs) ifs >> lastUsed;
            auto now = std::chrono::system_clock::now().time_since_epoch().count();
            // crude: if last_used not bumped in ttlSec, exit
            auto diff = (now - lastUsed) / 1'000'000'000LL;  // ns → s
            if (diff > ttlSec) {
                log::debug("keeper {} idle, exiting", subosName);
                std::exit(0);
            }
        }
    }
    // Parent: record pid
    {
        std::ofstream ofs(s.pidFile);
        ofs << pid;
    }
    touch_activity(subosName);
    return pid;
#endif
}

// nsenter into the keeper's mount namespace and exec the command.
// Returns the command's exit code.
int nsenter_and_exec(const std::string& subosName, const std::string& cmd) {
#if !defined(__linux__)
    return -1;
#else
    auto s = state_for(subosName);
    std::ifstream ifs(s.pidFile);
    pid_t pid;
    ifs >> pid;
    if (pid <= 0) return -1;

    touch_activity(subosName);

    auto nsCmd = std::format("nsenter --mount=/proc/{}/ns/mnt -- sh -c '{}'", pid, cmd);
    return std::system(nsCmd.c_str());
#endif
}

// Stop keeper (called by `subos stop`)
int stop_keeper(const std::string& subosName) {
    auto s = state_for(subosName);
    if (!fs::exists(s.pidFile)) return 0;
#if defined(__linux__)
    std::ifstream ifs(s.pidFile);
    pid_t pid;
    ifs >> pid;
    if (pid > 0) ::kill(pid, SIGTERM);
#endif
    std::error_code ec;
    fs::remove(s.pidFile, ec);
    fs::remove(s.lastUsedFile, ec);
    return 0;
}

// Auto-trigger condition: storage=image|tmpfs + sandbox + Linux
bool should_auto_keeper(const std::string& storage, bool sandbox) {
#if !defined(__linux__)
    return false;
#else
    if (!sandbox) return false;
    return storage == "image" || storage == "tmpfs";
#endif
}

} // namespace xlings::subos::keeper
```

- [ ] **Step 5: Wire keeper into `use_sandbox_mode_`**

Modify `use_sandbox_mode_` in `subos.cppm`:

```cpp
import xlings.core.subos.keeper;

// In use_sandbox_mode_ before bwrap exec:
auto storage = read_storage_mode_str_(name);  // helper to read .xlings.json storage field
if (keeper::should_auto_keeper(storage, /*sandbox=*/true)) {
    if (keeper::is_alive(name)) {
        // Reuse: nsenter
        if (!cmd.empty()) {
            return keeper::nsenter_and_exec(name, cmd);
        }
        // For interactive shells with keeper, also nsenter
        return keeper::nsenter_and_exec(name, "/bin/sh -i");
    }
    // Spawn fresh keeper after bwrap mounts up
    // (engineer: integrate spawn at the right point after mount setup)
}

// Then proceed with normal bwrap exec
```

(Engineer: the exact integration point depends on the existing bwrap flow. The keeper needs to be forked from a process that has already entered the mount namespace. Likely after bwrap setup but before the user command runs.)

- [ ] **Step 6: Add `subos stop` CLI**

In `src/cli.cppm`:

```cpp
auto sub_stop = subos_parser.add_subparser("stop");
sub_stop.add_argument("name");
// Dispatch:
return subos::keeper::stop_keeper(sub_stop.get<std::string>("name"));
```

- [ ] **Step 7: Register keeper.cppm in xmake.lua**

- [ ] **Step 8: Build and run test**

```bash
xmake -y && ./tests/e2e/subos_xpkg_keeper_test.sh
```

Expected: PASS on Linux. SKIP on macOS/Windows.

- [ ] **Step 9: Commit**

```bash
git add src/core/subos/keeper.cppm src/core/subos.cppm src/cli.cppm xmake.lua \
        tests/e2e/subos_xpkg_keeper_test.sh
git commit -m "feat(subos): auto-keeper for high-frequency sandbox exec (Linux)

Auto-spawns a keeper process holding the mount namespace when
storage=image|tmpfs + --sandbox on Linux. Subsequent --cmd execs
nsenter into the existing namespace, ~10ms vs ~100-500ms cold mount.
TTL=5min idle, then keeper self-exits and mount tears down.

Adds 'subos stop <name>' for explicit cleanup.

Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M4)"
```

### Task 9: Auto-keeper TTL self-kill + stale PID cleanup

**Files:**
- Modify: `src/core/subos/keeper.cppm` (refine spawn_keeper loop)
- Test: extend `tests/e2e/subos_xpkg_keeper_test.sh`

- [ ] **Step 1: Write test for stale PID cleanup**

Append to keeper test:

```bash
# Simulate stale PID: write a fake old PID then run cmd
echo "999999" > "$XLINGS_HOME/subos/k-test/.keeper.pid"
xlings subos use k-test --sandbox --cmd "echo cleanup-ok" > /dev/null

# After exec, the stale PID should have been cleaned and new one written
new_pid=$(cat "$XLINGS_HOME/subos/k-test/.keeper.pid" 2>/dev/null || echo "")
[[ "$new_pid" != "999999" ]] || {
    echo "FAIL: stale PID not replaced"
    exit 1
}
```

- [ ] **Step 2: Add stale PID handling in `is_alive`**

Already handled in Step 4 of Task 8 (`kill(pid, 0) == 0` check). Verify by inspecting code — if pid file exists but process dead, we should clean and respawn. Add cleanup:

```cpp
bool is_alive(const std::string& subosName) {
    auto s = state_for(subosName);
    if (!fs::exists(s.pidFile)) return false;
    std::ifstream ifs(s.pidFile);
    pid_t pid;
    ifs >> pid;
    if (pid <= 0 || ::kill(pid, 0) != 0) {
        // Stale — clean up
        std::error_code ec;
        fs::remove(s.pidFile, ec);
        fs::remove(s.lastUsedFile, ec);
        return false;
    }
    return true;
}
```

- [ ] **Step 3: Build, test, commit**

```bash
xmake -y && ./tests/e2e/subos_xpkg_keeper_test.sh
git add src/core/subos/keeper.cppm tests/e2e/subos_xpkg_keeper_test.sh
git commit -m "feat(subos): keeper stale PID cleanup + auto-respawn"
```

---

## Phase 4: M5 — Keeper flag overrides

### Task 10: --no-keep / --ttl / --keep flags

**Files:**
- Modify: `src/cli.cppm` (subos use argparse)
- Modify: `src/core/subos.cppm` (thread flags through)
- Modify: `src/core/subos/keeper.cppm` (accept TTL parameter, --keep = infinite)

- [ ] **Step 1: Add flags to argparse**

```cpp
sub_use.add_argument("--no-keep").flag().help("Disable auto-keeper for this exec");
sub_use.add_argument("--keep").flag().help("Use a never-expiring keeper");
sub_use.add_argument("--ttl").default_value(0).scan<'i', int>().help("Keeper idle TTL in seconds (default 300)");
```

- [ ] **Step 2: Thread to use_sandbox_mode_ / use_spawn_shell**

Add `KeeperPolicy` struct or simple param trio:

```cpp
struct KeeperPolicy {
    bool no_keep = false;
    bool keep_forever = false;
    int ttl_sec = 0;  // 0 = use default
};
```

- [ ] **Step 3: Honor flags in keeper.cppm**

```cpp
pid_t spawn_keeper(..., int ttlSec) {
    auto effectiveTtl = (ttlSec > 0) ? ttlSec : DEFAULT_TTL_SEC;
    // pass effectiveTtl to keeper loop; if INT_MAX, never exit on idle
}
```

`--keep` → ttl = INT_MAX. `--no-keep` → skip keeper entirely. `--ttl N` → ttl = N.

- [ ] **Step 4: Write tests for each flag**

Append to keeper test:

```bash
# --no-keep: no PID file should be created
xlings subos new no-keep-test --storage tmpfs
xlings subos use no-keep-test --sandbox --no-keep --cmd "echo nk"
test ! -f "$XLINGS_HOME/subos/no-keep-test/.keeper.pid" || {
    echo "FAIL: --no-keep still created keeper"
    exit 1
}

# --ttl <small>: keeper should self-exit after that interval
xlings subos new ttl-test --storage tmpfs
xlings subos use ttl-test --sandbox --ttl 1 --cmd "echo ttl"
test -f "$XLINGS_HOME/subos/ttl-test/.keeper.pid" || {
    echo "FAIL: --ttl 1 should still spawn keeper"
    exit 1
}
# Wait 15s (idle > 1s, keeper polls every 10s)
sleep 15
test ! -f "$XLINGS_HOME/subos/ttl-test/.keeper.pid" || {
    echo "FAIL: keeper didn't self-exit after TTL"
    exit 1
}
```

- [ ] **Step 5: Build, test, commit**

```bash
xmake -y && ./tests/e2e/subos_xpkg_keeper_test.sh
git add src/cli.cppm src/core/subos.cppm src/core/subos/keeper.cppm \
        tests/e2e/subos_xpkg_keeper_test.sh
git commit -m "feat(subos): --no-keep / --ttl / --keep flag overrides for keeper

Adds explicit keeper policy flags:
- --no-keep: skip keeper, fresh mount each exec
- --ttl <sec>: custom idle timeout
- --keep: never-expiring keeper (manual stop required)

Default behavior (auto-keeper, TTL=5min) unchanged.

Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M5)"
```

---

## Final: Integration verification + PR

### Task 11: Full e2e suite run + branch + PR

- [ ] **Step 1: Run all subos-xpkg e2e tests in sequence**

```bash
chmod +x tests/e2e/subos_xpkg_*.sh
for t in tests/e2e/subos_xpkg_*.sh; do
    echo "=== $t ==="
    "$t" || { echo "FAIL: $t"; exit 1; }
done
echo "All PASS"
```

- [ ] **Step 2: Verify CI configuration covers new tests**

Read `.github/workflows/xlings-ci-linux.yml`. If e2e runner discovers `tests/e2e/*.sh` automatically (per existing pattern), no change needed. Otherwise add explicit invocation.

- [ ] **Step 3: Create feature branch**

```bash
git checkout -b feat/subos-as-xpkg
```

(All commits done on this branch from Phase 0 onward.)

- [ ] **Step 4: Push branch**

```bash
git push -u origin feat/subos-as-xpkg
```

- [ ] **Step 5: Open PR**

```bash
gh pr create --draft --title "feat(subos): subos-as-xpkg system (M1-M5)" \
  --body "$(cat <<'EOF'
## Summary

Implements subos-as-xpkg system per design `.agents/docs/subos-as-xpkg-design-2026-05-16.md` rev4. Subos environments can now be distributed as standard xpkg packages with `type = "subos"`, forked 0s via `--from`, executed non-interactively via `--cmd`, and high-frequency sandbox exec accelerated by auto-keeper (Linux).

## Milestones included

- **M1**: `type = "subos"` installer dispatch + default hooks (install/config/uninstall)
- **M2**: `xlings subos new --from <local-subos | subos:pkg@ver>` with auto-install
- **M3**: `xlings subos use <name> --cmd "<string>"` non-interactive exec (POSIX + Windows)
- **M4**: Auto-keeper (Linux, storage=image|tmpfs + sandbox; TTL=5min idle)
- **M5**: `--no-keep` / `--ttl <sec>` / `--keep` flag overrides + `subos stop`

## Implementation surface

- Upstream `mcpplibs/libxpkg`: +1 `PackageType::Subos` enum value
- xlings:
  - `src/core/xim/libxpkg/types/subos.cppm` — new (~80 lines)
  - `src/core/subos/keeper.cppm` — new (~250 lines, Linux only)
  - `src/core/subos.cppm` — extended (+~200 lines for new_from, use_spawn_shell --cmd, keeper integration)
  - `src/core/xim/installer.cppm` — +dispatch branches
  - `src/cli.cppm` — argparse for new flags
- `tests/e2e/subos_xpkg_*.sh` — 4 new e2e tests

## Test plan

- [x] `subos_xpkg_install_test.sh` — type=subos install path
- [x] `subos_xpkg_use_cmd_test.sh` — --cmd in shell + sandbox modes
- [x] `subos_xpkg_fork_test.sh` — local fork + pkg-spec auto-install fork
- [x] `subos_xpkg_keeper_test.sh` (Linux) — auto-keeper, stale PID, --no-keep, --ttl
- [ ] Cross-platform CI (Linux + macOS + Windows) green

## Out of scope (deferred to Future, see design §11)

- One-line throwaway `subos use subos:xxx --sandbox --cmd ...`
- Binary cache for fast first-fork
- Overlayfs / COW layered subos
- Subos pkg with embedded user data
EOF
)"
```

- [ ] **Step 6: Verify PR checks pass**

```bash
gh pr checks
```

If any fail, fix and re-push (do NOT bypass).

---

## Self-Review Checklist

- [x] Spec coverage: Phase 0 + M1-M5 → 11 tasks → each design decision (D1-D9, E1-E5) covered
- [x] No placeholders: all code blocks contain actual code or clear "explore X first" + "implement following pattern Y"
- [x] Type consistency: `new_from()` signature consistent across cli.cppm dispatch and subos.cppm export; `keeper::is_alive/spawn_keeper/stop_keeper/touch_activity` consistent
- [x] Parallelization marked: Tasks 2-3 (Track A, M1) ∥ Tasks 4-5 (Track B, M3); rest sequential
- [x] Test code present in every task with assertions and expected behavior
- [x] Build commands and expected output included

**Known caveats:**
- Task 4 Step 6 (use_sandbox_mode_ integration with cmd) requires reading existing bwrap argv construction — engineer must adapt to current shape
- Task 8 Step 5 (keeper integration with use_sandbox_mode_) requires careful fork point selection (keeper must fork from a process IN the mount namespace, after bwrap setup)
- Cross-platform `cp -ac` for macOS clonefile assumes recent macOS; verify on target CI runners
