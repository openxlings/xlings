# xlings subos 分级隔离架构（light / medium / heavy / full）

> 与前两份分析（`subos-mount-namespace-analysis.md`、`subos-full-isolation-analysis.md`）的总集。
> 把 subos 隔离能力切成 **4 档**，让用户按需付出复杂度，xlings 只做"约定 + 编排"层，重活交给现有后端（bwrap / systemd-nspawn）。

---

## 0. 设计哲学

| 原则 | 含义 |
|------|------|
| **复杂度按需付费** | 90% 用户只需 light，95% 需求 medium 能覆盖；heavy/full 是高阶选项 |
| **xlings 不重造轮子** | 容器编排已有成熟工具（bwrap、nspawn）。xlings 提供"目录约定 + 启动配置 + UX"，不写自己的容器运行时 |
| **统一目录约定** | 4 个 mode 复用同一个 subos 目录结构；高 mode 只是多用了某些子目录 |
| **平滑升级路径** | `light → medium → heavy → full` 单向兼容；`subos upgrade <name> --mode <next>` 可推升等级 |
| **统一回退策略** | 任何 mode 不可用（内核/平台限制）时，自动降级或显式报错，用户始终知道自己在哪一档 |

---

## 1. 四档总览

| 档位 | 一句话定义 | 隔离手段 | 主要受众 |
|------|----------|---------|---------|
| **light** | 把 subos 的 `local/*` 覆盖到宿主 `/usr/local/*` | mount-ns + 5 个 bind | 默认；切换工具链版本 |
| **medium** | light + 私有 `/home`、`/opt`、`/tmp` | mount-ns + 10 个 bind/tmpfs | 干净开发环境；防 dotfile 污染 |
| **heavy** | OverlayFS 让 subos 看起来像独立 `/` | user-ns + mount-ns + overlay + pivot_root | 可复现实验；"独立 OS 视图" |
| **full** | 真正完整 rootfs（Alpine/Debian/Arch） | systemd-nspawn / bwrap + 完整 mount 编排 | 跨发行版分发；教学；CI 一致性 |

---

## 2. 各档详细规格

### 2.1 light — 增强 PATH

**目录结构：**
```
$XLINGS_HOME/subos/<name>/
├── .xlings.json
├── local/
│   ├── bin/
│   ├── lib/        (含 pkgconfig/、cmake/)
│   ├── include/
│   └── share/
├── xvm/                  ← 现有 xvm 工具版本
└── generations/          ← 现有快照（与隔离无关）
```

**挂载映射：**
| 源 | 目标 | 备注 |
|---|------|-----|
| `subos/<name>/local/bin`     | `/usr/local/bin`     | 主 PATH |
| `subos/<name>/local/lib`     | `/usr/local/lib`     | ld.so 默认搜 |
| `subos/<name>/local/include` | `/usr/local/include` | gcc/clang 默认 |
| `subos/<name>/local/share`   | `/usr/local/share`   | locale/cmake/pkgconfig |
| `subos/<name>/local/libexec` | `/usr/local/libexec` | 可选 |

**启动流程：**
```
unshare -U -m -r --map-root-user
  → mount --make-rprivate /
  → for d in bin lib include share; do
      mount --bind subos/<n>/local/$d /usr/local/$d
    done
  → exec $SHELL
```

**特性：**
- mount 数：≤6
- 启动延迟：< 50ms
- 内核要求：unprivileged user-ns（≥3.8 通用）
- 跨发行版可移植：否（subos 内的二进制依赖宿主 libc）
- macOS/Windows：自动降级为 PATH-only 模式
- 实现代码量：≈ 200 行 C++（含命令注册、平台检测、回退逻辑）
- 外部依赖：无
- 用户认知模型："subos 就是 PATH 加强版"

**适用场景：**
- 切换 gcc/clang/python/node 版本
- 包管理器装的工具落进 subos，不污染宿主
- 多个项目并行开发，互不干扰

**不适合：**
- 需要替换 libc / glibc 版本
- 跨发行版分发产物
- 需要清空 `~/.config` 试错

---

### 2.2 medium — 私有用户空间

**目录结构（在 light 基础上新增）：**
```
$XLINGS_HOME/subos/<name>/
├── ...（light 全部）
├── home/                 ← 映射为 $HOME（per-subos 用户数据）
├── opt/                  ← 映射为 /opt/xlings/<name>
└── tmp-seed/             ← 可选：启动时拷到 tmpfs 的初始化模板
```

**新增挂载：**
| 源 | 目标 | 备注 |
|---|------|-----|
| `subos/<name>/home` | `$HOME` | dotfile/缓存私有 |
| `subos/<name>/opt`  | `/opt/xlings/<name>` | 第三方包发行 |
| (tmpfs)             | `/tmp` | namespace 私有 tmpfs |
| (可选 tmpfs)        | `/var/tmp` | 部分构建工具用 |

**启动流程（在 light 基础上）：**
```
... light 步骤 ...
mount --bind subos/<n>/home  $HOME
mount --bind subos/<n>/opt   /opt/xlings/<n>
mount -t tmpfs tmpfs /tmp
[ -d subos/<n>/tmp-seed ] && cp -a subos/<n>/tmp-seed/. /tmp/
```

**特性：**
- mount 数：≤10
- 启动延迟：< 100ms
- 内核要求：与 light 相同
- 跨发行版可移植：否（仍依赖宿主 libc/coreutils）
- macOS/Windows：降级为 light-without-bind（仅 PATH + 设 `HOME=$XLINGS_HOME/subos/<n>/home`）
- 实现代码量：在 light 基础上 +100 行
- 外部依赖：无
- 用户认知模型："subos 是干净的开发盒"

**适用场景：**
- 学习新工具链：dotfiles 不被污染
- 项目级 .cache/.config 隔离
- CI 本地复现：每跑一次 `xlings subos run` 都从 tmp-seed 重置 `/tmp`
- 教学场景：学生每次上课从模板 home 启动

**不适合：**
- 想要换 libc/glibc
- 想要不同发行版的 coreutils（`/bin/sh` 仍是宿主的）

---

### 2.3 heavy — OverlayFS 完整视图

**目录结构：**
```
$XLINGS_HOME/subos/<name>/
├── .xlings.json
├── upper/                ← OverlayFS upperdir（subos 对宿主 / 的差量）
├── work/                 ← OverlayFS workdir
├── etc-overrides/        ← subos 自管理的 /etc 子集
│   ├── passwd
│   ├── nsswitch.conf
│   ├── ld.so.conf.d/
│   ├── profile.d/
│   └── hostname
├── meta/
│   └── base.json         ← 记录基线宿主信息
└── xvm/
```

**挂载策略：**
- 宿主整个 `/` 作 `lowerdir`
- `subos/<name>/upper` 作 `upperdir`
- 在 namespace 内 mount overlay 到 `/tmp/newroot`，再 `pivot_root`
- procfs/sysfs/devfs 单独处理
- 关键 `/etc` 条目从 `etc-overrides/` 或宿主 bind 进来

**启动流程（完整）：**
```bash
unshare -U -m -r --map-root-user --propagation private --pid --fork --uts \
  bwrap --new-session --die-with-parent \
        --proc /proc --dev /dev --tmpfs /tmp --tmpfs /run \
        --bind /sys /sys \
        --overlay-src / --tmp-overlay /        # bwrap 0.7+ 支持 overlay
        --bind $SUBOS/etc-overrides/passwd     /etc/passwd \
        --bind $SUBOS/etc-overrides/nsswitch   /etc/nsswitch.conf \
        --ro-bind /etc/resolv.conf             /etc/resolv.conf \
        --ro-bind /etc/localtime               /etc/localtime \
        --ro-bind /etc/ssl/certs               /etc/ssl/certs \
        --bind $HOME/.xlings-data/<n>          /root \
        --hostname xlings-<n> \
        -- /bin/bash -l
```

> bwrap 已经原生支持 overlay 挂载（`--overlay-src` + `--tmp-overlay`），xlings 不需要自己写 OverlayFS 逻辑。

**特性：**
- mount 数：20+（含 procfs/sysfs/devfs/tmpfs）
- 启动延迟：200-500ms
- 内核要求：
  - unprivileged user-ns
  - unprivileged OverlayFS（**kernel ≥ 5.11**）
  - 缺一不可，否则降级 medium
- 跨发行版可移植：**部分**（subos 修改可移植，但 lower 仍是宿主，宿主不同表现可能差异）
- macOS/Windows：**不支持**，需用户用 Lima/WSL2
- 实现代码量：≈ 400 行（命令 + 启动配置生成 + bwrap 调用 + 错误处理）
- 外部依赖：**bubblewrap** 包（多数发行版仓库可装）
- 用户认知模型："subos 是宿主的差量克隆"

**适用场景：**
- 实验性：装一堆库后不想留下痕迹（discard upper 即恢复）
- 修改 `/etc` 验证配置变更
- 体积敏感：subos 只存差量，不复制整个发行版
- 故障复现：固化 upper 后分发给同事

**不适合：**
- 跨发行版分发（lower 是宿主，朋友用别的发行版结果不同）
- 需要不同 libc（lower 决定了 glibc 版本）

**关键限制：**
- OverlayFS 无法替换 lower 中的二进制本身的依赖关系
- 删除 lower 中的文件在 upper 留 character device whiteout（导出有兼容性问题）

---

### 2.4 full — 独立 rootfs

**目录结构：**
```
$XLINGS_HOME/subos/<name>/
├── .xlings.json
├── rootfs/                ← 完整发行版根
│   ├── bin/  sbin/  usr/  lib/  etc/  home/  var/  ...
├── meta/
│   ├── base.json          ← 来源（alpine:3.19 / debian:trixie / arch:rolling）
│   ├── arch.txt           ← x86_64 / aarch64
│   └── hooks/
│       ├── pre-enter.sh
│       └── post-enter.sh
└── xvm/                   ← 共享 xvm 数据（可选，subos 内重定向）
```

**挂载策略：**
- `subos/<name>/rootfs/` 作为新根（不需要 overlay）
- 必须绑定的宿主接口：`/proc`(fresh)、`/sys`(rbind ro)、`/dev`(rbind 或 devtmpfs+白名单)、`/run`(tmpfs 或选择性 bind)
- 必须绑定的动态信息：`/etc/resolv.conf`、`/etc/localtime`
- 可选绑定：`/run/user/$UID`（GUI/音频）、`$HOME` 子目录、宿主 GPU 用户态库

**bootstrap：**
```
xlings subos new dev --mode full --base alpine:3.19
  → 下载 alpine-minirootfs-3.19-x86_64.tar.gz（≈3 MB 压缩，10 MB 解压）
  → 解压到 subos/dev/rootfs/
  → 写 meta/base.json
  → 注入 xlings 自身（subos/dev/rootfs/usr/local/bin/xlings）

xlings subos new dev --mode full --base debian:trixie-slim  → ≈30 MB 压缩
xlings subos new dev --mode full --base archlinux           → ≈80 MB 压缩
```

**启动流程：**
```bash
systemd-nspawn \
  --quiet --as-pid2 --keep-unit --user=$USER \
  --directory=$SUBOS/rootfs \
  --bind-ro=/etc/resolv.conf:/etc/resolv.conf \
  --bind-ro=/etc/localtime:/etc/localtime \
  --bind=$HOME/.xlings-data/<n>:/home/$USER \
  --hostname=xlings-<n> \
  --capability=CAP_NET_BIND_SERVICE \
  /bin/sh -l

# 或 bwrap 后端（不需要 root，弱化容器特性）
bwrap --bind $SUBOS/rootfs / \
      --proc /proc --dev /dev --tmpfs /tmp --tmpfs /run \
      --ro-bind /sys /sys \
      --ro-bind /etc/resolv.conf /etc/resolv.conf \
      --ro-bind /etc/localtime /etc/localtime \
      --uid 0 --gid 0 \
      --hostname xlings-<n> \
      --new-session \
      /bin/sh -l
```

**特性：**
- mount 数：30+
- 启动延迟：500ms-2s（systemd-nspawn 略慢）
- 内核要求：
  - bwrap 后端：unprivileged user-ns
  - nspawn 后端：root 或 systemd ≥ 252 + unpriv user-ns
- 跨发行版可移植：**强**（subos 内是独立发行版，宿主只提供内核）
- macOS/Windows：**不支持**
- 实现代码量：≈ 500 行（bootstrap、命令、启动、hooks）
- 外部依赖：bubblewrap **或** systemd-container；下载/解压需 curl + tar
- 用户认知模型："subos 是一个迷你 Linux 发行版"

**适用场景：**
- 跨发行版交付：作业、实验、CI 一致性
- 教学：给学生干净的 Alpine/Debian 实验环境
- 古老/特殊发行版（CentOS 7、musl-only）需要测试
- 对比不同 glibc/libstdc++ 版本行为

**不适合：**
- 90% 普通工具版本切换需求（杀鸡用牛刀）
- 体积敏感场景（每个 full subos 几十到几百 MB）
- 需要紧密集成宿主的 GUI/音频/GPU（要逐个直通配置）

---

## 3. 横向对比矩阵

### 3.1 隔离强度

| 资源 | light | medium | heavy | full |
|------|:----:|:------:|:-----:|:----:|
| PATH 工具 | ✓ | ✓ | ✓ | ✓ |
| `/usr/local` 库 | ✓ | ✓ | ✓ | ✓ |
| `$HOME` (dotfiles) | ✗ | ✓ | ✓ | ✓ |
| `/tmp` | ✗ | ✓ | ✓ | ✓ |
| `/etc` 配置 | ✗ | ✗ | ✓ | ✓ |
| 系统库 (`/usr/lib`) | ✗ | ✗ | 部分 | ✓ |
| `/bin /sbin` | ✗ | ✗ | 部分 | ✓ |
| 主机名 (UTS) | ✗ | ✗ | ✓ | ✓ |
| PID 视图 | ✗ | ✗ | 可选 | 可选 |
| 网络栈 | ✗ | ✗ | ✗ | ✗（默认共享） |

> 网络默认全档共享宿主，避免破坏开发体验（pip install、apt 下载）。需要 net-ns 的请走 docker。

### 3.2 资源开销

| 维度 | light | medium | heavy | full |
|------|:----:|:------:|:-----:|:----:|
| subos 体积 | 几 MB | 几 MB | 几 MB（差量） | 10–500 MB |
| 启动延迟 | <50ms | <100ms | 200–500ms | 500ms–2s |
| 内存开销 | 0 | tmpfs（按需） | tmpfs + overlay 元数据 | 完整 OS 进程 |
| 磁盘 inode | 低 | 低 | 中（whiteout） | 高（全 rootfs） |

### 3.3 兼容性矩阵

| 平台/内核 | light | medium | heavy | full(bwrap) | full(nspawn) |
|----------|:----:|:------:|:-----:|:----------:|:----------:|
| Linux ≥ 5.11 + user-ns | ✓ | ✓ | ✓ | ✓ | ✓ |
| Linux 3.8–5.10 + user-ns | ✓ | ✓ | ✗→降 medium | ✓ | 需 root |
| Linux 无 unpriv user-ns（RHEL≤8） | 降 PATH | 降 PATH | ✗ | 需 root | 需 root |
| Ubuntu 24+ AppArmor 限 user-ns | 提示开关 | 提示开关 | 提示开关 | 提示开关 | 提示开关 |
| macOS / Windows | 降 PATH | 降 PATH | ✗（建议 Lima/WSL2） | ✗ | ✗ |

### 3.4 实现成本（xlings 侧代码量估算）

| 模块 | light | medium | heavy | full |
|------|:----:|:------:|:-----:|:----:|
| 命令注册（new/enter/run） | 100 | 100 | 100 | 100 |
| 平台/内核检测 + 降级 | 80 | 80 | 120 | 120 |
| 目录约定与初始化 | 50 | 80 | 120 | 200 |
| 启动编排 | 80 | 120 | 200 | 250 |
| Bootstrap（基线下载/解压） | 0 | 0 | 0 | 200 |
| Hook/退出清理 | 30 | 50 | 80 | 100 |
| **小计（增量）** | **≈340** | **+170** | **+260** | **+450** |

> 数字仅 C++ 主体；Lua 配置、shell helper 不计。

### 3.5 用户认知模型

| 档位 | 一句话比喻 | 学习曲线 |
|------|----------|---------|
| light | "PATH 加强版，每个 subos 一套工具" | 5 分钟 |
| medium | "干净的开发盒，dotfile 不污染" | 15 分钟 |
| heavy | "宿主的克隆体，可丢弃可固化" | 1 小时 |
| full | "一个迷你 Linux 发行版" | 半天 |

### 3.6 适用场景

| 场景 | 推荐档位 |
|------|---------|
| 切 gcc / python / node 版本 | light |
| 学新框架不污染 home | medium |
| 多项目并行不串味 | medium |
| 教学：给学生独立环境 | medium 或 full（Alpine） |
| 试验性：装一堆包后想清干净 | heavy |
| 复现 CI：跨机器一致 | full |
| 跨发行版交付 | full |
| 调研旧 glibc 行为 | full |
| 与同事分享开发环境 | full（导出 rootfs） |

---

## 4. 决策树

```
是否需要不同 libc 或不同发行版?
├── 是 → full
└── 否
    └── 是否需要 /etc 隔离 / 完整 OS 视图?
        ├── 是 → heavy（如内核/平台不支持，提示并阻止）
        └── 否
            └── 是否需要 dotfile / /tmp 隔离?
                ├── 是 → medium
                └── 否 → light（默认）
```

CLI 参数对应：
```
xlings subos new <name>                 # 默认 light
xlings subos new <name> --mode medium
xlings subos new <name> --mode heavy
xlings subos new <name> --mode full --base alpine:3.19
xlings subos upgrade <name> --mode <next>   # 可单向推升
```

---

## 5. 升级路径与兼容性

### 5.1 单向升级规则

```
light → medium → heavy → full
```

每一档是上一档的超集（启动配置和目录约定上）：

- **light → medium**: 新增 `home/`、`opt/` 目录；启动多 3 条 mount。`local/` 子树原样保留。
- **medium → heavy**: 新增 `upper/`、`work/`、`etc-overrides/`；`local/` 在 namespace 内仍生效（只是变成 upper 的一部分）。
- **heavy → full**: 把 `upper/` 实体化成完整 `rootfs/`（用 base + 应用 upper 差量），删除 lower 依赖。需要 bootstrap base image。

### 5.2 降级规则

降级**默认禁止**——丢弃数据风险高。
若用户明确要求：

```
xlings subos downgrade <name> --mode light --keep-data
  → 把 home/ opt/ etc-overrides/ rootfs/ 等高档目录归档到 archived/
  → 保留 local/ 子树
```

### 5.3 在线启动期降级（autodegrade）

当用户请求 mode 内核不支持时：

```
xlings subos enter dev   # subos.mode = heavy

# 内核检测：unprivileged overlay 不支持
[xlings] WARNING: subos 'dev' configured as 'heavy', but kernel lacks
                  unprivileged OverlayFS (need ≥5.11). Auto-degrading to 'medium'.
                  To disable autodegrade, set XLINGS_NO_AUTODEGRADE=1.
```

可通过环境变量 `XLINGS_NO_AUTODEGRADE=1` 关闭，强制报错退出。

---

## 6. 后端选型（xlings 不重造轮子）

| 后端 | 用于哪些档位 | 优点 | 缺点 |
|------|-------------|------|------|
| **xlings 原生 unshare** | light、medium | 0 外部依赖；启动快；可控 | 自己处理 namespace 细节，但 light/medium 简单可控 |
| **bubblewrap (bwrap)** | heavy、full（轻量） | 专为 sandbox 设计；user-ns 友好；overlay 内置 | 需安装 bwrap 包 |
| **systemd-nspawn** | full（重量） | 接近真实容器；machinectl 管理；自带 cgroup/资源限制 | 多数情况要 root；与 systemd 强绑 |
| **chroot（弃）** | 不用 | 实现简单 | 必须 root；无 namespace 隔离 |
| **runc / docker / podman** | 不用 | 工业级 | 引入容器运行时整层；过重 |

**推荐组合：**

```
light    → 原生 unshare
medium   → 原生 unshare
heavy    → bwrap （动态检测，不在则降级 medium 并告警）
full     → bwrap (默认) | systemd-nspawn (高级模式 / CI)
```

**xlings 的代码层级：**

```
┌──────────────────────────────────────────┐
│  xlings subos {new, enter, run, …}       │  ← 命令层（用户接口）
├──────────────────────────────────────────┤
│  目录约定 + 配置生成 + mode 仲裁          │  ← xlings 自有逻辑
├──────────────────────────────────────────┤
│  unshare(2)  │  bwrap  │  nspawn         │  ← 后端（系统能力）
└──────────────────────────────────────────┘
            ↓                ↓
        Linux 内核     Linux 内核
```

xlings 永远不写自己的 namespace 编排细节，全交给后端。这是"不重造轮子"的边界。

---

## 7. 配置文件设计

每个 subos 的 `.xlings.json` 增加 `mode` 字段：

```json
{
  "mode": "medium",
  "version": 1,
  "workspace": { ... },

  "light": {
    "local": "local"
  },
  "medium": {
    "home": "home",
    "opt": "opt",
    "tmp": "tmpfs",
    "tmp_seed": "tmp-seed"
  },
  "heavy": {
    "overlay": {
      "upper": "upper",
      "work": "work"
    },
    "etc_overrides": "etc-overrides",
    "backend": "bwrap"
  },
  "full": {
    "base": "alpine:3.19",
    "rootfs": "rootfs",
    "backend": "bwrap",
    "binds": [
      { "host": "/etc/resolv.conf", "guest": "/etc/resolv.conf", "ro": true },
      { "host": "/run/user/1000", "guest": "/run/user/1000" }
    ]
  }
}
```

启动时 xlings 读 `mode` 字段，按对应配置挂相应子段。同一 subos 一旦 `mode` 提升，下次启动以新 mode 编排，旧子段保留以备降级。

---

## 8. 用户故事示例

### 故事 1：Web 开发者切 Node 版本

```bash
xlings subos new node18                   # mode=light（默认）
xlings xpkg install node@18 --in node18
xlings subos enter node18
$ node --version
v18.20.0
$ exit
```

### 故事 2：学生学 Rust 不污染 dotfiles

```bash
xlings subos new rust-class --mode medium
xlings xpkg install rustup --in rust-class
xlings subos enter rust-class
$ cd $HOME && ls -la
.cargo  .rustup     # 只有学习相关的，无主用户的 .ssh / .gnupg
$ exit
```

### 故事 3：实验性折腾完整 dotfiles 后丢弃

```bash
xlings subos new wild --mode heavy
xlings subos enter wild
$ # 装一堆 deb、改 /etc、跑各种命令
$ exit
xlings subos discard wild     # 清空 upper/，subos 回到全新差量
```

### 故事 4：跨发行版交付作业

```bash
# 老师机器（Arch）
xlings subos new course --mode full --base debian:trixie-slim
xlings subos enter course
$ apt install gcc make ...
$ exit
xlings subos export course > course.tar.zst

# 学生机器（Ubuntu / Fedora / Arch）
xlings subos import course.tar.zst
xlings subos enter course
$ # 完全一致的 Debian Trixie 环境
```

---

## 9. 风险与限制（按档位）

### light 档位风险

| 风险 | 应对 |
|------|------|
| 内核 user-ns 禁用 | 降级 PATH-only（兼容现有行为） |
| pkg-config 不默认含 `/usr/local/lib/pkgconfig` | 启动设 `PKG_CONFIG_PATH` |
| ld.so 不默认搜 `/usr/local/lib` | 启动设 `LD_LIBRARY_PATH` 兜底 |

### medium 档位风险

| 风险 | 应对 |
|------|------|
| `$HOME` 私有后 SSH key、git config 不可见 | 文档说明；提供 `--share-ssh`、`--share-git` 选项做选择性 bind |
| 私有 `/tmp` 后 X11 socket 看不到 | tmp-seed 注入 `/tmp/.X11-unix` 链接；或选择性 bind |
| 工具假定 `$HOME` 路径不变 | 设 `HOME=$XLINGS_HOME/subos/<n>/home`，多数工具兼容 |

### heavy 档位风险

| 风险 | 应对 |
|------|------|
| OverlayFS 老内核不可用 | 检测 `/proc/filesystems` 和 user-ns 能力；自动降级 |
| upper 目录中 whiteout 导出复杂 | `subos export` 转换 whiteout 为 OCI 格式，或固化为 lower-merged |
| /etc 部分 bind 后宿主切网导致 DNS 错乱 | resolv.conf 用 `--ro-bind`，宿主切网后退出再进重新生效 |
| 内存压力 | overlay 的脏页、tmpfs 都吃内存；文档建议 |

### full 档位风险

| 风险 | 应对 |
|------|------|
| Base image 下载源 | 自建镜像源 + 校验签名；fallback 官方源 |
| Bootstrap 失败（架构/版本不匹配） | meta/arch.txt 严格检查 |
| 与宿主 GPU 驱动不匹配 | 文档警告；提供 `--share-gpu` 选项做必要 bind |
| systemd-user / dbus 不可用 | full 内默认无 systemd；用户需求大时再考虑 nspawn 后端 |
| 体积爆炸（多 subos） | 提供 `subos prune` 清理；inter-subos hardlink 去重（同 base 共享） |

---

## 10. Roadmap 建议

| 里程碑 | 内容 | 估期 | 风险 |
|--------|------|------|------|
| **M1**: 落地 light | 新增 `enter`/`run` 命令；目录引入 `local/`；user-ns 启动；macOS 降级 PATH | 2-3 周 | 低 |
| **M2**: 升级 medium | 新增 `home/`、`opt/`、tmpfs；`--mode medium`；selective bind 选项 | 1-2 周 | 低 |
| **M3**: heavy PoC | 独立分支，bwrap overlay 启动；不进主线 | 2 周 | 中 |
| **M4**: heavy 主线 | 整合 bwrap 后端；`/etc-overrides` 模板；autodegrade | 2-3 周 | 中 |
| **M5**: full bootstrap | base image 下载；rootfs 解压；`--base` 参数 | 3-4 周 | 中 |
| **M6**: full 启动 | bwrap full mode；`/proc /sys /dev` 编排；hooks | 2 周 | 中高 |
| **M7**: export/import | 跨主机分发；OCI 格式兼容 | 2 周 | 中 |
| **M8**: nspawn 后端（可选） | 高级用户的容器化体验 | 2 周 | 低（后端可插拔） |

总体：M1-M2 是真正主线（覆盖 90% 需求）；M3-M5 视用户反馈；M6+ 视 xlings 战略选择。

---

## 11. 决策建议（给项目维护者）

**必做：**
- M1 + M2（light/medium 是 xlings 工具属性的天然延伸，零依赖、跨平台降级好）

**评估后做：**
- M3-M4（heavy）：当用户反馈"想要一个比 nix-shell 重、比 docker 轻的环境"时
- M5-M7（full）：当 xlings 想正式做"开发环境分发平台"时

**不做（红线）：**
- 不写自己的 OverlayFS / namespace 实现细节
- 不引入容器运行时（runc/containerd）
- 不维护自己的 base image 注册表（用官方 Alpine/Debian/Arch tarball + 校验）
- 不做 net-ns（破坏开发体验，不是 xlings 定位）

**核心定位提醒：**

xlings = "**目录约定 + 启动编排**"。
内核能力交给 user-ns / mount-ns；编排细节交给 bwrap / nspawn。
xlings 提供**一致的 UX 和教学友好的抽象**，这才是差异化。

---

## 12. 一套架构能做完吗？

**简答：能。约 70% 共享骨架 + 30% 显式插件化的差异点。**

详细分析见独立文档：[`2026-05-05-subos-unified-architecture.md`](./2026-05-05-subos-unified-architecture.md)。

该文档涵盖：
- 共享骨架的具体边界（命令层、配置 schema、EventStream、Mode 仲裁、Autodegrade 等）
- 8 项无法消除的真实差异点（Bootstrap I/O、MountPlan 内容、后端能力、`/etc` 策略、内核检测、降级链、Export 格式、平台降级）
- 架构图：共享骨架 + 三类策略点（PlanBuilder / BackendDriver / BootstrapStrategy）
- 关键抽象 `MountPlan` 数据结构设计
- 模块划分与代码组织建议
- 各模式专属依赖矩阵
- 渐进引入策略（M1-M4 分阶段引入抽象，避免过度设计）

> 以下是该文档的精简摘要，供快速浏览。

### 12.1 共享骨架（4 档完全一样的部分，约 70%）

| 模块 | 内容 |
|------|------|
| **命令层** | `subos {new, use, enter, run, exec, list, info, remove, upgrade, export, import, discard}` 命令解析与路由——所有 mode 共用同一组命令 |
| **配置 schema** | `.xlings.json` 的统一 schema（含 `mode` 字段 + 各 mode 子段）；读写、版本迁移逻辑 |
| **目录所有权** | `$XLINGS_HOME/subos/<name>/` 路径解析、生命周期、引用计数（与 payload 共享机制对接） |
| **EventStream 事件** | `subos_created`、`subos_switched`、`subos_entered`、`subos_exited`、错误码分类——与现有 `cli.cppm` 一致的事件流 |
| **Hook 系统** | `pre-enter.sh`、`post-enter.sh` 钩子模板与执行——4 档统一 |
| **Mode 仲裁器** | 读 `.xlings.json` 的 `mode` → 探测内核能力 → 决定实际生效 mode（autodegrade）—— 仲裁结果是策略点的输入 |
| **平台分支** | Linux / macOS / Windows 一级分支；非 Linux 直接走 PATH 模式 |
| **错误码与诊断** | `unprivileged_userns_disabled`、`overlay_unsupported`、`bwrap_not_found` 等统一错误码与提示文本 |

### 12.2 真实差异点（无法消除的部分，约 30%）

下面这些差异**不是实现细节**，是模式语义本身的差异。每条单独说明为什么必须分支处理：

| # | 差异点 | 为什么不能合并 |
|---|--------|---------------|
| 1 | **Bootstrap I/O** | full 需要下载 + 校验 + 解压 base image（curl + sha256 + tar），其他 mode 只 `mkdir`。工作量差 100×，必须独立模块 |
| 2 | **MountPlan 内容** | 4 档的挂载列表、根策略、tmpfs 数量、`/etc` 处理完全不同。不能用一个模板参数化——每档逻辑都需要自己的 builder |
| 3 | **后端能力差异** | unshare 不能做 unprivileged OverlayFS；bwrap 不能做 PID-ns 1（除非 `--as-pid2`）；nspawn 强依赖 systemd。后端各有命令语法和能力盲区，必须有 backend trait 抽象 |
| 4 | **`/etc` 策略表** | heavy / full 需要一份**精心维护**的 `/etc` 路径清单（哪些 A、哪些 H、哪些 stub）。light / medium 完全不碰。这是数据，不是代码——以资源文件存在，每档独立 |
| 5 | **内核能力检测** | light/medium 只查 `unprivileged_userns_clone`；heavy 还要查 OverlayFS 是否支持 unprivileged mount；full(nspawn) 查 systemd 版本。检测项是 mode 的**输入条件**，每档独立 |
| 6 | **Autodegrade 链** | `full → heavy` 没有自然语义（rootfs 不能退成 overlay）；`heavy → medium` 有（丢弃 overlay，留 home/local）；`medium → light` 有；`light → PATH-only` 有。降级矩阵需要逐对定义，不是单一函数 |
| 7 | **Export 格式** | light/medium：tar 整个 subos 目录；heavy：upper 含 character device whiteout，要转 OCI 标准；full：tar `rootfs/`。三种格式，三套打包流水线 |
| 8 | **平台降级路径** | light/medium 在 macOS/Windows 退到 PATH-only 仍可用；heavy/full 没有降级路径——只能拒绝并提示用 Lima/WSL2 |

### 12.3 推荐架构（共享骨架 + 三类策略点）

```
┌────────────────────────────────────────────────────────────┐
│  命令层  subos {new, enter, run, …}                        │  ← 共享
├────────────────────────────────────────────────────────────┤
│  Mode 仲裁  (read .xlings.json → probe → autodegrade)      │  ← 共享
├────────────────────────────────────────────────────────────┤
│  ┌─ Bootstrap 策略 ────┐  ┌─ MountPlan 构建 ──┐            │  ← 策略点
│  │ NoOp (light/med/hv)│  │ LightPlan          │            │
│  │ ImageFetch (full)   │  │ MediumPlan         │            │
│  └────────────────────┘  │ HeavyPlan          │            │
│                          │ FullPlan           │            │
│                          └────────────────────┘            │
├────────────────────────────────────────────────────────────┤
│  Backend Driver 接口  execute(MountPlan)                    │  ← 策略点
│  ┌──────────────┐  ┌───────────┐  ┌──────────────┐          │
│  │ UnshareDriver│  │ BwrapDrv  │  │ NspawnDriver │          │
│  └──────────────┘  └───────────┘  └──────────────┘          │
├────────────────────────────────────────────────────────────┤
│  Export 策略  ┌─ TarPlain ─┐ ┌─ OverlayToOCI ─┐ ┌─ Rootfs ─┐│  ← 策略点
└────────────────────────────────────────────────────────────┘
```

### 12.4 关键抽象：MountPlan

把 4 档差异收敛到一个**声明式数据结构**，让骨架代码只处理数据、后端只消费数据：

```cpp
struct MountPlan {
    enum class Root { Passthrough, Overlay, Rootfs };
    Root root_strategy;

    // overlay
    fs::path overlay_lower;     // 仅 heavy: 通常 "/"
    fs::path overlay_upper;
    fs::path overlay_work;

    // rootfs
    fs::path rootfs_dir;        // 仅 full

    // 通用
    std::vector<BindMount>   binds;     // 源、目标、ro/rw、可选 missing
    std::vector<TmpfsMount>  tmpfs;     // 目标、size、mode
    std::vector<KernelMount> kernel;    // proc / sysfs / devfs / devpts
    std::map<std::string, std::string> env;
    std::optional<std::string> hostname;
    std::vector<std::string> shell_cmd;

    // 后端能力声明（驱动用来判断能不能跑）
    struct Caps {
        bool need_user_ns = true;
        bool need_overlay = false;     // heavy
        bool need_pivot_root = false;  // heavy / full
        bool need_pid_ns = false;
        bool need_uts_ns = false;
    } caps;
};
```

每档实现 `MountPlanBuilder::build(SubosConfig) → MountPlan`：
- `LightPlan::build`：5 个 BindMount，root_strategy=Passthrough
- `MediumPlan::build`：调 LightPlan + 4 个新 bind/tmpfs
- `HeavyPlan::build`：root_strategy=Overlay + procfs/sysfs/devfs + `/etc` 策略表展开
- `FullPlan::build`：root_strategy=Rootfs + 完整 mount 编排

后端实现 `BackendDriver::execute(MountPlan)`：
- 收到 plan 先 `caps_check`，能力不够返回错误（仲裁器据此 autodegrade）
- 能力够则把 plan 翻译成自己的命令格式（unshare 拼 shell、bwrap 拼参数、nspawn 拼参数）
- 启动子进程 + 等待 + 清理

**这一层抽象的核心收益：** 新增 mode 只写 `XxxPlanBuilder`，新增后端只写 `XxxDriver`。骨架不动。

### 12.5 模块划分（代码组织）

```
src/core/subos/
├── subos.cppm                  (现有) — 命令 facade
├── mode.cppm                   (新)   — Mode enum、字符串解析
├── config_v2.cppm              (改)   — .xlings.json 新 schema
├── arbiter.cppm                (新)   — 仲裁器：probe → autodegrade
├── capability.cppm             (新)   — 内核/平台能力探测
├── plan/
│   ├── plan.cppm               (新)   — MountPlan 数据结构
│   ├── plan_light.cppm         (新)
│   ├── plan_medium.cppm        (新)
│   ├── plan_heavy.cppm         (新)
│   └── plan_full.cppm          (新)
├── backend/
│   ├── backend.cppm            (新)   — BackendDriver 接口
│   ├── backend_unshare.cppm    (新)
│   ├── backend_bwrap.cppm      (新)
│   └── backend_nspawn.cppm     (新)   — 可选/晚期
├── bootstrap/
│   ├── bootstrap.cppm          (新)   — Bootstrap 策略接口
│   ├── bootstrap_noop.cppm     (新)   — light/medium/heavy
│   └── bootstrap_image.cppm    (新)   — full（fetch + verify + extract）
├── export/
│   ├── export.cppm             (新)   — Export 策略接口
│   ├── export_tar.cppm         (新)
│   ├── export_overlay.cppm     (新)
│   └── export_rootfs.cppm      (新)
└── etc_policy/
    ├── heavy.toml              (新)   — heavy 模式 `/etc` 处理清单（数据）
    └── full_alpine.toml        (新)   — full+alpine 模式 `/etc` 处理清单
```

### 12.6 各模式专属依赖一览

| 依赖 | light | medium | heavy | full(bwrap) | full(nspawn) |
|------|:----:|:------:|:-----:|:----------:|:----------:|
| `unshare(2)` | ✓ | ✓ | ✓ | — | — |
| `bubblewrap` 二进制 | — | — | ✓ | ✓ | — |
| `systemd-nspawn` | — | — | — | — | ✓ |
| `curl` / `wget` | — | — | — | ✓（fetch） | ✓ |
| `tar` + `zstd`/`gzip` | — | — | — | ✓ | ✓ |
| `sha256sum` | — | — | — | ✓ | ✓ |
| Linux 内核 ≥ 5.11 | — | — | ✓ | (推荐) | (推荐) |
| Linux 内核 ≥ 3.8 | ✓ | ✓ | — | ✓ | ✓ |
| systemd ≥ 252 | — | — | — | — | ✓ |

xlings 启动时探测可用项，配置 `.xlings.json` 时 mode 选择面随之收窄。

### 12.7 渐进引入策略（不要一开始就建全套）

| 阶段 | 抽象层引入程度 | 理由 |
|------|-------------|------|
| **M1**（仅 light） | 只有简单 `EnterContext` 结构 + 一个函数；不引入 MountPlan / BackendDriver 抽象 | 一个 mode 不需要插件化 |
| **M2**（+ medium） | 抽出 `MountPlan` 数据结构；两个 PlanBuilder；仍单后端（unshare） | 两个 mode 已经能看出共性，正好抽数据结构 |
| **M3**（+ heavy） | 引入 `BackendDriver` 接口；新增 BwrapDriver；引入 `Capability` 探测 | 出现第二种后端时，自然需要 trait |
| **M4**（+ full） | 引入 `BootstrapStrategy`、`ExportStrategy`；可能加 NspawnDriver | 这是最复杂的 mode，所有抽象在此被复用 |

**反模式**：在 M1 阶段就把 MountPlan / BackendDriver / BootstrapStrategy / ExportStrategy 全部抽出来——这是为不存在的需求过度设计。**先简单再演化**，每个抽象都被两个以上具体场景验证后再固化。

### 12.8 一句话回答

> **能用一套架构做。**
> 共享：命令层、配置 schema、目录约定、EventStream、Hook、平台检测、Mode 仲裁、Autodegrade 框架。
> 插件化：MountPlan 构建（4 个）、Backend Driver（3 个）、Bootstrap 策略（2 个）、Export 策略（3 个）、`/etc` 处理清单（数据文件）。
> 这些插件点不是"为了优雅"——是模式语义本身的天然分歧线。但骨架是统一的，新增模式或后端不需要动主干。
