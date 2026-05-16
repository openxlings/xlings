# xlings subos 四档隔离细节对比 —— default / medium / heavy / full

> 对前一份 `2026-05-05-subos-tiered-mode-architecture.md` 的修订:把 light 退化为 default(PATH + 环境变量注入,无 namespace),与 medium / heavy / full 形成新的四档梯度。
>
> 本文聚焦"逐维度细节对比";配套的实现路径见 [`2026-05-06-subos-mode-implementation-details.md`](./2026-05-06-subos-mode-implementation-details.md)。

---

## 一、四档定义快照

| | default | medium | heavy | full |
|---|---|---|---|---|
| **一句话** | "PATH + ENV,subos 给宿主看" | "进 mount-ns,有干净的 /usr/local + HOME + /tmp" | "OverlayFS,subos 是宿主的差量" | "独立 rootfs,subos 是迷你 Linux" |
| **隔离单元** | 进程环境变量 | mount namespace | mount + user-ns + overlay + pivot_root | mount + user-ns + UTS [+ PID] + 完整 rootfs |
| **能否退出宿主** | 否(就在宿主里) | 否(只是 mount 视图换了) | 接近"是"(看到的 / 是 overlay) | 是(看到的是另一发行版) |
| **xlings 提供物** | env 注入 + shim 调度 | mount plan + unshare 编排 | mount plan + bwrap 调度 + overlay 配置 | rootfs bootstrap + bwrap/nspawn 调度 + hooks |

---

## 二、隔离强度 —— 逐资源逐档

| 资源 | default | medium | heavy | full |
|---|:-:|:-:|:-:|:-:|
| `$PATH` 工具优先级 | ✓ env | ✓ bind `/usr/local/bin` | ✓ overlay | ✓ rootfs |
| `ld.so` 默认搜 | ✓ `LD_LIBRARY_PATH` | ✓ bind `/usr/local/lib` | ✓ overlay | ✓ rootfs |
| `pkg-config` 默认查 | ✓ `PKG_CONFIG_PATH` | ✓ bind `/usr/local/lib/pkgconfig` | ✓ overlay | ✓ rootfs |
| C/C++ 默认头 | ✓ `CPATH` / `C_INCLUDE_PATH` | ✓ bind `/usr/local/include` | ✓ overlay | ✓ rootfs |
| **硬编码 `/usr/local` 路径**(dlopen / RUNPATH) | ✗ | ✓ | ✓ | ✓ |
| **`$HOME` / dotfile 隔离** | ✗(共享宿主) | ✓ bind 到 `subos/<n>/home` | ✓ | ✓ |
| **`/tmp` 私有** | ✗ | ✓ tmpfs | ✓ tmpfs | ✓ tmpfs |
| `/opt/<name>` | ✗ | ✓ bind | ✓ | ✓ |
| **`/etc` 子集替换**(passwd/nsswitch/profile) | ✗ | ✗ | ✓ etc-overrides | ✓ rootfs 自带 |
| `/usr/lib` / `/usr/bin` 系统库与命令 | 宿主原样 | 宿主原样 | overlay 之上可改 | 完全独立 |
| **替换 libc / glibc 版本** | ✗ | ✗ | ✗(lower=宿主) | ✓ |
| 主机名(UTS) | ✗ | ✗ | ✓ | ✓ |
| PID 视图 | ✗ | ✗ | 可选 | 可选 |
| 网络栈 | 共享宿主 | 共享宿主 | 共享宿主 | 共享宿主(默认) |
| **跨发行版一致** | ✗ | ✗ | 部分(bin/lib 在 lower) | ✓ |

> 网络默认四档全共享,不破坏 pip/apt 等开发体验。需要 net-ns 自行用 docker/podman。

---

## 三、性能与资源开销

| 维度 | default | medium | heavy | full |
|---|---|---|---|---|
| **启动延迟** | ~0(只是 export 几个变量) | < 100 ms(unshare + 4-9 bind) | 200–500 ms(bwrap + overlay 初始化 + procfs/sysfs/devfs) | 500 ms – 2 s(nspawn 更慢) |
| **subos 体积** | 几 KB(只有 `local/*` + xvm 元数据) | 几 MB(home/ 模板 + tmp-seed) | 几 MB ~ 几十 MB(upper 差量) | 10 – 500 MB(完整 rootfs) |
| **常驻内存** | 0 | 0(tmpfs 按需) | tmpfs + overlay 元数据 + bwrap 进程 | overlay/rootfs + 容器运行时进程 |
| **磁盘 inode 占用** | 极低 | 低 | 中(whiteout) | 高(整发行版) |
| **首次 cold 创建** | 即时(`mkdir`) | 即时 | 即时 | 数秒 ~ 数十秒(下载 + 解压 base) |
| **每次 enter 的 fork 数** | 0(就在当前 shell) | 1 | 2-3(unshare + bwrap + 子 shell) | 3+ |

---

## 四、内核 / 平台兼容性

### 4.1 Linux 兼容矩阵

| 内核 / 平台 | default | medium | heavy | full(bwrap) | full(nspawn) |
|---|:-:|:-:|:-:|:-:|:-:|
| Linux ≥ 5.11 + user-ns 开放 | ✓ | ✓ | ✓ | ✓ | ✓ |
| Linux 3.8–5.10 + user-ns 开放 | ✓ | ✓ | ✗(降 medium) | ✓ | 需 root |
| Linux 无 unprivileged user-ns(老 RHEL ≤8) | ✓ | ✗(自动回退 default) | ✗ | 需 root | 需 root |
| Ubuntu 24+ AppArmor 限 user-ns | ✓ | 提示开关 | 提示开关 | 提示开关 | 提示开关 |

### 4.2 跨平台兼容矩阵

| 平台 | default | medium | heavy | full |
|---|:-:|:-:|:-:|:-:|
| Linux | ✓ | ✓ | ✓ | ✓ |
| macOS | ✓(完全等同) | ✗(自动降 default) | ✗(建议 Lima) | ✗(建议 Lima/colima) |
| Windows | ✓ | ✗(自动降 default) | ✗(建议 WSL2) | ✗(建议 WSL2) |

> **默认档是唯一三平台行为对称的**。其它三档在非 Linux 上要么降级要么拒绝 —— 这意味着 default 始终是兜底。

### 4.3 外部依赖

| 二进制 | default | medium | heavy | full(bwrap) | full(nspawn) |
|---|:-:|:-:|:-:|:-:|:-:|
| `unshare(2)` | — | ✓ | ✓ | — | — |
| `bubblewrap` | — | — | ✓ | ✓ | — |
| `systemd-nspawn` | — | — | — | — | ✓ |
| `curl` / `wget` | — | — | — | ✓ | ✓ |
| `tar` + `zstd`/`gzip` | — | — | — | ✓ | ✓ |
| `sha256sum` | — | — | — | ✓ | ✓ |
| systemd ≥ 252 | — | — | — | — | ✓ |

---

## 五、实现成本(C++ 主体 LOC 估算,不含 Lua/shell)

| 模块 | default | medium | heavy | full |
|---|:-:|:-:|:-:|:-:|
| 命令注册(new/use/enter/run/info/list/remove) | 100(已有) | +50(新加 enter/run) | +50 | +50 |
| 平台检测 + 降级 | 50(已有) | +40 | +60 | +60 |
| 目录约定与初始化 | 50(已有) | +50 | +120 | +200 |
| 启动编排 | 30(env 注入) | +120 | +200 | +250 |
| Bootstrap(下载 + 校验 + 解压) | 0 | 0 | 0 | +200 |
| Hook / 退出清理 | 30(已有) | +50 | +80 | +100 |
| **小计(增量)** | **≈260(基本已有)** | **+310** | **+510** | **+860** |

> **default 几乎复用现有代码**,改动是:`xlings subos use` 在 export PATH 时多写几行 env;`subos.cppm` 增 `local/` 子树约定。即便算入这部分新增,也不超过 80 行。
>
> **medium 是真正的"新增最大单档"**:首次引入 mount-ns、首次需要 caps probe + autodegrade、首次写降级链。后面 heavy/full 主要是在 medium 的骨架上扩展。

---

## 六、用户认知模型 / UX

| | default | medium | heavy | full |
|---|---|---|---|---|
| **比喻** | "PATH 加强版 + 库找得到" | "干净的开发盒" | "宿主的克隆体" | "迷你 Linux 发行版" |
| **学习曲线** | 0(就是现在的 xlings) | ~15 min | ~1 h | 半天 |
| **最大惊讶来源** | 几乎无;偶尔遇到硬编码 `/usr/local` 路径不灵 | 私有 HOME 后 ssh-key/git config 看不到 | upper 差量丢失语义;whiteout | base image 装 apt 后 GUI/音频不通 |
| **典型命令** | `xlings use python@3.12` | `xlings subos enter dev` | `xlings subos enter wild`<br>`xlings subos discard wild` | `xlings subos new lab --mode full --base alpine:3.19` |
| **退出语义** | 不需要"退出";关 shell 即可 | `exit` 子 shell | `exit` 子 shell + bwrap 自销 | `exit` + 容器自销 |
| **可观测性** | 宿主直接看到 `subos/<n>/local/*` | bind 视图,宿主仍能直接读源目录 | overlay,upper 是裸目录可读 | rootfs,完全独立 |

---

## 七、Bootstrap 与生命周期

### 7.1 创建(new)成本

| | default | medium | heavy | full |
|---|---|---|---|---|
| **I/O 量** | mkdir 几个目录 | mkdir + 拷贝 tmp-seed 模板(可选) | mkdir + 写 etc-overrides 模板 | 下载 base(3 MB Alpine ~ 80 MB Arch) + 校验 + 解压 |
| **网络** | 不需要 | 不需要 | 不需要 | **必须**(除非有本地缓存) |
| **失败模式** | 几乎不会失败 | 同 default | overlay caps 检测失败 → 降 medium | 下载 / 解压 / 架构不匹配 |
| **典型耗时** | < 100 ms | < 200 ms | < 500 ms | 5 s ~ 1 min(随 base 大小) |

### 7.2 销毁(remove)成本

| | default | medium | heavy | full |
|---|---|---|---|---|
| **删除路径** | `local/*` + xvm/* | + home/ + opt/ + tmp-seed/ | + upper/ + work/ + etc-overrides/ | + rootfs/ |
| **chattr / 特殊文件** | 无 | 无 | upper 含 character device whiteout(一般 `rm -rf` 仍能删) | rootfs 内可能含 setuid / 设备节点(同上) |
| **典型耗时** | < 1 s | < 2 s | < 5 s | 10 s ~ 1 min |

### 7.3 Hook 系统

四档共用同一组 hook(`pre-enter.sh` / `post-enter.sh`),钩子文件位于 `subos/<n>/hooks/`。default 档没有"enter"动作,hook 退化为"在 `xlings subos use <n>` 后 source 一次 init.sh"。

---

## 八、IDE / 工具链集成

这是经常被忽视但用户最敏感的维度。

| | default | medium | heavy | full |
|---|---|---|---|---|
| **VSCode / Cursor 直接打开能用** | ✓(IDE 在宿主,看到 subos `local/*` 就在硬盘上) | ⚠️ 需 IDE 经 `xlings subos run -- code .` 才能继承 mount-ns 视图 | ⚠️ 同 medium | ⚠️ 需 remote-container / SSH 远程模式 |
| **clangd / cmake 找头** | ✓ via `CPATH` / `compile_commands.json` | ✓ 在 namespace 内自然找到 | ✓ | ✓(在 rootfs 内) |
| **debugger(gdb/lldb)** | ✓ | ⚠️ pid-ns 关闭时可附加宿主进程,开了不行 | ⚠️ 同上 | ⚠️ 需进入 rootfs |
| **language server 索引** | ✓ 直接索引 `local/include` | ✓ 索引宿主路径,bind 镜像即可 | ⚠️ overlay merged 视图复杂 | ⚠️ 完全独立 sysroot,clangd 需 `--sysroot` |
| **CI 一键复用** | ✓ env 一致即可 | ✓ enter 后跑 | ✓ | ✓(import 镜像) |

> **IDE 友好度的折点在 default → medium 之间**。一旦进 namespace,IDE 集成就需要额外配置或 wrapper。这是把 default 留下来作为独立 tier 的最强 UX 论据。

---

## 九、跨发行版可移植性

| | default | medium | heavy | full |
|---|---|---|---|---|
| **subos 在另一台同发行版机器** | ✓(只要装了同 xpkg) | ✓ | ✓ | ✓ |
| **subos 在另一台不同发行版** | ✗(二进制依赖宿主 libc) | ✗ | ⚠️(lower 是宿主,行为可能差) | ✓ |
| **subos 跨架构(x86 → arm)** | ✗ | ✗ | ✗ | ⚠️(rootfs 必须匹配架构) |
| **典型分发产物大小** | xpkg 元数据级 | < 100 MB tar | < 200 MB tar | 50 MB ~ 1 GB tar.zst |

---

## 十、可逆性 / 清理 / 导出

| 操作 | default | medium | heavy | full |
|---|---|---|---|---|
| **退出后宿主有残留?** | 无(就是宿主原样) | 无(namespace 销毁,bind 不见) | 无(同 medium) | 无(同 medium) |
| **想清空"折腾的痕迹"** | 重装 xpkg 即可 | 删 `home/`、`opt/`、tmpfs 即可 | `subos discard <n>`(清空 upper)→ 回到差量起点 | 删 rootfs 重 bootstrap |
| **导出格式** | tar(`local/` + 元数据) | tar(整目录) | OCI(转换 whiteout)或 tar(upper 自描述) | tar(整 rootfs) |
| **跨机器 import** | 简单 | 简单 | 中等(bwrap 版本敏感) | 复杂(需匹配 base + 架构) |

---

## 十一、升级路径

```
default ──→ medium ──→ heavy ──→ full
   ↑             ↑          ↑           ↑
   |             |          |           +─ 必须 bootstrap base image,无法回退
   |             |          +─ upper 升 rootfs:apply diff + base
   |             +─ 加 home/opt/tmpfs;local/ 原样保留
   +─ 加 mount-ns;env 注入仍生效(作为 fallback)
```

| 升级动作 | 数据是否丢 | 是否需要重启 | 是否可逆 |
|---|---|---|---|
| `default → medium` | 不丢(local/* 原样) | 不需要 | 可逆(降回 default 即丢 mount-ns 配置,数据保留) |
| `medium → heavy` | 不丢(home/opt 进 upper) | 不需要 | 不建议(降级会丢 upper 改动;`--keep-data` 归档) |
| `heavy → full` | upper 烧入 rootfs(差量被吸收) | 必须重新 bootstrap | **不可逆** |

---

## 十二、适用场景 vs 反场景

| 场景 | 推荐档 | 理由 |
|---|:-:|---|
| 切 gcc / python / node 版本 | **default** | env 注入 + shim 已经够;不需要 namespace |
| 多项目并行、互不干扰 PATH | **default** | 切 subos = 切 PATH |
| 装包不污染宿主 `/usr/local` | **default** | xpkg --in 已落到 `subos/<n>/local/` |
| 想要硬编码 `/usr/local/lib` 的 wheel 也能正确 dlopen | **medium** | 必须 bind |
| 学新框架不想污染 `~/.cache` | **medium** | 私有 HOME |
| 教学:学生独立环境 | **medium** 或 full(Alpine) | 视是否需要不同发行版 |
| 实验性:装一堆包后想清干净 | **heavy** | discard upper |
| 修改 `/etc` 验证配置变更 | **heavy** | etc-overrides |
| 复现 CI 跨机器 | **full** | rootfs 一致 |
| 跨发行版交付作业 | **full** | 唯一可移植到不同发行版 |
| 调研旧 glibc 行为 | **full** | 唯一能换 libc |
| 需要不同 `/bin/sh` 实现(dash vs bash vs busybox) | **full** | 其它档 `/bin` 都是宿主 |

| 反场景 | 不要用 | 改用 |
|---|---|---|
| "我只是想切个 python 版本" | medium / heavy / full | **default** |
| "我想跨平台一致" | medium / heavy / full | **default** |
| "我要 GPU/音频/X11 紧耦合" | full | medium 或 heavy |
| "我要替换 libc" | medium / heavy | **full** |
| "我要部署到生产" | 任何 subos | docker / podman / k8s |

---

## 十三、风险与降级

| 风险 | default | medium | heavy | full |
|---|---|---|---|---|
| **AppArmor 阻断 user-ns(Ubuntu 24+)** | 无影响 | 启动失败 → 自动降 default + 提示 sysctl | 同 medium | 同 medium |
| **OverlayFS 老内核不支持** | — | — | 启动失败 → 降 medium | — |
| **bwrap 未装** | — | — | 启动失败 → 降 medium 并提示 `apt install bubblewrap` | 同 heavy(若用 bwrap 后端) |
| **下载 base image 失败** | — | — | — | 创建失败,可重试或换源 |
| **私有 HOME 后 ssh-key 不见** | — | 提供 `--share-ssh` 选项 | 同 | 同 |
| **私有 /tmp 后 X11 socket 看不到** | — | tmp-seed 注入 `/tmp/.X11-unix` 链接 | 同 | 同 |
| **DNS 切网后失效** | 无影响(共享宿主) | 无影响 | resolv.conf 用 ro-bind,切网需重进 | 同 |
| **subos 体积爆炸** | 几乎不会 | 不会 | upper 长期累积可能涨 | rootfs 几百 MB,需 prune |

**降级链(autodegrade):**

```
full   ─x── (无自然降级,full 只能拒绝并报错)
heavy  ──→ medium  (丢 overlay,留 home/local)
medium ──→ default (丢 mount-ns,只剩 env 注入;数据保留)
default ─x── (这就是底,不再降)
```

---

## 十四、总览矩阵 + 决策树

### 14.1 一表收尾

| 维度 | default | medium | heavy | full |
|---|:-:|:-:|:-:|:-:|
| 隔离面 | env 4-5 个变量 | mount(5-9 个 bind + tmpfs) | overlay + 完整 mount 编排 | 独立 rootfs + 完整 mount |
| 启动延迟 | ~0 | < 100 ms | 200–500 ms | 500 ms – 2 s |
| subos 体积 | KB | MB | MB(差量) | 10–500 MB |
| 内核要求 | 任何 | unprivileged user-ns | + unprivileged overlay(≥5.11) | + bwrap 或 nspawn |
| 跨发行版 | ✗ | ✗ | 部分 | ✓ |
| macOS/Windows | ✓ | ✗ | ✗ | ✗ |
| 替换 libc | ✗ | ✗ | ✗ | ✓ |
| `/etc` 配置 | ✗ | ✗ | ✓ | ✓ |
| 私有 HOME/tmp | ✗ | ✓ | ✓ | ✓ |
| 硬编码 `/usr/local` 兼容 | ✗ | ✓ | ✓ | ✓ |
| 外部依赖 | 无 | 无 | bwrap | bwrap **或** nspawn + curl + tar + sha256 |
| 实现增量 LOC | ~80(在已有基础上) | +310 | +510 | +860 |
| IDE 直接友好 | ✓ | 需 wrapper | 需 wrapper | 需 remote |
| 用户学习曲线 | 0 | 15 min | 1 h | 半天 |
| 适配比例(经验估) | 80% | 15% | 4% | 1% |

### 14.2 决策树(收敛后)

```
你需要不同 libc 或不同发行版?
├── 是 ──────────────────────────→ full
└── 否
    └── 你需要修改 /etc 或丢弃式实验?
        ├── 是 ────────────────────→ heavy
        └── 否
            └── 你在意 dotfile / /tmp 隔离,或工具硬编码 /usr/local 路径?
                ├── 是 ─────────────→ medium
                └── 否 ─────────────→ default(默认)
```

### 14.3 推荐的 CLI 投影

```bash
xlings subos new <n>                     # default(默认)
xlings subos new <n> --mode medium       # 进 mount-ns
xlings subos new <n> --mode heavy        # overlay
xlings subos new <n> --mode full --base alpine:3.19
xlings subos upgrade <n> --mode <next>   # 单向推升
```

---

## 关键判断

1. **default 是"白送"档**:复用 PATH/shim/xvm/xpkg --in 已有能力,只多 export 几个环境变量,80 行 C++ 内搞定;同时它是 macOS/Windows 的唯一选择,所以无论如何都得有。**作为命名独立的 tier 比"叫 light"更诚实** —— 它本来就是 xlings 现在的常态。
2. **medium 是真正的"分水岭"**:首次引入 mount-ns、首次需要内核能力探测和 autodegrade、首次有 IDE 集成成本。一切复杂度从这里起步。如果用户场景里很少触发"硬编码 `/usr/local` 路径不灵"或"私有 HOME",这一档可能也只是 4-5% 用户用到。
3. **heavy 的位置最尴尬**:对老内核(< 5.11)不可用,对 macOS/Windows 不可用,对教学场景又不如 full(Alpine)直观。它的甜点是"我想试着改宿主某些东西又能丢回去" —— 这个需求有,但人群很窄。**可以放进 M3+ 的 PoC 阶段,看真实反馈再决定是否进主线**。
4. **full 是战略品**:体积大、bootstrap 复杂,但解决的问题(跨发行版分发、教学独立环境、libc 调研)是其它档完全做不到的。值得有,但建议 M5+ 才上,且必须先把 default + medium 打磨稳。
5. **IDE 集成的折点 = default → medium 边界**。这条边界本身值得在产品文档里显著标记 —— 用户决定"要不要上 medium"时,IDE 友好度的下降可能比性能或复杂度更影响选择。

---

## 落地优先级(更新版 Roadmap)

| 里程碑 | 内容 | 估期 | 触发条件 |
|---|---|---|---|
| **M0** | default 完善 —— 在 `xlings subos use` 中 export `LD_LIBRARY_PATH` / `CPATH` / `PKG_CONFIG_PATH` / `CMAKE_PREFIX_PATH`;subos 目录补 `local/{bin,lib,include,share,libexec}` 约定;xpkg 装包默认落 `local/` | 1 周 | 立即 |
| **M1** | medium —— `enter`/`run` 命令 + unshare 编排 + caps probe + 自动降 default | 3-4 周 | M0 落地后 |
| **M2** | heavy PoC(独立分支),验证 bwrap overlay 路径与降级 | 2 周 | 有 ≥3 用户提"想要 discard 式实验" |
| **M3** | full bootstrap(下载 + 校验 + 解压 + meta) | 3-4 周 | xlings 战略明确"做开发环境分发" |
| **M4** | full 启动 + hooks + export/import | 2-3 周 | M3 完成 |
| **M5** | nspawn 后端(可选) | 2 周 | 出现 CI / 教学用户提需求 |
