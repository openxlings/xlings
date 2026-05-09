# Sandbox V5 — 双后端 (bwrap + proot) 设计方案

**Status**: 实现中
**Target**: 0.4.25 (与 proot /bin fix 同版本)
**Replaces**: 无(增量,V4 接口 `--sandbox` 不变)

**V5.1 修正**(评审反馈):去掉 install hook 内 `sudo setcap` 方案。
setcap 依赖 xattr 文件系统 + `CAP_SETFCAP` + `libcap` 命令 —— Docker
容器 / Alpine / NFS / WSL2 多场景不适用。改为:
- 优先用系统 bwrap(`/usr/bin/bwrap`,distro 包自带正确权限)
- 其次 xim:bwrap(user-ns 可用的系统直接能跑)
- 兜底 proot(零权限要求)
- Ubuntu 24+ bwrap 不可用时提示 `sudo apt install bubblewrap`

## 一、命令面

```
xlings subos use <name> --sandbox [backend]
```

- `backend` 可选,缺省 = 自动选择(bwrap 优先,proot 兜底)
- `backend = bwrap` — 强制 bwrap(不降级)
- `backend = proot` — 强制 proot(跳过 bwrap 检测)

示例:

```bash
xlings subos use mybox --sandbox           # 自动选最优后端
xlings subos use mybox --sandbox bwrap     # 强制 bwrap
xlings subos use mybox --sandbox proot     # 强制 proot
xlings subos use mybox                      # 普通进入(不变)
```

90% 用户只用 `--sandbox`,不关心后端。高级用户 / 调试时可指定。

## 二、后端对比

| 维度 | bwrap | proot |
|---|---|---|
| 机制 | mount namespace(kernel 级) | ptrace syscall 拦截(用户态) |
| 性能 | ✅ 接近原生 | ❌ syscall-heavy 慢 30-50% |
| native npm 模块 | ✅ 不干扰 | ❌ 某些 crash(double free) |
| `/bin` 问题 | ✅ `--ro-bind / /` 天然包含 | ⚠️ 需 `--bind=/bin:/bin` 补 |
| strace/gdb 调试 | ✅ 正常 | ❌ ptrace 冲突 |
| 权限要求 | ⚠️ 需一次 `sudo`(setcap) | ✅ 无 |
| Ubuntu 24+ | ⚠️ user-ns 受限,setcap 绕开 | ✅ 直接可用 |
| 安装 | `xlings install bwrap`(hook 内 sudo setcap) | `xlings install proot` |
| 静态二进制大小 | ~200KB | ~1.5MB |

## 三、自动检测 + 安装流程

```
xlings subos use <name> --sandbox [backend]
                  ↓
         ┌── backend 指定了? ──┐
         │ YES                  │ NO
         ↓                      ↓
    locate + probe           detect_backend_()
    指定的 backend             自动选最优
         │                      │
         ↓                      ↓
    成功? → exec            ┌── bwrap ──┐
    失败? → 报错            │ locate    │
    (不降级)                │ probe     │
                            │ 成功? ────→ exec bwrap ✅
                            │ 失败? ↓   │
                            └───────────┘
                            ┌── proot ──┐
                            │ locate    │
                            │ 成功? ────→ exec proot ⚠️
                            │ 失败? ↓   │
                            └───────────┘
                            ┌── auto install ──┐
                            │ install bwrap     │
                            │ (hook 内 sudo     │
                            │  setcap)          │
                            │ probe 成功? ──────→ exec bwrap ✅
                            │ probe 失败? ↓     │
                            │ install proot     │
                            │ 成功? ────────────→ exec proot ⚠️
                            │ 失败? ────────────→ 报错
                            └───────────────────┘
```

## 四、subos 磁盘布局(backend 无关)

```
~/.xlings/subos/<name>/
  bin/                       ← subos shims(xlings, 装的包)
  lib/  usr/  generations/   ← 现有 subos 结构
  .xlings.json               ← workspace(per-subos)

  # sandbox 专属(lazy init,首次 --sandbox 时创建):
  home/<user>/               ← sandbox 私有 $HOME
    .bashrc                  ← seed: source xlings profile
    .profile                 ← seed: source .bashrc
    .config/fish/config.fish ← seed: source xlings fish profile
  tmp/                       ← sandbox 私有 /tmp
  etc/                       ← sandbox NSS 模板
    passwd                   ← root + 真实 user
    group
    hosts
    nsswitch.conf
  subos/                     ← 空 marker(project-discovery boundary)
```

**同一个目录,任意 backend,任意次数进出。** dotfile 持久,workspace 持久。backend 是运行时选择,不留痕迹。

## 五、FS 映射对比

### bwrap 映射

```
sandbox view ← source                                  | role
═══════════════════════════════════════════════════════════════
(全部)         ← --ro-bind / /          (host 只读)     | 一条搞定 /bin /usr /lib /etc /...
/dev           ← --dev /dev             (新 devfs)      | 设备节点
/proc          ← --proc /proc          (新 procfs)     | 进程信息
/home          ← <subos>/home/         (sandbox RW)    | dotfile 隔离
~/.xlings      ← host ~/.xlings        (RW 覆盖)       | xlings 共享
/tmp           ← <subos>/tmp/          (sandbox RW)    | 私有 tmp
/etc/passwd    ← <subos>/etc/passwd    (覆盖)           | sandbox NSS
/etc/group     ← <subos>/etc/group     (覆盖)           |
/etc/hosts     ← <subos>/etc/hosts     (覆盖)           |
/etc/nsswitch  ← <subos>/etc/nsswitch  (覆盖)           |
```

### proot 映射(现有 V4,保留)

```
sandbox view ← source                                  | role
═══════════════════════════════════════════════════════════════
/              ← -r <subos>/           (chroot root)   | 空 rootfs
/bin           ← --bind=/bin:/bin      (host)           | POSIX /bin
/usr           ← --bind=/usr:/usr      (host RO)        | POSIX /usr
/lib           ← --bind=/usr/lib:/lib  (host RO)        | usrmerge
/lib64         ← --bind=/usr/lib64:... (host RO)        | loader
/proc /sys /dev ← --bind=...           (host)           | kernel
/etc/resolv.conf ← host                                | DNS
/etc/ld.so.cache ← host                                | loader cache
/etc/{passwd,...} ← <subos>/etc/...    (sandbox)        | NSS
/home          ← <subos>/home/        (sandbox RW)     | dotfile
~/.xlings      ← host ~/.xlings       (RW 覆盖)        | xlings 共享
/tmp           ← <subos>/tmp/         (sandbox RW)     | 私有 tmp
```

### 对比要点

| 维度 | bwrap | proot |
|---|---|---|
| bind 条数 | ~10 条 | ~15 条 |
| 起点 | host 整体只读 → 覆盖隔离部分 | 空 rootfs → 补 host 部分 |
| /bin /usr /lib | 天然在 `--ro-bind / /` 里 | 需单独 bind |
| project-discovery | 不触发(host / 没有 `<subos>/.xlings.json`) | 需 `<subos>/subos/` marker |

## 六、bwrap argv 构建

```cpp
std::vector<std::string>
build_bwrap_argv_(const fs::path& bwrap_bin,
                  const fs::path& subos_dir,
                  const fs::path& host_xlings_home,
                  const std::string& user,
                  const std::string& shell)
{
    auto etc = subos_dir / "etc";
    auto user_home = "/home/" + user;
    return {
        bwrap_bin.string(),

        // ── host 整体只读 ──
        // 一条解决 /bin /usr /lib /lib64 /etc/resolv.conf /etc/ld.so.cache
        // 以及其他所有 host 路径。bwrap 后续 --bind 覆盖特定路径。
        "--ro-bind", "/", "/",

        // ── kernel pseudo-fs ──
        "--dev", "/dev",
        "--proc", "/proc",

        // ── sandbox 私有覆盖 ──
        "--bind", (subos_dir / "home").string(), "/home",
        "--bind", host_xlings_home.string(), user_home + "/.xlings",
        "--bind", (subos_dir / "tmp").string(), "/tmp",

        // ── sandbox NSS 模板(覆盖 host /etc/*) ──
        "--bind", (etc / "passwd").string(), "/etc/passwd",
        "--bind", (etc / "group").string(), "/etc/group",
        "--bind", (etc / "hosts").string(), "/etc/hosts",
        "--bind", (etc / "nsswitch.conf").string(), "/etc/nsswitch.conf",

        // ── 入口 ──
        "--chdir", user_home,
        "--", shell, "-i",
    };
}
```

## 七、Backend 检测 + 自动安装

### locate 逻辑

```cpp
// 探测 bwrap 二进制位置(跟 locate_proot_ 同结构)
// 优先级: xim 池 > PATH
std::expected<fs::path, std::string>
locate_bwrap_(const fs::path& home_dir) {
    // 1. xim:bwrap — ~/.xlings/data/xpkgs/xim-x-bwrap/<ver>/bin/bwrap
    auto xpkgs = home_dir / "data" / "xpkgs" / "xim-x-bwrap";
    // iterate versions...
    
    // 2. system bwrap — /usr/bin/bwrap (distro 包,可能有 setuid/capability)
    // 3. PATH 搜索
}
```

### probe 逻辑(关键 — 唯一靠谱的检测)

```cpp
// 不猜 sysctl / AppArmor / capability — 直接跑一次
bool probe_bwrap_(const fs::path& bwrap_bin) {
    std::string cmd = bwrap_bin.string()
        + " --ro-bind / / -- /bin/true 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}
// 耗时 < 50ms,sandbox 启动时跑一次
```

### detect 逻辑

```cpp
enum class SandboxBackend { Bwrap, Proot };

struct BackendInfo {
    SandboxBackend type;
    fs::path binary;
};

std::optional<BackendInfo>
detect_backend_(const fs::path& home_dir) {
    // 1. 系统 bwrap(distro 包,自带 setuid / AppArmor 豁免 — 最靠谱)
    if (auto bin = locate_in_path_("bwrap")) {
        if (probe_bwrap_(*bin))
            return BackendInfo{ SandboxBackend::Bwrap, *bin };
    }

    // 2. xim:bwrap(user-ns 可用的系统直接跑)
    if (auto bin = locate_in_xpkgs_("bwrap", home_dir)) {
        if (probe_bwrap_(*bin))
            return BackendInfo{ SandboxBackend::Bwrap, *bin };
    }

    // bwrap 存在但 probe 失败 — 提示装系统包
    if (locate_in_path_("bwrap") || locate_in_xpkgs_("bwrap", home_dir)) {
        log::info("bwrap found but sandbox probe failed (namespace restricted?)");
        log::info("  for best experience: sudo apt install bubblewrap");
    }

    // 3. proot(零权限要求,兜底)
    if (auto bin = locate_proot_(home_dir))
        return BackendInfo{ SandboxBackend::Proot, *bin };

    return std::nullopt;
}
```

### auto-install 逻辑

```cpp
int auto_install_backend_(const fs::path& home_dir, EventStream& stream) {
    // 1. 装 bwrap(无 sudo,只装二进制)
    log::info("installing sandbox backend...");
    std::vector<std::string> bwrap = {"xim:bwrap"};
    xim::cmd_install(bwrap, /*yes=*/true, /*noDeps=*/false, stream);

    if (auto bin = locate_in_xpkgs_("bwrap", home_dir); bin && probe_bwrap_(*bin)) {
        return 0;  // user-ns 可用的系统(Fedora/Arch/Debian/Ubuntu 22),直接成功
    }

    // 2. bwrap 装了但权限不够(Ubuntu 24+) → 提示 + 装 proot 兜底
    log::info("bwrap installed but needs system permissions");
    log::info("  to enable: sudo apt install bubblewrap");
    log::info("  using proot fallback for now");

    std::vector<std::string> proot = {"xim:proot"};
    return xim::cmd_install(proot, /*yes=*/true, /*noDeps=*/false, stream);
}
```

## 八、xim:bwrap xpkg

bwrap xpkg 的 install hook **不做 setcap**。只下载 + 解压静态二进制。

权限由以下方式获得:
- **系统 bwrap**(`sudo apt install bubblewrap`)—— distro 包自带 setuid / AppArmor profile
- **xim:bwrap 在 user-ns 可用的系统**(Fedora / Arch / Debian / Ubuntu 22)—— 直接能用
- **两者都不行** → proot 兜底,提示用户 `sudo apt install bubblewrap`

不在 hook 内 sudo 的理由:
- `setcap` 需要 xattr 文件系统 + `libcap` 命令 + `CAP_SETFCAP` —— Docker / Alpine / NFS / WSL2 不适用
- `cap_sys_admin` 权限过大,安全敏感用户可能不接受
- 系统包管理器(apt/dnf)已有成熟的权限配置机制,不必重造

## 九、use_sandbox_mode_ 统一入口

```cpp
int use_sandbox_mode_(const std::string& name, EventStream& stream,
                      const std::string& preferred_backend = "") {
    // ... validate, nesting check ...

    auto& p = Config::paths();
    auto subos_dir = p.homeDir / "subos" / name;

    // ── Backend 选择 ──
    std::optional<BackendInfo> backend;

    if (preferred_backend == "bwrap") {
        auto bin = locate_bwrap_(p.homeDir);
        if (!bin || !probe_bwrap_(*bin)) {
            stream.emit(ErrorEvent{ .message = "bwrap not available or permission denied" });
            return 1;
        }
        backend = BackendInfo{ SandboxBackend::Bwrap, *bin };
    } else if (preferred_backend == "proot") {
        auto bin = locate_proot_(p.homeDir);
        if (!bin) {
            stream.emit(ErrorEvent{ .message = "proot not found" });
            return 1;
        }
        backend = BackendInfo{ SandboxBackend::Proot, *bin };
    } else {
        // 自动检测
        backend = detect_backend_(p.homeDir);
        if (!backend) {
            auto rc = auto_install_backend_(p.homeDir, stream);
            if (rc != 0) { /* error */ return 1; }
            backend = detect_backend_(p.homeDir);
            if (!backend) { /* error */ return 1; }
        }
    }

    log::debug("sandbox backend: {}",
               backend->type == SandboxBackend::Bwrap ? "bwrap" : "proot");

    // ── lazy init ──
    auto user = utils::get_env_or_default("USER");
    auto uid = ::getuid(), gid = ::getgid();
    init_sandbox_dirs_(subos_dir, user, uid, gid);

    // ── env (统一,跟 backend 无关) ──
    auto user_home = "/home/" + user;
    auto shell = utils::get_env_or_default("SHELL");
    if (shell.empty()) shell = "/bin/sh";
    if (shell.starts_with("/bin/") && backend->type == SandboxBackend::Proot) {
        // proot: /bin 是 host bind,但 shell translate 仍需要(V4 逻辑保留)
        auto candidate = "/usr/bin/" + shell.substr(5);
        if (fs::exists(candidate)) shell = candidate;
    }
    // bwrap: /bin 天然是 host 的,不需要 translate

    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    platform::set_env_variable("XLINGS_SUBOS_MODE", "sandbox");
    platform::set_env_variable("HOME", user_home);
    platform::set_env_variable("PATH", std::format(
        "{}/.xlings/subos/{}/bin:{}/.xlings/bin:/usr/local/bin:/usr/bin:/bin",
        user_home, name, user_home));

    // ── 统一 argv 构建 ──
    std::vector<std::string> argv;
    if (backend->type == SandboxBackend::Bwrap) {
        argv = build_bwrap_argv_(backend->binary, subos_dir, p.homeDir, user, shell);
    } else {
        argv = build_proot_argv_(backend->binary, subos_dir, p.homeDir, user, shell);
    }

    // ── emit event + flush + exec ──
    nlohmann::json payload;
    payload["name"] = name;
    payload["mode"] = "sandbox";
    payload["backend"] = (backend->type == SandboxBackend::Bwrap) ? "bwrap" : "proot";
    stream.emit(DataEvent{"subos_entering", payload.dump()});

    std::cout.flush();
    std::cerr.flush();

    std::vector<char*> c_argv;
    for (auto& s : argv) c_argv.push_back(const_cast<char*>(s.c_str()));
    c_argv.push_back(nullptr);
    ::execvp(c_argv[0], c_argv.data());
    log::error("failed to exec {}", backend->binary.string());
    return 127;
}
```

## 十、命令解析

```cpp
// subos use 解析(V5 变化)
if (a == "--sandbox") {
    sandbox = true;
    // 检查下一个 arg 是否是 backend 名(非 -- 开头 && 是 bwrap/proot)
    if (i + 1 < argc) {
        std::string next = argv[i + 1];
        if (next == "bwrap" || next == "proot") {
            sandbox_backend = next;
            ++i;  // consume
        }
    }
}
```

## 十一、提示符(不变)

V4 已实现的 `<xsubos:<name>>` 不变。backend 类型不体现在提示符里(用户不需要关心)。

可选:`entering subos` 消息里标注 backend:

```
▸ entering subos mybox (sandbox: bwrap)
▸ entering subos mybox (sandbox: proot)
```

## 十二、E2E 测试

```bash
# 现有 S1-S15 不变(proot 路径)

# 新增:
S16: detect_backend 自动选择
  - 如果 bwrap probe 成功 → 验证用的是 bwrap
  - 如果 bwrap probe 失败 → 验证 fallback proot

S17: --sandbox bwrap 强制指定
  - bwrap 可用 → 成功进入
  - bwrap 不可用 → 报错(不降级)

S18: --sandbox proot 强制指定
  - 直接用 proot(跳 bwrap 检测)

S19: bwrap sandbox 内 npm install 无 crash(对比 proot)
  - bwrap 内: npm install --prefix /tmp/x @anthropic-ai/claude-code → ✅
  - (proot 内同操作 → double free,已知限制)

S20: 同一 subos 交替 bwrap ↔ proot,dotfile 持久
  - bwrap 进,写 ~/.config/test
  - proot 进,读 ~/.config/test → 存在 ✓
```

## 十三、完整用户旅程

### 首次使用(全新系统)

```
$ xlings subos new mybox
✓ subos created: mybox

$ xlings subos use mybox --sandbox
[info] no sandbox backend found, installing...
  ◆ xim:bwrap@0.11.2
  ✓ downloaded
  [info] setting up namespace permission...
  [sudo] password for speak: ********
  ✓ bwrap sandbox ready

▸ entering subos mybox (sandbox: bwrap)
<xsubos:mybox> $ npm install -g claude-code    ← native module 正常 ✅
<xsubos:mybox> $ claude --version
Claude Code 2.1.90
```

### Ubuntu 24+ 用户取消 sudo

```
$ xlings subos use mybox --sandbox
[info] no sandbox backend found, installing...
  ◆ xim:bwrap@0.11.2
  [info] setting up namespace permission...
  [sudo] password for speak: ^C               ← 用户取消
  [info] setcap skipped, installing proot fallback...
  ◆ xim:proot@5.4.0
  ✓ installed

▸ entering subos mybox (sandbox: proot)        ← 能用,有 ptrace 限制
```

### 日常使用

```
$ xlings subos use mybox --sandbox             ← 自动 bwrap(或 proot)
▸ entering subos mybox (sandbox: bwrap)

$ xlings subos use mybox --sandbox proot       ← 调试:强制 proot
▸ entering subos mybox (sandbox: proot)

$ xlings subos use mybox                       ← 普通(不变)
▸ entering subos mybox
```

## 十四、改动量估算

| 文件 | 改动 |
|---|---|
| `src/core/subos.cppm` | `locate_bwrap_` ~30 LOC;`probe_bwrap_` ~10 LOC;`detect_backend_` ~30 LOC;`auto_install_backend_` ~20 LOC;`build_bwrap_argv_` ~30 LOC;修改 `use_sandbox_mode_` ~30 LOC;修改 `run()` 解析 ~5 LOC |
| `src/core/subos.cppm` imports | 加回 `import xlings.core.xim.commands`(auto-install 需要) |
| `tests/e2e/subos_sandbox_test.sh` | S16-S20 ~60 LOC |
| `.agents/docs/sandbox-v4-design.md` | 更新 backend 部分 |
| xim-pkgindex `pkgs/b/bwrap.lua` | install hook 加 setcap ~15 LOC |
| **总计** | **~200 LOC 新增 + xpkg 改动** |

## 十五、不在本设计内(后续)

- `--sandbox --no-net`:网络隔离(bwrap `--unshare-net`)
- `--sandbox --pid`:PID 隔离(bwrap `--unshare-pid`)
- FUSE overlay:per-sandbox xpkg 独立池
- `xlings subos export/import`:sandbox 快照导出/恢复
- Windows / macOS sandbox(WSL2 / hypervisor 方向)
