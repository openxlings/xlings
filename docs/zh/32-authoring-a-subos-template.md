# 编写 SubOS 模板

[English](../32-authoring-a-subos-template.md) | **简体中文**

SubOS 可以作为包分发（`type = "subos"`）：安装它就是安装一个模板，`subos new --from` 几乎瞬间从它创建实例——
它声明的 payload 是共享的，不复制。

## 工作机制

```mermaid
sequenceDiagram
    participant A as author
    participant R as index
    participant U as user or agent
    participant S as instance
    A->>R: publishes a type="subos" package (.xlings.json + files)
    U->>U: xlings install subos:py-ds@1.0.0
    U->>S: xlings subos new exp --from subos:py-ds@1.0.0
    Note over S: the declaration and files are copied; payloads are shared
    U->>S: xlings subos exec exp -- python run.py
```

## 一个模板

```lua
package = {
    spec = "1",
    name = "py-ds",
    namespace = "subos",
    type = "subos",
    archs = {"x86_64", "aarch64"},
    xpm = {
        linux = {
            deps = { "python@3.12", "numpy@2.1" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {},
        },
    },
}
```

没有钩子时，内置处理器创建骨架并记录模板的声明；依赖成为实例的 workspace。模板可以携带文件
（payload 根目录放 `.xlings.json` 和其他文件），也可以定义 `install()`、`config()`、`uninstall()` 做更多事。

作为根的 SubOS，模板是一份 manifest（`subos_kind: "rootfs"`、`from`、`abi`、`packages`、`policy`、`boot`）；
官方的就是 Luban 的各个 edition——manifest 见 [Luban：edition](luban/02-editions.md)。

## 使用

```bash
xlings install subos:py-ds@1.0.0                   # the template itself
xlings subos new exp --from subos:py-ds@1.0.0      # an instance of it
xlings subos new exp2 --from exp                   # from an instance
xlings subos exec exp -- python -V
xlings subos remove exp
```

## 取舍

| | |
|---|---|
| 格式 | 普通 xpkg 加 `type = "subos"`：同一条安装管线 |
| 路径 | `xpkgs/<ns>-x-<name>/<ver>/`，与所有包一致 |
| 新实例 | 复制声明和文件（文件系统支持时写时复制）；payload 共享 |
| 存储 | 创建实例时选择，从不由模板决定 |
| 升级 | 显式：模板的多个版本并存；实例用 `subos upgrade` 迁移 |
