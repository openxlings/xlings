# xpkg Marker 自动注册设计文档

> 问题分析 + 方案 A（compat 兼容性兜底）+ 方案 D（规范检测）

---

## 1. 问题描述

当一个 xpkg 代表的是一个库或工具集（如 SDK），其 `package.name` 本身并非可执行文件。
xpkg 作者在 config hook 中可能只注册了子工具（如 `tool-a`, `tool-b`），但忘记注册
`package.name` 自身。或者包根本没有 config hook。

### 1.1 当前检测机制（三层）

| 层级 | 位置 | 检测方式 | 依赖 xvm:add() |
|------|------|----------|----------------|
| Tier 1 | `catalog.cppm:306-312` | install 目录存在且非空 | 否 |
| Tier 2 | `installer.cppm:1235-1241` | `installed` hook 自定义检测 | 否 |
| Tier 3 | `installer.cppm:1253-1276` | XVM VersionDB + payload 校验 | **是** |

### 1.2 未注册时的具体影响

| 功能 | 行为 | 根因 |
|------|------|------|
| `xlings install`（重复安装检测） | ✅ 正常（Tier 1 目录检查） | `catalog.cppm:306` |
| `xlings list` | ❌ **不显示**（默认 subos 模式） | `commands.cppm:643` 要求 `workspace_installed` 有记录 |
| `xlings list --all` | ✅ 正常 | 只检查 `match.installed`（目录存在） |
| `xlings uninstall` | ❌ 不完整 | `detach_current_subos_` 操作 `workspace_installed`，无记录 |
| `xlings use pkg@ver` 级联切换 | ❌ 不可用 | binding tree 无 root 节点 |
| subos 间隔离追踪 | ❌ 失效 | `workspace_installed[pkg]` 为空 |

### 1.3 关键代码路径

```
installer.cppm:1443-1473 — config 阶段分支：

  Build deps       → skip config entirely
  Script (no hook) → script::default_config()
  Subos (no hook)  → subos::default_config()
  Normal type      → run_config_hook_()
                       ├─ line 757: no config hook → return true (不调用 process_xvm_operations_)
                       └─ line 767: process_xvm_operations_(node, dataDir, executor, useAfterInstall)
                                      └─ 遍历 xvm_ops，仅处理 hook 中显式 xvm:add() 的条目
```

**无论哪条路径，如果没有 `xvm:add(node.name, ...)`，`workspace_installed` 中就不会有 `node.name` 的记录。**

---

## 2. 方案 A：compat 兼容性自动补注册

### 2.1 定位

放入 **compat 模块**，作为一键兼容性处理。遵循 `xself/compat.cppm` 的既有模式：
带版本号的命名空间，在未来版本中可一次性移除。

### 2.2 设计

在 config 阶段所有路径执行完毕后（`installer.cppm:1473` 后、snapshot 保存前），
检查 `workspace_installed` 中是否已有 `node.name` 的记录。若没有，自动注入一条
`type = "marker"` 的 VersionDB 条目 + workspace_installed 记录。

### 2.3 marker 类型的语义

- **不创建 PATH shim** — `process_xvm_operations_` 中 `line 600: if (type == "program")` 天然排除
- **不安装 lib 符号链接** — `line 656: if (type == "lib")` 分支也不匹配
- **作为 binding root** — 其他子工具通过 `binding="pkg-name@version"` 绑定到 marker，
  `xlings use pkg-name@2.0` 即可级联切换所有绑定的工具（`commands.cppm:215-231` binding tree 遍历）
- **参与 list/uninstall/subos 追踪** — `workspace_installed` 有记录，所有功能正常

### 2.4 实现位置

```
src/core/xim/installer.cppm

新增 compat 函数（建议放在 detail_ namespace 中）:
```

```cpp
// COMPAT(0.4.37 → drop in 0.6.0): auto-register marker for packages
// whose config hook didn't register package.name in xvm.
// Once all existing xpkgs are updated to explicitly register their
// binding root, this compat shim can be removed.
namespace compat_marker_ {

void ensure_binding_root_registered_(
    const PlanNode& node,
    const std::filesystem::path& dataDir)
{
    if (node.kind == DepKind::Build) return;

    // Compute version_ns (same logic as process_xvm_operations_)
    std::string version_ns;
    {
        auto& globalRepos = Config::global_index_repos();
        bool isPrimary = !globalRepos.empty()
            && node.namespaceName == globalRepos[0].name;
        if (!isPrimary && !node.namespaceName.empty())
            version_ns = node.namespaceName;
    }

    auto ver_key = xvm::make_ns_version(version_ns, node.version);
    const auto& wsi = Config::workspace_installed();

    // Check: is node.name already tracked in current subos?
    if (auto it = wsi.find(node.name); it != wsi.end()) {
        for (auto& v : it->second) {
            if (v == ver_key || xvm::strip_namespace(v) == node.version)
                return;  // already registered — nothing to do
        }
    }

    // Also check VersionDB — another subos may have registered it
    // but current subos hasn't opted in yet. In that case we still
    // need the workspace_installed entry for this subos.
    auto db = Config::versions();
    auto resolved = xvm::match_version(db, node.name, node.version);

    if (resolved.empty()) {
        // Not in VersionDB at all — inject marker entry
        std::string path = ((node.storeRoot.empty()
            ? (dataDir / "xpkgs") : node.storeRoot)
            / detail_::effective_store_name_(node)
            / node.version).string();

        xvm::add_version(Config::versions_mut(),
                         node.name, node.version, path,
                         "marker",  // type
                         "",        // filename
                         "",        // alias
                         version_ns,
                         "");       // binding
        Config::save_versions();
    }

    // Ensure current subos tracks this version
    Config::workspace_installed_mut()[node.name].push_back(ver_key);
    Config::save_workspace();

    log::debug("[{}] COMPAT: auto-registered as marker (binding root), ver={}",
               node.name, ver_key);
}

} // namespace compat_marker_
```

### 2.5 调用位置

```cpp
// installer.cppm — 安装主循环中，config 阶段之后（约 line 1473 后）
// 在 snapshot 保存之前插入：

            // COMPAT(0.4.37 → drop in 0.6.0): ensure package.name is registered
            // as a marker binding root if config hook didn't do it explicitly.
            detail_::compat_marker_::ensure_binding_root_registered_(node, dataDir);

            if (auto snapshot = detail_::save_xpkg_snapshot_(...)) {
                // ...existing code...
```

### 2.6 移除条件

当所有官方/社区 xpkg 都已更新为显式注册 binding root 后，
按 `compat.cppm` 的既有模式删除 `compat_marker_` namespace 即可：

1. 删除 `namespace compat_marker_ { ... }` 代码块
2. 删除调用点的 `ensure_binding_root_registered_()` 和 COMPAT 注释
3. 编译 — 所有引用自动报错，无需 grep

### 2.7 version_ns 重复计算优化

当前 `version_ns` 在 `process_xvm_operations_` 内部计算（line 562-571），
compat 函数需要重复同样的逻辑。建议将 `version_ns` 的计算提取为独立函数：

```cpp
// detail_ namespace
std::string compute_version_ns_(const PlanNode& node) {
    auto& globalRepos = Config::global_index_repos();
    bool isPrimary = !globalRepos.empty()
        && node.namespaceName == globalRepos[0].name;
    return (!isPrimary && !node.namespaceName.empty())
        ? node.namespaceName : std::string{};
}
```

然后 `process_xvm_operations_` 和 `ensure_binding_root_registered_` 都调用它。

---

## 3. 方案 D：xpkg 规范检测（Lint）

### 3.1 定位

作为 xpkg 质量检测工具，在安装时 warning + 独立 lint 命令。
长期保留，属于开发者工具链的一部分。

### 3.2 检测规则

#### 规则 D1：package.name 未注册（严重度：Warning）

**触发条件**：config hook 执行后，`xvm_operations()` 中没有任何 `op.name == node.name && op.op == "add"` 的条目。

**输出**：
```
⚠ [xpkg-lint] 'my-sdk@1.0': config hook did not register package.name via xvm:add()
  ├─ impact: package won't appear in `xlings list`, subos tracking broken
  ├─ fix: add xvm:add("my-sdk", version, install_dir, "marker") in config hook
  └─ note: auto-registered as marker by compat layer (temporary)
```

**实现位置**：`process_xvm_operations_` 末尾（line 748 前）

```cpp
// Lint D1: warn if package.name was not registered
{
    bool root_registered = false;
    for (auto& op : xvm_ops) {
        if (op.op == "add" && op.name == node.name) {
            root_registered = true;
            break;
        }
    }
    if (!root_registered) {
        log::warn("[xpkg-lint] '{}@{}': config hook did not register "
                  "package.name via xvm:add() — package won't appear in "
                  "`xlings list` without compat layer",
                  node.name, node.version);
    }
}
```

#### 规则 D2：子工具未绑定到 package.name（严重度：Info）

**触发条件**：config hook 注册了多个 `type="program"` 的条目，但没有任何条目的
`binding` 指向 `package.name`。

**输出**：
```
ℹ [xpkg-lint] 'my-sdk@1.0': 3 tools registered but none bound to package.name
  ├─ registered: tool-a, tool-b, tool-c
  ├─ impact: `xlings use my-sdk@2.0` won't cascade-switch these tools
  └─ fix: add binding="my-sdk@1.0" to each xvm:add() call
```

**实现**：

```cpp
// Lint D2: warn about unbound tools
{
    bool has_root = false;
    std::vector<std::string> unbound_tools;
    for (auto& op : xvm_ops) {
        if (op.op != "add") continue;
        if (op.name == node.name) { has_root = true; continue; }
        std::string type = op.type.empty() ? "program" : op.type;
        if (type == "program" && op.binding.empty()) {
            unbound_tools.push_back(op.name);
        }
    }
    if (has_root && !unbound_tools.empty()) {
        std::string names;
        for (size_t i = 0; i < unbound_tools.size(); ++i) {
            if (i > 0) names += ", ";
            names += unbound_tools[i];
        }
        log::info("[xpkg-lint] '{}': {} tool(s) not bound to package.name: {} — "
                  "`xlings use {}@<ver>` won't cascade-switch them",
                  node.name, unbound_tools.size(), names, node.name);
    }
}
```

#### 规则 D3：无 config hook 且非 Script/Subos 类型（严重度：Info）

**触发条件**：普通包没有 config hook，也没有 installed hook。

**输出**：
```
ℹ [xpkg-lint] 'my-lib@1.0': no config hook defined
  └─ hint: add a config hook with xvm:add() to enable version management
```

**实现位置**：`installer.cppm` config 阶段分支中（line 1466 前），
当走到 `run_config_hook_` 且 `executor.has_hook(Config) == false` 时触发。

实际上 `run_config_hook_` 内部 line 757 已经 early-return，可以在调用前检查：

```cpp
} else {
    // Lint D3
    if (!executor.has_hook(mcpplibs::xpkg::HookType::Config)) {
        log::info("[xpkg-lint] '{}@{}': no config hook — version management "
                  "features (list, use, uninstall) rely on compat auto-marker",
                  node.name, node.version);
    }
    if (!detail_::run_config_hook_(node, dataDir, executor, ctx,
                                    onStatus, useAfterInstall)) {
        // ...existing error handling...
    }
}
```

### 3.3 独立 lint 子命令（可选，未来增强）

```
xlings lint <pkg>.lua
```

在不执行安装的情况下，对 xpkg 文件进行静态分析：
- 沙箱执行 config hook，收集 xvm_operations
- 运行 D1/D2/D3 规则
- 输出检测报告

这需要对 `PackageExecutor` 做 dry-run 模式支持，属于较大改动，建议作为后续迭代。

### 3.4 完整规则汇总

| 规则 | 严重度 | 触发条件 | 实现位置 |
|------|--------|----------|----------|
| D1 | Warning | config hook 未注册 package.name | `process_xvm_operations_` 末尾 |
| D2 | Info | 子工具未绑定到 package.name | `process_xvm_operations_` 末尾 |
| D3 | Info | 无 config hook（普通包） | `installer.cppm` config 分支 |

---

## 4. 整体实施路径

```
Phase 1 (当前版本, e.g. 0.4.37):
  ├─ 实现 A: compat_marker_::ensure_binding_root_registered_()
  ├─ 实现 D1/D3: 安装时 warning 输出
  └─ 提取 compute_version_ns_() 公共函数

Phase 2 (下一版本):
  ├─ 实现 D2: 子工具绑定检测
  ├─ 更新官方 xpkg 仓库中的包，显式注册 marker + binding
  └─ 文档：xpkg 编写规范中增加 binding root 最佳实践

Phase 3 (0.6.0+):
  ├─ 确认社区 xpkg 已更新
  ├─ D1 严重度从 Warning 提升为 Error（可选）
  └─ 移除 compat_marker_ 兼容层
```

---

## 5. marker 作为 binding root 的完整示例

### xpkg config hook 最佳实践

```lua
function config(ctx)
    local ver = ctx.version
    local dir = ctx.install_dir

    -- 1. 注册 binding root（marker，不创建 shim）
    xvm:add("my-sdk", ver, dir, "marker")

    -- 2. 注册子工具，绑定到 root
    xvm:add("tool-a", ver, dir .. "/bin", "program", {
        binding = "my-sdk@" .. ver
    })
    xvm:add("tool-b", ver, dir .. "/bin", "program", {
        binding = "my-sdk@" .. ver
    })

    -- 3. 注册库（可选）
    xvm:add("libsdk", ver, dir .. "/lib", "lib", {
        filename = "libsdk.so",
        binding = "my-sdk@" .. ver
    })
end
```

### 版本切换行为

```bash
$ xlings use my-sdk@2.0
# binding tree 遍历 (commands.cppm:215-231):
#   my-sdk@2.0 (marker, no shim)
#   ├─ tool-a@2.0 (program, shim updated)
#   ├─ tool-b@2.0 (program, shim updated)
#   └─ libsdk@2.0 (lib, symlink updated)
```

---

## 6. 影响范围

| 组件 | 变更类型 | 文件 |
|------|----------|------|
| installer | 新增 compat 函数 + lint warn | `src/core/xim/installer.cppm` |
| xvm/db | 无变更（"marker" 只是 type 字符串） | — |
| xvm/commands | 无变更（binding tree 遍历不区分 type） | — |
| xvm/shim | 无变更（`type != "program"` 不创建 shim） | — |
| cmd_list | 可选：marker 类型显示 `[lib/sdk]` 标签 | `src/core/xim/commands.cppm` |
