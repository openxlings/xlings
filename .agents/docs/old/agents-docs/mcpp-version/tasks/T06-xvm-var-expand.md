# T06 — xvm shims.rs: 新增 `${XLINGS_HOME}` 变量展开

> **Wave**: 1（无前置依赖，可立即执行）
> **预估改动**: ~15 行 Rust，1 个文件

---

## 1. 任务概述

在 `core/xvm/xvmlib/shims.rs` 的 `set_vdata()` 方法中，对 `path` 和 `envs` 的值做 `${XLINGS_HOME}` / `${XLINGS_DATA}` 变量替换，使 workspace.yaml 中的路径在 `XLINGS_HOME` 移动后仍然有效。这是 RPATH 方案的运行时层核心。

**设计背景**: 详见 [../rpath-and-os-vision.md §3](../rpath-and-os-vision.md)

---

## 2. 依赖前置

无（Wave 1，独立任务）

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/xvm/xvmlib/shims.rs` | 新增 `expand_xlings_vars()` 并在 `set_vdata()` 中调用 |

---

## 4. 实施步骤

### 4.1 新增辅助函数 `expand_xlings_vars()`

在 `shims.rs` 文件顶部的 `use` 声明之后，`Program` struct 定义之前，新增：

```rust
// 展开 ${XLINGS_HOME} 和 ${XLINGS_DATA} 变量占位符
// 无变量的字符串原样返回，向后兼容
fn expand_xlings_vars(value: &str) -> String {
    let home = std::env::var("XLINGS_HOME").unwrap_or_default();
    let data = std::env::var("XLINGS_DATA").unwrap_or_default();
    value
        .replace("${XLINGS_HOME}", &home)
        .replace("${XLINGS_DATA}", &data)
}
```

### 4.2 修改 `set_vdata()` 方法

找到 `set_vdata()` 方法（当前约第 160 行），将其修改为：

```rust
pub fn set_vdata(&mut self, vdata: &VData) {
    // path 也展开，支持 ${XLINGS_HOME}/xim/xpkgs/... 写法
    self.set_path(&expand_xlings_vars(&vdata.path));

    if let Some(envs) = &vdata.envs {
        self.add_envs(
            envs.iter()
                .map(|(k, v)| (k.as_str(), expand_xlings_vars(v)))  // ← 展开后再注入
                .collect::<Vec<_>>()
                .iter()
                .map(|(k, v)| (k.as_str(), v.as_str()))
                .collect::<Vec<_>>()
                .as_slice()
        );
    }

    if let Some(bindings) = &vdata.bindings {
        self.bindings = bindings.clone();
    }
    self.alias = vdata.alias.clone();
}
```

> **注意**: 当前 `set_vdata()` 通过 `add_envs()` 传入 `&[(str, str)]`，需注意 `expand_xlings_vars` 返回 `String`（owned），需先收集再取引用，如上所示。

### 4.3 构建验证

```bash
cd core/xvm
cargo build --release
# 期望: 无编译错误
```

---

## 5. 验收标准

### 5.1 变量替换功能测试

准备测试用 workspace.yaml：

```yaml
versions:
  gcc:
    path: "${XLINGS_HOME}/xim/xpkgs/fromsource-x-gcc/15.0"
    envs:
      LD_LIBRARY_PATH: "${XLINGS_HOME}/xim/xpkgs/fromsource-x-gmp/6.3.0/lib"
```

执行验证：

```bash
export XLINGS_HOME=/tmp/xlings-test
# 调用 xvm run gcc --version
# 期望: LD_LIBRARY_PATH 中包含 /tmp/xlings-test/xim/xpkgs/fromsource-x-gmp/6.3.0/lib
```

### 5.2 向后兼容验证

不含变量的现有 yaml 条目（如 `path: "/home/user/.xlings/xim/xpkgs/gcc/15.0"`）的行为不变。

### 5.3 移动 XLINGS_HOME 后的验证

```bash
mv ~/.xlings /tmp/xlings-moved
export XLINGS_HOME=/tmp/xlings-moved
# 安装了 gcc（workspace.yaml 用了 ${XLINGS_HOME} 写法）
gcc --version   # 期望: 正常执行，无 library not found 错误
```

### 5.4 回归测试

```bash
cd core/xvm
cargo test   # 现有测试全部通过
```

---

## 6. xpkg 脚本约定（配套规范）

此任务实现后，xpkg 的 `config()` 钩子应使用以下写法（跨机器可移植）：

```lua
function config()
    xvm.add("gcc", {
        path = "${XLINGS_HOME}/xim/xpkgs/fromsource-x-gcc/15.0",
        envs = {
            LD_LIBRARY_PATH = table.concat({
                "${XLINGS_HOME}/xim/xpkgs/fromsource-x-gmp/6.3.0/lib",
                "${XLINGS_HOME}/xim/xpkgs/fromsource-x-mpfr/4.2.0/lib",
                "${XLINGS_HOME}/xim/xpkgs/fromsource-x-mpc/1.3.1/lib",
            }, ":"),
        },
    })
end
```

现有使用绝对路径的 xpkg 脚本**无需立即修改**（向后兼容），可在后续维护中逐步迁移。
