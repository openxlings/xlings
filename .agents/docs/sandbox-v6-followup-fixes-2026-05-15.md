# Subos Sandbox V6 — Followup Fixes & Design Revisions

**Status**: 部分完成 — bwrap rebuild ✅ done (xim-pkgindex `99a1f9b`)；xlings 主仓侧 6 项待办
**Trigger**: 2026-05-15 用户实测 V6 image storage + bwrap 链路发现连锁问题
**Depends on**: V5 双后端、V6 storage isolation、PR #193 (xim:bwrap setuid)

## 进度
- ✅ **P0-2** xlings-res/bwrap rebuild with `-Dsupport_setuid=true` (2026-05-15 07:21, binary 137K→145K, probe exit=0)
- ⬜ **P0-1, P0-3, P0-4, P1-5, P1-6, P2-8** — xlings 主仓内待办，见下

## 一、问题来源

用户实测序列：

```fish
$ xlings subos use mode-image-test
[xlings] storage=image requires sandbox, entering sandbox mode...
[sudo] password for speak:
[error] image storage requires bwrap (proot does not support mount namespace)
[error]   run: xlings install bwrap

$ xlings install bwrap     # 已装，再装一遍
✓ 1 package(s) installed

$ xlings subos use mode-image-test
[error] image storage requires bwrap (proot does not support mount namespace)   # 死循环

$ sudo rm .xlings/subos/test-img -rf
rm: cannot remove '.xlings/subos/test-img/.mountpoint': Device or resource busy
```

链条上有 6 个独立问题在叠加，逐项剖析见下。

## 二、问题清单与修复方案

> **图例**:
> 🐧 = Linux 专有 | 🍎 = macOS 专有 | 🪟 = Windows 专有 | 🌐 = 跨平台
> P0 = 阻塞性 / P1 = 应做 / P2 = 锦上添花

---

### P0-1 🌐 解耦 storage mode 与 sandbox mode

**症状**: 用户 `xlings subos use <image-subos>` 不带 `--sandbox` 时被自动强制升级为 sandbox 模式，引发后续所有 bwrap 探测、sudo prompt、错误连锁。

**根因**: V6 设计第 16/33/57-65 行强制 "非 shared 模式自动 sandbox"。这违反 V4 设计原则 (`docs/plans/2026-05-09-subos-sandbox-design.md:20`)：
> "`--sandbox` 是 `use` 的修饰符,不再是 subos 的 type —— 一个 boolean flag。同一个 subos 可以选择带或不带 sandbox 隔离进入，正交两个维度。"

storage（数据放哪）和 use mode（怎么用）本来就该正交：

| `xlings subos use foo` | storage=shared | storage=image | storage=tmpfs |
|---|---|---|---|
| 不带 `--sandbox` (shell-level) | env/PATH 切换 | **同左**：image 文件存在但不挂载、不用 | **同左**：tmpfs 不创建、不用 |
| 带 `--sandbox` (sandbox-level) | bwrap/proot 进 home bind 视图 | bwrap 挂 image 到 `/home/$user` | bwrap 给 `/home/$user` 一块 tmpfs |

**修复**: 移除 `src/core/subos.cppm:1307-1318` 强制提升逻辑。改为：
- shell-level 永远只做 env/tools 切换（跨平台一致：L1）
- storage 模式仅在 `--sandbox` 路径中被消费
- shell-level 进入非 shared subos 时打一行 info 提示，说明 storage 当前未激活

**跨平台影响**:
- 🐧 Linux: bwrap 才挂 image / tmpfs；shell-level 不碰挂载
- 🍎 macOS / 🪟 Windows: image / tmpfs 当前就**不支持**（V6 表格已标"不支持"）。shell-level 行为变成 "image storage configured but not activated (sandbox-only on this platform; full FS isolation requires Linux)"
- `subos new --storage image` 在 macOS/Windows 上应当 **直接拒绝创建**（创建时检测，不是 use 时才暴露），错误信息明确"image/tmpfs storage is Linux-only"

**附带价值**: 解耦后，今天的"`rm -rf .mountpoint` busy" 和 "use 死循环" 在 shell-level 路径自动消失 —— shell 不挂载就没残留。挂载相关的全部 corner case 收敛到 sandbox 路径。

**文件**:
- `src/core/subos.cppm:1307-1318` — 删除强制提升
- `src/core/subos.cppm` — `subos new` 加跨平台 storage 校验
- `.agents/docs/sandbox-v6-storage-isolation-design.md:16,33,57-65` — 设计文档同步更新

---

### P0-2 🐧 `xlings-res/bwrap` 构建时启用 setuid 模式 ✅ DONE (2026-05-15)

**症状**: `chmod 4755` 后 binary 启动报 `setuid use of bubblewrap is not supported in this build` 自杀退出。

**根因**: bubblewrap 0.10+ meson 默认 `-Dpriv_mode=none`，编译期砍掉 setuid 路径并加运行时自检（带 setuid 位即拒绝）。`xlings-res/bwrap` 镜像构建没传 `-Dpriv_mode=setuid`，与 PR #193 install hook 的 `chmod 4755` 直接冲突。

**修复**: 在 `xlings-res/bwrap` 的构建脚本中加 meson 选项：

```sh
meson setup _build \
    -Dpriv_mode=setuid \
    -Drequire_userns=false \
    --buildtype=release --strip
meson compile -C _build
```

重 build 重发资产（建议 bump 0.11.2 → 0.11.3 避开镜像缓存）。`xim-pkgindex/pkgs/b/bwrap.lua` 和 xlings 主仓代码一行不动，install hook 的 `chmod 4755` 立刻生效。

**跨平台影响**:
- 🐧 仅 Linux —— bwrap 本身就是 Linux-only
- 🍎 🪟 macOS/Windows 用 env redirect 实现 sandbox (L2)，与 bwrap 无关

**验证矩阵**（Linux 内必跨发行版）:
- Ubuntu 24.04（apparmor_restrict_unprivileged_userns=1）
- Ubuntu 22.04（无该 sysctl 文件）
- Fedora 40+（默认禁 unprivileged_userns_clone）
- Alpine 3.20+（musl，无 AppArmor）
- WSL2（特殊内核子集）
- minimal Docker (`alpine:latest`、`debian:slim`)

---

### P0-3 🌐 `probe_bwrap_` 透出 stderr，错误消息分流

**症状**: probe 失败时用户只看到 `run: xlings install bwrap`，重装无效，陷入死循环。

**根因**: `src/core/subos.cppm:795-799` 的 probe：
```cpp
auto cmd = bwrap_bin.string() + " --ro-bind / / -- /bin/true 2>/dev/null";
return std::system(cmd.c_str()) == 0;
```
返回 bool，stderr 直接丢弃，三种完全不同的失败被压扁成"bwrap 不可用"。

**修复**: probe 改返回 `(ok, stderr)`：

```cpp
struct ProbeResult { bool ok; std::string err; };
ProbeResult probe_bwrap_(const fs::path& bwrap_bin) {
    auto cmd = bwrap_bin.string() + " --ro-bind / / -- /bin/true 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    std::string out; char buf[256];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    return { pclose(p) == 0, std::move(out) };
}
```

下游错误分流（在 `use_sandbox_mode_` line 1081-1101、1142 几处）：

| stderr 关键字 | 归类 | hint |
|---|---|---|
| `setuid use of bubblewrap is not supported` | build-config 错误 | "xim:bwrap binary lacks setuid support; rebuild required" + issue link |
| `setting up uid map: Permission denied` | AppArmor / LSM 拦截 | "kernel restricts unprivileged userns; check apparmor_restrict_unprivileged_userns" |
| `clone() failed: Operation not permitted` | 内核 userns 关闭 | "enable kernel.unprivileged_userns_clone=1" |
| (其他) | 未分类 | 原文透传 |

同时**区分 "not installed" vs "installed but probe failed"**（`subos.cppm:1142`），避免无脑提示 `xlings install bwrap`：

```cpp
auto bin = locate_bwrap_(p.homeDir);
if (!bin) {
    // 没装
    hint = "run: xlings install bwrap";
} else {
    auto pr = probe_bwrap_(*bin);
    if (!pr.ok) {
        // 装了但 probe 失败 → 分流抛真实错误
        hint = classify_probe_error_(pr.err, *bin);
    }
}
```

**跨平台影响**:
- 🐧 主要修在 Linux 路径（bwrap/proot 探测）
- 🍎 🪟 同样的"诊断信息透明化"原则适用于 macOS/Windows L2 的失败路径 —— 比如 env 不可设置、目录权限不足等，错误信息也要给真实原因而不是占位符

---

### P0-4 🐧 `subos remove` 先 umount image，避免 EBUSY + rm 误入挂载

**症状**:
```
sudo rm .xlings/subos/test-img -rf
rm: cannot remove '.xlings/subos/test-img/.mountpoint': Device or resource busy
```
而且 `rm -rf` 在 EBUSY 之前已经递归进了挂载内部，**把 image 里的内容删了一遍**（home.img 标 `(deleted)`，但内核还持有 inode）。

**根因**: `remove` 实现没考虑 image storage 的活挂载 + 异常退出残留挂载。

**修复**: 在 `subos remove` 流程开头：
1. 读取 storage mode；如果是 image，检测 `<subos>/.mountpoint` 是否为活挂载（`mountpoint -q` 或 `findmnt`）
2. 活挂载 → `sudo umount <path>`（失败给清晰错误，不静默）
3. 再执行 `rm -rf`

**跨平台影响**:
- 🐧 image / tmpfs 只在 Linux 存在挂载，仅 Linux 路径需要这条
- 🍎 🪟 image / tmpfs 在 macOS/Windows 应在创建时被拒绝（见 P0-1），所以 remove 路径无需特殊处理

---

### P1-5 🐧 sandbox 入口扫 stale mount

**症状**: subos 进程异常退出（kill -9、终端 force close、xlings crash）会留下 `.mountpoint` 挂载。下次 `subos use <name> --sandbox` 会撞 "already mounted" 或其它二次挂载错误。

**根因**: `mount_image_` (subos.cppm:308) 现在用 `is_mounted_` 短路返回，但没区分"上次正常使用复用"和"上次 crash 残留"。

**修复（sandbox 路径开头）**:
1. 检测 `.mountpoint` 是否 mounted
2. 若 mounted，检查是否有活进程持有（`fuser -m` 或 `lsof`）
3. 无活进程 → 视为 stale，主动 umount + 重新 mount
4. 有活进程 → 复用（当前 multi-terminal 设计意图）

**跨平台影响**:
- 🐧 仅 Linux

---

### P1-6 🌐 `subos new --storage image|tmpfs` 在非 Linux 平台直接拒绝

**症状**: V6 表格虽然标了 macOS/Windows 不支持 image/tmpfs，但创建命令是否真的拒绝、错误消息是否明确，需要确认。

**根因**: 平台门禁逻辑可能放在 use 时而非 new 时，导致用户能创建一个"在本平台永远用不了"的 subos。

**修复**: `subos new` 校验链路加：
```cpp
if (storage != StorageMode::Shared && !platform::is_linux()) {
    error("storage=image|tmpfs is Linux-only; "
          "macOS/Windows sandbox uses HOME redirect (L2) which has no "
          "private FS image");
}
```

**跨平台影响**:
- 🌐 三平台都需要这条门禁；错误消息应说明原因，不是"unsupported"。

---

### P2-7 🐧 install hook 的 sudo prompt 体验

**症状**: `xlings install bwrap` 中途突然弹 sudo password prompt，没有前置说明。

**根因**: `xim-pkgindex/pkgs/b/bwrap.lua:64-66`:
```lua
log.info("Setting bwrap setuid root (sudo required)...")
os.exec("sudo chown root:root " .. bwrap)
os.exec("sudo chmod 4755 " .. bwrap)
```

修了 P0-2 后 `chmod 4755` 仍必须 sudo（setuid root 是真需要 root 写）。

**修复**:
- install 开始时提前提示"本次安装将设置 setuid root，需要 sudo 密码"
- 检测 `sudo -n true`，若已缓存则静默，未缓存则提示用户即将输入密码
- 提供 link 到设计文档说明为什么需要 setuid

**跨平台影响**:
- 🐧 仅 Linux 需要

---

### P2-8 🌐 文档/注释同步

修完上面几条后，以下文档/注释会与实际行为脱节，需同步：

| 文件 | 当前内容 | 修订要点 |
|---|---|---|
| `.agents/docs/sandbox-v6-storage-isolation-design.md:16,33,57-65` | "非 shared 模式自动 sandbox" | 改为正交：storage 仅在 sandbox 路径消费；shell-level 永远只做 env 切换 |
| `.agents/docs/sandbox-v6-storage-isolation-design.md:42-43` | "macOS/Windows 不支持" | 补充：拒绝在 `subos new` 时创建 |
| `src/core/subos.cppm:770-774` | "xim:bwrap ... is the only one guaranteed to work" | 重 build 后该说法成立；注释保留但补 "requires `-Dpriv_mode=setuid` at build time" |
| `xim-pkgindex/pkgs/b/bwrap.lua:5` | description "setuid-less namespace sandbox" | 改为 "namespace sandbox (setuid-enabled build for cross-distro reach)" |
| `xim-pkgindex/pkgs/b/bwrap.lua:26-34` | xpkg 注释提及 musl-static prebuilt | 补一句 "built with `-Dpriv_mode=setuid` meson option" |
| `.agents/docs/changelog.md:11` | "Ubuntu 24+ bwrap namespace 受限时提示 `sudo apt install bubblewrap`,自动用 proot" | 当前已无该 fallback，删掉或改为"自动 install xim:bwrap" |

---

## 三、跨平台行为矩阵（修后预期）

```
xlings subos use <name>                  # L1 (shell-level, 三平台一致)
xlings subos use <name> --sandbox        # 🐧 L3 (bwrap/proot)  🍎 🪟 L2 (env redirect)

xlings subos new <name>                  # 🌐 storage=shared (默认)
xlings subos new <name> --storage shared # 🌐 三平台支持
xlings subos new <name> --storage image  # 🐧 仅 Linux ── 🍎 🪟 拒绝创建
xlings subos new <name> --storage tmpfs  # 🐧 仅 Linux ── 🍎 🪟 拒绝创建
```

**L1 行为对所有平台、所有 storage 一致**：
- 设置 `XLINGS_ACTIVE_SUBOS`、`PATH`、prompt 标识
- 不挂载、不重定向、不调用任何特权操作
- image / tmpfs storage 在 L1 路径下"存在但不激活"，仅提示

**L2/L3 行为按平台分化**：
- 🐧 L3: bwrap (setuid 模式) 优先，proot fallback；image/tmpfs 通过 bwrap mount namespace 实现
- 🍎 L2: `HOME` `TMPDIR` `XDG_*` env redirect 到 subos 目录（已实现 in v0.4.28）
- 🪟 L2: `USERPROFILE` `%TEMP%` env redirect（已实现 in v0.4.28）

---

## 四、执行顺序

### Batch A (hotfix, 1-2 天) — 立刻见效，不依赖 bwrap 修好

```
P0-1  解耦 storage/sandbox          subos.cppm:1307-1318 删除 + new 加跨平台校验
P0-3  probe stderr 透出 + 错误分流   subos.cppm:795-799, 1142
P0-4  remove 先 umount              subos remove 实现
P2-8  文档/注释同步                  设计文档 + bwrap.lua + changelog
```

完成 Batch A 后用户的实测序列变成：
```fish
$ xlings subos use mode-image-test
[info] storage=image is sandbox-only; entering shell-level (use --sandbox to activate)
<xsubos:mode-image-test>$    # 直接进 shell，无 sudo、无 bwrap 错误

$ xlings subos remove mode-image-test
✓ subos removed              # 内部先 umount，rm 顺畅
```

—— **bwrap 还没修，但用户路径已经不撞坑**。

### Batch B (main, 1 周) — 真正修 bwrap

```
P0-2  xlings-res/bwrap 重 build (priv_mode=setuid)
      ↓ 发布 0.11.3 资产
      ↓ 跨发行版测试矩阵
```

发完之后 `--sandbox` 路径在 Linux 全发行版打通。

### Batch C (polish, 接下来 1-2 周)

```
P1-5  sandbox 入口扫 stale mount
P1-6  new 时跨平台 storage 校验（如果 Batch A 没一起做掉）
P2-7  install hook sudo prompt 体验
```

---

## 五、不做的事

- ❌ `xlings subos doctor` 子命令 —— V6 没存量用户，不需要诊断兜底；probe stderr 透出（P0-3）已足够定位
- ❌ 回滚 `b491887` 添加 `/usr/bin/bwrap` fallback —— 违反"跨发行版一致"原则；P0-2 修完后没必要
- ❌ 让用户改 `apparmor_restrict_unprivileged_userns=0` —— 设计已否决（要求宿主配置）
- ❌ image 失败时降级 shared —— 解耦后用户可直接 shell-level 进入，不需要"自动降级"

---

## 六、附录：本次会话定位过程（用于 onboarding）

### 关键证据
```
$ /home/speak/.xlings/data/xpkgs/xim-x-bwrap/0.11.2/bin/bwrap --ro-bind / / -- /bin/true
bwrap: setuid use of bubblewrap is not supported in this build       ← 关键
exit=1

$ cp <bwrap> /tmp/x && chmod 0755 /tmp/x && /tmp/x --ro-bind / / -- /bin/true
bwrap: setting up uid map: Permission denied                          ← AppArmor 拦截
exit=1

$ /usr/bin/bwrap --ro-bind / / -- /bin/true
exit=0                                                                 ← 系统 0.9.0 能跑
```

### 触发提交
| Commit | 仓库 | 时间 | 改动 |
|---|---|---|---|
| `ba54990` | xim-pkgindex | 2026-05-14 19:23 | install hook 加 `chmod 4755` (#193) |
| `b491887` | xlings | 2026-05-14 19:48 | 砍掉 `/usr/bin/bwrap` fallback，xim 池独占 |

两者叠加的设计假设是"xim:bwrap 自带 setuid → 跨 distro 通用"，但 bubblewrap 0.10+ 默认 `priv_mode=none` 让 binary 自带"反 setuid"，假设失效。

### 上游 build option
bubblewrap `meson_options.txt`:
```
option('priv_mode', type: 'combo',
       choices: ['none', 'setuid', 'setcap'], value: 'none',
       description: 'how the bwrap binary obtains privileges')
```
P0-2 的本质就是把这个选项从 `none` 改回 `setuid`。
