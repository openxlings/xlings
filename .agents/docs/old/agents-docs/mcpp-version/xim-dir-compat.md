# xim 目录兼容方案（临时）

> **状态**: 已实现（临时兼容方案）
> **涉及语言**: C++ / Lua / Shell / PowerShell
> **涉及文件**: `core/cmdprocessor.cppm`, `core/xim/xmake.lua`, `tools/linux_release.sh`, `tools/windows_release.ps1`

---

## 1. 问题背景

xim（包管理器）的 Lua 源码在不同场景下存放位置不同：

| 场景 | xim 目录位置 | xmake.lua 引用路径 |
|------|------------|-------------------|
| 源码树（开发） | `core/xim/` | `path.join(os.projectdir(), "core", "xim")` |
| release 包（安装） | `$XLINGS_HOME/xim/` | `path.join(os.projectdir(), "xim")` |
| 多版本包（xpkgs） | `$XLINGS_DATA/xpkgs/xlings/<ver>/xim/` | `path.join(os.projectdir(), "xim")` |

当通过 `xlings install xlings` 安装多个版本时，xvm 管理版本切换，每个版本的 xlings 二进制位于：

```
$XLINGS_DATA/xpkgs/xlings/<ver>/bin/xlings
```

每个版本都携带自己的 xim 代码：

```
$XLINGS_DATA/xpkgs/xlings/<ver>/xim/
```

**但 `xim_exec()` 始终使用 `$XLINGS_HOME` 作为 xmake 项目目录**，导致所有版本都加载全局的 `$XLINGS_HOME/xim/` 代码，而非各自版本的 xim 代码。

---

## 2. 解决方案

### 2.1 C++ 侧：按可执行文件位置解析 xim 项目目录

在 `core/cmdprocessor.cppm` 中新增 `find_xim_project_dir()`，优先检查可执行文件自身位置附近的 xim 目录：

```cpp
std::filesystem::path find_xim_project_dir() {
    namespace fs = std::filesystem;
    auto exePath = platform::get_executable_path();
    if (!exePath.empty()) {
        auto candidate = exePath.parent_path().parent_path();
        if (fs::exists(candidate / "xim") && fs::exists(candidate / "xmake.lua")) {
            return candidate;
        }
    }
    return Config::paths().homeDir;
}
```

解析优先级：

1. `exe/../xim/` + `exe/../xmake.lua` 同时存在 → 使用 `exe/..`（版本特定）
2. 否则回退到 `$XLINGS_HOME`（全局/开发场景）

`xim_exec()` 和 `script` 命令均改为使用此函数：

```cpp
int xim_exec(...) {
    auto projectDir = find_xim_project_dir();
    std::string cmd = "xmake xim -P \"" + projectDir.string() + "\"";
    ...
}
```

### 2.2 Lua 侧：xmake.lua 自动检测目录布局

`core/xim/xmake.lua` 不再硬编码 `core/xim/` 路径，改为自动检测：

```lua
local xim_root
if os.isdir(path.join(os.projectdir(), "core", "xim")) then
    xim_root = path.join(os.projectdir(), "core", "xim")
else
    xim_root = path.join(os.projectdir(), "xim")
end
add_moduledirs(xim_root)
```

这使同一份 `xmake.lua` 在两种布局下都能工作。

### 2.3 release 脚本简化

`tools/linux_release.sh` 和 `tools/windows_release.ps1` 中原先内联生成 `xmake.lua` 的 40+ 行代码，替换为直接复制 `core/xim/xmake.lua`：

```bash
# linux_release.sh
cp core/xim/xmake.lua "$OUT_DIR/xmake.lua"
```

```powershell
# windows_release.ps1
Copy-Item "core\xim\xmake.lua" "$OUT_DIR\xmake.lua"
```

消除 release 脚本中 xmake.lua 的重复定义，确保源码和发布包始终一致。

---

## 3. 各场景解析路径

### 3.1 全局安装（正常使用）

```
二进制:   $XLINGS_HOME/bin/xlings
xim:     $XLINGS_HOME/xim/
xmake:   $XLINGS_HOME/xmake.lua

find_xim_project_dir():
  candidate = $XLINGS_HOME/bin/../ = $XLINGS_HOME
  $XLINGS_HOME/xim/ ✓  $XLINGS_HOME/xmake.lua ✓
  → 返回 $XLINGS_HOME
```

### 3.2 多版本（xlings install xlings）

```
二进制:   $XLINGS_DATA/xpkgs/xlings/0.4.0/bin/xlings
xim:     $XLINGS_DATA/xpkgs/xlings/0.4.0/xim/
xmake:   $XLINGS_DATA/xpkgs/xlings/0.4.0/xmake.lua

find_xim_project_dir():
  candidate = .../xpkgs/xlings/0.4.0/bin/../ = .../xpkgs/xlings/0.4.0/
  .../xpkgs/xlings/0.4.0/xim/ ✓  .../xpkgs/xlings/0.4.0/xmake.lua ✓
  → 返回 .../xpkgs/xlings/0.4.0/  （版本特定 xim）
```

### 3.3 开发构建

```
二进制:   build/linux/x86_64/release/xlings
无 xim:  build/linux/x86_64/ 下无 xim/ 和 xmake.lua

find_xim_project_dir():
  candidate = build/linux/x86_64/
  xim/ ✗
  → 回退到 $XLINGS_HOME（使用全局安装的 xim）
```

---

## 4. 临时性说明

此方案是多版本 xlings 共存的临时兼容措施。长期来看，当 xim（Lua）迁移到 C++ 后，xim 代码将编译进 xlings 二进制本身，不再需要外部 xim 目录和 xmake 项目文件，此兼容逻辑可移除。

---

## 5. 前置条件

xlings 安装为包时，其 xpkg 包定义需确保以下文件打包到安装目录：

```
$XLINGS_DATA/xpkgs/xlings/<ver>/
├── bin/xlings        ← 版本特定二进制
├── xim/              ← 版本特定 xim Lua 代码
├── xmake.lua         ← xim task 定义（自动检测布局）
└── ...
```

`xmake.lua` 和 `xim/` 目录是 `find_xim_project_dir()` 的检测条件，缺少任一将回退到全局 `$XLINGS_HOME`。
