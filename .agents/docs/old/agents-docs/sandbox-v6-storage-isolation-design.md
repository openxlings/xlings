# Sandbox V6 — 存储隔离设计方案

**Status**: 设计中
**Target**: 0.5.x
**Depends on**: V5 双后端 (bwrap + proot), xim:bwrap setuid 模式 (PR #290)

## 一、动机

V5 沙箱的 home/tmp 数据直接存储在 host 目录下（`~/.xlings/subos/<name>/home/`），
host 上可以直接浏览沙箱内的文件和目录结构。对于隐私敏感场景，需要让 host 上看不到
明文数据和目录结构。

## 二、设计原则

1. **storage 是 subos 的创建时属性**，不是运行时选项
2. **storage 与 sandbox 是正交两个维度**（V4 一致）：shell-level 永远只换 env/PATH；image / tmpfs 只在 `--sandbox` 路径里被消费
3. **xlings 共享部分（`~/.xlings`）始终复用**，不受 storage 模式影响
4. **向后兼容**，现有 subos 默认 `shared`，行为不变
5. **工具链自管理**，所有依赖通过 xim 包生态安装

> **设计变更**: 早期 V6 (2026-05) 让"非 shared 自动进入 sandbox"，把 storage 和 sandbox 焊死成一个轴，违反 V4 正交原则。该耦合已在 followup PR (`fix/subos-sandbox-ux-hotfix`) 移除 — 详见 `sandbox-v6-followup-fixes-2026-05-15.md`。

## 三、存储模式

### 3.1 命令面

```bash
# 创建时指定 storage 模式
xlings subos new dev                          # 默认 shared
xlings subos new dev --storage image          # ext4 镜像
xlings subos new dev --storage tmpfs          # 内存盘

# 使用时 storage 与 sandbox 维度正交（修订自早期 V6 设计）
xlings subos use dev                          # shell-level（env/PATH 切换）
                                              # storage=image|tmpfs 时打 info：
                                              #   "storage=X is sandbox-only;
                                              #    entering shell-level
                                              #    (use --sandbox to activate)"
xlings subos use dev --sandbox                # sandbox-level
                                              # storage=image → 挂 image
                                              # storage=tmpfs → tmpfs
                                              # storage=shared → 普通 bind
```

### 3.2 模式对比

| 模式 | host 看到 | 持久化 | 性能 | 需要 sudo | bwrap | proot |
|------|----------|--------|------|-----------|-------|-------|
| `shared` | 完整目录结构 | 是 | 0% | 否 | 支持 | 支持 |
| `image` | 一个 `.img` 文件 | 是 | ~0% | mount 时 | 支持 | 不支持 |
| `tmpfs` | 无（内存中） | 否 | 0% | 否 | 支持 | 不支持 |

### 3.3 use 时的 storage / sandbox 维度（修订）

```
use_spawn_shell() 入口:
    ↓
sandbox 参数 = 用户显式提供 (--sandbox or not)，与 storage 无关
    ↓
sandbox=false:
    走 shell-level 路径 (env/PATH 切换，无挂载、无特权操作)
    ├── storage=shared → 直接进
    └── storage=image|tmpfs → 打一行 info 提示 storage 未激活，进 shell

sandbox=true:
    走 sandbox 路径 (bwrap 优先, proot fallback)
    ├── storage=shared → 普通 bind mount 模板
    ├── storage=image  → 挂 home.img 到 .mountpoint
    ├── storage=tmpfs  → bwrap --tmpfs
    └── image/tmpfs + backend=proot → 拒绝，提示 bwrap 不可用真实原因
```

## 四、磁盘布局

### 4.1 shared 模式（不变）

```
~/.xlings/subos/dev/
├── bin/ lib/ usr/ generations/    ← subos 基础结构
├── .xlings.json                   ← workspace 配置
├── home/speak/                    ← 明文目录（host 可见）
├── tmp/                           ← 明文目录（host 可见）
├── etc/                           ← NSS 模板
├── subos/                         ← project-discovery marker
└── sandbox-root/                  ← proot chroot root
```

### 4.2 image 模式

```
~/.xlings/subos/dev/
├── bin/ lib/ usr/ generations/    ← subos 基础结构
├── .xlings.json                   ← workspace 配置（含 storage 字段）
├── home.img                       ← sparse ext4 镜像（host 只看到此文件）
├── .mountpoint/                   ← 运行时挂载点（仅沙箱会话期间有内容）
│   └── home/speak/               ← 解密后的真实数据
├── etc/                           ← NSS 模板（不敏感，明文）
├── subos/
└── sandbox-root/
```

`/tmp` 在 image 模式下使用 bwrap `--tmpfs /tmp`（内存盘，退出丢失）。

### 4.3 tmpfs 模式

```
~/.xlings/subos/dev/
├── bin/ lib/ usr/ generations/
├── .xlings.json
├── etc/                           ← NSS 模板
├── subos/
└── sandbox-root/
（无 home/ 无 tmp/ 无 .img — 全在内存，退出丢失）
```

## 五、配置持久化

### 5.1 subos/.xlings.json 新增字段

```json
{
  "workspace": { ... },
  "storage": "image",
  "imageSize": "50G"
}
```

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `storage` | string | `"shared"` | `shared` / `image` / `tmpfs` |
| `imageSize` | string | `"50G"` | image 模式的逻辑上限（sparse file，实际按需占用） |

### 5.2 全局 ~/.xlings.json 不变

storage 是 per-subos 属性，不影响全局配置。

## 六、核心实现

### 6.1 新增类型

```cpp
enum class StorageMode { Shared, Image, Tmpfs };

struct StorageState {
    StorageMode mode;
    fs::path image_path;      // image 模式：.img 文件路径
    fs::path mountpoint;      // image 模式：挂载点目录
};

// 从 subos 配置读取 storage 模式
StorageMode read_storage_mode_(const fs::path& subos_dir);
```

### 6.2 create() 改造

```cpp
export int create(const std::string& name, const fs::path& customDir,
                  StorageMode storage, const std::string& imageSize,
                  EventStream& stream) {
    // ... 现有逻辑 ...

    // 写入 storage 配置
    nlohmann::json j;
    j["workspace"] = nlohmann::json::object();
    j["storage"] = storage_to_string_(storage);
    if (storage == StorageMode::Image) {
        j["imageSize"] = imageSize;
    }
    write_config_json_(subosConfig, j);

    // image 模式：创建 sparse ext4 镜像
    if (storage == StorageMode::Image) {
        auto img = dir / "home.img";
        // sparse file — 声称 50G，实际只占 ~30MB
        std::system(fmt::format("truncate -s {} {}", imageSize, img).c_str());
        std::system(fmt::format("mkfs.ext4 -F -m 0 -q {}", img).c_str());
        fs::create_directories(dir / ".mountpoint");
    }

    // tmpfs 模式：不创建 home/ tmp/（运行时内存生成）
    // shared 模式：不变
}
```

### 6.3 run() 命令解析改造

```cpp
// xlings subos new <name> [--storage <mode>] [--image-size <size>]
if (sub == "new") {
    std::string name;
    StorageMode storage = StorageMode::Shared;
    std::string imageSize = "50G";
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--storage" && i + 1 < argc) {
            auto s = std::string(argv[++i]);
            if (s == "image") storage = StorageMode::Image;
            else if (s == "tmpfs") storage = StorageMode::Tmpfs;
            else if (s == "shared") storage = StorageMode::Shared;
            else { usageError("unknown storage: " + s); return 1; }
        }
        else if (a == "--image-size" && i + 1 < argc) {
            imageSize = argv[++i];
        }
        else if (!a.empty() && a[0] != '-' && name.empty()) {
            name = std::move(a);
        }
        // ...
    }
    return create(name, {}, storage, imageSize, stream);
}
```

### 6.4 use_spawn_shell() 改造

```cpp
int use_spawn_shell(const std::string& name, EventStream& stream,
                    bool sandbox, const std::string& sandbox_backend) {
    auto storage = sandbox_detail_::read_storage_mode_(subos_dir);

    if (storage != StorageMode::Shared && !sandbox) {
        // 非 shared 模式自动启用 sandbox
        log::info("storage={} requires sandbox, entering sandbox mode...",
                  storage_to_string_(storage));
        sandbox = true;
    }

    if (sandbox) {
        return use_sandbox_mode_(name, stream, sandbox_backend, storage);
    } else {
        // 现有 shell 模式不变
    }
}
```

### 6.5 use_sandbox_mode_() 改造

关键变化：进入沙箱前挂载镜像，退出后卸载。

```cpp
int use_sandbox_mode_(const std::string& name, EventStream& stream,
                      const std::string& preferred_backend,
                      StorageMode storage) {
    // ... 现有 validate / backend 选择 ...

    // image/tmpfs 仅支持 bwrap
    if (storage != StorageMode::Shared
        && backend->type != SandboxBackend::Bwrap) {
        stream.emit(ErrorEvent{
            .message = "image/tmpfs storage requires bwrap backend",
            .hint = "run: xlings install bwrap",
        });
        return 1;
    }

    // ── image 模式：挂载镜像 ──
    fs::path mountpoint;
    if (storage == StorageMode::Image) {
        auto img = subos_dir / "home.img";
        mountpoint = subos_dir / ".mountpoint";
        auto rc = mount_image_(img, mountpoint);
        if (rc != 0) {
            stream.emit(ErrorEvent{.message = "failed to mount home.img"});
            return 1;
        }
        // init_sandbox_dirs_ 在 mountpoint 下初始化
        init_sandbox_dirs_for_mountpoint_(mountpoint, user, uid, gid);
    } else if (storage == StorageMode::Shared) {
        init_sandbox_dirs_(subos_dir, user, uid, gid);
    }
    // tmpfs 模式：不需要初始化（bwrap --tmpfs 处理）

    // ── 构建 argv ──
    auto argv = build_bwrap_argv_(backend->binary, subos_dir,
                                   p.homeDir, user, shell, storage,
                                   mountpoint);

    // ── 执行（fork+exec+wait，退出后卸载镜像）──
    if (storage == StorageMode::Image) {
        // 不能 execvp（需要退出后 unmount）
        auto pid = fork();
        if (pid == 0) {
            // 子进程：进入沙箱
            std::vector<char*> c_argv = ...;
            ::execvp(c_argv[0], c_argv.data());
            _exit(127);
        }
        int status;
        ::waitpid(pid, &status, 0);
        unmount_image_(mountpoint);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    } else {
        // shared/tmpfs：现有 execvp 逻辑
        ::execvp(c_argv[0], c_argv.data());
    }
}
```

### 6.6 sandbox_binds_() 改造

```cpp
std::vector<SandboxBind>
sandbox_binds_(const fs::path& subos_dir,
               const fs::path& host_xlings_home,
               const std::string& user,
               StorageMode storage,
               const fs::path& mountpoint)
{
    // ... Host RO 部分不变 ...

    auto user_home = "/home/" + user;

    // ── Sandbox RW: 按 storage 模式 ──
    if (storage == StorageMode::Shared) {
        binds.push_back({(subos_dir / "home").string(), "/home", false});
        binds.push_back({(subos_dir / "tmp").string(), "/tmp", false});
    }
    else if (storage == StorageMode::Image) {
        // 从挂载点映射
        binds.push_back({mountpoint.string(), "/home", false});
        // tmp 用 tmpfs（由 build_bwrap_argv_ 添加 --tmpfs /tmp）
    }
    // tmpfs: 不添加 home/tmp bind（由 bwrap --tmpfs 处理）

    // xlings 共享始终不变
    binds.push_back({host_xlings_home.string(),
                     user_home + "/.xlings", false});

    // NSS 部分不变
    // ...

    return binds;
}
```

### 6.7 build_bwrap_argv_() 改造

```cpp
std::vector<std::string>
build_bwrap_argv_(..., StorageMode storage, const fs::path& mountpoint)
{
    // ... 现有逻辑 ...

    // tmpfs 模式：bwrap 原生 --tmpfs
    if (storage == StorageMode::Tmpfs) {
        argv.insert(argv.end(), {"--tmpfs", "/home/" + user});
        argv.insert(argv.end(), {"--tmpfs", "/tmp"});
    }

    // image 模式：tmp 也用 tmpfs
    if (storage == StorageMode::Image) {
        argv.insert(argv.end(), {"--tmpfs", "/tmp"});
    }

    // ... 其余不变 ...
}
```

### 6.8 镜像管理函数

```cpp
// 创建 sparse ext4 镜像
int init_image_(const fs::path& img, const std::string& size) {
    if (fs::exists(img)) return 0;  // 幂等
    std::system(fmt::format("truncate -s {} {}", size, img).c_str());
    return std::system(fmt::format("mkfs.ext4 -F -m 0 -q {}", img).c_str());
}

// 挂载镜像
// 优先 udisksctl（桌面 Linux 无需 sudo），回退 sudo mount
int mount_image_(const fs::path& img, const fs::path& mountpoint) {
    fs::create_directories(mountpoint);

    // 检查是否已挂载（支持多终端复用同一 subos）
    auto check = fmt::format("mountpoint -q {} 2>/dev/null", mountpoint);
    if (std::system(check.c_str()) == 0) return 0;  // 已挂载

    // 方式 1：udisksctl（polkit 授权，桌面 Linux 无需 sudo）
    auto udisks = fmt::format(
        "udisksctl loop-setup --file {} --no-user-interaction 2>/dev/null",
        img);
    if (std::system(udisks.c_str()) == 0) {
        // 获取 loop 设备，挂载
        auto mount_cmd = fmt::format(
            "LOOP=$(losetup -j {} | head -1 | cut -d: -f1) && "
            "udisksctl mount -b $LOOP --no-user-interaction 2>/dev/null && "
            "mount --bind $(udisksctl info -b $LOOP | grep MountPoints | "
            "awk '{{print $2}}') {}",
            img, mountpoint);
        if (std::system(mount_cmd.c_str()) == 0) return 0;
    }

    // 方式 2：sudo mount（通用回退）
    auto sudo_mount = fmt::format(
        "sudo mount -o loop {} {}", img, mountpoint);
    return std::system(sudo_mount.c_str());
}

// 卸载镜像
int unmount_image_(const fs::path& mountpoint) {
    auto cmd = fmt::format("sudo umount {} 2>/dev/null || "
                           "fusermount -u {} 2>/dev/null",
                           mountpoint, mountpoint);
    return std::system(cmd.c_str());
}
```

## 七、ext4 镜像特性

### 7.1 Sparse File（按需分配）

```bash
truncate -s 50G home.img     # 逻辑 50G
du -h home.img               # 实际 ~30MB（只占已写入的空间）
```

写多少占多少，无预分配浪费。

### 7.2 在线扩容

```bash
truncate -s 100G home.img    # 扩大逻辑上限
sudo resize2fs /dev/loop0    # 在线扩容文件系统（不停机）
```

可由 xlings 自动化：检测使用率 > 80% 时自动扩容。

### 7.3 缩容

```bash
sudo resize2fs /dev/loop0 20G   # 缩小文件系统
truncate -s 20G home.img        # 缩小文件
```

需要先 umount，非在线操作。

## 八、工具依赖

### 8.1 依赖清单

| 工具 | 用途 | 来源 | storage 模式 |
|------|------|------|-------------|
| bwrap | 沙箱隔离 | xim:bwrap（setuid） | 全部 |
| proot | 沙箱隔离（回退） | xim:proot | 仅 shared |
| truncate | 创建 sparse file | coreutils（系统自带） | image |
| mkfs.ext4 | 格式化镜像 | e2fsprogs（系统自带） | image |
| mount/umount | 挂载/卸载镜像 | util-linux（系统自带） | image |
| losetup | loop 设备管理 | util-linux（系统自带） | image |
| udisksctl | 免 sudo 挂载（可选） | udisks2（桌面 Linux 自带） | image |
| resize2fs | 在线扩容（可选） | e2fsprogs（系统自带） | image |

### 8.2 系统工具可用性

| 工具 | Ubuntu | Fedora | Arch | Debian | Alpine |
|------|--------|--------|------|--------|--------|
| coreutils | 预装 | 预装 | 预装 | 预装 | 预装 |
| e2fsprogs | 预装 | 预装 | 预装 | 预装 | 需安装 |
| util-linux | 预装 | 预装 | 预装 | 预装 | 预装 |
| udisks2 | 桌面版预装 | 桌面版预装 | 需安装 | 桌面版预装 | 无 |

所有核心依赖（truncate, mkfs.ext4, mount, losetup）在主流桌面 Linux 上均预装。
udisksctl 仅用于免 sudo 优化，缺失时回退 sudo mount。

### 8.3 xim 包生态依赖

| 包 | 状态 | 说明 |
|---|------|------|
| xim:bwrap | 已有（PR #290） | setuid 模式，config hook 设权限 |
| xim:proot | 已有 | 零权限沙箱回退 |

无需新增 xim 包。image 模式的工具均为 Linux 标准系统工具。

## 九、进程模型变化

### 9.1 当前（shared 模式）

```
xlings (execvp) → bwrap → shell
                  （xlings 进程被替换，无法做清理）
```

### 9.2 image 模式

```
xlings (fork) → 子进程 (execvp) → bwrap → shell
  │                                         │
  ├── waitpid() ← ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┘ (shell exit)
  └── unmount_image_()  ← 清理挂载
```

image 模式改用 fork+exec+wait 模型，父进程等待沙箱退出后执行 unmount。

### 9.3 多终端复用

同一 subos 多个终端同时进入时：
- 首次进入：mount 镜像
- 后续进入：检测已挂载（`mountpoint -q`），跳过 mount
- 最后一个退出：unmount（需引用计数或检测无进程使用）

引用计数方案：
```
~/.xlings/subos/dev/.mount-refcount    ← 原子文件，记录活跃会话数
进入时 +1，退出时 -1，减到 0 时 unmount
```

## 十、用户旅程

### 10.1 创建 image 模式 subos

```
$ xlings subos new secure-dev --storage image
✓ subos created: secure-dev (storage: image, size: 50G sparse)

$ ls ~/.xlings/subos/secure-dev/
bin/  etc/  generations/  home.img  lib/  .mountpoint/  .xlings.json  usr/
                          ^^^^^^^^
                          host 只看到这个文件
```

### 10.2 使用 image 模式 subos

```
$ xlings subos use secure-dev
[info] storage=image requires sandbox, entering sandbox mode...
[sudo] password for speak: ********           ← 首次需要 sudo mount
▸ entering subos secure-dev (sandbox: bwrap, storage: image)

<xsubos:secure-dev> $ echo "secret data" > ~/private.txt
<xsubos:secure-dev> $ exit

$ cat ~/.xlings/subos/secure-dev/home/speak/private.txt
cat: No such file or directory                ← host 看不到！
$ ls ~/.xlings/subos/secure-dev/home.img
home.img                                       ← 只有镜像文件
```

### 10.3 创建 tmpfs 模式 subos

```
$ xlings subos new throwaway --storage tmpfs
✓ subos created: throwaway (storage: tmpfs)

$ xlings subos use throwaway
[info] storage=tmpfs requires sandbox, entering sandbox mode...
▸ entering subos throwaway (sandbox: bwrap, storage: tmpfs)

<xsubos:throwaway> $ echo "temp data" > ~/test.txt
<xsubos:throwaway> $ exit
                                               ← 数据已丢失
```

## 十一、后续扩展（不在本期）

| 功能 | 说明 |
|------|------|
| `--storage encrypted` | 在 image 基础上叠加 LUKS 加密层 |
| 自动扩容 | 监控使用率 > 80%，truncate + resize2fs |
| 快照/备份 | `cp home.img home.img.bak` 即可 |
| `xlings subos export/import` | 导出 .img 文件供迁移 |
| COW 镜像 | qcow2 格式，支持增量快照 |
| `--sandbox --no-net` | 网络隔离（bwrap `--unshare-net`） |

## 十二、改动量估算

| 文件 | 改动 |
|------|------|
| `src/core/subos.cppm` | `StorageMode` 枚举 + `read_storage_mode_` ~20 LOC; `create()` 增加 storage 参数 ~30 LOC; `run()` new 子命令解析 ~15 LOC; `use_spawn_shell()` 自动 sandbox 逻辑 ~15 LOC; `use_sandbox_mode_()` fork+wait+unmount ~40 LOC; `sandbox_binds_()` 按 storage 分支 ~15 LOC; `build_bwrap_argv_()` tmpfs 支持 ~10 LOC; `mount_image_` / `unmount_image_` / `init_image_` ~50 LOC |
| `src/core/subos.cppm` | 多终端引用计数 ~30 LOC |
| tests | image/tmpfs E2E 测试 ~50 LOC |
| **总计** | **~275 LOC 新增/修改** |

## 十三、兼容性矩阵

| 场景 | 影响 |
|------|------|
| 现有 subos（无 storage 字段） | 默认 `shared`，行为不变 |
| proot 用户 + image/tmpfs | 报错提示需要 bwrap |
| 首次 image use | 需一次 sudo（mount），后续可用 udisksctl 免 sudo |
| 多终端同时 use | 引用计数管理 mount/unmount |
| `xlings subos remove` | image 模式额外删除 .img 和 .mountpoint |
| `xlings subos info` | 显示 storage 模式和 .img 实际占用大小 |
