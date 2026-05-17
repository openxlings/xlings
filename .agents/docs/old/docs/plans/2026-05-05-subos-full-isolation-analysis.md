# xlings subos 全量隔离方案分析（xlings 自维护 OS 环境）

> 与 `subos-mount-namespace-analysis.md` 的"轻量选择性挂载"互为补集。
> 本文聚焦另一条路线：**除内核接口外，整个 `/` 下面都由 xlings 接管**——subos 成为完整的"OS 视图"，是 xlings 自己维护的发行版式隔离环境。

---

## 1. 设计意图重新表述

把 subos 从"工具集合"升级为"完整运行视图"：

```
host /           subos 视图 /
─────────        ──────────
/bin     ──┐     /bin           ← subos 自带 sh、coreutils、busybox …
/sbin    ──┤     /sbin
/usr/bin ──┼──→  /usr/bin       ← xlings 维护的工具链、shim
/usr/lib ──┤     /usr/lib       ← subos 自带的 glibc / musl / libstdc++
/lib*    ──┤     /lib*
/etc     ──┤     /etc           ← subos 自己的 passwd、ld.so.conf、ssl/certs …
/home    ──┤     /home          ← subos 私有用户数据
/var     ──┤     /var
/opt     ──┤     /opt
/usr/local       /usr/local

/proc    ──→     /proc          ← namespace 内重新 mount 的 procfs
/sys     ──→     /sys           ← bind from host（只读）
/dev     ──→     /dev           ← bind from host（或 devtmpfs）
/run     ──→     /run           ← 大部分 fresh tmpfs；少量 bind（X11/wayland 套接字）
```

效果：用户 `xlings subos enter dev` 后，`ls /` 看到的是 subos 维护的世界，宿主完全不可见（除了内核接口）。**这就是一个轻量的、xlings 自己管理的容器**。

---

## 2. 完整路径清单：每条路径的处置

下表是 FHS 标准路径完整列表。每条路径标注**来源**：

- **A**（subos 自有）= bind subos 的对应子目录
- **H**（继承宿主）= bind 宿主的对应路径进 namespace（通常 ro）
- **F**（fresh）= namespace 内新挂载（procfs/sysfs/tmpfs）
- **S**（跳过）= 不处理（subos 内若不存在就保持空）

| 路径 | 来源 | 说明 |
|------|------|------|
| `/bin` | A | 必须有 `sh`、`ls`、`cat`、`mv`、`cp`、`rm`、`ln`、`mkdir`、`echo`、`env` |
| `/sbin` | A | `mount`、`umount`、`ip`、`route`（多数现代发行版只是 `/usr/sbin` 的 symlink） |
| `/usr/bin` | A | 主体二进制（绝大部分发行版的命令都在这） |
| `/usr/sbin` | A | 系统命令 |
| `/usr/lib` | A | **关键**：`libc.so.6`、`ld-linux-*.so.2`、`libpthread`、`libdl`、`libm`、`libstdc++`、`libgcc_s`、NSS 模块（`libnss_files`、`libnss_dns`） |
| `/usr/lib64` | A | 同上（64-bit multilib） |
| `/lib` | A 或 symlink | 现代发行版多数是 `/usr/lib` 的 symlink（merged-/usr） |
| `/lib64` | A 或 symlink | 同上 |
| `/usr/include` | A | 头文件 |
| `/usr/share` | A | locale 数据、terminfo、zoneinfo、CA 证书（部分发行版在此）、man、doc |
| `/usr/local` | A | 与轻量方案一致 |
| `/etc` | A + 选择性 H | 见 §3 |
| `/home` | A 或 H | 选择题：每 subos 私有 `/home` vs 共享宿主 `/home` |
| `/root` | A | subos 的 root home |
| `/opt` | A 或 S | 第三方包 |
| `/srv` | S 或 A | 多数场景留空 |
| `/mnt` | S | 留空 |
| `/media` | S | 留空 |
| `/tmp` | **F**（tmpfs） | namespace 私有 tmpfs；进程退出销毁 |
| `/var` | A + 部分 F | `/var/log`、`/var/cache` 走 A；`/var/run` → F (tmpfs) |
| `/run` | **F** + 选择性 H | namespace 内默认 fresh tmpfs；可选 bind `/run/user/$UID/{wayland-*,pulse,bus}` 让 GUI/音频可用 |
| `/proc` | **F** | namespace 内 `mount -t proc proc /proc` |
| `/sys` | H | bind 宿主 `/sys`（只读）；想完全隔离需 sysfs namespace + cgroup |
| `/dev` | H 或部分 F | bind 宿主 `/dev`（rw 共享）；或 mount devtmpfs + 创建少数节点（null/zero/full/random/urandom/tty/ptmx + /dev/pts） |
| `/boot` | S | namespace 看到无所谓 |
| `/lost+found` | S | 跳过 |

---

## 3. `/etc` 必须细分（最大坑点）

`/etc` **不能整体 A**——subos 不可能维护 DNS 解析、用户表、时区这些动态信息。需混合处理：

| `/etc` 子条目 | 来源 | 原因 |
|--------------|------|------|
| `/etc/passwd`、`/etc/group`、`/etc/shadow` | A（stub） | subos 维护一个最小用户表（含 root + 当前用户），否则 `id`、`whoami`、shell prompt 全坏 |
| `/etc/nsswitch.conf` | A | 必须存在，否则 glibc nss 报错 |
| `/etc/resolv.conf` | **H**（bind ro） | DNS 必须用宿主的，否则 namespace 内无法上网 |
| `/etc/hosts` | A 或 H | 通常 H（共享宿主自定义 hosts） |
| `/etc/hostname` | A | subos 可设独立主机名 |
| `/etc/localtime` | H | bind 宿主，时区一致 |
| `/etc/ssl/certs/`、`/etc/pki/tls/certs/`（CA 证书） | A 或 H | A：subos 自带 ca-certificates 包；H：直接用宿主，简单可靠 |
| `/etc/ld.so.conf`、`/etc/ld.so.conf.d/`、`/etc/ld.so.cache` | A | 必须 subos 自己的，否则动态库搜索路径错乱 |
| `/etc/profile`、`/etc/profile.d/`、`/etc/bash.bashrc` | A | shell 启动脚本 |
| `/etc/environment` | A | 环境变量 |
| `/etc/locale.gen`、`/etc/locale.conf`、`/etc/default/locale` | A | locale 配置 |
| `/etc/pam.d/`、`/etc/security/` | A 或 S | 仅当 subos 内跑 sudo/su 才需要 |
| `/etc/sudoers` | S | 不建议；subos 不应要 sudo |
| `/etc/systemd/`、`/etc/init.d/` | S | subos 不跑 init |
| `/etc/fstab` | S | namespace 内不挂任何卷 |
| `/etc/machine-id` | A | 可与宿主不同 |
| `/etc/mtab` | symlink → `/proc/self/mounts` | 标准做法 |

**实现建议**：在 namespace 启动脚本里先 `mount --bind subos/etc /etc`，再针对动态条目 `mount --bind /host-original/etc/resolv.conf /etc/resolv.conf` 等分别覆盖。

---

## 4. 三种实现 Flavor 对比

### Flavor A：完整 mini-distro（重）

subos 是一个完整的 rootfs（debootstrap、alpine bootstrap、archlinux pacstrap）。

```
xlings subos new dev --base alpine:3.19
  → 下载/解包 alpine minirootfs.tar.gz 到 $XLINGS_HOME/subos/dev/
  → 完整目录树：/bin /sbin /usr /lib /etc /home /var ...
  → 自带 apk / dpkg / pacman 包管理
xlings subos enter dev
  → unshare + bind 整棵树 + procfs/sysfs/devfs + 选择性 /etc 覆盖
  → exec sh
```

**评估**：
- 优点：真·可复现；同一 subos 在 Ubuntu/Fedora/Arch 宿主上行为一致；可作为"产物"分发；版本完全独立（subos 内可装 glibc 2.39，宿主用 2.35）。
- 缺点：体积大（Alpine ≥5 MB 解压后 ~10 MB；Debian-slim ≥30 MB 解压后 ~80 MB）；要依赖 debootstrap/skopeo 等 bootstrap 工具；本质是再造容器运行时。
- 与已有方案重合度：和 docker/podman/lxc/nspawn/bwrap 高度重叠。

### Flavor B：OverlayFS 混合（中）

下层（lowerdir）= 宿主 `/`，上层（upperdir）= subos。在 namespace 内挂 overlay 后看到合并视图。

```
mount -t overlay overlay \
  -o lowerdir=/,upperdir=$SUBOS/upper,workdir=$SUBOS/work \
  /merged
chroot /merged
# 或 pivot_root，再处理 /proc /sys /dev
```

**评估**：
- 优点：subos 体积小（只装差异部分）；用户感知是"完整 / 视图但有 subos 修改"；写入自动落入 upperdir。
- 缺点：OverlayFS 对 user namespace 的支持取决于内核版本（5.11+ 支持 unprivileged overlay；老内核要 root）；语义复杂（whiteout、redirect、metacopy）；删除文件留 character device whiteout。
- **xlings 的真正甜点**——既有完整 OS 视图，又能"差量"维护 subos。

### Flavor C：选择性全 bind（轻+激进）

subos 自带骨架（`/bin/sh`、glibc、coreutils），其他空着；需要时 xlings 从宿主"复制注入"。

```
xlings subos populate dev --from-host coreutils glibc bash
  → 把宿主的对应文件 cp/hardlink 进 subos/<name>/{usr/bin,usr/lib,…}
xlings subos enter dev
  → bind 完整 subos 树到 / 下
```

**评估**：
- 优点：起步快、可控、不依赖外部 bootstrap。
- 缺点：复制宿主文件的可移植性差（subos 在 Ubuntu 宿主上做的，搬到 Fedora 宿主可能跑不起来）；维护成本高（要追踪每个二进制的依赖闭包）。

---

## 5. 实施细节（Flavor B 推荐路径）

### 5.1 命令设计

```
xlings subos new dev --mode isolated [--base alpine:3.19 | --base host-overlay]
xlings subos enter dev
xlings subos run dev -- <cmd>
xlings subos commit dev          # 把 upper 层固化为基线
xlings subos export dev > tar    # 导出整 subos
```

### 5.2 namespace 编排（伪代码）

```bash
unshare --user --map-root-user \
        --mount --propagation private \
        --pid --fork \
        --uts \
        bash <<'NS'
  set -e
  SUBOS=$XLINGS_SUBOS_DIR

  # 1. 在 namespace 内构造 / 视图
  mount --make-rprivate /
  mkdir -p /tmp/xlings-newroot

  # Flavor B: overlay
  mount -t overlay xlings-overlay \
    -o lowerdir=/,upperdir=$SUBOS/upper,workdir=$SUBOS/work \
    /tmp/xlings-newroot

  # 或 Flavor A: 直接挂 subos 全树
  # mount --bind $SUBOS/rootfs /tmp/xlings-newroot

  cd /tmp/xlings-newroot

  # 2. 内核接口
  mkdir -p proc sys dev run tmp
  mount -t proc proc proc
  mount --rbind /sys sys && mount --make-rslave sys
  mount --rbind /dev dev && mount --make-rslave dev
  mount -t tmpfs tmpfs tmp
  mount -t tmpfs tmpfs run

  # 3. 必须共享的运行时数据
  mount --bind /etc/resolv.conf etc/resolv.conf
  mount --bind /etc/localtime   etc/localtime
  [ -d /run/user/$UID ] && mount --rbind /run/user/$UID run/user/$UID  # X11/wayland/pulse

  # 4. pivot_root
  mkdir -p old_root
  pivot_root . old_root
  cd /
  umount -l /old_root
  rmdir /old_root

  # 5. 进入 subos shell
  hostname xlings-$XLINGS_SUBOS_NAME
  export PATH=/usr/local/bin:/usr/bin:/bin
  exec /bin/sh -l
NS
```

### 5.3 启动序列关键点

- **顺序敏感**：先 user-ns（拿到 namespace 内 root），再 mount-ns 准备 / 视图，再 pivot-ns 切根。
- **`pivot_root` vs `chroot`**：pivot_root 更彻底（namespace 内根 inode 真的换掉，`/proc/self/root` 也变），强烈推荐。
- **propagation=private**：避免 namespace 内 mount 泄回宿主；同样宿主新挂载也不进 namespace。
- **`/proc` 必须重新挂载**：bind 宿主 `/proc` 在 user namespace 内字段会受限（`/proc/<pid>/`、`/proc/cmdline`）。

### 5.4 内核要求与权限

| 能力 | 内核要求 | 限制场景 |
|------|---------|---------|
| `unshare --user` | unprivileged user namespace 启用 | RHEL 8 默认关，Ubuntu 24 AppArmor 默认拦截，Debian 12 同 |
| `unshare --mount` | 所有 ≥3.8 内核 | 通用 |
| `pivot_root` 在 user-ns 内 | 5.0+ | 老内核要 root 才能 pivot |
| OverlayFS 在 user-ns 内 | 5.11+ | 老内核必须 root mount |
| `mount --rbind /sys` 只读 | 通用 | 需 `--make-rslave` 防止传播 |
| 创建 `/dev/pts` 私有 | 通用 | `mount -t devpts -o newinstance,...` |

**回退策略**：
- 内核不支持 unpriv user-ns → 退到 chroot（要 sudo）或 PATH 模式。
- 不支持 unpriv overlay → 退到 Flavor A（完整 rootfs，纯 bind）。
- macOS/Windows → 退到 PATH 模式或建议用 Lima/WSL2。

---

## 6. 与"轻量方案"的差异（重新对照）

| 维度 | 轻量（仅 `/usr/local/*`） | 全量（本文方案） |
|------|-------------------------|----------------|
| 隔离强度 | 弱（共享宿主 libc 和 PATH） | 强（独立 libc、独立 /etc、独立 /home） |
| 体积 | 几 MB | Flavor A: 50–500MB；Flavor B: 几 MB upper |
| 启动复杂度 | 4 个 bind | 20+ 步 mount + pivot_root + procfs |
| 可复现性 | 弱（依赖宿主二进制） | 强（独立 rootfs） |
| 跨发行版可移植 | 否 | **是**（核心收益） |
| 跨 macOS/Windows | 不支持 namespace 即降级 | 不支持 namespace 即不可用 |
| 与 Docker 重叠度 | 低（更像 nix-shell） | 高（更像 docker run） |
| 用户认知模型 | "增强 PATH" | "进入一个轻量 OS" |
| IDE 集成 | LSP/IDE 留宿主，subos 仅给 build | 进 subos = 整套环境，IDE 也要进 |

---

## 7. 风险清单（全量方案专属）

| 风险 | 影响 | 应对 |
|------|------|------|
| **glibc 版本不匹配** | subos 内 glibc 太新，宿主内核太老（`unsupported syscalls`） | 启动前检测内核版本 vs subos 内 `ld.so --version`，不兼容时报错 |
| **NSS 模块缺失** | `getpwnam` 返 NULL，`whoami` 报错 | subos 必须包含 `libnss_files.so.2`、最小 `/etc/passwd` |
| **CA 证书过期** | subos 内 `curl https://...` 失败 | bind 宿主 `/etc/ssl/certs` 或定期更新 ca-certificates |
| **DNS 失败** | bind `resolv.conf` 后宿主切换网络（VPN）不感知 | 用 `/etc/resolv.conf` symlink 到 `../run/systemd/resolve/stub-resolv.conf`，或动态检测重新 bind |
| **GUI/音频失效** | subos 内开 GUI 找不到 X11/wayland/pulse 套接字 | bind `/run/user/$UID`、`/tmp/.X11-unix`、`$XAUTHORITY` |
| **GPU 不可用** | NVIDIA/Mesa 驱动用户态库版本必须与内核驱动匹配 | bind 宿主 `/usr/lib/x86_64-linux-gnu/libGL*` 等 GPU 库；或不在 subos 跑 GPU |
| **systemd 用户服务不可达** | 套接字在宿主 `/run/user/$UID/systemd/`，不 bind 就丢 | 选择性 bind |
| **SELinux/AppArmor label 不匹配** | namespace 内访问被拒 | 文档列出，不在 hardened 系统里默认启用 |
| **PID/UTS 未隔离副作用** | 进 PID-ns 后看不到宿主进程，调试不便 | PID-ns 改为可选；UTS-ns 给独立 hostname |
| **/dev 共享带来安全问题** | namespace 内进程能写宿主 /dev/sda | 改 devtmpfs 私有 + 白名单设备节点 |
| **subos 体积膨胀** | 每个 subos 几百 MB | Flavor B 用 overlay；Flavor A 用硬链接去重 |
| **磁盘 inode 用尽** | 多个 Flavor A subos 共解压百万小文件 | 推荐 reflink (XFS/btrfs) 或 overlay 共享 lower |
| **回滚不可用** | namespace 内修改即 subos 文件被改 | overlay upper 独立目录 + `subos commit/discard` |
| **退出后清理不彻底** | 异常退出留下挂载残留 | namespace 进程退出内核自动 GC；只需保证 upper 不锁定 |

---

## 8. xlings 应不应该走全量隔离？关键判断

**支持全量的论据：**

1. **真正解决发行版差异**——用户在 Arch 上开发的 subos，搬到 Ubuntu CI 行为一致。这是 PATH/轻量方案做不到的。
2. **教学场景天然契合**——xlings 面向开发学习者，"给学生一个干净的 Linux 实验环境"是核心需求。
3. **subos 可作为"作业产物"**——`xlings subos export` 把整套环境打包，老师/学生互发。

**反对全量的论据：**

1. **重复造容器**——Docker、podman、systemd-nspawn、bwrap、distrobox、toolbx 全部已有；xlings 再做一层，差异化在哪？
2. **macOS/Windows 退化为不可用**——xlings 是跨平台工具，全量方案在非 Linux 上完全不工作（不像轻量方案降级到 PATH 仍可用）。
3. **维护成本指数级上升**——bootstrap、CA、DNS、GPU、systemd-user、GUI 这些坑要逐个填。
4. **用户心智模型变了**——subos 从"工具集合"变成"小操作系统"，学习曲线陡升。

**我的判断：xlings 不应一步到位走全量隔离，但应该把架构留出"全量"的口子。**

具体地：

```
轻量基础（/usr/local/* 覆盖）
        ↓ 用户 opt-in 升级
中量（独立 /home + 独立 /opt + tmpfs /tmp）
        ↓ 用户 opt-in 升级
重量（OverlayFS 完整视图）
        ↓
全量（独立 rootfs，本文 Flavor A）
```

让用户按需付出复杂度成本。`xlings subos new` 加个 `--mode {light,medium,heavy,full}` 参数即可。

---

## 9. 推荐结论

如果一定要做全量隔离，**采取 Flavor B（OverlayFS 混合）作为主路径**：

- 体积可控（subos 只存差量）
- 用户视图完整（看起来像独立 OS）
- 与 xlings 已有目录约定兼容（subos `upper/` 子目录即 overlay upperdir）
- 回滚/导出/分发都自然（fix `upper/` 即固化版本）

**绝对要做的内核接口处理：**

| 路径 | 做法 |
|------|------|
| `/proc` | namespace 内 `mount -t proc proc /proc` |
| `/sys` | `mount --rbind /sys` + `--make-rslave`，只读 |
| `/dev` | `mount --rbind /dev` + `--make-rslave` 或私有 devtmpfs |
| `/run` | tmpfs；选择性 bind `/run/user/$UID` |

**最低限度的宿主直通（不能 subos 自维护的）：**

- `/etc/resolv.conf`（DNS 动态）
- `/etc/localtime`（时区）
- `/etc/ssl/certs/`（除非 subos 自带 ca-certificates）
- 可选 `/run/user/$UID/{wayland-*,pulse,bus}`（GUI/音频）

**其他全部 subos 自维护**——这就是你问的"xlings 自己维护 OS 环境"的具体边界。

---

## 10. 行动建议

1. 先做轻量方案（前一份分析的 §7 阶段 1-2）落地，让用户有"渐进升级路径"。
2. 在 `subos new` 加 `--mode` 参数，预留全量接口；`light` 是默认。
3. **PoC 全量方案**：写一个独立脚本 `xlings-subos-bootstrap-alpine.sh`，验证 OverlayFS + user-ns + pivot_root + procfs 在主流发行版上能跑通；不进主仓库。
4. 等 PoC 稳定 + 真实需求验证后，再决定是否把 `mode=heavy/full` 引入主线。
5. 不要试图重新发明容器运行时——必要时直接调用 `bwrap` 或 `systemd-nspawn` 作为后端，xlings 只做"目录约定 + 启动配置"层。
