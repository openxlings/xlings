# T24 — init 模块 + config/xvm 模板 + xself cmd_init

> **Wave**: 独立
> **预估改动**: ~120 行 C++ + 2 个 YAML 文件
> **设计文档**: [../shim-unified-design.md](../shim-unified-design.md)

---

## 1. 任务概述

新建 `core/self/init.cppm` 模块，统一短命令 shim 生成逻辑；新建 `config/xvm/` 默认模板；修改 `xself.cppm` 的 `cmd_init` 调用 `ensure_subos_shims`；修改 `subos.cppm` 的 `list` 使用 init 的 SHIM 名单做 toolCount 排除。

---

## 2. 涉及文件

| 文件 | 操作 |
|------|------|
| `config/xvm/versions.xvm.yaml` | 新建 |
| `config/xvm/.workspace.xvm.yaml` | 新建 |
| `core/self/init.cppm` | 新建（xlings.xself:init 分区） |
| `core/xself.cppm` | 修改：export import :init；cmd_init 调用 ensure_subos_shims |
| `core/subos.cppm` | 修改：list 的 toolCount 使用 init 的 SHIM 名单 |

---

## 3. 实施步骤

### 3.1 新建 config/xvm/ 模板

**config/xvm/versions.xvm.yaml**:

```yaml
---
xlings:
  bootstrap:
    path: "../../bin"
xvm:
  bootstrap:
    path: "../../bin"
xvm-shim:
  bootstrap:
    path: "../../bin"
xmake:
  bootstrap:
    path: "../../bin"
```

**config/xvm/.workspace.xvm.yaml**:

```yaml
---
xvm-wmetadata:
  name: global
  active: true
  inherit: true
versions:
  xlings: bootstrap
  xvm: bootstrap
  xvm-shim: bootstrap
  xmake: bootstrap
```

### 3.2 新建 core/self/init.cppm

作为 `xlings.xself:init` 分区，导出：

- `SHIM_NAMES_BASE`：7 个基础 shim 名
- `SHIM_NAMES_OPTIONAL`：可选 shim（xmake）
- `ensure_subos_shims(target_bin_dir, shim_src, pkg_root)`：复制 shim，pkg_root 非空时检测可选 shim

逻辑：
1. 解析 shim_src 扩展名（"" 或 ".exe"）
2. 遍历 SHIM_NAMES_BASE 复制
3. 若 pkg_root 非空，遍历 SHIM_NAMES_OPTIONAL，仅当 pkg_root/bin/<name> 存在时复制
4. 调用 platform::make_files_executable

### 3.3 修改 xself.cppm

- 增加 `export import :init;`
- 在 `cmd_init` 中，创建目录和 subos/current 后，调用 `init::ensure_subos_shims(p.binDir, shim_src, p.homeDir)`
- shim_src：`homeDir/bin/xvm-shim` 或 `homeDir/bin/xvm-shim.exe`，不存在则跳过并打印提示

### 3.4 修改 subos.cppm list

- 导入 init 的 SHIM_NAMES（或提供 `is_builtin_shim(name)`）
- toolCount 排除逻辑改为：若 stem 在 SHIM_NAMES_BASE 或 SHIM_NAMES_OPTIONAL 中则排除

---

## 4. 验收

```bash
# 开发环境（无 bin/xvm-shim）init 不报错
xlings self init

# 发布包解压后（有 bin）
XLINGS_HOME=/path/to/package ./bin/xlings self init
ls subos/default/bin/  # 应有 8 个 shim（含 xmake）
```
