# 2026 Linux 无特权文件系统隔离方案调研报告

> 调研日期: 2026-05-11
> 背景: xlings subos sandbox 需要无特权的文件系统视图隔离（chroot + bind mount），当前使用 bwrap 优先 + proot 兜底的双后端架构

## 核心矛盾

Linux 文件系统隔离分两大阵营，没有银弹：

| 类型 | 代表 | 优势 | 致命限制 |
|------|------|------|----------|
| 内核命名空间 | bwrap, podman | 性能接近原生 | 需要 user namespace 或 root |
| 用户态拦截 | proot | 零特权要求 | 性能差 30-50%，ptrace 路径解析 bug |
| 访问限制 | Landlock | 零特权，内核级 | 无法做路径重映射 |
| libc 拦截 | fakechroot, LD_PRELOAD | 零特权，低开销 | Go/Rust/静态链接绕过 |

## 各方案详细分析

### A. proot（ptrace-based）— 当前 fallback

- **版本**: v5.4.0（2023-05），3 年无新版本，基本停滞
- **原理**: ptrace 拦截每个系统调用，在用户态翻译路径
- **优势**: 零特权，零内核特性依赖
- **性能**: 30-50% 开销（每个 syscall 经过 SIGSTOP/SIGCONT）
- **已知问题**:
  - `detranslate_path()` bug：rootfs 前缀与 bind 路径歧义，`cd` 后 cwd 被错误翻译
  - 与 gdb/strace 冲突（ptrace 独占）
  - npm native modules 可能触发 double free（ptrace 对 mmap/brk 拦截问题）
  - `mknod` 错误、部分 glibc 内部 syscall 绕过拦截
- **维护状态**: 99 个 open issues，20 个 open PR，VHSgunzo 分支更活跃

### B. bubblewrap (bwrap) — 当前优先后端

- **版本**: v0.11.2（2024-04），活跃维护
- **原理**: Linux user namespace + mount namespace，内核级隔离
- **优势**: 性能接近原生（内核 bind mount），Flatpak 生态依赖
- **2026 user namespace 形势**:
  - **Ubuntu 24.04+**: AppArmor 4.0 管控 userns，系统包 `/usr/bin/bwrap` 有 AppArmor profile 可正常工作，自编译的无 profile 会失败
  - **Fedora**: 一直开放，无限制
  - **Arch**: 无限制
  - **趋势**: 从"完全禁止"转向"有条件允许"，`apt install bubblewrap` 基本解决
- **v0.11.0 新特性**: overlay mount（`--overlay`, `--tmp-overlay`）, `--bind-fd`

### C. Landlock LSM

- **内核版本**: Linux 5.13+，ABI v9 新增 UNIX socket 支持
- **原理**: 内核安全模块，进程自主设置访问控制策略
- **优势**: 零特权，零内核特性依赖，性能接近原生
- **致命限制**: 只能做访问控制（allow/deny），**无法做路径重映射**（不能让 `/foo` 显示为 `/bar`）
- **适用场景**: 可作为 proot/bwrap 的补充安全层（限制 sandbox 访问范围）
- **工具**: `landrun`（2.2k stars）是流行的 CLI 封装

### D. LD_PRELOAD / FUSE 方案

#### fakechroot (LD_PRELOAD)
- **版本**: v2.20.1（2019-03），7 年未更新
- **原理**: 通过 LD_PRELOAD 拦截 libc 函数（open, stat, chdir 等）
- **致命缺陷**: Go 和 Rust 程序直接使用 syscall，完全绕过 LD_PRELOAD
- **结论**: 不可行

#### fuse-overlayfs
- **版本**: v1.16（2025-11），活跃维护
- **原理**: FUSE 用户态文件系统，提供 overlay 语义
- **限制**: 只能做 overlay（上层可写 + 下层只读），不能做任意 bind mount；需要 user namespace
- **结论**: 不适合 xlings 场景

#### 自定义 LD_PRELOAD
- **复杂度**: ~1000-2000 行 C 代码
- **致命缺陷**: 同 fakechroot，Go/Rust/静态链接绕过
- **结论**: 不可行

### E. Namespace 相关

#### unshare -m（mount namespace）
- 需要 `CAP_SYS_ADMIN` 或 user namespace
- 没有 user namespace 就需要 root
- **结论**: 等价于 bwrap 的限制

#### setns（加入已有 namespace）
- 需要 `CAP_SYS_ADMIN` 或目标 namespace 所有权
- **结论**: 不适合无特权场景

### F. 未来技术方向

#### seccomp SECCOMP_RET_USER_NOTIF
- **内核版本**: Linux 5.0+
- **原理**: seccomp 过滤器将匹配的 syscall 通知给监控进程，监控进程可以修改/模拟 syscall
- **优势**: 不需要 root/userns，比 ptrace 快（10-20% 开销），不与调试器冲突
- **路径重映射可行性**: 理论上可行——监控进程读取目标进程内存中的路径参数，翻译后通过 `SECCOMP_IOCTL_NOTIF_ADDFD` 注入重映射的 fd
- **复杂度**: 极高。需处理 TOCTOU 竞争、PID 重用、信号中断、多线程等。估计 3000-5000 行 C 代码，3-6 人月
- **现状**: 没有任何现成工具基于此做路径重映射。容器运行时（runc）用它模拟 mount/mknod，但不做路径翻译
- **结论**: 唯一能真正替代 proot 的技术方向，但工程量巨大

#### io_uring
- **不可行**: io_uring 是异步 I/O 提交接口，无法拦截/重写其他进程的请求。且 Ubuntu 23.10+ 默认禁止无特权 io_uring

### G. 容器运行时

#### Podman rootless
- 需要 user namespace + `/etc/subuid` 配置
- 启动开销 1-3 秒，OCI 容器镜像
- **结论**: 对 xlings 过重

#### systemd-nspawn
- 目录模式需要 root
- **结论**: 不适合

#### Nitrobox（2025-2026 新项目）
- 需要 user namespace
- 5 stars，太年轻
- **结论**: 不适合当前采用

## 总结矩阵

| 方案 | 无需 root | 无需 userns | 路径重映射 | 性能开销 | 维护状态 | xlings 可行性 |
|------|-----------|------------|-----------|---------|---------|-------------|
| proot | ✅ | ✅ | ✅ | 30-50% | 停滞 | 当前 fallback |
| bwrap | 条件性 | 仅系统包 | ✅ | ~0% | 活跃 | **推荐优先** |
| Landlock | ✅ | ✅ | ❌ | ~0% | 活跃 | 仅补充安全 |
| LD_PRELOAD | ✅ | ✅ | ✅ | ~5% | 停滞 | ❌ 绕过问题 |
| seccomp UNOTIF | ✅ | ✅ | 理论上 | 10-20% | 内核稳定 | 未来方向 |
| podman | ✅ | ❌ | ✅ | ~0% | 活跃 | 过重 |

## 建议

### 短期（当前版本）
1. **强化 bwrap 引导**: 首次 sandbox 使用时更明确提示 `sudo apt install bubblewrap`
2. **绕过 proot detranslate bug**: 避免通过 `~/.xlings` bind 访问 subos rootfs 内的系统路径
3. **Landlock 补充安全层**: 在 proot 模式下叠加 Landlock 限制（~200 行代码）

### 长期
- **seccomp UNOTIF** 是唯一能替代 proot 且无需特权的技术方向
- 如果 proot bug 持续影响用户，可考虑投入开发
- 关注 proot 社区是否有活跃分支解决已知问题
