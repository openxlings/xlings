# #615 实施计划：shim 身份（一次发布）

设计：`2026-09-26-issue615-shim-identity-design.md`。本文只写任务、依赖和验证方式。

## 实施中对设计的两处收敛（已同步回设计文档）

1. **标记改为"所有平台一个机制"**：一个固定的 `.rodata` 字符串 `xvm::kMulticallMarker`，各平台一样，不再在 Windows 上单独用 PE 资源。
   - 只在写者路径和 doctor 里扫描文件内容，每个文件对象扫描一次，所以 PE 资源在 Windows 上省下的那点时间没有意义；
   - 一个机制意味着 Linux CI 能测到 Windows 用的同一段代码。
2. **老 binary 用已有的特征字符串识别，不再用哈希集合 K**：`[xlings:self]: failed to create shim` 是 `create_shim` 的错误格式串。实测 0.4.40、0.4.68、2026.9.20.1、2026.9.26.1、2026.9.26.2 各含 1 次；mcpp 的 binary 里没有。
   - 于是 K、大小预筛、"K 必须在写之前构建"（设计 R1、R2）都不再需要；
   - 旧 payload 被 GC 清掉的 home 也照样能识别；
   - 放在 `COMPAT(2026.9 → drop in 2027.3)` 里。

## 任务与依赖

```
T1 marker + classify ──┬─> T2 plan/apply repoint ──┬─> T4 repoint_all (xself)  ──┬─> T5 entry replace hook
                       │                            │                             ├─> T6 self init / doctor --fix
                       │                            │                             └─> T7 self update post-check
                       ├─> T8 doctor finding ───────┘
                       └─> T9 handoff (platform + main.cpp)
T3 replace_with: same content → no-op (independent)
T10 unit tests (T1–T4)      T11 Linux e2e (T6/T8)      T12 Windows e2e (T5/T6/T9; parallel writer)
T13 docs: AGENTS.md / design / mcpp.toml comment      T14 version bump 2026.9.26.3
T15 PR + CI → T16 self review → T17 release → T18 gtc mirror + pkgindex bump → T19 real verification
```

| 任务 | 内容 | 验证 |
|---|---|---|
| T1 | `xvm::shim_identity`：`kMulticallMarker`、`ShimState{Current,Stale,Foreign,Unknown}`、`Classifier`（按 `(dev, ino)` 或文件 ID 缓存；> 256 MB 直接判为 Foreign；读失败判为 Unknown） | unit |
| T2 | `ActualScan` 增加 `stale` 和 `unknown`；`plan_table` 增加 `toRepoint`；`apply_table` 执行 repoint 并报告 | unit |
| T3 | `entry_binary::replace_with`：字节完全相同就不替换 | unit（在 Linux 上用硬链接模拟 Windows） |
| T4 | `xself::repoint_stale_shims()`：覆盖所有真实 subos 和 `knownProjects` 的项目 subos，持有状态锁，清理 `*.xlings.old*` | unit 和 e2e |
| T5 | `cmd_use` 和 installer 在 self-replace 之后调用 T4 | Windows e2e 升级场景 |
| T6 | `ensure_home_layout` 持锁并调用 T4；doctor `--fix` 调用 T4 | Linux 和 Windows e2e |
| T7 | `self update` 结束后检查 PATH 实际命中的文件：老 binary 形态的 Stale → 报错 | 代码审查（完整链路要等真实 release） |
| T8 | 新增 `FindingKind::ShimDispatcherStale`：老 binary 形态报 Error，计入 `issues()`；带转交能力的报 Notice；提示命令写 entry 完整路径 | e2e |
| T9 | `main()` 开头的转交：Windows 用 `CreateProcessW` 加 job，POSIX 用 `execv`；设置 `XLINGS_HANDOFF_TRACE` 时打印转交信息；子进程启动后清除 `XLINGS_HANDOFF` | Windows e2e；Linux e2e 覆盖 copy 形态 |
| T10–T12 | 测试 | CI |
| T13 | 文档 | review |

并行：T12（Windows e2e 脚本）的规格不依赖实现细节，只依赖第 3 节列出的可观察契约，所以交给一个子 agent 并行写，只写这一个文件。其余任务串行。原因是磁盘只剩 26 GB，一个 worktree 的构建约 8 GB，开第二个构建不现实。

## 可观察契约（测试依据）

- `self doctor`：老 binary 形态的旧 shim → 输出包含 `stale shim`，退出码非 0。
- `self doctor --fix` 和 `self init` 执行之后：每个旧 shim 和 entry 字节相同，并且是同一个文件对象（Windows 上是硬链接，POSIX 上是符号链接）。
- 带转交能力的旧 shim：设置 `XLINGS_HANDOFF_TRACE=1` 时，stderr 输出 `xlings: handoff <own> -> <entry>`，退出码和 entry 执行同样参数时相同。
- 用字节相同的 payload 执行 `use` 或 `install --use`：entry 的文件对象不变。
