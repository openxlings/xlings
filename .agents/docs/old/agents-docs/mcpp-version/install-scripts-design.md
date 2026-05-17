# xlings 安装脚本方案与设计

> **版本**: 0.3.0+  
> **更新时间**: 2026-02-25  
> **涉及文件**:
> - `core/self/install.cppm` — 安装逻辑（C++ 模块分区 `xlings.xself:install`）
> - `core/xself.cppm` — self 子命令主模块
> - `tools/other/quick_install.sh` / `tools/other/quick_install.ps1` — 一键在线安装脚本

---

## 1. 总体架构

安装逻辑已内化到 `xlings` 二进制中，通过 `xlings self install` 完成。薄的 quick install 脚本仅负责下载和解压。

```
用户
 │
 ├─ 方式 A: 一键在线安装 (quick_install)
 │   │
 │   │  curl/irm 下载脚本 → 查询 GitHub API 获取最新版本
 │   │  → 下载对应平台 release 包 → 解压 → 调用 ./bin/xlings self install
 │   │
 │   └─────────────┐
 │                  ▼
 └─ 方式 B: 手动下载 release 包 → 解压 → 运行 ./bin/xlings self install
                    │
                    ▼
            xlings self install (C++ 内置安装器)
                    │
                    ├─ 检测源包目录和已有安装
                    ├─ 版本比对 + 用户确认
                    ├─ 选择性清理（不删除 data/subos）+ 选择性拷贝
                    ├─ 升级时 data/subos 完全保留，不合并
                    ├─ 必要时重建 subos/current 链接
                    ├─ 写入 shell profile / 注册表 PATH
                    └─ 验证安装结果
```

---

## 2. Release 包命名规范

| 平台 | 架构 | 包名格式 | 示例 |
|------|------|----------|------|
| Linux | x86_64 | `xlings-{ver}-linux-x86_64.tar.gz` | `xlings-0.3.2-linux-x86_64.tar.gz` |
| macOS | arm64 | `xlings-{ver}-macosx-arm64.tar.gz` | `xlings-0.3.2-macosx-arm64.tar.gz` |
| Windows | x86_64 | `xlings-{ver}-windows-x86_64.zip` | `xlings-0.3.2-windows-x86_64.zip` |

> **注意**: macOS 平台标识为 `macosx`（与 xmake 保持一致），不是 `macos`。

---

## 3. `xlings self install` — 内置安装器

### 3.1 定位

安装逻辑编译在 `xlings` 二进制内（`core/self/install.cppm`），用户解压 release 包后直接运行 `./bin/xlings self install`。无需额外脚本。

### 3.2 流程

```
1. 检测源包目录: 通过可执行文件路径向上遍历，确认在合法 release 包内
2. 检测已有安装: 检查 XLINGS_HOME 环境变量，在 PATH 中搜索 xlings 二进制
3. 读取版本: 从源包和目标的 .xlings.json 分别读取版本号
4. 用户确认: 同版本重装确认 / 覆盖确认
5. 选择性清理: 仅删除 bin/config/xim/.xlings.json/xmake.lua 等，不删除 data/、subos/
6. 选择性拷贝: 按 entry 遍历 release；target 已有 data/ 或 subos/ 则直接跳过
7. 首次安装: 若 target 无 data/ 或 subos/，则从 release 完整复制
8. 权限修复: Unix 上 chmod +x bin/* 和 subos/default/bin/*
9. 符号链接: 仅当 subos/current 不存在或损坏时创建，不覆盖有效链接（如用户已切换 subos）
10. Shell profile: 配置 bash/zsh/fish profile (Unix) 或 PowerShell profile + PATH (Windows)
11. 验证: 从新位置运行 xlings -h 确认可执行
```

> **data/subos 保留**：采用「直接不删除、选择性不覆盖」，不再使用备份/恢复，避免复制过程中 symlink 处理导致 subos 损坏。

### 3.3 平台差异

| 步骤 | Unix (Linux/macOS) | Windows |
|------|-------------------|---------|
| 权限修复 | `chmod 0755` | 无操作 |
| 目录链接 | `fs::create_directory_symlink` | `cmd /c mklink /J` (NTFS junction) |
| Shell profile | `.bashrc` / `.zshrc` / `.profile` / fish `config.fish` | `$PROFILE` (PowerShell) |
| PATH 配置 | 通过 source profile 脚本 | `[System.Environment]::SetEnvironmentVariable` |

### 3.4 非交互模式

在 CI 等无 TTY 环境中，`ask_yes_no()` 自动取默认值（通常为 yes），无需人工干预。

---

## 4. quick_install — 一键在线安装脚本

### 4.1 定位

面向终端用户的零前提安装入口，一行命令完成安装：

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/d2learn/xlings/main/tools/other/quick_install.sh | bash

# Windows (PowerShell)
powershell -ExecutionPolicy Bypass -c "irm https://raw.githubusercontent.com/d2learn/xlings/main/tools/other/quick_install.ps1 | iex"
```

### 4.2 Unix 版 (`quick_install.sh`)

**前置依赖**: `curl`, `tar`

**流程**:

```
1. 检测平台:
   - OS: uname -s → linux / macos
   - Arch: uname -m → x86_64 / arm64
   - 映射: macos → macosx (release 命名规范)

2. 查询最新版本:
   - GitHub API: GET /repos/d2learn/xlings/releases/latest
   - 解析 tag_name 获取版本号

3. 构造下载 URL:
   - 格式: https://github.com/d2learn/xlings/releases/download/{tag}/xlings-{ver}-{platform}-{arch}.tar.gz
   - 支持 XLINGS_GITHUB_MIRROR 环境变量指定镜像前缀

4. 下载 & 解压:
   - mktemp 创建隔离临时目录
   - curl 下载 → tar 解压
   - trap EXIT 自动清理

5. 运行安装:
   - cd 到解压目录
   - ./bin/xlings self install (TTY 可用时重定向 stdin 从 /dev/tty)
```

**环境变量支持**:

| 变量 | 作用 | 示例 |
|------|------|------|
| `XLINGS_GITHUB_MIRROR` | 替换 GitHub 下载前缀 | `https://mirror.ghproxy.com/https://github.com` |
| `XLINGS_HOME` | 自定义安装路径（透传给 self install） | `/opt/xlings` |

### 4.3 Windows 版 (`quick_install.ps1`)

**前置依赖**: PowerShell 5+

**流程**:

```
1. 检测架构:
   - 64-bit → x86_64
   - ARM64 → arm64

2. 查询最新版本:
   - Invoke-RestMethod GitHub API
   - 解析 tag_name

3. 构造下载 URL:
   - 格式: .../{tag}/xlings-{ver}-windows-{arch}.zip
   - 支持 XLINGS_GITHUB_MIRROR

4. 下载 & 解压:
   - 临时目录 (GUID 命名, 避免冲突)
   - Invoke-WebRequest 下载 → Expand-Archive 解压
   - finally 块自动清理

5. 运行安装:
   - & $xlingsBin self install
```

---

## 5. 与旧版安装流程对比

### 旧版流程 (v0.0.4 及之前)

```
quick_install → 下载 main 分支 zip → 解压源码 → install.unix.sh / install.win.bat
                                                    │
                                                    ├─ 下载 xmake 二进制
                                                    ├─ 运行 xmake xlings --project=. unused self enforce-install
                                                    ├─ 创建系统用户 /home/xlings
                                                    └─ 从源码构建安装
```

**问题**:
- 依赖 `xmake` 构建系统（需额外下载）
- 依赖已废弃的 `task("xlings")` xmake 任务和 `self enforce-install` 子命令
- 需要从源码编译，耗时且需要编译工具链
- 硬编码路径（`/home/xlings`），需要 root 权限创建系统用户

### 新版流程 (v0.3.0+)

```
quick_install → GitHub API 查最新版 → 下载预编译 release 包 → xlings self install
                                                                  │
                                                                  ├─ 选择性拷贝到 XLINGS_HOME（不删除、不覆盖 data/subos）
                                                                  ├─ 升级时 data/subos 完全保留
                                                                  ├─ 必要时重建 subos/current 链接
                                                                  └─ 配置 PATH / shell profile
```

**改进**:
- 零编译依赖，下载即用
- 无需 xmake / git / 编译器
- 安装逻辑内化到 C++ 二进制中，无外部脚本依赖
- 支持自定义安装路径（`XLINGS_HOME` 环境变量）
- 支持 GitHub 镜像加速（`XLINGS_GITHUB_MIRROR`）
- 自动检测 OS 和 CPU 架构，下载对应包
- 自动查询最新版本，无需硬编码版本号
- 升级安装时保留用户缓存数据

---

## 6. 文件关系总览

```
core/
├── xself.cppm                  ← self 子命令主模块 (xlings.xself)
└── self/
    └── install.cppm            ← 安装逻辑分区 (xlings.xself:install)
tools/
├── linux_release.sh            ← Linux 打包脚本
├── macos_release.sh            ← macOS 打包脚本
├── windows_release.ps1         ← Windows 打包脚本
└── other/
    ├── quick_install.sh        ← Unix 一键在线安装 (下载 release → xlings self install)
    └── quick_install.ps1       ← Windows 一键在线安装 (下载 release → xlings self install)
```

**调用链**:

```
quick_install.sh ──下载──→ xlings-{ver}-{platform}-{arch}.tar.gz
                               │
                               └─ 内含 bin/xlings (C++ 二进制，包含 self install 逻辑)
                                       │
                                       └─ ./bin/xlings self install → 安装到 XLINGS_HOME

linux_release.sh ──构建──→ xlings-{ver}-linux-x86_64/
macos_release.sh ──构建──→ xlings-{ver}-macosx-arm64/
windows_release.ps1 ──构建──→ xlings-{ver}-windows-x86_64/
                               │
                               └─ 用户解压后运行 ./bin/xlings self install
```
