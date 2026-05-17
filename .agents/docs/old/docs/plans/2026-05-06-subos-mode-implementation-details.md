# xlings subos 各 mode 技术实现细节

> 配套文档:[`2026-05-06-subos-four-tier-comparison.md`](./2026-05-06-subos-four-tier-comparison.md)。
>
> 本文目标:把"对比"里讨论的 4 档 (default / medium / heavy / full) 落到**可写代码的颗粒度** —— 每档涵盖目录布局、配置 schema、启动序列、模块划分、关键代码骨架、失败/降级处理、测试策略。
>
> 编码约定:C++23 module 形式,沿用现有 `export module xlings.core.subos.*;` 命名;新模块全部放在 `src/core/subos/` 下;现有 `src/core/subos.cppm` 退化为 facade(只剩对外 export)。

---

## 0. 共享基础设施(4 档复用)

### 0.1 模块/文件划分

```
src/core/subos/
├── subos.cppm                     (改) facade,对外暴露命令接口
├── mode.cppm                      (新) Mode enum + 字符串解析 + 升降级关系
├── config_v2.cppm                 (改) .xlings.json schema(含 mode 字段 + 各档子段)
├── capability.cppm                (新) 内核 / 平台能力探测
├── arbiter.cppm                   (新) 仲裁器:read mode → probe → autodegrade
├── hook.cppm                      (新) pre-enter / post-enter 执行器
├── env.cppm                       (新) 环境变量注入(default 主路径,其它档共享)
├── plan/
│   ├── plan.cppm                  (M2 引入) MountPlan 数据结构
│   ├── plan_medium.cppm           (M1)
│   ├── plan_heavy.cppm            (M2)
│   └── plan_full.cppm             (M3)
├── backend/
│   ├── backend.cppm               (M2 引入) BackendDriver 接口
│   ├── backend_unshare.cppm       (M1)
│   ├── backend_bwrap.cppm         (M2)
│   └── backend_nspawn.cppm        (M5,可选)
├── bootstrap/
│   ├── bootstrap.cppm             (M3 引入) Bootstrap 策略接口
│   ├── bootstrap_noop.cppm        (M0/M1)  default/medium/heavy
│   └── bootstrap_image.cppm       (M3)     full
├── exporter/
│   ├── exporter.cppm              (M4) Export 策略接口
│   ├── exporter_tar.cppm
│   ├── exporter_overlay.cppm
│   └── exporter_rootfs.cppm
└── etc_policy/
    ├── heavy.toml                 (M2,数据文件) /etc 处理清单
    └── full_alpine.toml           (M3,数据文件)
```

### 0.2 Mode 枚举

```cpp
// src/core/subos/mode.cppm
export module xlings.core.subos.mode;
import std;

namespace xlings::subos {

export enum class Mode : uint8_t {
    Default = 0,   // PATH + ENV,无 namespace
    Medium  = 1,   // mount-ns + bind /usr/local + 私有 HOME/tmp
    Heavy   = 2,   // bwrap + OverlayFS
    Full    = 3,   // bwrap/nspawn + 独立 rootfs
};

// 用于排序和"是否更高档"判定
export constexpr uint8_t rank(Mode m) noexcept { return static_cast<uint8_t>(m); }

export std::string_view to_string(Mode m) noexcept {
    switch (m) {
        case Mode::Default: return "default";
        case Mode::Medium:  return "medium";
        case Mode::Heavy:   return "heavy";
        case Mode::Full:    return "full";
    }
    return "default";
}

export std::expected<Mode, std::string> parse(std::string_view s) noexcept {
    if (s == "default" || s.empty()) return Mode::Default;
    if (s == "medium")               return Mode::Medium;
    if (s == "heavy")                return Mode::Heavy;
    if (s == "full")                 return Mode::Full;
    return std::unexpected(std::format("unknown subos mode: '{}'", s));
}

// 自然降级链(每档掉一级);Default 是底
export Mode degrade(Mode m) noexcept {
    switch (m) {
        case Mode::Full:    return Mode::Heavy;   // ⚠️ 实际仲裁会拒绝(rootfs ≠ overlay)
        case Mode::Heavy:   return Mode::Medium;
        case Mode::Medium:  return Mode::Default;
        case Mode::Default: return Mode::Default;
    }
    return Mode::Default;
}

} // namespace
```

### 0.3 配置 schema(v2)

`subos/<n>/.xlings.json`:

```json
{
  "version": 2,
  "name": "dev",
  "mode": "medium",
  "created": "2026-05-06T10:00:00Z",

  "default": {
    "env": {
      "LD_LIBRARY_PATH": "$SUBOS/local/lib:$ORIG",
      "CPATH": "$SUBOS/local/include:$ORIG",
      "PKG_CONFIG_PATH": "$SUBOS/local/lib/pkgconfig:$SUBOS/local/share/pkgconfig:$ORIG",
      "CMAKE_PREFIX_PATH": "$SUBOS/local:$ORIG"
    }
  },

  "medium": {
    "binds": [
      { "src": "local/bin",     "dst": "/usr/local/bin" },
      { "src": "local/lib",     "dst": "/usr/local/lib" },
      { "src": "local/include", "dst": "/usr/local/include" },
      { "src": "local/share",   "dst": "/usr/local/share" },
      { "src": "local/libexec", "dst": "/usr/local/libexec" }
    ],
    "home":     "home",
    "opt":      "opt",
    "tmp":      "tmpfs",
    "tmp_seed": "tmp-seed",
    "share_ssh": false,
    "share_git": false
  },

  "heavy": {
    "overlay": {
      "lower":  ["/"],
      "upper":  "upper",
      "work":   "work"
    },
    "etc_overrides": "etc-overrides",
    "etc_policy":    "heavy.toml",
    "backend":       "bwrap"
  },

  "full": {
    "base":    "alpine:3.19",
    "rootfs":  "rootfs",
    "backend": "bwrap",
    "binds": [
      { "src_host": "/etc/resolv.conf", "dst": "/etc/resolv.conf", "ro": true },
      { "src_host": "/etc/localtime",   "dst": "/etc/localtime",   "ro": true }
    ],
    "shared_caps": ["CAP_NET_BIND_SERVICE"]
  },

  "hooks": {
    "pre_enter":  "hooks/pre-enter.sh",
    "post_enter": "hooks/post-enter.sh"
  }
}
```

读取规则:仲裁器只读 `mode` 字段决定走哪个分支;高档子段保留以备降级回看。

### 0.4 能力探测器

```cpp
// src/core/subos/capability.cppm
export module xlings.core.subos.capability;
import std;

namespace xlings::subos {

export struct Capability {
    bool linux            = false;   // 平台是 Linux
    bool unpriv_userns    = false;   // /proc/sys/kernel/unprivileged_userns_clone == 1
    bool unpriv_overlay   = false;   // mount overlay 不需要 root(kernel ≥5.11)
    bool bwrap_present    = false;   // bwrap 在 PATH
    bool bwrap_overlay    = false;   // bwrap --overlay-src 支持(≥0.7)
    bool bwrap_tmp_overlay= false;   // bwrap --tmp-overlay 支持(≥0.10)
    bool nspawn_present   = false;
    bool systemd_252      = false;
    bool apparmor_blocks_userns = false;  // Ubuntu 24+ 限制
    int  kernel_major     = 0;
    int  kernel_minor     = 0;
    std::string platform_name;        // "linux" / "macosx" / "windows"
    std::string distro_id;            // "ubuntu" / "rhel" / "alpine" / ...
};

export Capability probe() noexcept;          // 一次性探测,缓存结果
export Capability& cached() noexcept;        // 进程级缓存

} // namespace
```

实现要点(`probe()`):
- 平台:`#if defined(__linux__)` 等编译期分支。
- `unpriv_userns`:读 `/proc/sys/kernel/unprivileged_userns_clone`,值为 `1` 即可;Arch 等发行版默认开。
- `unpriv_overlay`:`mount("overlay", "/tmp/xlings-probe-XXXXXX", "overlay", 0, "lowerdir=/etc,upperdir=...,workdir=...")` 用 user-ns 跑一次,看是否成功;失败立即清理并标 false。
- `bwrap_present`:`which bwrap`(用 `xlings::platform::find_executable`)。
- `bwrap_overlay`:`bwrap --version` 解析版本号。
- `apparmor_blocks_userns`:读 `/sys/kernel/security/apparmor/profiles` 或检测 `kernel.apparmor_restrict_unprivileged_userns` sysctl。
- 缓存:`probe()` 第一次跑全;后续 `cached()` 直接返回。`xlings subos doctor` 命令提供"重新探测"。

### 0.5 仲裁器(autodegrade)

```cpp
// src/core/subos/arbiter.cppm
export module xlings.core.subos.arbiter;
import std;
import xlings.core.subos.mode;
import xlings.core.subos.capability;

namespace xlings::subos {

export struct ArbitratedMode {
    Mode requested;
    Mode effective;            // 实际生效(可能被降级)
    std::vector<std::string> degrade_reasons;
};

export ArbitratedMode arbitrate(Mode requested,
                                const Capability& caps,
                                bool no_autodegrade) noexcept;

} // namespace
```

仲裁规则伪代码:

```cpp
ArbitratedMode arbitrate(Mode req, const Capability& c, bool no_auto) {
    auto try_ = [&](Mode m) -> std::optional<std::string> {
        switch (m) {
        case Mode::Default:
            return std::nullopt;  // 永远可用
        case Mode::Medium:
            if (!c.linux)            return "non-linux platform";
            if (!c.unpriv_userns)    return "unprivileged user namespaces disabled";
            if (c.apparmor_blocks_userns)
                                     return "apparmor restricts unprivileged user-ns "
                                            "(set sysctl kernel.apparmor_restrict_unprivileged_userns=0)";
            return std::nullopt;
        case Mode::Heavy:
            if (auto m_err = try_(Mode::Medium); m_err)  return *m_err;
            if (!c.unpriv_overlay)   return "kernel lacks unprivileged OverlayFS (need ≥5.11)";
            if (!c.bwrap_present)    return "bubblewrap binary not found";
            if (!c.bwrap_overlay)    return "bwrap version too old (need ≥0.7 for --overlay-src)";
            return std::nullopt;
        case Mode::Full:
            if (!c.linux)            return "non-linux platform";
            if (!c.bwrap_present && !c.nspawn_present)
                                     return "neither bwrap nor systemd-nspawn available";
            return std::nullopt;
        }
        return std::nullopt;
    };

    ArbitratedMode out{ req, req, {} };
    while (auto err = try_(out.effective)) {
        out.degrade_reasons.push_back(
            std::format("{} unavailable: {}", to_string(out.effective), *err));
        if (no_autodegrade)
            return out;  // 调用方据此报错退出
        Mode next = degrade(out.effective);
        if (next == Mode::Full && out.effective == Mode::Full) {
            // full 没有自然降级目标(rootfs ≠ overlay)
            return out;
        }
        if (next == out.effective) break;
        out.effective = next;
    }
    return out;
}
```

调用方判断:
- `effective != requested`:打印 warn,展示 `degrade_reasons`,提示可设 `XLINGS_NO_AUTODEGRADE=1`。
- `effective == Default && requested != Default && no_autodegrade`:exit 1。

### 0.6 EventStream 事件

复用现有 `xlings::runtime::EventStream`,新增事件类型:

| 事件 ID | payload |
|---|---|
| `subos_created` | `{ name, mode, base?, dir }` |
| `subos_arbitrated` | `{ requested, effective, degrade_reasons[] }` |
| `subos_entering` | `{ name, mode, backend }` |
| `subos_entered` | `{ name, pid, mode }` |
| `subos_exited` | `{ name, exitcode }` |
| `subos_bootstrap_progress` | `{ phase, percent, bytes_done, bytes_total }` (full 专用) |
| `subos_capability_warning` | `{ key, message }` |

CLI 渲染层把 `subos_arbitrated` 映射成 `[xlings] WARNING: requested 'heavy', running as 'medium' …` 之类的友好输出。

### 0.7 Hook 系统

```
subos/<name>/hooks/
├── pre-enter.sh    # 进入前(default 档:在 use 时执行;其它档:进 namespace 前)
└── post-enter.sh   # 进入后(default 档:env 注入后;其它档:在 namespace 内 source)
```

执行约定:
- POSIX shell;由 xlings 用 `bash --noprofile --norc -- <hook>` 执行。
- 环境变量:`XLINGS_SUBOS_NAME`、`XLINGS_SUBOS_DIR`、`XLINGS_SUBOS_MODE`、`XLINGS_SUBOS_BACKEND`(后两者在 default 档为空)。
- 退出码非 0 → 终止 enter 流程,event `subos_capability_warning`。

---

## 1. default 档实现

### 1.1 目标回顾

> 不进 namespace,只通过环境变量让宿主进程看到 subos 的工具与库。
> 是 90% 用户的默认档,也是 macOS / Windows 的唯一可用档。

### 1.2 目录布局

```
$XLINGS_HOME/subos/<name>/
├── .xlings.json              # mode = "default"
├── local/
│   ├── bin/                  # xpkg 默认安装到这里
│   ├── lib/                  # 含 pkgconfig/、cmake/
│   ├── include/
│   ├── share/
│   └── libexec/
├── xvm/                      # 现有版本数据库
├── generations/              # 现有快照
└── hooks/                    # 可选
```

关键:`local/` 子树是新增的"安装根"。`xpkg install --in <subos>` 不再写到 `subos/<n>/data/xpkgs/`,而落到 `subos/<n>/local/`,使其与 medium 的 bind 源点对齐。

### 1.3 启动序列(`xlings subos use <n>`)

```
1. 读取 subos/<n>/.xlings.json,确认 mode == default
2. 执行 hooks/pre-enter.sh(若存在)
3. 计算环境变量并输出到 shell:
   PATH               = $SUBOS/local/bin:$SUBOS/bin:$ORIG_PATH
   LD_LIBRARY_PATH    = $SUBOS/local/lib:$ORIG_LD
   CPATH              = $SUBOS/local/include:$ORIG_CPATH
   PKG_CONFIG_PATH    = $SUBOS/local/lib/pkgconfig:$SUBOS/local/share/pkgconfig:$ORIG_PC
   CMAKE_PREFIX_PATH  = $SUBOS/local:$ORIG_CMAKE
   XLINGS_ACTIVE_SUBOS= <name>
   XLINGS_SUBOS_DIR   = $SUBOS
4. 写入 subos/current 符号链接(沿用现有逻辑)
5. 执行 hooks/post-enter.sh(若存在)
```

注意:
- "输出到 shell"在 xlings 里已经通过 `xlings-profile.sh` + `eval $(xlings env)` 实现;default 档只是把要 export 的变量列表扩大。
- `$ORIG_*` 表示用户原 shell 里的同名变量;若为空则跳过冒号。
- 不 fork 新进程,完全在当前 shell 内生效。

### 1.4 关键代码骨架

```cpp
// src/core/subos/env.cppm
export module xlings.core.subos.env;
import std;
import xlings.core.subos.config_v2;

namespace xlings::subos {

export struct EnvDelta {
    std::map<std::string, std::string> to_set;  // 完整新值(prepended with $ORIG)
    std::vector<std::string>           to_unset;
};

// 计算 default 档应该 export 的 env 变量。pure function,易测。
export EnvDelta compute_env_default(const SubosConfig& cfg,
                                    const fs::path& subos_dir,
                                    const std::map<std::string, std::string>& current_env) noexcept;

// 把 EnvDelta 序列化为 sh 可 source 的字符串(供 `xlings env` 命令)
export std::string emit_sh(const EnvDelta& d) noexcept;

// 把 EnvDelta 序列化为 fish 形式
export std::string emit_fish(const EnvDelta& d) noexcept;

// 写入符号链接 subos/current → subos/<name>
export std::expected<void, std::string>
activate_default(const std::string& name) noexcept;

} // namespace
```

`compute_env_default` 实现要点:
- 从 `cfg.default_.env` 拿到模板(支持 `$SUBOS` 和 `$ORIG` 占位)。
- 替换 `$SUBOS` 为绝对路径;替换 `$ORIG` 为 `current_env` 里的同名变量(空则去掉冒号)。
- 顺序:`$SUBOS_LOCAL` 永远在前(确保 subos 工具优先)。

### 1.5 失败模式 & 处理

| 失败 | 处理 |
|---|---|
| `local/` 子树缺失 | `subos use` 自动 `mkdir -p`(惰性,首次) |
| pre-enter hook 非 0 | 终止 use,event `subos_capability_warning`,不修改 current 链接 |
| `.xlings.json` 损坏 / mode 字段无效 | 报错并提示 `xlings subos doctor <n>`;不静默回退 |

### 1.6 测试

`tests/e2e/subos_default_use_test.sh`:
1. `xlings subos new dev` → 检查 `subos/dev/local/{bin,lib,...}` 存在。
2. `eval $(xlings subos use dev --shell sh)` → `echo $LD_LIBRARY_PATH` 含 `subos/dev/local/lib`。
3. `xlings xpkg install foo --in dev` → 检查产物落到 `subos/dev/local/bin/foo`。
4. `eval $(xlings subos use other) && eval $(xlings subos use dev)` → 切换两次后 PATH 不重复堆叠。
5. macOS / Windows runner 上同样跑通 1–4(行为对称)。

---

## 2. medium 档实现

### 2.1 目标回顾

> 在 default 基础上进 mount namespace:bind `local/*` 到 `/usr/local/*`、私有 HOME / /tmp / /opt。
> Linux only,不可用时自动降 default。

### 2.2 目录布局(在 default 基础上)

```
$XLINGS_HOME/subos/<name>/
├── ...(default 全部)
├── home/                     # 映射为 $HOME
├── opt/                      # 映射为 /opt/xlings/<name>
└── tmp-seed/                 # 启动时拷到 tmpfs 的初始化模板(可选)
```

### 2.3 配置 schema 切片

```json
"medium": {
  "binds": [
    { "src": "local/bin",     "dst": "/usr/local/bin" },
    { "src": "local/lib",     "dst": "/usr/local/lib" },
    { "src": "local/include", "dst": "/usr/local/include" },
    { "src": "local/share",   "dst": "/usr/local/share" },
    { "src": "local/libexec", "dst": "/usr/local/libexec" }
  ],
  "home":     "home",
  "opt":      "opt",
  "tmp":      "tmpfs",          // tmpfs | bind:<path> | none
  "tmp_seed": "tmp-seed",       // 可选,启动时拷到 /tmp
  "share_ssh": false,           // 真值时 ro-bind $HOME/.ssh
  "share_git": false            // 真值时 ro-bind $HOME/.gitconfig
}
```

### 2.4 启动序列(`xlings subos enter <n>`)

```
1. arbitrate(Medium, caps) → 若 effective != Medium,自动降级或报错
2. 执行 hooks/pre-enter.sh(在宿主上下文)
3. 构建 MountPlan(LightPlan + HomePlan + OptPlan + TmpfsPlan)
4. UnshareDriver.execute(plan):
   a. unshare(CLONE_NEWUSER | CLONE_NEWNS)
   b. 写 /proc/self/{uid_map,setgroups,gid_map} 把当前 uid 映射到 root
   c. mount("none", "/", NULL, MS_REC|MS_PRIVATE, NULL)  避免传播
   d. 对 plan.binds 逐项:
        if (!exists(dst)) mkdir_p(dst)  // namespace 内创建,不影响宿主
        mount(src, dst, NULL, MS_BIND, NULL)
   e. 若有 home bind:mount(subos/<n>/home, $HOME, NULL, MS_BIND, NULL)
   f. 若有 opt bind:mkdir_p(/opt/xlings/<n>) + mount(subos/<n>/opt, ...)
   g. 若 tmp == tmpfs:mount("tmpfs", "/tmp", "tmpfs", 0, "size=512M,mode=1777")
   h. 若 tmp_seed 存在:cp -a subos/<n>/tmp-seed/. /tmp/
   i. setenv 所有 default 档的 env(确保 namespace 内 PATH 等也对)
   j. setenv("XLINGS_ACTIVE_SUBOS", name)
   k. exec(post_enter_hook) — 失败则退出
   l. exec($SHELL, "-l")
5. 子进程退出 → namespace 自动销毁
6. 父进程 wait,emit subos_exited 事件
```

### 2.5 MountPlan 数据结构(M2 时也复用)

```cpp
// src/core/subos/plan/plan.cppm
export module xlings.core.subos.plan;
import std;

namespace xlings::subos {

namespace fs = std::filesystem;

export struct BindMount {
    fs::path src;       // 宿主侧绝对路径
    fs::path dst;       // namespace 内绝对路径
    bool     ro       = false;
    bool     mkdir_dst = true;   // dst 不存在时是否创建(在 ns 内)
    bool     optional = false;   // src 不存在时是否跳过
};

export struct TmpfsMount {
    fs::path    dst;
    std::string size_opt = "512M";
    uint32_t    mode     = 0777;  // 默认 1777 sticky
};

export struct KernelMount {
    enum class Kind { Proc, Sysfs, Devtmpfs, Devpts, Mqueue };
    Kind     kind;
    fs::path dst;
    bool     ro = false;
};

export struct MountPlan {
    enum class Root { Passthrough, Overlay, Rootfs };
    Root root_strategy = Root::Passthrough;

    // overlay(heavy 用)
    std::vector<fs::path> overlay_lower;
    fs::path overlay_upper;
    fs::path overlay_work;

    // rootfs(full 用)
    fs::path rootfs_dir;

    std::vector<BindMount>   binds;
    std::vector<TmpfsMount>  tmpfs;
    std::vector<KernelMount> kernel;

    std::map<std::string, std::string> env;
    std::optional<std::string>          hostname;
    std::vector<std::string>            shell_cmd;  // 默认 ["$SHELL","-l"]

    // 后端能力声明
    struct Caps {
        bool need_user_ns    = true;
        bool need_overlay    = false;
        bool need_pivot_root = false;
        bool need_pid_ns     = false;
        bool need_uts_ns     = false;
    } caps;
};

} // namespace
```

### 2.6 medium plan builder

```cpp
// src/core/subos/plan/plan_medium.cppm
export module xlings.core.subos.plan.medium;
import std;
import xlings.core.subos.plan;
import xlings.core.subos.config_v2;

namespace xlings::subos {

export MountPlan build_medium_plan(const SubosConfig& cfg) {
    MountPlan p;
    p.root_strategy = MountPlan::Root::Passthrough;

    auto sub = cfg.dir;
    for (auto& b : cfg.medium.binds) {
        if (!fs::exists(sub / b.src)) {
            // 自动 mkdir 源,保持源/目标一一对应
            std::error_code ec; fs::create_directories(sub / b.src, ec);
        }
        p.binds.push_back({
            .src = sub / b.src,
            .dst = b.dst,
            .ro  = false,
            .mkdir_dst = true,
        });
    }
    if (!cfg.medium.home.empty()) {
        p.binds.push_back({
            .src = sub / cfg.medium.home,
            .dst = std::getenv("HOME"),
            .mkdir_dst = false,  // $HOME 一定存在
        });
    }
    if (!cfg.medium.opt.empty()) {
        p.binds.push_back({
            .src = sub / cfg.medium.opt,
            .dst = fs::path("/opt/xlings") / cfg.name,
            .mkdir_dst = true,
        });
    }
    if (cfg.medium.tmp == "tmpfs") {
        p.tmpfs.push_back({ .dst = "/tmp" });
    }
    // 选择性共享
    if (cfg.medium.share_ssh) {
        p.binds.push_back({
            .src = fs::path(std::getenv("HOME")) / ".ssh",
            .dst = fs::path(std::getenv("HOME")) / ".ssh",
            .ro = true, .optional = true,
        });
    }
    if (cfg.medium.share_git) {
        p.binds.push_back({
            .src = fs::path(std::getenv("HOME")) / ".gitconfig",
            .dst = fs::path(std::getenv("HOME")) / ".gitconfig",
            .ro = true, .optional = true,
        });
    }

    // env 沿用 default 档,确保 namespace 内 PATH 等仍对
    p.env = compute_env_default(cfg, sub, current_env_map()).to_set;
    p.env["XLINGS_ACTIVE_SUBOS"] = cfg.name;
    p.env["XLINGS_SUBOS_MODE"]   = "medium";

    p.caps.need_user_ns = true;
    return p;
}

} // namespace
```

### 2.7 unshare 后端

```cpp
// src/core/subos/backend/backend_unshare.cppm
export module xlings.core.subos.backend.unshare;
import std;
import xlings.core.subos.plan;
import xlings.core.subos.backend;

namespace xlings::subos {

export class UnshareDriver final : public BackendDriver {
public:
    Capability::Need caps_required() const noexcept override {
        return { .user_ns = true };
    }

    std::expected<int, std::string>
    execute(const MountPlan& plan,
            const fs::path&  cwd,
            std::span<const std::string> shell_cmd) override {
        // C++23 模块内,直接调底层 syscall(libc 通过 std::system 不稳)
        // 实际实现走 fork + 子进程内 unshare(2) + bind mount(2) + execve(2)
        return run_in_user_mount_ns_(plan, cwd, shell_cmd);
    }
};

} // namespace
```

`run_in_user_mount_ns_` 关键步骤:

```cpp
static int run_in_user_mount_ns_(const MountPlan& plan,
                                 const fs::path& cwd,
                                 std::span<const std::string> argv) {
    // 1. fork
    pid_t pid = fork();
    if (pid < 0) return errno;
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }

    // 子进程
    // 2. unshare user-ns + mount-ns(单调用)
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
        log::error("unshare failed: {}", strerror(errno));
        _exit(127);
    }

    // 3. 建立 root 映射(在 user-ns 内取得 root 身份)
    write_uid_map_();   // /proc/self/uid_map = "0 <uid> 1\n"
    deny_setgroups_();  // /proc/self/setgroups = "deny"
    write_gid_map_();   // /proc/self/gid_map = "0 <gid> 1\n"

    // 4. propagation private,避免传播到宿主
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        log::error("mount(/, MS_PRIVATE) failed: {}", strerror(errno));
        _exit(127);
    }

    // 5. tmpfs 先于 bind(/tmp 必须早 mount,seed 才能落进去)
    for (auto& t : plan.tmpfs) {
        mount("tmpfs", t.dst.c_str(), "tmpfs", 0,
              std::format("size={},mode={:o}", t.size_opt, t.mode).c_str());
    }

    // 6. binds
    for (auto& b : plan.binds) {
        if (b.optional && !fs::exists(b.src)) continue;
        if (b.mkdir_dst) std::error_code ec; fs::create_directories(b.dst, ec);
        unsigned long flags = MS_BIND;
        if (b.ro) flags |= MS_RDONLY | MS_REMOUNT;  // 二次 remount 才能 ro-bind
        if (mount(b.src.c_str(), b.dst.c_str(), nullptr, MS_BIND, nullptr) != 0) {
            log::warn("bind {} → {} failed: {}", b.src, b.dst, strerror(errno));
            continue;
        }
        if (b.ro) {
            mount(nullptr, b.dst.c_str(), nullptr,
                  MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr);
        }
    }

    // 7. tmp_seed cp(在 tmpfs mount 之后)
    seed_tmp_(plan);

    // 8. env
    for (auto& [k, v] : plan.env) setenv(k.c_str(), v.c_str(), 1);

    // 9. cwd
    if (!cwd.empty()) chdir(cwd.c_str());

    // 10. exec
    auto args = to_argv_(argv);
    execvp(args[0], args.data());
    _exit(127);
}
```

### 2.8 失败 & 自动降级

| 失败 | 处理 |
|---|---|
| `unshare` ENOSPC / EPERM | 探测 caps 已经会先发现;execute 之前仲裁应已降到 default |
| `mount` 单条 bind 失败 | warn 并继续(部分隔离);env `XLINGS_SUBOS_DEGRADED=1` 让调用方知道 |
| pre-enter hook 非 0 | 不进 namespace,event subos_capability_warning |
| `execvp` 失败 | exit 127,父进程上报 |

### 2.9 测试

`tests/e2e/subos_medium_enter_test.sh`(仅 Linux):
1. 创建 medium subos,写一个 `local/bin/hello` 脚本。
2. `xlings subos enter dev -c "which hello"` → 输出 `/usr/local/bin/hello`。
3. enter 内 `touch ~/foo` → 退出后宿主 `~/foo` **不存在**;检查 `subos/dev/home/foo` **存在**。
4. enter 内 `touch /tmp/x` → 退出后宿主 `/tmp/x` 不存在(tmpfs 销毁)。
5. enter 内 `pkg-config --list-all | grep subospc` → 列到 subos 的 .pc。
6. 关 user-ns(`sysctl kernel.unprivileged_userns_clone=0`)后再跑 → 自动降 default,exit 0。

---

## 3. heavy 档实现

### 3.1 目标回顾

> 在 medium 基础上引入 OverlayFS:让用户看到一个"宿主的克隆体" —— 改它不影响真宿主,upper 可丢弃。
> 后端用 bubblewrap(原生支持 `--overlay-src`)。

### 3.2 目录布局(在 medium 基础上)

```
$XLINGS_HOME/subos/<name>/
├── ...(medium 全部)
├── upper/                    # OverlayFS upperdir(差量)
├── work/                     # OverlayFS workdir
├── etc-overrides/            # 自管理的 /etc 子集
│   ├── passwd
│   ├── nsswitch.conf
│   ├── hostname
│   └── ld.so.conf.d/
└── meta/
    └── base.json             # 记录基线宿主(uname -a / lsb_release)
```

### 3.3 配置 schema 切片

```json
"heavy": {
  "overlay": {
    "lower": ["/"],            // 默认整个 /
    "upper": "upper",
    "work":  "work"
  },
  "etc_overrides": "etc-overrides",
  "etc_policy":    "heavy.toml",   // 数据文件,定义 /etc 哪些 bind / 替换 / stub
  "backend":       "bwrap"
}
```

`etc_policy/heavy.toml` 示例:

```toml
[bind]
"/etc/resolv.conf"  = { from = "host", ro = true }
"/etc/localtime"    = { from = "host", ro = true }
"/etc/ssl/certs"    = { from = "host", ro = true }

[override]
"/etc/passwd"       = { from = "etc-overrides/passwd" }
"/etc/nsswitch.conf"= { from = "etc-overrides/nsswitch.conf" }
"/etc/hostname"     = { from = "etc-overrides/hostname" }
```

### 3.4 启动序列(用 bwrap)

```
1. arbitrate(Heavy, caps) → 检查 unpriv_overlay && bwrap_overlay
2. pre-enter hook(宿主上下文)
3. 构建 MountPlan(HeavyPlan):
     root_strategy = Overlay
     overlay_lower = ["/"]
     overlay_upper = subos/<n>/upper
     overlay_work  = subos/<n>/work
     binds = [/etc/* 按 etc-policy 表展开] + [home/opt 同 medium]
     tmpfs = [/tmp, /run]
     kernel = [Proc, Sysfs(ro), Devtmpfs]
4. BwrapDriver.execute(plan)
5. 同 medium:wait + emit
```

### 3.5 BwrapDriver

```cpp
// src/core/subos/backend/backend_bwrap.cppm
export module xlings.core.subos.backend.bwrap;
import std;
import xlings.core.subos.backend;
import xlings.core.subos.plan;

namespace xlings::subos {

export class BwrapDriver final : public BackendDriver {
public:
    Capability::Need caps_required() const noexcept override {
        return {
            .user_ns = true,
            .overlay = true,
            .bwrap   = true,
        };
    }

    std::expected<int, std::string>
    execute(const MountPlan& plan,
            const fs::path&  cwd,
            std::span<const std::string> shell_cmd) override {
        std::vector<std::string> argv = { "bwrap", "--die-with-parent",
                                           "--new-session" };
        // 1. root 策略
        if (plan.root_strategy == MountPlan::Root::Overlay) {
            for (auto& l : plan.overlay_lower)
                argv.insert(argv.end(), { "--overlay-src", l.string() });
            argv.insert(argv.end(), { "--overlay",
                                      plan.overlay_upper.string(),
                                      plan.overlay_work.string(), "/" });
        } else if (plan.root_strategy == MountPlan::Root::Rootfs) {
            argv.insert(argv.end(), { "--bind", plan.rootfs_dir.string(), "/" });
        }
        // 2. kernel mounts
        for (auto& k : plan.kernel) {
            switch (k.kind) {
            case KernelMount::Kind::Proc:
                argv.insert(argv.end(), { "--proc", k.dst.string() }); break;
            case KernelMount::Kind::Sysfs:
                argv.insert(argv.end(), { "--ro-bind", "/sys", k.dst.string() }); break;
            case KernelMount::Kind::Devtmpfs:
                argv.insert(argv.end(), { "--dev", k.dst.string() }); break;
            // ...
            }
        }
        // 3. binds
        for (auto& b : plan.binds) {
            const char* op = b.ro ? "--ro-bind" : "--bind";
            if (b.optional) op = b.ro ? "--ro-bind-try" : "--bind-try";
            argv.insert(argv.end(), { op, b.src.string(), b.dst.string() });
        }
        // 4. tmpfs
        for (auto& t : plan.tmpfs)
            argv.insert(argv.end(), { "--tmpfs", t.dst.string() });
        // 5. env
        for (auto& [k, v] : plan.env)
            argv.insert(argv.end(), { "--setenv", k, v });
        // 6. hostname
        if (plan.hostname)
            argv.insert(argv.end(), { "--hostname", *plan.hostname });
        // 7. cwd + cmd
        if (!cwd.empty())
            argv.insert(argv.end(), { "--chdir", cwd.string() });
        argv.push_back("--");
        for (auto& s : shell_cmd) argv.push_back(s);

        return spawn_and_wait_(argv);
    }
};

} // namespace
```

### 3.6 etc-overrides 模板生成

`xlings subos new <n> --mode heavy` 时,生成默认模板:

```bash
# subos/<n>/etc-overrides/passwd
root:x:0:0::/root:/bin/sh
xlings:x:1000:1000::/home/xlings:/bin/sh

# subos/<n>/etc-overrides/nsswitch.conf
passwd:  files
group:   files
hosts:   files dns

# subos/<n>/etc-overrides/hostname
xlings-<n>
```

用户可手动编辑;`xlings subos doctor <n>` 校验是否完整。

### 3.7 `subos discard` 命令

```
xlings subos discard <n>   # 删 subos/<n>/upper/* 和 work/*,回到差量起点
```

实现:
```cpp
export std::expected<void, std::string> discard(const std::string& name) {
    auto cfg = read_config(name);
    if (cfg.mode != Mode::Heavy)
        return std::unexpected("discard only valid for heavy mode");
    auto upper = cfg.dir / cfg.heavy.overlay.upper;
    auto work  = cfg.dir / cfg.heavy.overlay.work;
    std::error_code ec;
    fs::remove_all(upper, ec);
    fs::remove_all(work,  ec);
    fs::create_directory(upper);
    fs::create_directory(work);
    return {};
}
```

### 3.8 失败 & 降级

| 失败 | 处理 |
|---|---|
| `bwrap` 不存在 | 仲裁前已发现 → 降 medium + 提示安装 |
| OverlayFS 不支持 | 同上 |
| `--overlay-src` 不识别(bwrap < 0.7) | 同上 |
| 切网后 DNS 失效 | resolv.conf 是 ro-bind,exit 后重进生效 |
| upper 累积过大 | `xlings subos info` 显示 upper 大小;`discard` 清理 |

### 3.9 测试

`tests/e2e/subos_heavy_enter_test.sh`(仅 Linux ≥5.11 + bwrap):
1. enter heavy,在 namespace 内 `apt-get install -y cowsay`(或写文件到 `/usr`)。
2. exit,宿主 `/usr/games/cowsay` 不存在;`subos/dev/upper/usr/games/cowsay` 存在。
3. `xlings subos discard dev` → upper 清空。
4. 再 enter,`cowsay` 不在(回到 lower)。
5. enter 内 `hostname` → 输出 `xlings-dev`(UTS namespace 生效)。
6. 内核降级到 5.4 / 关 user-ns / 卸 bwrap → 三种条件分别触发自动降 medium 或 default。

---

## 4. full 档实现

### 4.1 目标回顾

> 完整独立 rootfs(Alpine / Debian / Arch),subos 看起来像迷你 Linux 发行版。
> 跨发行版可移植,可换 libc。bootstrap 阶段需要下载 base image。

### 4.2 目录布局

```
$XLINGS_HOME/subos/<name>/
├── .xlings.json
├── rootfs/                   # 完整 rootfs(独立 /usr,/lib,/etc,...)
├── meta/
│   ├── base.json             # { distro, version, arch, sha256, source_url }
│   ├── arch.txt              # x86_64 / aarch64
│   └── hooks/                # bootstrap-time hooks(post-extract.sh 等)
├── home/                     # 映射到 rootfs 内 $HOME
└── shared/                   # 可选,映射到 rootfs 内 /shared(跨主机数据)
```

### 4.3 Bootstrap 流水线

`xlings subos new <n> --mode full --base alpine:3.19` 执行步骤:

```
1. 解析 base spec → BaseSpec { distro, version, arch }
   - alpine:3.19  → URL 模板 https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/x86_64/alpine-minirootfs-3.19.7-x86_64.tar.gz
   - debian:trixie-slim → docker hub OCI manifest 或镜像源 tarball
   - arch:rolling → https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst
2. 下载到 $XLINGS_HOME/cache/subos-bases/<distro>-<version>-<arch>.tar.gz
   - 用 xim::downloader 复用 sha256 / HEAD-fallback / Last-Modified 缓存(0.4.15+ 能力)
3. 校验 sha256(base spec 自带预定义哈希;由 xlings 配套发布的 catalog 提供)
4. 解压到 subos/<n>/rootfs/
5. 写 meta/base.json
6. 注入 xlings 自身二进制到 rootfs/usr/local/bin/xlings(可选)
7. 注入最小 init 文件:
   - rootfs/etc/resolv.conf  → 启动时 ro-bind 替换
   - rootfs/etc/passwd       → 至少含 root + 当前 uid 映射用户
   - rootfs/etc/hosts        → localhost
8. 执行 meta/hooks/post-extract.sh(若存在,如 alpine 用 apk add 装 ca-certs)
9. event subos_created
```

### 4.4 配置 schema 切片

```json
"full": {
  "base":    "alpine:3.19",
  "rootfs":  "rootfs",
  "backend": "bwrap",                 // bwrap | nspawn

  "binds": [
    { "src_host": "/etc/resolv.conf", "dst": "/etc/resolv.conf", "ro": true },
    { "src_host": "/etc/localtime",   "dst": "/etc/localtime",   "ro": true }
  ],

  "kernel": {
    "proc":  true,
    "sysfs": "ro",       // ro | rw | none
    "dev":   "minimal"   // full | minimal | none
  },

  "uts_hostname": "xlings-<name>",

  "shared": {
    "host":  "shared",
    "guest": "/shared"
  }
}
```

### 4.5 启动序列(bwrap 后端)

```
1. arbitrate(Full, caps)
   - linux + (bwrap || nspawn);bwrap 优先
2. pre-enter hook(宿主上下文)
3. 构建 FullPlan:
     root_strategy = Rootfs
     rootfs_dir = subos/<n>/rootfs
     binds = config.full.binds + home/shared
     tmpfs = [/tmp, /run]
     kernel = [Proc, Sysfs(ro), Devtmpfs(minimal)]
     hostname = config.full.uts_hostname
     shell_cmd = ["/bin/sh", "-l"]
4. BwrapDriver.execute(plan)
5. 子进程退出 → 容器自销
```

### 4.6 BootstrapStrategy

```cpp
// src/core/subos/bootstrap/bootstrap.cppm
export module xlings.core.subos.bootstrap;
import std;
import xlings.core.subos.config_v2;

namespace xlings::subos {

export struct BootstrapResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> notes;
};

export class BootstrapStrategy {
public:
    virtual ~BootstrapStrategy() = default;
    virtual BootstrapResult create(SubosConfig& cfg) = 0;
};

export std::unique_ptr<BootstrapStrategy> for_mode(Mode m);

} // namespace
```

`bootstrap_image.cppm`(full 用)关键流程:

```cpp
class ImageBootstrap final : public BootstrapStrategy {
public:
    BootstrapResult create(SubosConfig& cfg) override {
        auto base = parse_base_spec(cfg.full.base);   // "alpine:3.19" -> {distro,ver}
        if (!base) return { false, base.error(), {} };

        auto cache_path = cache_dir() /
            std::format("{}-{}-{}.tar.gz", base->distro, base->version, base->arch);

        // 1. 下载(复用 xim::downloader)
        if (!fs::exists(cache_path)) {
            auto url = url_template_for(*base);
            auto sha = expected_sha_for(*base);   // 来自 xlings 维护的 base catalog
            DownloadTask task{ url, cache_path, sha };
            if (auto r = downloader::fetch(task); !r)
                return { false, r.error(), {} };
        }

        // 2. 解压
        auto rootfs = cfg.dir / cfg.full.rootfs;
        fs::create_directories(rootfs);
        if (auto r = extract_tar_gz(cache_path, rootfs); !r)
            return { false, r.error(), {} };

        // 3. 写 meta
        write_base_meta(cfg.dir / "meta" / "base.json", *base, cache_path);

        // 4. 注入 xlings 自身(可选)
        if (cfg.full.inject_xlings) {
            auto self = current_executable_path();
            fs::copy_file(self, rootfs / "usr/local/bin/xlings",
                          fs::copy_options::overwrite_existing);
        }

        // 5. post-extract hook(distro-specific,如 alpine 装 ca-certs)
        run_post_extract_hook(*base, rootfs);

        return { true, "", {} };
    }
};
```

### 4.7 nspawn 后端(可选)

```cpp
// src/core/subos/backend/backend_nspawn.cppm
export class NspawnDriver final : public BackendDriver {
public:
    Capability::Need caps_required() const noexcept override {
        return { .systemd_252 = true, .nspawn = true };
    }
    std::expected<int, std::string>
    execute(const MountPlan& plan, ...) override {
        std::vector<std::string> argv = {
            "systemd-nspawn",
            "--quiet", "--as-pid2", "--keep-unit",
            std::format("--user={}", current_user()),
            std::format("--directory={}", plan.rootfs_dir.string()),
            std::format("--hostname={}", plan.hostname.value_or("xlings")),
        };
        for (auto& b : plan.binds) {
            const char* op = b.ro ? "--bind-ro" : "--bind";
            argv.push_back(std::format("{}={}:{}", op, b.src.string(), b.dst.string()));
        }
        // ... cap 处理、env、cmd ...
        return spawn_and_wait_(argv);
    }
};
```

### 4.8 Export / Import

```cpp
// src/core/subos/exporter/exporter_rootfs.cppm
export class RootfsExporter final : public Exporter {
public:
    std::expected<void, std::string>
    write_to(const SubosConfig& cfg, const fs::path& dst) override {
        // tar.zst:rootfs/ + meta/ + .xlings.json
        return tar_zst_create(dst, {
            { cfg.dir / cfg.full.rootfs, "rootfs" },
            { cfg.dir / "meta",          "meta" },
            { cfg.dir / ".xlings.json",  ".xlings.json" },
        });
    }
};

// import 反向:解压到 subos/<name>/,校验 meta.arch == 当前架构
```

### 4.9 失败 & 降级

| 失败 | 处理 |
|---|---|
| 下载失败 | 重试 3 次,后改换镜像源(catalog 维护多个 URL) |
| sha 不匹配 | 立即终止,event subos_capability_warning |
| 架构不匹配(导入跨架构 image) | 拒绝,提示 `--force-arch=...`(危险) |
| bwrap & nspawn 都不在 | 仲裁阶段已发现,无降级目标 → 报错退出 |
| /dev 缺关键设备 | bwrap `--dev` 自动建 minimal /dev;不应失败 |

### 4.10 测试

`tests/e2e/subos_full_alpine_test.sh`(Linux + bwrap):
1. `xlings subos new lab --mode full --base alpine:3.19 -y` → 创建耗时 ≤ 30s。
2. `xlings subos enter lab -c "cat /etc/os-release"` → 输出 `Alpine Linux v3.19`。
3. enter 内 `apk add gcc`,跑 hello world,exit。
4. 第二次 enter 仍能用 gcc(rootfs 持久)。
5. `xlings subos export lab > /tmp/lab.tar.zst` → 成功,文件大小 ≥ 5 MB。
6. 删 lab,`xlings subos import lab2 < /tmp/lab.tar.zst` → 复活,验证 gcc 仍在。
7. 跨发行版机器(Ubuntu / Arch / Fedora)分别 import,os-release 始终是 Alpine。

---

## 5. 测试矩阵总览

| 用例 | default | medium | heavy | full |
|---|:-:|:-:|:-:|:-:|
| `subos new` 创建目录骨架 | ✓ | ✓ | ✓ | ✓ |
| `subos use` env 注入正确 | ✓ | ✓(隐式) | ✓ | ✓ |
| `xpkg install --in <n>` 装到 `local/` | ✓ | ✓ | ✓ | ✓ |
| `subos enter` 进入隔离 | ✗(N/A) | ✓ | ✓ | ✓ |
| 退出后宿主无残留 | ✓(没改过) | ✓ | ✓ | ✓ |
| `discard` 清 upper | ✗ | ✗ | ✓ | ✗ |
| `export/import` 可移植 | ✓ tar | ✓ tar | ⚠️ tar(同发行版) | ✓ tar.zst |
| autodegrade 触发 | ✗(底) | ✓→default | ✓→medium | ✗(无降级,报错) |
| macOS / Windows 行为 | 等同 Linux | 自动降 default | 拒绝 | 拒绝 |
| pre/post-enter hook 执行 | ✓(use 时) | ✓ | ✓ | ✓ |
| EventStream 事件齐全 | ✓ | ✓ | ✓ | ✓ |

---

## 6. 渐进引入路径(代码层面)

| 阶段 | 引入物 | 不引入 |
|---|---|---|
| **M0**(default 完善) | `mode.cppm`、`config_v2.cppm`、`env.cppm`、`bootstrap_noop.cppm`、`hook.cppm` | 不引入 MountPlan / BackendDriver / Capability(还用不到) |
| **M1**(medium 落地) | `capability.cppm`、`arbiter.cppm`、`plan.cppm`、`plan_medium.cppm`、`backend.cppm`、`backend_unshare.cppm` | 不引入 Bootstrap 抽象(noop 对所有非 full 都够) |
| **M2**(heavy 主线) | `plan_heavy.cppm`、`backend_bwrap.cppm`、`etc_policy/heavy.toml` | 不引入 ExportStrategy(用 tar 直存够用) |
| **M3**(full bootstrap) | `bootstrap.cppm` 接口 + `bootstrap_image.cppm`、`plan_full.cppm`、catalog 模块(base 元数据) | 暂不引入 nspawn |
| **M4**(export/import + nspawn) | `exporter/*.cppm`、`backend_nspawn.cppm`、OCI 转换器(若需要) | — |

**反模式提醒**:M0 阶段就把 `MountPlan` / `BackendDriver` / `BootstrapStrategy` / `ExportStrategy` 全建出来 —— 是为不存在的需求过度设计。每个抽象都要被两个以上具体场景验证后再固化。

---

## 7. 关键不变式(invariants)

无论哪档,xlings 必须维持:

1. **`xlings subos remove <n>` 后,`subos/<n>/` 目录完全消失,且无遗留 mount**(`mount | grep subos/<n>` 为空)。
2. **`xlings subos use <n>` 后退出 shell,宿主任何路径都不被改动**(default 档:取决于用户在 shell 里做了什么;medium+:namespace 自销保证)。
3. **`subos/<n>/local/` 在所有 mode 下都是 xpkg 的安装根**(default 走 env、medium+ 走 bind),为升降级保留兼容。
4. **`.xlings.json` 的 `mode` 字段 ⟺ subos 实际行为**;升级 mode 必须写回 schema,否则下次 enter 还是旧档。
5. **macOS / Windows 上 `mode != default` 永远走 autodegrade 或报错**,不假装在跑。

---

## 8. 当前代码改动估计

| 文件 | 动作 | 行数 |
|---|---|---|
| `src/core/subos.cppm` | 改:facade,转发到新模块 | -200 / +50 |
| `src/core/subos/mode.cppm` | 新 | ~50 |
| `src/core/subos/config_v2.cppm` | 新(可与现 subos.cppm 中 read/write_config 合并演进) | ~150 |
| `src/core/subos/env.cppm` | 新 | ~120 |
| `src/core/subos/capability.cppm` | 新(M1) | ~200 |
| `src/core/subos/arbiter.cppm` | 新(M1) | ~80 |
| `src/core/subos/hook.cppm` | 新 | ~80 |
| `src/core/subos/plan/plan*.cppm` | 新(M1+M2+M3) | ~80 + ~100 + ~150 + ~200 |
| `src/core/subos/backend/backend*.cppm` | 新(M1+M2+M5) | ~50 + ~250 + ~150 + ~200 |
| `src/core/subos/bootstrap/bootstrap*.cppm` | 新(M3) | ~50 + ~250 |
| `src/core/subos/exporter/*.cppm` | 新(M4) | ~300 |
| `tests/e2e/subos_*_test.sh` | 新 | ~150 × 4 = ~600 |
| `tests/unit/subos_*_test.cpp` | 新(以 plan/arbiter/env 的纯函数为主) | ~400 |

总计预估:**≈ 3500–4000 行 C++ + 600 行 shell 测试**,跨 5 个里程碑,12–16 周。

---

## 9. 与现有代码的衔接点

| 现有点 | 改动 |
|---|---|
| `src/core/subos.cppm` | 退化为 facade;`list_all` / `info` / `remove` 等保留对外接口,内部转发到 `subos/*` |
| `src/core/xself/init.cppm:206-244` | shell profile 写入 `eval $(xlings subos use $XLINGS_DEFAULT_SUBOS)` 或 `xlings env` |
| `config/shell/xlings-profile.sh:9-13` | 由仅 `PATH=` 扩展为 source `xlings env` 输出 |
| `src/core/xim/installer.cppm` | `--in <subos>` 安装时,目标根从 `data/xpkgs/...` 改为 `subos/<n>/local/...`(新约定);旧 subos 通过 `subos migrate` 一键迁移 |
| `src/core/xvm/*` | xvm 元数据继续放 `subos/<n>/xvm/`,不动 |
| EventStream | 在 `src/cli/cli.cppm` / agent 的事件渲染表里加 7 个新事件类型 |

---

## 10. 一句话总结

> xlings subos 4 档共享同一个**目录约定 + 命令骨架 + 仲裁器 + Hook + EventStream**,差异完全收敛在 `MountPlan` 数据 / `BackendDriver` 实现 / `BootstrapStrategy` / `Exporter` 这 4 个策略点上。
> default 是 0 namespace 的"白送档",medium 是真正的"进 ns"分水岭,heavy / full 则按需付费 —— 全程不重造容器运行时,xlings 只负责"让用户用对档位"。
