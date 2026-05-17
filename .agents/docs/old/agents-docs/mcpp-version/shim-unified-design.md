# 短命令与 xvm 配置全平台统一设计

> **状态**: 设计完成，待实施
> **关联任务**: [T24](tasks/T24-shim-init-module.md)、[T25](tasks/T25-subos-create-unified.md)、[T26](tasks/T26-release-scripts-unified.md)

---

## 1. 背景与问题

### 1.1 短命令列表分散且不一致

| 位置 | 短命令列表 | 备注 |
|------|------------|------|
| core/subos.cppm | xvm-shim, xlings, xvm, xim, xinstall, xsubos, xself | 7 个，无 xmake |
| tools/linux_release.sh | xlings, xvm, xvm-shim, **xmake**, xim, xinstall, xsubos, xself | 8 个 |
| tools/macos_release.sh | xlings, xvm, xvm-shim | **仅 3 个** |
| tools/windows_release.ps1 | xlings, xvm, xvm-shim, xim, xinstall, xsubos, xself | 7 个，无 xmake |

### 1.2 xvm 配置各脚本内联

versions.xvm.yaml、.workspace.xvm.yaml 在 linux_release.sh、macos_release.sh、windows_release.ps1、subos.cppm 中各自手写，无单一数据源。

### 1.3 xmake 打包不统一

仅 Linux 打包 bin/xmake，macOS/Windows 不打包，导致短命令与离线体验不一致。

---

## 2. 设计目标

- **短命令单一数据源**：列表定义在 core/self/init.cppm
- **xvm 配置单一数据源**：模板在 config/xvm/
- **统一入口**：xlings self init 负责创建 subos/default 的 shim
- **三平台一致**：Linux、macOS、Windows 均打包 xmake，均含 8 个短命令 shim
- **打包时复制两次**：config/xvm 复制到 config/xvm 与 subos/default/xvm，无需两套模板

---

## 3. 方案概览

### 3.1 短命令生成

- 新建 `core/self/init.cppm`，导出 `ensure_subos_shims(target_bin_dir, shim_src, pkg_root)`
- 短命令名：SHIM_NAMES_BASE（7 个）+ SHIM_NAMES_OPTIONAL（xmake，仅当 bin/xmake 存在时创建）
- `xlings self init`、`subos new`、发布脚本均调用该逻辑

### 3.2 xvm 配置

```
config/xvm/                    # 默认模板（放配置里，仅此一套）
├── versions.xvm.yaml
└── .workspace.xvm.yaml

# 打包时复制两次
cp -r config/xvm/* "$OUT_DIR/config/xvm/"
cp -r config/xvm/* "$OUT_DIR/subos/default/xvm/"

# subos new 时：从 homeDir/config/xvm/ 复制到 subos/<name>/xvm/
```

### 3.3 xmake 三平台统一

| 平台 | xmake bundle URL |
|------|------------------|
| Linux x86_64 | xmake-bundle-v3.0.7.linux.x86_64 |
| macOS arm64 | xmake-bundle-v3.0.7.macos.arm64 |
| Windows x64 | xmake-bundle-v3.0.7.win64.exe |

---

## 4. 实施任务

| ID | 任务 | 预估改动 |
|----|------|----------|
| [T24](tasks/T24-shim-init-module.md) | init 模块 + config/xvm 模板 + xself cmd_init | ~120 行 |
| [T25](tasks/T25-subos-create-unified.md) | subos create 改用 init + 从 config 复制 xvm | ~40 行 |
| [T26](tasks/T26-release-scripts-unified.md) | 发布脚本三平台统一 | ~80 行 |

---

## 5. 验证清单

- `xlings self init` 在空目录下可生成 7 个基础 shim（无 bin/xmake 时）
- 三平台发布包运行 init 后，subos/default/bin 均含 8 个 shim（含 xmake）
- `xlings subos new test` 创建的 test/bin 含 7 或 8 个 shim（取决于 bin/xmake）
- 各平台 `xim -h`、`xsubos list`、`xself --help` 可用
- subos list 的 toolCount 正确排除基础 shim
