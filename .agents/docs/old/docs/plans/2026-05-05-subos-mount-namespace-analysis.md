# xlings subos 升级到 Mount Namespace 的可行性分析

> 调研对象：把当前 PATH-based 的 subos 切换升级为内核级（mount namespace）+ shell 级生效的目录映射方案。
> 目标：让 `$XLINGS_HOME/subos/<name>/{bin,lib,usr,...}` 在进入 subos 后"看起来像"挂在 `/` 下，对子进程透明。

---

## 1. 现状回顾

| 维度 | 当前实现 |
|------|---------|
| 目录结构 | `$XLINGS_HOME/subos/<name>/{bin, lib, usr, xvm, generations}` |
| 激活机制 | 修改 `$XLINGS_HOME/subos/current` 符号链接 + shell profile 注入 `PATH=$XLINGS_HOME/subos/current/bin:$XLINGS_HOME/bin:$PATH` |
| 隔离层级 | 仅命令查找路径（PATH）；动态库、头文件、pkg-config 路径完全不隔离 |
| 内核能力 | 不使用任何 namespace / chroot / mount |
| 关键源码 | `src/core/subos.cppm`、`src/core/xself/init.cppm:206-244`、`config/shell/xlings-profile.sh:9-13` |

**PATH-based 模式的痛点：**

1. **库引用断裂**：CMake、autotools、`pkg-config` 默认在 `/usr/local/lib`、`/usr/lib`、`/usr/include` 搜索。subos 的 `lib/`、`usr/include/` 不在这些标准路径里，依赖 `LD_LIBRARY_PATH`、`CPATH` 等额外变量逐项打补丁。
2. **shebang 失效**：subos 的脚本若 `#!/usr/bin/env python` 拿到的是宿主 Python，而非 subos 内的版本。
3. **构建系统打硬编码**：很多 `Makefile`、`.pc` 文件含绝对路径 `/usr/local/...`，PATH 改不动这些。
4. **不可逆/半残**：要"完全模拟" subos 落在 `/` 下，需要劫持几十个环境变量，仍然挡不住硬编码路径。

**升级到 mount namespace 的核心收益：**

- 一次性把 subos 的目录树"挂"到子进程视图里 `/` 下的标准位置，所有遵循 FHS 的工具自动找到正确文件，无需逐工具适配。
- 进程退出后 namespace 自动销毁，宿主环境零残留，比 PATH 模式更干净。
- 对宿主用户、IDE、服务进程完全不可见，避免 PATH 污染。

---

## 2. 候选技术路径对比

| 方案 | 描述 | 评估 |
|------|------|------|
| A. 全 chroot / pivot_root | 把 subos 当作完整 rootfs，`chroot` 进入 | 需要完整 `/bin`、`/lib`、`/etc`、`/proc` 子集；与"轻量虚拟环境"定位不符；过度容器化 |
| **B. 私有 mount namespace + 选择性 bind 挂载** | `unshare -m`，仅 bind 几个关键路径 | **推荐**。轻量、可逆、shell 级生效、与现有 subos 目录结构对齐 |
| C. OverlayFS 叠加 | 把 subos 作为 upperdir 叠在宿主 `/` 上 | 复杂、需要 root、不易理解、合并语义混乱 |
| D. bubblewrap / systemd-nspawn 包装 | 调用现成容器工具 | 引入外部依赖；本质仍是 B + 政策层 |

**结论：选 B。** 它是 xlings 最自然的演进，且不需要 root（user namespace + mount namespace 组合）。

---

## 3. 目录映射分析（FHS 视角）

把 subos 子目录映射到宿主 `/` 下，必须区分**安全可覆盖**、**可选覆盖**、**严禁覆盖**三档。判据是：覆盖后 namespace 内的 sh、ld.so、coreutils、resolv 等基础设施是否仍能工作。

### 3.1 严禁覆盖（覆盖即破坏 namespace）

| 路径 | 覆盖后果 |
|------|---------|
| `/bin`、`/sbin`、`/usr/bin`、`/usr/sbin` | 失去 `sh`、`ls`、`mount`、`cat` 等，namespace 内 shell 立即不可用 |
| `/lib`、`/lib64`、`/usr/lib`、`/usr/lib64` | 失去 `libc.so.6`、`ld-linux-*.so.2`，所有动态可执行文件 EXEC 失败 |
| `/etc` | 丢 `passwd`、`nsswitch.conf`、`resolv.conf`、`ssl/certs/`，DNS 解析、用户查找、HTTPS 全坏 |
| `/proc` | 内核接口，必须从宿主继承（或重新 mount procfs） |
| `/sys` | 同上 |
| `/dev` | 终端、tty、随机数、null 设备依赖 |
| `/run` | systemd 运行时、socket、PID 文件 |

> 即使 subos 内确实想替换部分系统库（比如 musl 替 glibc），也应该走"完整 sysroot"的方案 A，而不是在轻量模式里挑战这一档。

### 3.2 推荐主映射点（FHS "本地软件" 区，覆盖最安全）

| subos 子目录 | 映射到 | 理由 |
|-------------|--------|------|
| `subos/<name>/local/bin`     | `/usr/local/bin`     | 几乎所有发行版默认 PATH 把它排在 `/usr/bin` 之前；不影响系统包 |
| `subos/<name>/local/lib`     | `/usr/local/lib`     | `ld.so` 默认搜索路径之一（通过 `/etc/ld.so.conf.d/libc.conf`）；`pkg-config` 默认查 `/usr/local/lib/pkgconfig` |
| `subos/<name>/local/include` | `/usr/local/include` | gcc/clang 默认搜索；CMake `find_path` 默认包含 |
| `subos/<name>/local/share`   | `/usr/local/share`   | 文档、locale、cmake 模块、pkgconfig 等 |
| `subos/<name>/local/libexec` | `/usr/local/libexec` | 工具内部辅助程序 |
| `subos/<name>/local/sbin`    | `/usr/local/sbin`    | 极少用到，对称性补齐 |

**为什么推荐 `/usr/local/*` 这一档：**

- FHS 明确把 `/usr/local` 留给"系统包管理器之外、本机安装的软件"——和 subos 语义完全一致。
- 默认 `PATH` 顺序（`/usr/local/bin` 在 `/usr/bin` 前）让 subos 工具优先级天然正确。
- `ld.so` 在大多数发行版默认搜索 `/usr/local/lib`，无需 `LD_LIBRARY_PATH`。
- `pkg-config` 默认 `prefix=/usr/local`，`.pc` 文件直接生效。
- 即使 namespace 失败回退到 PATH 模式，PATH 优先级语义也一致，迁移成本低。

> **建议同时调整 subos 目录约定**：在 `subos/<name>/` 内新增 `local/` 子树，让 xpkg/xvm 安装产物落在 `local/{bin,lib,include,share}`。这样 bind-mount 一对一对应，且也兼容"不进 namespace、直接 `cmake -DCMAKE_PREFIX_PATH=$SUBOS/local`"的传统用法。

### 3.3 可选映射点（按需启用）

| subos 子目录 | 映射到 | 用途 / 取舍 |
|-------------|--------|------------|
| `subos/<name>/opt`  | `/opt/xlings/<name>` | 整个 subos 暴露为 `/opt` 子树，发行版无关，适合 JetBrains、Anaconda 类发行包 |
| `subos/<name>/home` | `$HOME/.xlings-data` 或 `$HOME` 子目录 | per-subos 用户数据；**不要**直接覆盖整个 `/home`（会让其他用户失踪） |
| (tmpfs) | `/tmp` | 私有 tmpfs，构建中间文件不外泄；可选 |
| `subos/<name>/var-cache` | `/var/cache/xlings/<name>` | 缓存数据，namespace 退出仍保留 |

### 3.4 不建议映射

- `/home`：作为整体覆盖会破坏多用户主目录视图。要做用户私有数据，挂到 `$HOME/.xlings-data/<name>` 这样的子路径更安全。
- `/root`：通常空，覆盖意义不大。
- `/srv`、`/mnt`、`/media`：FHS 语义不匹配，与 subos 无关。
- `/var/lib`、`/var/log`：触碰 systemd、journald、apt 数据库，风险大于收益。

### 3.5 关键风险图

```
风险等级（覆盖后果）
高 ── /lib*, /usr/lib*, /etc, /proc, /sys, /dev, /run     ← 严禁
 │   /bin, /sbin, /usr/bin, /usr/sbin                      ← 严禁
 │   /home（整体）、/var/lib                                ← 不建议
 │   /tmp、/var/cache、/opt                                 ← 可选
低 ── /usr/local/{bin,lib,include,share,libexec,sbin}      ← 推荐
```

---

## 4. 实施方案

### 4.1 命令设计

```
xlings subos enter [name]                # 进入 subos，spawn 一个新 shell
xlings subos run   [name] -- <cmd ...>   # 在 subos 内一次性执行命令
xlings subos exec  [name] -- <cmd ...>   # 同 run，但 exec 替换当前进程
```

`enter` 是核心入口——用户 `exit` 即退出 namespace，shell 级语义一目了然。

### 4.2 实现核心（伪代码）

```bash
# 内部由 xlings 调用，不直接暴露给用户
exec unshare \
     --user --map-root-user \
     --mount --propagation private \
     -- bash -c '
  set -e
  SUBOS="'"$SUBOS_DIR"'"
  for d in bin lib include share libexec sbin; do
    [ -d "$SUBOS/local/$d" ] || continue
    [ -d "/usr/local/$d" ]   || mkdir -p "/usr/local/$d"  # 仅在 namespace 内
    mount --bind "$SUBOS/local/$d" "/usr/local/$d"
  done
  # 可选项
  [ -d "$SUBOS/opt" ]  && mount --bind "$SUBOS/opt"  "/opt/xlings/'"$NAME"'"
  [ -d "$SUBOS/home" ] && mount --bind "$SUBOS/home" "$HOME/.xlings-data"
  export XLINGS_ACTIVE_SUBOS="'"$NAME"'"
  exec "$SHELL" -l
'
```

要点：
- `--user --map-root-user` 让非 root 用户也能在 namespace 内 `mount`（依赖内核 unprivileged user namespace）。
- `--propagation private` 防止 namespace 内的挂载传播到宿主。
- `unshare` 后子进程退出，namespace 自动销毁。

### 4.3 与现有架构的衔接

- 保留现有 `$XLINGS_HOME/subos/current` 符号链接和 PATH 模式作为**默认与回退**。
- 新增 `xlings subos enter` 子命令；目前 `subos.cppm` 已有 new/use/list/info/remove，添加 `enter`/`run` 行数预计 100-200 行。
- 在 `subos new` 时一并创建 `local/{bin,lib,include,share}` 目录骨架，xpkg/xvm 后续安装策略调整为优先落到 `local/` 子树。
- `xself init` 写 shell profile 时无需变化（PATH 模式继续兜底）。

### 4.4 macOS / Windows 兼容

- Mount namespace 是 **Linux 专属**。macOS 上 `xlings subos enter` 应：
  - 检测平台，回退到"在子 shell 内 export PATH/LD_LIBRARY_PATH/PKG_CONFIG_PATH 等变量"的"轻 enter"模式；
  - 或直接报错指明仅 Linux 支持完整 namespace 隔离。
- Windows 同理，使用现有 PATH 模式。

---

## 5. 风险与限制

| 风险 | 触发场景 | 应对 |
|------|---------|------|
| **内核禁用 unprivileged user namespace** | RHEL ≤8、部分企业 hardened kernel、Debian 12+AppArmor、Ubuntu 24+ 默认 profile | 启动前检测 `/proc/sys/kernel/unprivileged_userns_clone`；缺失时回退 PATH 模式并打印诊断信息（含开启命令） |
| **AppArmor 限制 unprivileged_userns** | Ubuntu 24.04+ 默认对未签名二进制阻断 | 文档说明 `sysctl kernel.apparmor_restrict_unprivileged_userns=0` 或安装专用 profile；自动回退 |
| **`ld.so` 缓存未更新** | subos 装了新库，namespace 内 `ldconfig` 未运行 | 在 enter 时跑 `ldconfig -N -X` 或依赖 `LD_LIBRARY_PATH` 兜底 |
| **`pkg-config` 路径** | 罕见发行版不默认含 `/usr/local/lib/pkgconfig` | enter 时设 `PKG_CONFIG_PATH` |
| **跨终端不可见** | 用户在终端 A `enter` 了，终端 B 看不到 | 文档说明 namespace 是进程局部的；提供 `xlings subos attach`（`nsenter --target $PID --mount --user`） |
| **bind 反向写入污染** | 用户在 namespace 内修改 `/usr/local/lib` 实际改到 subos | **这是预期行为**（这就是 subos 的修改方式）。如需只读，用 `--bind,ro` |
| **IDE / language server 无法看到 subos 视图** | VSCode/Cursor 在宿主进程，看不到 namespace 内的 `/usr/local` | 让 IDE 启动器经过 `xlings subos run -- code .`；或保留 PATH 模式 + sysroot 路径（CMake/clangd 走 `--sysroot`） |
| **挂载点 `/usr/local/X` 不存在** | 极简发行版可能没有这些目录 | 在 namespace 内 `mkdir -p` 后再 bind（namespace 私有，不影响宿主） |
| **subprocess 离开 namespace** | 用 `systemctl --user start` 启服务，服务在宿主 namespace 内 | 这是设计预期：长驻服务不应进 subos |
| **fuse / sshfs / NFS 上的 subos** | bind-mount 在某些 FUSE 后端不工作 | 文档列出限制；推荐 subos 放在本地文件系统 |

---

## 6. 演进路线建议

| 阶段 | 内容 | 风险 |
|------|------|------|
| **1. 兼容引入** | 新增 `xlings subos enter`/`run` 子命令；Linux 启用 namespace 模式，其他平台/不支持的内核回退 PATH。默认行为不变。 | 低 |
| **2. 目录约定调整** | subos 目录结构补 `local/{bin,lib,include,share}`；xpkg/xvm 安装产物落到 `local/` 子树。旧 subos 通过 `subos migrate` 一键迁移。 | 中（需要 xpkg 侧配合） |
| **3. 可选 sysroot 模式** | 高级用户启用完整 chroot / `systemd-nspawn` / `bwrap`，做完整发行版隔离 | 高，作为后续可选项 |

---

## 7. 决策建议（一页摘要）

**做：**
- 在 subos 目录里新增 `local/{bin,lib,include,share}` 子树作为"挂载源"。
- 主映射点固定为 **`/usr/local/{bin,lib,include,share,libexec,sbin}`**——FHS 友好、覆盖最安全、不需要伪造系统基础设施。
- 用 **user namespace + mount namespace（`unshare -U -m -r`）** 实现，无需 root。
- 新增 `xlings subos enter` 命令，shell 级生效，退出 shell 自动销毁挂载。
- 内核不支持时自动回退到现有 PATH 模式，保留双轨。

**不做：**
- 不要覆盖 `/bin`、`/sbin`、`/lib*`、`/usr/bin`、`/usr/lib*`、`/etc`、`/proc`、`/sys`、`/dev`、`/run`——这是 namespace 自爆点。
- 不要覆盖整体 `/home`——破坏多用户视图。
- 不要追求"一次性做成完整容器"——xlings 定位是开发工具，过度容器化引入 networking、user mapping、capability、cgroup 等大量复杂度，性价比低。

**评估关键决策：**
- 主映射点为什么选 `/usr/local/*` 而非 `/opt`：因为 ld.so / pkg-config / cmake / 默认 PATH 这一整套链路对 `/usr/local` 的支持是开箱即用的，而对 `/opt` 都需要额外配置。
- 为什么不挂 `/lib`：覆盖动态链接器即等于禁用 namespace 内所有动态可执行文件，破坏代价远大于收益。subos 的库应该走 `/usr/local/lib` 路径，由 ld.so 默认搜索路径自然解析。

---

## 附：参考实现风格

最小可运行原型（验证内核支持）：

```bash
#!/usr/bin/env bash
SUBOS="${1:?subos dir required}"
unshare -U -m -r --map-root-user -- bash -c "
  mount --make-rprivate /
  for d in bin lib include share; do
    [ -d \"$SUBOS/local/\$d\" ] && mount --bind \"$SUBOS/local/\$d\" \"/usr/local/\$d\"
  done
  echo '[xlings] entered subos: $SUBOS'
  exec \"\$SHELL\"
"
```

测试验证清单：
- `which gcc` 看到 subos 内的 gcc；
- `pkg-config --list-all` 列出 subos 的 .pc；
- `cmake --find-package -DNAME=Foo -DCOMPILER_ID=GNU -DLANGUAGE=CXX -DMODE=EXIST` 能找到 subos 内的库；
- `exit` 后宿主 `/usr/local/*` 仍是原状。
