# `xlings remove <pkg>` 误删整个版本数据库

**状态：** 待修复
**组件：** `src/core/xim/installer.cppm` · `xim-pkgindex` 里各 package 的 uninstall hook

## 现象

在同一个包下装了多个版本后，执行不带版本号的 `remove`，仅物理删除当前激活版本的一个目录，但把该包在版本数据库里的所有条目都抹掉了 —— 剩下的版本变成磁盘上的孤儿，`use` 不到、`remove` 不到、`list` 不到。

复现（用本仓库构建的 xlings + 独立 XLINGS_HOME，避免干扰系统）：

```bash
XHOME=$PWD/.xlings-home-dev
XBIN=$PWD/build/linux/x86_64/release/xlings
env -i HOME=$HOME PATH=/usr/bin:/bin XLINGS_HOME=$XHOME $XBIN self init
env -i HOME=$HOME PATH=/usr/bin:/bin XLINGS_HOME=$XHOME $XBIN install node@20 -y
env -i HOME=$HOME PATH=/usr/bin:/bin XLINGS_HOME=$XHOME $XBIN install node@22 -y
env -i HOME=$HOME PATH=/usr/bin:/bin XLINGS_HOME=$XHOME $XBIN install node@24 -y
# active = 24.4.1
env -i HOME=$HOME PATH=/usr/bin:/bin XLINGS_HOME=$XHOME $XBIN remove node -y
#   ✓ node removed
env -i HOME=$HOME PATH=/usr/bin:/bin XLINGS_HOME=$XHOME $XBIN use node
#   [error] 'node' not found in version database
ls $XHOME/data/xpkgs/xim-x-node/
#   20.19.0  22.17.1        ← 文件还在，但不可管理
```

## 根因

两处配合导致的：

### 1. `node.lua` 的 `uninstall` hook 不带版本

`xim-pkgindex/pkgs/n/node.lua:110-117`：

```lua
function uninstall()
    xvm.remove("node")                              -- ← 不传 version
    xvm.remove("nodejs")                            -- ← 不传 version
    xvm.remove("npm", "node-" .. pkginfo.version())
    xvm.remove("npx", "node-" .. pkginfo.version())
    return true
end
```

`xvm.remove("node")` 产生的 xvm_operation 里 `op.version == ""`。

### 2. installer 在 `op.version` 为空时整包擦除

`src/core/xim/installer.cppm:1179-1183`：

```cpp
if (op.version.empty()) {
    Config::versions_mut().erase(op.name);           // ← 把整个 node 的所有版本条目清掉
} else {
    xvm::remove_version(Config::versions_mut(), op.name, op.version);
}
```

而此时 installer 自己在函数上方（行 1061–1075）早已把不带版本的 target 解析成了具体版本（`resolvedMatch->version`），也计算出了 `detachVersion`。这个权威版本信息没有被 xvm_ops 处理环节使用，反而去信任 hook 返回的可能缺失的 `op.version`，导致错杀。

## 期望行为（规格）

**命令形式：** `xlings remove <pkg>[@<version>]`，每次只删一个版本。

**分支 1：不带版本**
1. 解析出当前 active 版本 V。
2. 物理删除 V 的 payload 目录。
3. 从版本数据库中仅移除 V 这一条。
4. 如果还有其他版本：**自动切到剩余版本中 semver 最高的那个**（作为新 active）。
5. 如果没有其他版本了：从版本数据库中彻底移除该 pkg 条目，同时清空 workspace 绑定。

**分支 2：带版本 `<pkg>@<version>`**
1. 物理删除指定版本 V 的 payload 目录。
2. 从版本数据库中仅移除 V 这一条。
3. 若 V 恰好是当前 active：按分支 1 的 4/5 规则处理（有其他版本就切到最高，没有就整包移除）。
4. 若 V 不是 active：active 保持不变。

**核心不变式**

> 版本数据库中 `pkg` 条目存在 ⟺ `data/xpkgs/xim-x-<pkg>/` 下至少有一个对应的版本目录，且 workspace 里的 active 绑定（若存在）一定指向其中一个实际存在的版本。

## 修复方案

### A. 在 installer 层兜底（推荐，治本）

**文件：** `src/core/xim/installer.cppm`
**位置：** `Installer::uninstall(...)` 内，`xvm_ops` 循环体（约 1162–1187 行）

改动点：

1. 处理 `op.op == "remove"` 分支时，如果 `op.version.empty()`，用外层已解析出的 `resolvedMatch->version`（或 `detachVersion`）作为权威版本，而不是把整个 name 擦除。
2. 从数据库移除该条目后：
   - 检查该 pkg 在数据库里是否还有其他版本。
   - 如果还有 ≥ 1 个，且被删的这条**原本是 active**（通过 detach 前记录或直接比对 workspace 的旧值判断），按 **semver 降序**挑最高版本作为新 active，写回 workspace。
   - 如果已经 0 个，从 workspace 中移除该条目，再把 versions_mut 中的 name 整条删掉（此时才是真正的"整包干掉"）。

伪代码：

```cpp
// 在 uninstall() 进入 xvm_ops 循环前记录
std::string wasActive = detachVersion;  // detach_current_subos_ 之前是 active 的那个版本
                                        // （若 detachVersion 空则说明删的不是 active）

for (auto& op : xvm_ops) {
    if (op.op == "remove") {
        remove_shim_(op.name);
        Config::workspace_mut().erase(op.name);  // 保持现有行为，下方再决定是否恢复

        // —— 修复：用权威 version，禁止在 version 空时整包擦除 ——
        std::string vsnToRemove = !op.version.empty() ? op.version : detachVersion;
        if (vsnToRemove.empty()) {
            // 极端 fallback：完全没有版本信息，保留旧语义（整包清）
            Config::versions_mut().erase(op.name);
            continue;
        }
        xvm::remove_version(Config::versions_mut(), op.name, vsnToRemove);

        // —— 自动切换到最高剩余版本 ——
        auto it = Config::versions_mut().find(op.name);
        if (it != Config::versions_mut().end() && !it->second.versions.empty()) {
            if (op.name == /* 被删的是 active 对应的 target */ detachTarget
                && vsnToRemove == wasActive) {
                auto next = pick_highest_semver(it->second.versions);  // 新增
                Config::workspace_mut()[op.name] = next;
            }
        }
    } else if (op.op == "remove_headers") {
        xvm::remove_headers(op.includedir, sysroot_include);
    }
}
```

新增辅助函数（放入 `src/core/xvm/db.cppm`）：

```cpp
// 从版本集合里按 semver 降序挑最高版本（已有 sort_desc 逻辑可复用）
std::string pick_highest_semver(const std::map<std::string, VersionData>& versions);
```

### B. 顺便把 xpkg hook 规范化（可选，治标）

推动 `xim-pkgindex` 里所有 `uninstall()` hook 都明确传 version：

```lua
xvm.remove("node", pkginfo.version())
```

但这是 optional；A 方案落地后即使 hook 不传 version，installer 也不会误删。A + B 都做，才算双保险。

## 影响面

- 任意被装了多版本的 xpkg（node / gcc / python / cmake / ...）
- 任何调用 `cmd_remove` 的上层（CLI、agent 自动化、测试套件）

## 测试计划

在 `tests/e2e/` 新增 `remove-multi-version.bats`（或 lua 等效）：

| # | 场景 | 操作 | 断言 |
|---|------|------|------|
| 1 | 不带版本，删 active，有剩余 | 装 20/22/24（active=24），`remove node` | payload 只少 24；db 里还有 20/22；active 变为 22（最高剩余，semver 降序） |
| 2 | 不带版本，只剩一个版本 | 装 22，`remove node` | payload 清空；db 无 node；workspace 无 node |
| 3 | 带版本，删非 active | 装 20/22/24（active=24），`remove node@20` | payload 少 20；db 里还有 22/24；active 仍为 24 |
| 4 | 带版本，删 active，有剩余 | 装 20/22/24（active=24），`remove node@24` | payload 少 24；db 里还有 20/22；active 自动切到 22 |
| 5 | 带版本，删不存在的版本 | `remove node@99` | 非 0 退出；payload 和 db 原样不动 |

回归：

```bash
xlings install foo@A && xlings install foo@B
xlings use foo A
xlings remove foo          # B 是 active → B 被删？不对，A 是 active。
                           #  预期：删 A（active），若 B 存在则 active 切 B
xlings list foo            # 应看到 B
xlings use foo             # 应列出 B，且 B 是 active
```

## 参考定位

| 文件 | 行 | 说明 |
|------|----|----|
| `src/core/xim/installer.cppm` | 1051–1076 | `uninstall()` 头部：已把不带版本的 target 解析为 `resolvedMatch->version` + `detachVersion` |
| `src/core/xim/installer.cppm` | 1108 | `detach_current_subos_()`：擦 active 绑定（但不动 versions_mut） |
| `src/core/xim/installer.cppm` | 1179–1183 | **bug 所在**：`op.version` 空 ⇒ 整包 erase |
| `src/core/xim/installer.cppm` | 339–364 | `remove_target_shims_()`：按 version 清 shim（已正确） |
| `src/core/xvm/db.cppm` | 69–92 | `remove_version()`：按 version 精确删（已正确，未被走到） |
| `src/core/xvm/db.cppm` | 122–134 | `sort_desc`：已有的 semver 降序排序，`pick_highest_semver` 可复用 |
| `xim-pkgindex/pkgs/n/node.lua` | 110–117 | 触发路径：`xvm.remove("node")` 不传 version |
