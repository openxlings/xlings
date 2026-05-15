# Subos-as-XPKG 设计方案

**日期**:2026-05-16
**状态**:Design draft(rev3),待 review 后转 implementation plan
**关联代码**:`src/core/subos.cppm`、`src/core/xim/installer.cppm`、`src/core/xim/resolver.cppm`、`core/xim/libxpkg/`

---

## 1. 背景与定位

### 1.1 xlings 定位

- **包管理器基础设施** — xim/xpkg/xvm 是底层,subos 是其上的环境抽象
- **OS-like** — subos 是子系统(类 chroot/container),不是 venv
- **Agent 时代的环境工具** — 非交互式、deterministic、结构化输出
- **万物皆包** — subos 自己必须能被表达为一个 xpkg

### 1.2 当前 0.4.35 状态

- `xlings subos new <name> [--storage image|tmpfs|shared] [--size N]` 创建
- `xlings subos use <name> [--sandbox] [--global]` 进入(默认交互 shell)
- `xlings subos remove <name>` 删除
- workspace 记录该 subos 启用了哪些 xpkg + 哪个版本
- xpkg 全局共享在 `~/.xlings/data/xpkgs/`

**缺失**:无 fork、无"subos 包"概念、agent 用法差。

---

## 2. 核心设计

### 2.1 一句话

> **Base subos = `xpkgs/` 里的不可变 xpkg payload(`type="subos"`);Subos 实例 = `subos/` 里的可变 fork。fork 只复制 shim + workspace 元数据,工具共享全局 `xpkgs/`,所以 fork 真·0s。**

### 2.2 复用清单(不引入新抽象)

| 维度 | 复用什么 |
|---|---|
| 包格式 | 现有 xpkg 结构,`spec = "1"` 不变;只新增一个 type 值 `"subos"` |
| 命名空间 | 现有 namespace 机制,`namespace = "subos"`(可选,信息性) |
| 包路径 | **完全沿用传统** `~/.xlings/data/xpkgs/<name>/<ver>/`,无 `subos-` 前缀 |
| 分发机制 | xim-pkgindex + mirror + 签名 |
| 安装管道 | xim resolver + downloader + installer + xvm 注册,**全部走标准路径** |
| Install/uninstall hook | **xim 提供 `type="subos"` 默认 hook**,作者可 override |
| 创建 CLI | `xlings install`(装 base) + `xlings subos new`(fork) |
| 进入 CLI | `xlings subos use`(扩展 `--cmd` 参数) |
| 临时数据 | 现有 `--storage tmpfs` 模式 |
| 隔离 | 现有 `--sandbox`(bwrap/proot) |

### 2.3 数据布局

```
~/.xlings/
├── data/
│   └── xpkgs/                      ← 全局共享,所有 xpkg 平级
│       ├── python/3.11/            ← 普通 xpkg(已存在)
│       ├── numpy/1.26/             ← 普通 xpkg(已存在)
│       └── py-ds/1.0.0/            ← 新:type="subos" 的包,路径无前缀
│           ├── .xlings.json          ← workspace 声明(tarball 携带或默认 hook 生成)
│           ├── bin/                  ← (可选)模板 shim / 静态文件
│           └── templates/            ← (可选)作者预放的模板文件(bashrc 等)
│
└── subos/                          ← 用户 subos 实例(已存在)
    ├── default/
    ├── exp/                        ← `subos new exp --from subos:py-ds@1.0.0` 出来的实例
    │   ├── bin/                      ← 由 fork 时调 `ensure_subos_shims` 生成
    │   ├── lib/ usr/ generations/    ← 标准 subos create 结构
    │   ├── .xlings.json              ← 继承自 base 的 workspace
    │   └── (home.img 视 --storage 而定)
    └── ...
```

**两个目录角色**:
- `xpkgs/` 是不可变 xpkg payload(用户约定只读,改要去 fork)
- `subos/` 是用户实例(完全可写)

---

## 3. 关键决策

### 3.1 决策矩阵(rev3 收敛)

| # | 议题 | 决策 | 理由 |
|---|---|---|---|
| **D1** | 包格式 | 沿用 xpkg,新增 **`type = "subos"`** 一个 type 值;namespace `"subos"` 可选作人友好标识 | type 是 xim dispatch 信号,namespace 是命名约定;二者职责分开 |
| **D2** | install 逻辑 | xim 内置 `type="subos"` 默认 install/config/uninstall hook;**作者可 override** | 90% 包不用写 hook;特殊需求(自定义初始化)可 override |
| **D3** | xvm 注册 | 默认 hook **正常调 xvm.add 注册**,把 subos base 当普通包对待 | 一致性优先 — base 包应能被 `xlings list` / 版本查询 / uninstall 正常处理 |
| **E1** | base 可见性 | 不进 `xlings subos list`,只作 `--from` 源(but **进 `xlings xvm list`** 作为已装包) | 隔离两个语义:"已装的包" vs "可用的 subos 实例" |
| **E2** | fork 复制粒度 | base 的 `.xlings.json` workspace + 静态文件;deps 共享 xpkgs/ | KB 级 + reflink 优先 = 0s |
| **E3** | 升级路径 | 显式 — `@1` 和 `@2` 在 xpkgs/ 并存,用户 `subos new --from subos:py-ds@2 ...` | 避免静默破坏 |
| **E4** | uninstall | 删 xpkg 不连累 fork 出来的 subos(deps 各自独立) | fork 是物理 copy 不是 link |
| **E5** | `--from` 自动 install | `--from subos:xxx@ver` 自动拉包;`--from <local-subos>` 走本地 fork | agent 永远 1 条命令 |
| **D6** | CLI 跑命令 | `xlings subos use <name> --cmd "<cmd>"`,POSIX 内部 `sh -c` | 复用 `use`,无 cmd 进交互 shell |
| **D7** | 临时数据 | 复用 `--storage tmpfs`,**不引入 `--ephemeral`** | tmpfs 已提供该语义 |
| **D8** | 一行式 throwaway | **MVP 不做**,作为 Future 留 | 显式两步(`subos new --from` + `subos use --cmd`)已足够;throwaway 的 cleanup/GC 复杂度延后再考虑 |
| **D9** | 多次 exec 性能 | **默认自动 keeper**(条件:`storage = image/tmpfs` + `--sandbox` + Linux);TTL=5min idle;`--no-keep` 关、`--ttl <sec>` 调、`--keep` 不超时;不做 daemon(Z) | agent 零配置享受性能优化;非 mount 场景自动跳过 |

### 3.2 显式砍掉

- ❌ 新顶层命令(`provision` / `exec` / `shell`)
- ❌ 新 Lua API 模块(`xim.libxpkg.subos`)
- ❌ install hook 调 `xlings subos new --dir`(命令保留为高级 API,不在文档推荐路径)
- ❌ `--ephemeral` 标志(D7)
- ❌ 一行式 throwaway(D8,MVP 不做)
- ❌ docker-exec 风格 daemon(D9 Z)
- ❌ Overlayfs / COW 分层
- ❌ 包内嵌 user data
- ❌ `.subos-meta` 标记文件(F1)
- ❌ 跳过 xvm 注册(早期短暂讨论,D3 推翻)
- ❌ 路径特殊前缀 `xpkgs/subos-<name>/` 或 `xpkgs/_subos/`(F8 推翻)

---

## 4. CLI 完整界面

### 4.1 装 base 包

```bash
xlings install subos:py-ds@1.0.0
# → 走标准 xim install pipeline
# → 安装 deps(python, numpy, pandas 到 xpkgs/, 注册到当前 workspace)
# → 解压 tarball(若有 url)到 xpkgs/py-ds/1.0.0/
# → 跑 type="subos" 默认 hook:验证 .xlings.json,xvm.add 注册
# → 完成:base 在 xpkgs/py-ds/1.0.0/,xvm 中有 py-ds 条目
# → base 不出现在 `xlings subos list`
```

### 4.2 fork 出实例

```bash
# persistent(默认 shared storage)
xlings subos new exp --from subos:py-ds@1.0.0

# ephemeral data(tmpfs 会话级清空)
xlings subos new task --from subos:py-ds@1.0.0 --storage tmpfs

# 本地 fork(从已有 subos 复制)
xlings subos new exp2 --from exp

# --from <pkg-spec> 时,若 base 未装,自动 install
xlings subos new exp --from subos:py-ds@1.0.0
# ↑ 若 subos:py-ds@1.0.0 未装 → 内部先调 xlings install,再 fork
```

### 4.3 进入 / 跑命令

```bash
xlings subos use exp                              # 交互 shell(已有行为)
xlings subos use exp --cmd "python script.py"     # 新:单命令并退出
xlings subos use exp --sandbox --cmd "..."        # 沙箱模式跑命令
                                                  # ↑ 若 storage=image/tmpfs+Linux,自动 keeper 起,TTL=5min
xlings subos use exp --sandbox --cmd "..."        # 5min 内再跑:复用 keeper,~10ms
xlings subos use exp --sandbox --no-keep --cmd "..." # 显式关 keeper(一次性脚本)
xlings subos use exp --sandbox --ttl 600 --cmd "..." # 自定义 TTL=10min
xlings subos use exp --sandbox --keep --cmd "..." # 永不超时(等价旧 --keep 语义)
xlings subos stop exp                             # 立即强收 keeper(逃生舱)
```

### 4.4 清理

```bash
xlings subos remove exp           # 显式删 subos 实例
xlings uninstall subos:py-ds      # 卸载 base xpkg(不连累已 fork 的实例)
```

### 4.5 典型 Agent 用法

```bash
# 多轮任务环境
xlings subos new task-${TASK_ID} --from subos:ds-py@latest --storage tmpfs
xlings subos use task-${TASK_ID} --sandbox --keep
xlings subos use task-${TASK_ID} --sandbox --cmd "pip install foo"
xlings subos use task-${TASK_ID} --sandbox --cmd "python step1.py"
xlings subos use task-${TASK_ID} --sandbox --cmd "python step2.py"
xlings subos stop task-${TASK_ID}
xlings subos remove task-${TASK_ID}
```

---

## 5. 实现表面

### 5.1 新增 / 修改

| # | 项 | 位置 | 量级 |
|---|---|---|---|
| **I1** | xim installer 识别 `type = "subos"` + 内置默认 hook(install/config/uninstall) | `src/core/xim/installer.cppm` + Lua handler | ~80 行 |
| **I2** | `xlings subos new --from <spec>` | `src/core/subos.cppm` 新 export;reflink/clonefile/copy 跨平台 detail;自动 install 集成;type 验证 | ~150 行 |
| **I3** | `xlings subos use --cmd <cmd>` | `use_spawn_shell` 接受 cmd,POSIX 改 `execl(shell, "-c", cmd, null)`;sandbox 分支同步 | ~50 行 |
| **I4** | **Auto-keeper** + `subos stop`(Linux,默认开)| 新文件 `src/core/subos/keeper.cppm`,`.keeper.pid` + `.last_used` 状态文件,周期检查 + 自杀,nsenter 复用,触发条件判断(storage+sandbox+platform) | ~250 行 |
| **I5** | 显式 keeper flag overrides(`--no-keep` / `--ttl <sec>` / `--keep`) | `src/core/subos.cppm` argparse + 配置传递 | ~30 行 |

**总量级**:核心(I1-I3)~280 行 + auto-keeper(I4)~250 行 + flag overrides(I5)~30 行 = **~560 行**。

### 5.2 跨平台 detail

| 操作 | Linux | macOS | Windows |
|---|---|---|---|
| fork 复制(`--from`) | `cp --reflink=auto`(btrfs/xfs 复用,其它全 copy) | `clonefile()`(APFS COW) | `CopyFileEx`(全 copy) |
| tmpfs 挂载 | `mount -t tmpfs`(bwrap inside) | 不支持(降级 shared) | 不支持(降级 shared) |
| keeper / nsenter | `setns()` | 不支持(降级 X) | 不支持(降级 X) |
| Sandbox cmd 执行 | `bwrap ... -- sh -c <cmd>` | `proot ... -- sh -c <cmd>` | `pwsh -Command <cmd>` |

---

## 6. 流程图

### 6.1 装 base 包

```
用户:xlings install subos:py-ds@1.0.0

xlings CLI
  → xim resolver
      解析包描述,见 type="subos"
      解析 xpm.deps(python@3.11 numpy@1.26 pandas@2.2)→ 加入 install 任务
  → 标准 install pipeline 依次装 deps,各自走自己的 type 路径,xvm.add 注册到当前 workspace
  → 下载 tarball(若包含 url)→ 解压到 xpkgs/py-ds/1.0.0/
  → 跑默认 install hook(type="subos" 内置):
      - 验证 .xlings.json 存在且含 workspace 字段(无则从 xpm.deps 自动生成)
      - xvm.add("py-ds", { bindir = <install_dir>/bin })  -- 标准注册
  → 完成
```

### 6.2 fork

```
用户:xlings subos new exp --from subos:py-ds@1.0.0

xlings CLI
  → 解析 --from spec
  → 若是 pkg-spec(含 `:` 或 `@`):
      - xvm 查询 py-ds@1.0.0 是否已装
      - 未装 → 递归调 xlings install(E5)
  → 定位 base 路径:xpkgs/py-ds/1.0.0/
  → 验证 type="subos"(读包描述符 / 已装 metadata)
  → 标准 subos create() — 造 bin/ lib/ usr/ generations/,应用 --storage
  → 复制 base/.xlings.json → subos/exp/.xlings.json(改写 name 字段)
  → 复制 base/templates/* → subos/exp/(若有)
  → 调 ensure_subos_shims() 生成 subos/exp/bin/ 的 shim
  → 注册到 ~/.xlings/.xlings.json 的 subos 表
  → 完成
```

### 6.3 sandbox 单命令

```
用户:xlings subos use exp --sandbox --cmd "python script.py"

xlings CLI
  → use_spawn_shell(name="exp", sandbox=true, cmd="python script.py")
  → 检测 storage(shared / image / tmpfs)
  → 启动 bwrap / proot,挂 bind/image
  → exec sh -c "python script.py" (POSIX) / pwsh -Command "..." (Win)
  → 命令退出 → 返回 exit code,umount(若 image / tmpfs)
```

### 6.4 keeper(--keep 高频 exec)

```
首次:xlings subos use exp --sandbox --keep
  → bwrap 启动 + 挂载完成
  → fork keeper 进程(进入新 mount namespace 后 sleep)
  → 写 ~/.xlings/subos/exp/.keeper.pid

后续:xlings subos use exp --sandbox --cmd "..."
  → 检测 .keeper.pid → 进程存活
  → nsenter --mount=/proc/<pid>/ns/mnt -- sh -c "..."
  → 不重复挂载,~10ms 启动

收回:xlings subos stop exp
  → kill keeper → umount → 删 .keeper.pid
```

---

## 7. 与 Agent 场景的契合

### 7.1 Agent 所需属性

| 属性 | 实现 |
|---|---|
| 非交互 | `--cmd` 形式,无 TTY 假设 |
| 结构化输出 | xlings 已有 `EventStream`(JSON 事件流),沿用 |
| 确定性 | base 包版本固定,fork 不带随机 user data |
| 跨架构 | 包描述跨平台,xpkg 选择本地架构版本 |
| 隔离 | `--sandbox` 强制 |
| 快速供给 | base 缓存 + fork 0s + keeper(高频) |
| 0 凭据外泄 | 包内不含 user data |

### 7.2 标准 Agent 任务流

```bash
# 准备阶段(可一次性,后续复用)
xlings install subos:ds-py@latest

# 任务开始
TASK_SUBOS="task-$(uuidgen)"
xlings subos new "$TASK_SUBOS" --from subos:ds-py@latest --storage tmpfs

# 高频 exec
xlings subos use "$TASK_SUBOS" --sandbox --keep
xlings subos use "$TASK_SUBOS" --sandbox --cmd "<step1>"
xlings subos use "$TASK_SUBOS" --sandbox --cmd "<step2>"
xlings subos stop "$TASK_SUBOS"

# 任务结束
xlings subos remove "$TASK_SUBOS"
```

---

## 8. 实施里程碑

| M | 内容 | 包含 | 用户可见能力 |
|---|---|---|---|
| **M1** | type="subos" 包打通 | I1 | `xlings install subos:xxx` 标准下载解压 + xvm 注册 |
| **M2** | fork + 自动 install | I2 | `xlings subos new exp --from subos:xxx`(0s) |
| **M3** | `--cmd` 单命令执行 | I3 | `xlings subos use exp --cmd ...` |
| **M4** | **Auto-keeper(默认开,Linux+sandbox+image/tmpfs)** | I4 | 同 M3 命令,但高频 exec 自动加速,**用户无感** |
| **M5** | 显式 keeper flags(高级用户)| I5 | `--no-keep` / `--ttl` / `--keep` / `subos stop` |

**并行性**:
- M1 / M3 可并行(独立代码路径:installer vs use_spawn_shell)
- M2 依赖 M1(测试 fork 需要 base 包)
- M4 依赖 M3(扩展 cmd 执行)
- M5 依赖 M4(给 M4 加 flags)

每个里程碑独立可发布,可单独走 PR + release。

---

## 9. 仍待落地时确认的细节

| # | 细节 | 当前假设 |
|---|---|---|
| F2 | fork 后 shim hardlink 是否需 rewrite | 不需要 — `XLINGS_ACTIVE_SUBOS` env 已做上下文区分,M2 实施时 e2e 验证 |
| F3 | 跨平台 reflink 不可用退化 | 提示"非 0s",不阻塞 |
| F5 | tmpfs 默认 size | 跟随 base 包声明;无声明 → 2G |
| F6 | type="subos" 默认 hook 的 override 边界 | 三个 hook(install/config/uninstall)各自独立可 override;作者写哪个,xim 用哪个,其它走默认 |
| F7 | base 包 namespace 是否强制 `"subos"` | **不强制**;`type="subos"` 是真相,namespace 是命名约定;但建议 pkgindex 收纳规则要求 namespace="subos" 以便人辨识 |
| F8 | 若 base 包描述符或 tarball 都不带 `.xlings.json`,默认 hook 怎么办 | 默认 hook 从 `xpm.deps` 自动合成最小 `.xlings.json`(workspace = 各 dep 的 `name@version`)|
| F9 | `xlings install subos:py-ds` 时,deps 是注册到当前 workspace 还是只装到 xpkgs/ | **注册到当前 workspace**(D3 一致性 — 普通包行为)— 用户若想隔离,先切到独立 subos 再装 |

---

## 10. 显式不在范围

- 远程协议 / agent SDK(本设计只到 CLI 层)
- 跨主机 subos 漂移 / 同步
- subos 内的资源限额(cgroup memory/cpu)
- 用户数据加密 / 凭据托管
- 包签名 / 供应链安全(走 xim 通用机制)

---

## 11. Future / 后置考虑

以下从本设计中明确移除 MVP,但可在 MVP 落地后单独评估:

| 项 | 移除原因 | 重启条件 |
|---|---|---|
| **一行式 throwaway** `subos use <pkg-spec> --sandbox --cmd ...` | cleanup/GC 复杂度;显式两步已够用 | agent 实测使用频率高、两步残留问题真实出现时 |
| **预构建 binary cache**(B1b) | 当前 xim 镜像基础设施足够 | 装 base 包耗时成为瓶颈时 |
| **`subos = {...}` 直写描述符**(Flavor 2,无 tarball) | 与 tarball 形态重复 | tarball-只含一个 .xlings.json 成为常见模式时 |
| **Overlayfs / COW 分层** | 跨平台不可行 | 不重启(放弃)|
| **包内嵌 user data**(B1c) | 隐私风险 | 不重启(走 `subos export --include-home` 单独路径)|

---

## Appendix A:type="subos" base 包的标准写法

### A.1 最简形态(零 hook,纯声明)

```lua
-- pkgs/p/py-ds.lua
package = {
    spec = "1",
    name = "py-ds",
    namespace = "subos",                   -- 命名约定,可选但推荐
    description = "Python DS base subos",
    licenses = {"MIT"},
    type = "subos",                        -- ★ 关键
    archs = {"x86_64", "arm64"},

    xpm = {
        linux = {
            deps = {"python@3.11", "numpy@1.26", "pandas@2.2"},
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {
                url = "https://github.com/xlings-res/subos-py-ds/releases/download/1.0.0/py-ds-1.0.0.tar.gz",
                sha256 = "...",
            }
        }
    }
}

-- 无 install/config/uninstall,由 xim 内置默认 hook 处理:
--   - 解压 tarball 到 install_dir
--   - 验证 .xlings.json 存在(否则从 xpm.deps 合成)
--   - xvm.add("py-ds", { bindir = path.join(install_dir, "bin") })
```

### A.2 tarball 内容

```
py-ds-1.0.0.tar.gz
├── .xlings.json        ← workspace 声明(可选,无则默认 hook 从 xpm.deps 自动生成)
└── templates/          ← (可选)模板文件,fork 时一并复制
    ├── bashrc
    └── README.md
```

`.xlings.json` 内容(若手写或 release 脚本生成):
```json
{
  "workspace": {
    "python": "3.11",
    "numpy": "1.26",
    "pandas": "2.2"
  },
  "env": {
    "PYTHONUNBUFFERED": "1"
  }
}
```

### A.3 作者 override(需要自定义逻辑时)

```lua
-- 同前面的 package 块 ...

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

-- 仅 override install,config/uninstall 仍走默认
function install()
    local dir = pkginfo.install_dir()

    -- 自定义初始化:从 git 克隆某个 dotfiles 模板
    os.execv("git", {"clone", "--depth=1", "https://example.com/dotfiles.git", path.join(dir, "templates")})

    -- 完成默认 hook 该做的事:注册到 xvm
    xvm.add(pkginfo.name(), { bindir = path.join(dir, "bin") })
    return true
end
```

---

## Appendix B:已废弃 / 不采纳的设计

供后续若有人重提时快速对照:

| 早期想法 | 不采纳原因 |
|---|---|
| `xlings provision` 新命令 | 违反"复用而非新增" |
| `xlings exec --in <subos>` 新动词 | `subos use --cmd` 已够 |
| `subos shell <name> -- <cmd>` | "shell" 字面歧义 |
| 声明式 subos manifest(workspace 列表写在包描述里)| install hook + tarball 已够 |
| 仅命名空间 `subos:`,不加 type | xim 无 dispatch 信号 — Rev 2 短暂采用,Rev 3 推翻 |
| `xim.libxpkg.subos` 新 Lua API 模块 | 过早抽象;`os.execv` 调 CLI 已够;最终连 hook 都不必,本项更不需要 |
| install hook 调 `xlings subos new --dir` | `--dir` 是泄漏;最终决定无 hook |
| `--ephemeral` 标志 | tmpfs storage 已提供该语义 |
| 一行式 throwaway `subos use <pkg-spec> --cmd ...` | cleanup 复杂度;MVP 后置评估(见第 11 节) |
| docker-exec 风格长会话 daemon(Z 方向) | 工作量 vs 收益不划算;`--keep` + nsenter 已够 |
| Overlayfs / base + overlay 实例 | Linux only、storage mode 组合爆炸 |
| 包内嵌 user data 层 | 隐私 / 凭据风险 |
| `.subos-meta` 标记文件(F1)| `.xlings.json` workspace 存在性已够判定 |
| 跳过 xvm 注册的特殊 dispatch | 一致性优先 — 当普通包注册;deps 同样正常注册到 active workspace(D3 / F9)|
| `xpkgs/subos-<name>/` 或 `xpkgs/_subos/` 特殊路径 | 路径完全沿用传统,无前缀(F8 / D1 配套)|

---

## 修订记录

- **2026-05-16 rev1** — 初稿,基于 brainstorming session 收敛
- **2026-05-16 rev2** — 砍掉 `xim.libxpkg.subos` 新 Lua API;实现量级 ~760 → ~480 行
- **2026-05-16 rev3** — 关键反转 + 简化:
  - ✅ 加回 `type = "subos"`(rev2 推翻 rev1 关于"只用 namespace"的决策)
  - ✅ xim 提供 type=subos **默认 hook**,作者可 override
  - ✅ 默认 hook **正常调 xvm.add 注册**(把 base 当普通包对待,D3)
  - ✅ 包路径**完全沿用传统** `xpkgs/<name>/<ver>/`,无前缀(F8)
  - ✅ deps **注册到当前 workspace**(D3 / F9 — 不再做"装但不注册"的特殊隔离)
  - ❌ 一行式 throwaway 砍出 MVP,转入 Future 章节(第 11 节)
  - ❌ `.subos-meta` 标记文件不需要(F1)
  - 实现量级:核心 ~280 行(I1-I3)+ keeper ~200 行(I4)
- **2026-05-16 rev4** — Auto-keeper 默认开,UX 进一步收敛:
  - ✅ Keeper 由 opt-in `--keep` 升级为**默认行为**(触发条件:storage=image/tmpfs + sandbox + Linux),TTL=5min idle
  - ✅ 显式 flag(`--no-keep` / `--ttl <sec>` / `--keep` 永不超时)拆出 I5(高级用户)
  - ✅ 里程碑加 M5(显式 flags),M4 重定义为 auto-keeper
  - 实现量级:I4 从 ~200 → ~250 行(多 ~50 行 TTL 逻辑),I5 ~30 行;总 ~560 行
