# macOS 无特权文件系统隔离方案调研报告

> 调研日期: 2026-05-12
> 背景: xlings subos sandbox 在 Linux 上通过 bwrap/proot 实现文件系统视图隔离，macOS 上目前仅有 L2 HOME 重定向（无真正的文件系统隔离）。本报告调研 macOS 上实现类似功能的可行方案。

## 当前 macOS 实现（L2 HOME redirect）

```cpp
// subos.cppm 中 macOS 的实现
// 仅设置 HOME/TMPDIR/XDG_* 环境变量指向 sandbox 目录
// 无真正的文件系统视图隔离
```

进程仍可访问任意路径，只是默认工作目录和配置目录被重定向。

## macOS 的根本限制

macOS (XNU 内核) 不支持：
- Linux user namespace（`CLONE_NEWUSER`）
- Linux mount namespace（`CLONE_NEWNS`）
- 无特权 chroot（`chroot` 需要 root）
- Linux ptrace syscall 拦截（macOS ptrace 无 `PTRACE_SYSCALL`）

这意味着 Linux 上的 bwrap 和 proot 在 macOS 上**均不可用**。

## 各方案详细分析

### A. sandbox-exec / Seatbelt（SBPL profiles）

- **原理**: Apple 内核级进程沙盒，通过 SBPL 配置文件控制进程的系统调用权限
- **路径重映射**: ❌ **不支持** — 只能限制访问（allow/deny），不能重映射路径
- **无需 root**: ✅ 任何用户进程均可使用
- **状态**: macOS 10.15 起标记为 deprecated，但至今仍可用（macOS 15+）。Apple 内部大量使用
- **适用场景**: 可作为**补充安全层**（限制 sandbox 内进程的访问范围），但无法替代文件系统视图隔离

### B. macOS App Sandbox（entitlements）

- **原理**: 通过代码签名 entitlements 限制进程访问
- **路径重映射**: ❌ 不支持
- **CLI 工具**: 不实际 — 需要 .app bundle、Info.plist、代码签名
- **结论**: 不适合 xlings 场景

### C. DYLD_INSERT_LIBRARIES（LD_PRELOAD 等价物）⭐ 推荐

- **原理**: 注入 dylib 到进程，拦截 libc 函数（open/stat/chdir 等），重写路径参数
- **路径重映射**: ✅ **支持** — 在 libc 层面翻译路径
- **无需 root**: ✅ 对用户编译的二进制和 Homebrew 安装的程序有效
- **性能**: 极低开销（函数调用级别的间接调用）
- **关键限制**: 
  - **SIP 保护的系统二进制**（`/usr/bin/ls`、`/bin/cat` 等）会被 SIP 剥离 `DYLD_*` 环境变量 → 无法拦截
  - 静态链接或直接使用 syscall 的程序（少见）绕过拦截
- **实现复杂度**: ~200-400 行 C 代码，使用 `DYLD_INTERPOSE` 宏
- **需要拦截的函数**: `open`, `openat`, `stat`, `lstat`, `access`, `chdir`, `getcwd`, `realpath`, `opendir`, `readlink` 及其 `$NOCANCEL` 变体

**评估**: 对开发者工具沙盒来说，SIP 限制是可接受的 — 被隔离的工具（编译器、解释器、构建系统）通常是用户安装的（Homebrew、xlings 自身安装），不受 SIP 影响。系统工具（`ls`、`cat`）即使看到真实路径也不影响隔离效果。

### D. FUSE-T + bindfs

- **原理**: FUSE-T 提供无内核扩展的用户态文件系统支持，bindfs 在 FUSE 上实现类似 `mount --bind` 的功能
- **路径重映射**: ✅ 通过 bindfs 挂载实现真正的 bind mount
- **无需 root**: ✅ FUSE-T 无需 kext（macOS 15+）
- **性能**: 中等开销（每次 I/O 经过用户态文件系统守护进程）
- **复杂度**: 高 — 需要 FUSE-T 作为依赖、多次挂载操作、退出时清理
- **隔离完整性**: 需配合 sandbox-exec 隐藏未挂载的路径
- **macFUSE vs FUSE-T**: 
  - macFUSE 在 macOS 15 及以前需要内核扩展（需进入恢复模式允许）
  - FUSE-T 完全用户态，macOS 15+ 原生支持
  - macOS 26 Tahoe: macFUSE 新增 FSKit 后端（纯用户态）

### E. ptrace / proot

- **macOS 上不可能**: macOS ptrace 不支持 `PTRACE_SYSCALL`，无法拦截和修改系统调用参数
- proot 仅支持 Linux
- **结论**: 不可行

### F. 轻量级虚拟机

| 方案 | 启动时间 | 内存占用 | 文件系统共享 | macOS 工具兼容 |
|------|---------|---------|------------|--------------|
| Apple Containerization (macOS 26) | 亚秒级 | 低 | VirtioFS | ❌ 运行 Linux |
| OrbStack | ~2 秒 | Docker Desktop 的 1/4.5 | 高度优化 | ❌ 运行 Linux |
| Lima/Colima | ~10-15 秒 | ~500MB | VirtioFS | ❌ 运行 Linux |

- **限制**: 所有方案运行的是 **Linux guest**，无法运行 macOS 原生工具（Xcode、swift 等）
- **适用场景**: 仅当 subos 只需要 Linux 工具链时（如纯编译环境）

### G. chroot

- macOS 有 `chroot(2)` 但需要 **root 权限**
- SIP 启用时，即使 root 也不能 chroot 到绕过 SIP 保护路径的位置
- **结论**: 不可行（无法无特权使用）

## 总结矩阵

| 方案 | 无需 root | 路径重映射 | 性能 | macOS 工具 | 实现复杂度 | 可行性 |
|------|-----------|-----------|------|-----------|-----------|--------|
| sandbox-exec | ✅ | ❌ 仅限制 | 极好 | ✅ | 低 | 仅补充安全 |
| **DYLD interposer** | ✅ | ✅ 部分 | 极好 | ✅ 大部分 | 中 | **⭐ 推荐** |
| FUSE-T + bindfs | ✅ | ✅ 完整 | 中等 | ✅ | 高 | 可选高级方案 |
| 轻量级 VM | ✅ | ✅ Linux | 中等 | ❌ | 高 | 仅 Linux 工具链 |
| App Sandbox | ✅ | ❌ | 极好 | ❌ | 高 | 不可行 |
| ptrace/proot | — | — | — | — | — | **不可能** |
| chroot | ❌ 需 root | ✅ | 极好 | ✅ | 低 | 不可行 |

## 建议的分层实现路径

### Tier 1: L2 HOME redirect（当前实现）
- 设置 `HOME`、`TMPDIR`、`XDG_*` 环境变量
- 零依赖，全平台兼容
- 无真正隔离

### Tier 2: DYLD_INSERT_LIBRARIES interposer（推荐首选）
- 编写 ~300 行 C dylib，拦截 open/stat/chdir 等函数，按 bind map 重写路径
- 复用 Linux 后端的 `sandbox_binds_()` 统一绑定列表
- 对用户安装的工具（Homebrew、xlings 管理的包）实现完整路径重映射
- SIP 保护的系统二进制会"泄漏"真实路径，但对开发者工具沙盒可接受
- 可选：在进程入口通过 `sandbox_init()` API 叠加 Seatbelt 限制

**实现要点**:
```c
// libxlings_sandbox.dylib
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

// Path remap table (populated from environment variable at load time)
static struct { const char* from; const char* to; } binds[32];

// Interpose open()
int my_open(const char* path, int flags, ...) {
    const char* remapped = remap_path(path);  // apply bind rules
    return original_open(remapped, flags, ...);
}

DYLD_INTERPOSE(my_open, open)
// ... repeat for openat, stat, lstat, access, chdir, realpath, opendir, readlink
```

### Tier 2.5: sandbox-exec 叠加
- 在 DYLD interposer 基础上，用 sandbox-exec 限制访问范围
- 阻止进程访问 bind list 之外的路径
- ~50 行 SBPL profile

### Tier 3: FUSE-T + bindfs（可选，opt-in）
- 适合需要完整文件系统视图隔离的用户
- 通过 `--sandbox-backend=fuse` 标志启用
- 需要安装 FUSE-T（`brew install fuse-t`）
- 真正的 bind mount 语义

## 实施建议

**近期（推荐）**: 实现 Tier 2 — DYLD interposer dylib
- 工作量：~2-3 天
- 效果：覆盖 80%+ 的使用场景（用户安装的工具链）
- 风险：低（SIP 限制是已知的、可接受的）
- 依赖：零额外依赖

**中期（可选）**: 实现 Tier 2.5 — 叠加 sandbox-exec
- 工作量：~1 天
- 效果：阻止意外的路径泄漏

**长期（按需）**: Tier 3 — FUSE-T 后端
- 仅在用户反馈需要完整隔离时实现
