# T25 — subos create 改用 init + 从 config 复制 xvm

> **Wave**: 依赖 T24
> **预估改动**: ~40 行 C++
> **设计文档**: [../shim-unified-design.md](../shim-unified-design.md)

---

## 1. 任务概述

修改 `core/subos.cppm` 的 `create` 函数：删除内联的 shim 复制循环和手写 xvm yaml；改为从 `homeDir/config/xvm/` 复制到 `dir/xvm/`；调用 `init::ensure_subos_shims(dir/bin, p.subosDir/bin/xvm-shim, p.homeDir)`。

---

## 2. 依赖

| 依赖 | 原因 |
|------|------|
| T24 | 需要 init::ensure_subos_shims；需要 config/xvm 模板存在 |

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/subos.cppm` | 修改 create 函数 |

---

## 4. 实施步骤

### 4.1 删除内联逻辑

- 删除 for 循环复制 xvm-shim 到 dir/bin 的代码
- 删除 platform::write_string_to_file 写入 versions.xvm.yaml 和 .workspace.xvm.yaml 的代码

### 4.2 从 config 复制 xvm

```cpp
auto configXvm = p.homeDir / "config" / "xvm";
if (fs::exists(configXvm)) {
    for (auto& entry : fs::directory_iterator(configXvm)) {
        if (entry.is_regular_file()) {
            fs::copy_file(entry.path(), dir / "xvm" / entry.path().filename(),
                fs::copy_options::overwrite_existing);
        }
    }
}
```

注意：config/xvm 可能尚不存在（开发环境），需做存在性检查；若不存在可保留原 minimal bootstrap 逻辑作为 fallback，或要求 T24 的 config 必须随项目存在。

### 4.3 调用 ensure_subos_shims

```cpp
auto shimSrc = p.subosDir / "bin" / "xvm-shim";
if (!fs::exists(shimSrc)) shimSrc = p.subosDir / "bin" / "xvm-shim.exe";
if (fs::exists(shimSrc)) {
    init::ensure_subos_shims(dir / "bin", shimSrc, p.homeDir);
}
```

---

## 5. 验收

```bash
xlings subos new test
ls ~/.xlings/subos/test/bin/   # 应有 7 或 8 个 shim
ls ~/.xlings/subos/test/xvm/  # 应有 versions.xvm.yaml、.workspace.xvm.yaml
```
