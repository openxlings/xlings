# Sandbox 跨平台设计:隔离级别 + Linux/macOS/Windows 统一方案

**Status**: 实现中
**Target**: 0.4.28
**Depends on**: 0.4.27 (统一 bind 列表 + 双后端)

## 一、隔离级别定义

| 级别 | 名称 | 隔离范围 | 共享范围 | 典型应用 |
|---|---|---|---|---|
| L0 | 无隔离 | 无 | 全部 | 直接运行程序 |
| L1 | Workspace | 工具版本 + PATH | $HOME、fs、网络 | `xlings subos use`(普通 use) |
| L2 | Dotfile | $HOME + $TMPDIR + XDG dirs | fs 视图、网络、/etc | macOS/Windows `--sandbox` |
| L3 | FS 视图 | $HOME + /tmp + /etc(部分) + 可见路径白名单 | 网络、kernel、PID | Linux `--sandbox`(proot/bwrap) |
| L4 | Namespace | FS + PID + 网络(可选) | kernel | Flatpak / bwrap --unshare-* |
| L5 | 容器 | FS + PID + 网络 + cgroup | kernel | Docker / Podman |
| L6 | VM | 全部(独立 kernel) | 无 | VirtualBox / QEMU / WSL2 |

### xlings 定位

```
xlings subos use <name>              → L1 (所有平台)
xlings subos use <name> --sandbox    → L3 (Linux) / L2 (macOS/Windows)
```

L2 和 L3 的**用户体验一致**(同一命令、同一提示符 `<xsubos:name>`、同一 xlings 行为)。底层隔离程度不同,但对 dev 工作流(隔离 shell 配置、npm/cargo 缓存、IDE settings 等)效果等价。

## 二、三平台 sandbox 实现对比

### 2.1 机制对比

| 维度 | Linux L3 (bwrap/proot) | macOS L2 (HOME 重定向) | Windows L2 (USERPROFILE 重定向) |
|---|---|---|---|
| 底层机制 | mount namespace / ptrace | 纯 env 变量 | 纯 env 变量 |
| 工具依赖 | bwrap 或 proot | 无 | 无 |
| $HOME 隔离 | ✅ bind mount | ✅ env 重定向 | ✅ env 重定向 |
| /tmp 隔离 | ✅ bind mount | ✅ $TMPDIR 重定向 | ✅ %TEMP% 重定向 |
| /etc 隔离 | ✅ 部分(passwd/group/hosts/nsswitch) | ❌ (macOS 用 Directory Services,无影响) | ❌ (Windows 不读 /etc) |
| /bin /usr 可见 | ✅ 精准 bind | ✅ 天然(env 不改 PATH 结构) | ✅ 天然 |
| 额外 sudo | bwrap 可能需要(Ubuntu 24+) | 不需要 | 不需要 |
| 性能开销 | proot 30-50% / bwrap ~0% | ~0% | ~0% |

### 2.2 隔离效果对比

| 场景 | Linux L3 | macOS L2 | Windows L2 |
|---|---|---|---|
| `vim ~/.bashrc` 写到 sandbox | ✅ | ✅ | ✅ |
| `npm install -g` 缓存隔离 | ✅ (~/.npm) | ✅ (~/.npm) | ✅ (%APPDATA%/npm) |
| `cargo build` 缓存隔离 | ✅ (~/.cargo) | ✅ (~/.cargo) | ✅ (%USERPROFILE%/.cargo) |
| fish/zsh/bash 配置隔离 | ✅ | ✅ | ✅ |
| git config 隔离 | ✅ (~/.gitconfig) | ✅ | ✅ |
| VSCode settings 隔离 | ✅ (~/.config/Code) | ✅ (~/Library/...) | ✅ (%APPDATA%/Code) |
| `/etc/passwd` 隔离 | ✅ sandbox 模板 | ❌ (无影响) | ❌ (无影响) |
| `/tmp` 隔离 | ✅ | ✅ | ✅ |
| `ls /etc/hostname` 隐藏 | ✅ | ❌ (可见) | ❌ (无此文件) |
| `ps aux` 看到 host 进程 | ✅ (可见) | ✅ (可见) | ✅ (可见) |

**核心 dev 场景(dotfile + 缓存隔离)三平台效果一致。**

## 三、macOS 实现方案

### 3.1 env 重定向列表

```cpp
// macOS sandbox env overrides
platform::set_env_variable("HOME", sandbox_home);
platform::set_env_variable("TMPDIR", sandbox_tmp);
// XDG (respected by many CLI tools even on macOS)
platform::set_env_variable("XDG_CONFIG_HOME", sandbox_home + "/.config");
platform::set_env_variable("XDG_DATA_HOME", sandbox_home + "/.local/share");
platform::set_env_variable("XDG_CACHE_HOME", sandbox_home + "/.cache");
platform::set_env_variable("XDG_STATE_HOME", sandbox_home + "/.local/state");
```

### 3.2 macOS 特有路径

macOS 应用常用 `~/Library/` 而非 XDG:

```
~/Library/Application Support/  → 应用数据
~/Library/Preferences/          → plist 配置
~/Library/Caches/               → 缓存
```

`HOME` 重定向会自动覆盖这些(因为 `~/Library` = `$HOME/Library`)。✅

### 3.3 exec 方式

跟普通 `subos use` 一样:exec `$SHELL -i`。不需要 proot/bwrap。

```cpp
// macOS/Windows sandbox: 跟 use_spawn_shell 几乎一样,
// 只是多设了 HOME/TMPDIR/XDG env
::execl(shell.c_str(), shell.c_str(), "-i", nullptr);
```

## 四、Windows 实现方案

### 4.1 env 重定向列表

```cpp
// Windows sandbox env overrides
platform::set_env_variable("USERPROFILE", sandbox_home);
platform::set_env_variable("APPDATA", sandbox_home + "\\AppData\\Roaming");
platform::set_env_variable("LOCALAPPDATA", sandbox_home + "\\AppData\\Local");
platform::set_env_variable("TEMP", sandbox_tmp);
platform::set_env_variable("TMP", sandbox_tmp);
// XDG (Git for Windows, cargo, npm respect these)
platform::set_env_variable("XDG_CONFIG_HOME", sandbox_home + "\\.config");
platform::set_env_variable("XDG_DATA_HOME", sandbox_home + "\\.local\\share");
platform::set_env_variable("XDG_CACHE_HOME", sandbox_home + "\\.cache");
```

### 4.2 Windows 特有目录

sandbox_home 下需预建:

```
<subos>/home/<user>/
  AppData/
    Roaming/    → %APPDATA%
    Local/      → %LOCALAPPDATA%
```

### 4.3 exec 方式

Windows 已有 CreateProcess 路径(现有 `use_spawn_shell` 的 Windows 分支):

```cpp
// 现有代码: CreateProcess + WaitForSingleObject
// 只需在 CreateProcess 前多设几个 env
```

## 五、代码结构(统一 use_sandbox_mode_)

```cpp
int use_sandbox_mode_(const std::string& name, EventStream& stream,
                      const std::string& preferred_backend = "") {
    // ... validate, nesting check ...
    
    auto user_home = sandbox_user_home_(user);  // /home/<user> (POSIX) or C:\...\<user> (Win)
    auto sandbox_home = (subos_dir / "home" / user).string();
    auto sandbox_tmp = (subos_dir / "tmp").string();
    
    // ── Lazy init ──
    init_sandbox_dirs_(subos_dir, user, ...);
    
    // ── Env (unified, all platforms) ──
    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    platform::set_env_variable("XLINGS_SUBOS_MODE", "sandbox");
    
#if defined(__linux__)
    // L3: FS 视图隔离 (bwrap/proot)
    auto backend = detect/select backend...
    set_linux_sandbox_env_(name, user_home, sandbox_home, sandbox_tmp);
    auto argv = build_xxx_argv_(...);
    execvp(...);
    
#elif defined(__APPLE__)
    // L2: HOME 重定向
    set_macos_sandbox_env_(sandbox_home, sandbox_tmp);
    exec_shell_(shell);
    
#elif defined(_WIN32)
    // L2: USERPROFILE 重定向
    set_windows_sandbox_env_(sandbox_home, sandbox_tmp);
    create_process_shell_();
    
#endif
}
```

### 各平台 env helper

```cpp
void set_linux_sandbox_env_(const std::string& name,
                            const std::string& user_home,
                            const std::string& sandbox_home,
                            const std::string& sandbox_tmp) {
    platform::set_env_variable("HOME", user_home);
    platform::set_env_variable("PATH", std::format(
        "{}/.xlings/subos/{}/bin:{}/.xlings/bin:/usr/local/bin:/usr/bin:/bin",
        user_home, name, user_home));
}

void set_macos_sandbox_env_(const std::string& sandbox_home,
                            const std::string& sandbox_tmp) {
    platform::set_env_variable("HOME", sandbox_home);
    platform::set_env_variable("TMPDIR", sandbox_tmp);
    platform::set_env_variable("XDG_CONFIG_HOME", sandbox_home + "/.config");
    platform::set_env_variable("XDG_DATA_HOME", sandbox_home + "/.local/share");
    platform::set_env_variable("XDG_CACHE_HOME", sandbox_home + "/.cache");
    platform::set_env_variable("XDG_STATE_HOME", sandbox_home + "/.local/state");
}

void set_windows_sandbox_env_(const std::string& sandbox_home,
                              const std::string& sandbox_tmp) {
    platform::set_env_variable("USERPROFILE", sandbox_home);
    platform::set_env_variable("APPDATA", sandbox_home + "\\AppData\\Roaming");
    platform::set_env_variable("LOCALAPPDATA", sandbox_home + "\\AppData\\Local");
    platform::set_env_variable("TEMP", sandbox_tmp);
    platform::set_env_variable("TMP", sandbox_tmp);
    platform::set_env_variable("XDG_CONFIG_HOME", sandbox_home + "\\.config");
    platform::set_env_variable("XDG_DATA_HOME", sandbox_home + "\\.local\\share");
    platform::set_env_variable("XDG_CACHE_HOME", sandbox_home + "\\.cache");
}
```

## 六、init_sandbox_dirs_ 跨平台

```cpp
void init_sandbox_dirs_(const fs::path& subos_dir,
                        const std::string& user, ...) {
    auto user_home = subos_dir / "home" / user;
    fs::create_directories(user_home);
    fs::create_directories(subos_dir / "tmp");
    
#if defined(__linux__)
    // /etc templates (L3 需要)
    auto etc = subos_dir / "etc";
    fs::create_directories(etc);
    // passwd, group, hosts, nsswitch.conf ...
    // subos/ marker dir ...
    
    // seed .bashrc / .profile / config.fish
    seed_shell_rc_files_(user_home);
    
#elif defined(__APPLE__)
    // macOS: seed shell rc
    seed_shell_rc_files_(user_home);
    // 不需要 /etc templates
    
#elif defined(_WIN32)
    // Windows: AppData 目录结构
    fs::create_directories(user_home / "AppData" / "Roaming");
    fs::create_directories(user_home / "AppData" / "Local");
    // 不需要 /etc, 不需要 .bashrc (pwsh 有 $PROFILE)
#endif
}
```

## 七、提示符(跨平台统一)

所有平台 `--sandbox` 都显示 `<xsubos:name>`:

- bash/zsh/fish: profile_resources.cppm 已有(读 `XLINGS_SUBOS_MODE=sandbox` → `<>` 括号)
- pwsh: profile_resources.cppm 已有

**零额外改动。** macOS/Windows 的 `use_sandbox_mode_` 只要设了 `XLINGS_SUBOS_MODE=sandbox`,profile 自动切括号。

## 八、E2E 测试

### Linux (已有, 15 scenarios)

S1-S15 全保留,涵盖 bwrap/proot 自动检测 + FS 隔离验证。

### macOS (新增)

```
SM1: subos use --sandbox 不报错(不再 reject)
SM2: $HOME 指向 <subos>/home/<user> (sandbox 私有)
SM3: dotfile 写隔离(sandbox 内 touch ~/.test,host 看不到)
SM4: $TMPDIR 隔离
SM5: XLINGS_SUBOS_MODE=sandbox 设置正确
SM6: xlings install/list 正常(xlings home 共享)
SM7: exit 后 host $HOME 不变
```

### Windows (新增)

```
SW1: subos use --sandbox 不报错
SW2: %USERPROFILE% 指向 sandbox home
SW3: %APPDATA% / %LOCALAPPDATA% 指向 sandbox
SW4: %TEMP% 隔离
SW5: XLINGS_SUBOS_MODE=sandbox
SW6: xlings install/list 正常
SW7: exit 后 host %USERPROFILE% 不变
```

## 九、改动量估算

| 文件 | Linux | macOS | Windows |
|---|---|---|---|
| `src/core/subos.cppm` | 不变 | +30 LOC(env helper + exec) | +30 LOC(env helper + CreateProcess) |
| `init_sandbox_dirs_` | 不变 | +5 LOC(seed rc) | +10 LOC(AppData dirs) |
| `tests/e2e/subos_sandbox_test.sh` | 不变 | +30 LOC(SM1-SM7) | +30 LOC(SW1-SW7,PowerShell) |
| **总计** | **0** | **~65 LOC** | **~70 LOC** |

## 十、不在本设计内

- L4+ 隔离(namespace/容器/VM)
- WSL2 集成(从 Windows host 透明进 WSL2 sandbox)
- Lima/Colima 集成(macOS 上 Linux VM sandbox)
- `--sandbox --no-net`(网络隔离,Linux-only,bwrap `--unshare-net`)
