# T23 — data/xpkgs + subos 混合视图落地

> **优先级**: P2
> **Wave**: xim-Wave 6
> **预估改动**: ~120 行 Lua + 少量文档/测试
> **设计文档**: [../xpkgs-subos-hybrid-design.md](../xpkgs-subos-hybrid-design.md)

---

## 1. 任务概述

将 `data/xpkgs`（真实包层）与 `subos`（版本视图层）的混合架构落地到可执行实现：

- 保留 `subos/lib` 聚合默认视图（兼容）
- 新增 per-program 闭包视图（隔离）
- 运行时解析优先使用闭包路径，聚合路径作为 fallback

目标是做到"不破坏现有使用习惯"前提下，支持同一 subos 内多版本依赖并存。

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| T14 | 复用 xpkgs 的基础能力，避免重复下载与重复存储 |
| T16 | namespace 解析统一后，闭包路径解析更稳定 |
| T17 | 可复用 dep_install_dir 能力获取依赖安装路径 |
| T18（建议） | 项目级依赖声明可作为闭包输入来源 |

---

## 3. 实现方案

### 3.1 闭包数据结构（xvm 注册层）

在程序注册元数据中增加可选字段，例如：

```lua
envs = {
    XLINGS_PROGRAM_LIBPATH = "/abs/path/a:/abs/path/b",
    XLINGS_EXTRA_LIBPATH = "/opt/vendor/lib"
}
```

说明：

- `XLINGS_PROGRAM_LIBPATH` 只记录程序专属闭包路径（来源于 deps）
- `XLINGS_EXTRA_LIBPATH` 仅用于补充路径输入
- `LD_LIBRARY_PATH` 不允许在 xpkg 里直写，必须由运行时统一组装

#### 最简单示例（与子方案同步）

旧写法（不推荐）：

```lua
xvm.add("demo-tool", {
    envs = {
        LD_LIBRARY_PATH = "/abs/a:/abs/b"
    }
})
```

新写法（推荐）：

```lua
xvm.add("demo-tool", {
    envs = {
        XLINGS_PROGRAM_LIBPATH = "/data/xpkgs/libA/1.0/lib64:/data/xpkgs/libB/2.1/lib64",
        XLINGS_EXTRA_LIBPATH = "/opt/vendor/lib"
    }
})
```

运行时合成结果（示意）：

```text
LD_LIBRARY_PATH=
/data/xpkgs/libA/1.0/lib64:
/data/xpkgs/libB/2.1/lib64:
/opt/vendor/lib:
${XLINGS_SUBOS}/lib:
${INHERITED_LD_LIBRARY_PATH}
```

建议落点：

- `core/xim/pm/XPackage.lua`
- `core/xim/pm/PkgManagerExecutor.lua`

### 3.2 闭包路径解析（直接指向 data/xpkgs）

基于包 deps 解析每个依赖的 `lib64/lib` 实际路径，生成闭包列表：

- 优先 `data/xpkgs/<effective_name>/<version>/lib64`
- 次选 `.../lib`
- 去重并保持顺序稳定
- 闭包最终路径在当前环境生成（运行时或激活时），不在发布阶段写死绝对路径

建议落点：

- `core/xim/libxpkg/pkginfo.lua`（或新增 helper）
- 复用/扩展 T17 的 `dep_install_dir`

### 3.3 运行时解析优先级

按以下顺序构造 loader 可见路径：

1. per-program 闭包路径（`XLINGS_PROGRAM_LIBPATH`）
2. 程序内 RUNPATH/RPATH（由 ELF 自带）
3. `subos/lib` 聚合路径（fallback）
4. 系统默认路径

建议落点：

- `core/xvm/xvmlib/shims.rs`（如果由 shim 负责拼接）
- 或 `core/xim/config/*.lua`（如果由 xvm.add 前置拼接）

### 3.4 保持聚合模式兼容

不移除现有 `subos/lib` 聚合逻辑，只调整其角色为 fallback。

这样做可保证：

- 未声明闭包的历史程序保持原行为
- 新程序可逐步切换闭包优先

### 3.5 安全与可观测

新增调试输出（debug 开关下）：

- 当前程序闭包路径
- 最终 LD_LIBRARY_PATH 组装结果
- 命中的解析顺序（closure vs aggregate）

### 3.6 真实包示例（用于评审与验收）

验收时建议使用以下 xpkg 作为样例矩阵：

- `xim-pkgindex/pkgs/d/d2x.lua`（程序包，deps 驱动闭包输入）
- `xim-pkgindex/pkgs/g/glibc.lua`（基础运行时库，不直写 LD）
- `xim-pkgindex/pkgs/o/openssl.lua`（共享库包，不直写 LD）
- `xim-pkgindex/pkgs/m/musl-gcc.lua`（迁移示例：`XLINGS_EXTRA_LIBPATH`）

---

## 4. 分阶段执行

### 阶段 A（最小可用）

- 为程序注册增加 `XLINGS_PROGRAM_LIBPATH`
- 运行时先拼闭包再拼聚合
- 保持默认行为不变

### 阶段 B（冲突验证）

- 构造 A 依赖 `b@0.0.1`、C 依赖 `b@0.0.2` 场景
- 验证在同一 subos 并存运行

### 阶段 C（运行时收敛）

- 关键程序改为环境内闭包优先（如 d2x、toolchain 入口）
- 发布产物只携带闭包规则/变量占位，不写死机器绝对路径
- 文档中明确聚合模式仅为兼容层

---

## 5. 验收标准

| 检查项 | 期望结果 | 通过 |
|--------|---------|------|
| 程序级闭包字段写入成功 | xvm 注册信息可见 `XLINGS_PROGRAM_LIBPATH` | [ ] |
| 同一 subos 多版本并存 | A 与 C 分别加载各自版本依赖 | [ ] |
| 历史程序不回归 | 未启用闭包的程序行为与当前一致 | [ ] |
| fallback 生效 | 闭包缺失时可退回 `subos/lib` 正常运行 | [ ] |
| 路径污染可观测 | debug 输出可定位最终库搜索路径 | [ ] |
| 规范约束生效 | xpkg 直写 `LD_LIBRARY_PATH` 被 CI 拦截 | [ ] |

---

## 6. 验证命令建议

```bash
# 1) 查看注册信息（示例）
xlings info <program>

# 2) 运行时打印库解析（建议提供 debug 开关）
XLINGS_DEBUG_LIBPATH=1 <program> --version

# 3) ELF 侧核验（可选）
readelf -d <program-binary> | rg "RUNPATH|RPATH"
```

