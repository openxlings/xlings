# Issue #356：无 SHA256 下载残片进入缓存的根因与修复方案

> 日期：2026-07-11  
> 状态：实施中（动态更新）  
> 关联 Issue：[openxlings/xlings#356](https://github.com/openxlings/xlings/issues/356)  
> 分析基线：`openxlings/xlings@37fedb7`、`openxlings/xim-pkgindex@b894897`

## 1. 结论

Issue #356 成立。最佳修复不是单独增加一个大小检查，也不是只给 `mcpp` 补 SHA256，而是以下三层组合：

1. **立即止血：给官方 `XLINGS_RES` 资源补 SHA256。** 先迁移 `mcpp` 的 0.0.81、`latest` 当前指向版本与其他活跃版本，再逐步回填可验证版本；索引形态必须先通过旧客户端兼容矩阵，不能未经验证直接发布 `res=true` 条目。
2. **根因修复：下载到唯一临时文件，验收成功后原子提交。** 网络传输、进程中断、候选镜像校验失败期间都不能写最终缓存路径。
3. **缓存策略修复：删除 HEAD 失败时“任意非空文件即命中”的规则。** 无 SHA256 的离线复用只接受由新下载事务写出的完整性 sidecar，且记录大小必须与当前文件一致；旧缓存没有完整提交证据时不能命中。

同时应在 `mcpplibs/tinyhttps` 修正 chunked 响应提前 EOF 被误判为正常结束的问题，并将响应的实际字节数、期望字节数和最终来源 URL 返回给 xlings。这样 Content-Length 校验属于传输层，缓存提交属于 xlings，制品哈希属于索引/发布链，责任边界清晰。

不建议把“libarchive 能打开并读到第一个 header”作为主要验收条件。截断通常发生在归档后半段，首 header 可正常读取；完整遍历则会让大型工具链在真正解压前额外做一次完整解压读取，成本过高。

## 2. 事实与影响范围

### 2.1 Issue 场景的可验证事实

- 在 `xim-pkgindex@b894897` 快照中，`mcpp@0.0.81` 使用裸 `"XLINGS_RES"`，解析出的 `DownloadTask::sha256` 为空。下列数量同样是该提交的审计快照，不代表实施时仓库 HEAD。
- 0.0.81 的上游 release 已提供每个平台的 `.sha256` 文件，`tools/mirror_res.sh` 也已镜像这些 sidecar，校验数据并不缺失，只是没有进入索引条目。
- Linux x86_64 制品的权威大小为 `12,628,937` 字节，SHA256 为 `47c41529a00930ad701a76bb53e0847220c0764eb1f8e6cf6d515c45fea8cfcc`。从 `xlings-res/mcpp` 下载的镜像制品与上游 sidecar 一致。
- Issue 中“正常包约 30 MB+”是估计值，不准确；114 KB 文件仍显著小于权威大小，不影响缺陷成立。
- 当前 `xim-pkgindex` 有 26 个包文件包含裸 `XLINGS_RES`，共 336 个裸条目；其中 `mcpp` 占 208 个，`xlings` 占 60 个。当前没有包使用已经被 xlings 支持的 `res=true + sha256-by-arch` 形态。

### 2.2 当前代码路径

```mermaid
flowchart LR
    A[索引裸 XLINGS_RES] --> B[DownloadTask.sha256 为空]
    B --> C[tinyhttps 直接写最终 destFile]
    C -->|进程被杀/异常退出| D[最终路径残留半文件]
    D --> E[下次安装执行 HEAD]
    E -->|HEAD 失败| F[只检查 localSize > 0]
    F --> G[误报缓存命中]
    G --> H[libarchive 解压]
    H --> I[Truncated tar archive]
```

关键位置：

- `src/core/xim/installer.cppm:1201-1225`：裸 `XLINGS_RES` 生成 URL，但没有生成校验值。
- `src/core/xim/downloader.cppm:362-409`：无 SHA256 时执行 HEAD 缓存判断；HEAD 失败后任意非空文件都会成功返回。
- `src/core/xim/downloader.cppm:439-478`：`tinyhttps` 直接以最终 `destFile` 为下载目标。
- `src/core/xim/downloader.cppm:481-500`：1 KiB 下限只能拒绝极小错误页，无法识别 114 KB 或更大的截断文件。
- `src/core/xim/downloader.cppm:502-523`：只有声明 SHA256 时才做强校验；无 SHA256 sidecar 不记录“完整提交”或实际文件大小。
- `tests/unit/test_main.cpp:832-870`：现有测试只验证 sidecar 的读写，没有验证 HEAD-fail 缓存准入策略。

### 2.3 Issue 根因描述需要补充的两点

第一，当前依赖的 `mcpplibs-tinyhttps` 0.2.0 在响应声明 `Content-Length > 0` 时，已经使用 `read_exact` 读取全部字节；提前断线会返回 `Read error`，xlings 外层随后删除目标文件。因此“普通断线一定留下并接受半文件”并不完整。稳定入口是：

- 进程在外层清理前被强制终止；
- 机器掉电或进程崩溃；
- 旧版本/其他路径已经留下最终路径半文件；
- chunked 响应在终止 chunk 到来前断开。

第二，`mcpplibs-tinyhttps` 0.2.0 到 0.2.3 的 chunked 读取逻辑相同：空的 chunk-size 行会被 `parse_hex` 当作 0，随后按合法终止 chunk 返回成功。这是独立的传输完整性缺陷，必须在上游修复；单靠 xlings 的 HEAD 检查无法可靠补偿。

## 3. 方案比较

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 只给 mcpp/XLINGS_RES 补 SHA256 | 最快；现有 hash cache path 可直接识别并删除 114 KB 文件；抵御镜像内容漂移 | 无法保护第三方无 SHA256 包；下载中断仍直接污染最终路径；发布机器人仍可能生成新裸条目 | 作为 P0 止血，不是完整修复 |
| B. 只增加 Content-Length/归档检查 | 能覆盖部分无哈希下载；改动较局部 | Content-Length 路径底层已经严格读取；chunked/connection-close 没有可靠长度；首 header 不能发现后半段截断；完整归档扫描代价高 | 不推荐作为主方案 |
| C. SHA256 迁移 + 事务下载 + 严格缓存准入 | 同时阻止新残片、修复旧缓存误命中、保留可证明的离线复用；适用于官方和第三方包 | 涉及 tinyhttps、xlings、xim-pkgindex/发布链三个仓库，需要分阶段落地 | **推荐** |

## 4. 推荐设计

### 4.1 传输层：明确“完整响应”的判定

在 `mcpplibs/tinyhttps` 中：

1. `Content-Length` 响应继续要求 `bytesWritten == contentLength`，并把这两个值返回给调用方。
2. chunked 响应只有读到语法合法的 `0\r\n` 终止 chunk 和尾部结束行才算成功；空 size line、非法十六进制、缺少 chunk 尾部 CRLF、提前 EOF 都返回错误。
3. connection-close 响应在 HTTP 语义上没有可比较的期望长度，只能报告 `expectedBytes = null`，不能伪造完整性保证。
4. `DownloadToFileResult`/xlings 包装结果至少携带：
   - `bytesWritten`
   - `expectedBytes`（可选）
   - `finalUrl` 或被接受的候选 URL
   - 明确的传输错误类型

这层只判断 HTTP 消息是否完整，不判断制品是否可信。

### 4.2 xlings 下载层：临时文件验收后原子提交

`download_one` 不再把 `destFile` 直接交给 tinyhttps：

1. 在缓存判断前取得 per-destination 跨进程锁，并持有到提交/失败清理完成，避免两个 xlings 进程同时判定 miss、删除或替换同一目标。锁实现放入 platform 层，Linux/macOS 使用 advisory lock，Windows 使用独占文件句柄。
2. 为每次下载生成同目录、唯一的 `stagingFile`，例如 `<filename>.part.<unique-id>`。同目录保证最终 `rename` 不跨文件系统。
3. tinyhttps 的所有候选 URL 都写 `stagingFile`。失败、取消或校验拒绝时只删除临时文件，不触碰已有的已提交缓存。
4. `onVerify` 对 `stagingFile` 计算 SHA256，不能继续捕获最终 `destFile`。
5. 候选传输成功后依次执行：
   - 若有期望长度，检查实际长度一致；
   - 若有 SHA256，检查 hash；
   - 若无 SHA256 且是归档，仅保留现有的极小错误页检查作为快速诊断，不把它当完整性证明。
6. 所有验收通过后，在锁内执行可恢复提交：已有无效目标先改名为同目录 backup，`stagingFile` 再 rename 为 `destFile`；第二步失败则恢复 backup，成功后删除 backup。该流程在两次 rename 之间最终路径短暂不存在，因此不能称为全过程原子替换。
7. 锁取得后先执行恢复状态机：只有 backup 时恢复为 live；live 与 backup 同时存在时保留 live 并删除已确认过期的 backup；live、backup、staging 的其他组合 fail closed 并记录诊断。若把机器掉电纳入保证，还需分别定义 POSIX `fsync`/目录同步和 Windows `FlushFileBuffers` 的持久化顺序。
8. 提交最终文件后，原子写入 sidecar。若在“文件已提交、sidecar 未写”之间崩溃，结果只是下次保守重下，不会误接收半文件。
9. 只有持有目标锁时才能清理该目标的过期 `.part.*`/backup；不要删除其他进程可能仍在写的临时文件，也不要把临时文件当缓存候选。

临时文件名必须支持并发 xlings 进程，不能使用固定 `.part`。唯一 ID 可由进程 ID、线程 ID、单调计数器和随机数组合；不需要暴露为公共接口。锁等待必须响应取消并有超时，超时错误应指出被占用的缓存目标。

### 4.3 sidecar v2：记录“已完整提交”而不假装是密码学证明

现有 sidecar 只有 `Last-Modified` 和 `ETag`。升级为带版本的记录：

```text
format: 2
complete: true
size: 12628937
source-url: <actual-source-url>
cache-identity: <package/version/platform/arch/resource-key>
last-modified: <http-last-modified>
etag: <http-etag>
```

含义：

- `complete=true` 只表示该文件经过新事务流程成功提交，不等价于可信 hash。
- `size` 是提交时实际大小，用于发现后续截断、覆盖或磁盘损坏。
- `source-url` 记录真正成功的候选。当前代码下载可能从 fallback 成功，却只 HEAD 主 URL；新设计应优先探测实际来源。
- `cache-identity` 必须绑定包、解析后的版本、平台、架构以及规范化主 URL 或官方资源键。HEAD 失败离线复用时必须匹配，防止不同任务因文件名和大小相同而误用彼此缓存。
- `source-url`、`size`、ETag、Last-Modified 应直接来自成功 GET 的结果，避免提交后再 HEAD 带来的额外失败和 TOCTOU。
- sidecar 自身用临时文件 + rename 写入，避免半行记录。
- 老 sidecar 不含 `format/complete/size`，按 legacy 处理，不能获得 HEAD-fail 离线准入资格。

### 4.4 缓存准入矩阵

| 条件 | 处理 |
|---|---|
| 声明 SHA256，缓存 hash 匹配 | 命中 |
| 声明 SHA256，缓存 hash 不匹配 | 删除文件与 sidecar，重新下载 |
| 无 SHA256，HEAD 成功且远端长度与本地一致，ETag/Last-Modified 与 sidecar 一致 | 命中 |
| 无 SHA256，HEAD 成功且无远端新鲜度字段，但长度一致、sidecar v2 完整且记录大小一致 | 可命中，记录 weak-cache 日志 |
| 无 SHA256，HEAD 失败，sidecar v2 完整且记录大小与本地一致 | 允许离线命中，并明确警告“非密码学验证” |
| 无 SHA256，HEAD 失败，只有 legacy sidecar 或任意非空文件 | **不得命中**；尝试 GET，GET 也失败则明确报错 |
| 存在 `.part.*`，最终文件不存在 | 清理或忽略 `.part.*`，重新下载 |

该矩阵保留了 0.4.15 引入的 airline-friendly 目标，但把“存在文件”升级为“存在新事务完整提交证据”。

### 4.5 解压失败后的自愈

事务下载不能证明无 SHA256、无长度的 connection-close 内容在业务上完整。对归档增加一层反应式自愈：

- `extract_archive` 将错误至少区分为输入归档错误和本地写入错误。
- `open`、`next_header`、`read_data_block` 的格式/截断错误视为输入归档错误；删除对应缓存和 sidecar，使下一次安装必然重新下载。
- `write_header`、`write_data_block`、磁盘空间、权限错误属于本地环境错误，不删除可能完全有效的大型缓存。
- 本轮不做“下载后完整预扫描归档”，避免 LLVM/GCC 等大包在解压前额外读取和解压一次。

### 4.6 官方资源层：让 SHA256 成为默认契约

当前工作树的自定义解析路径支持以下索引形态，但旧客户端兼容性尚未证明：

```lua
["0.0.81"] = {
    res = true,
    sha256 = {
        x86_64 = "<sha256-hex>",
        aarch64 = "<sha256-hex>",
    },
},
```

因此 mcpp 止血不需要等待新的 xpkg schema，但发布到全局索引前必须选择经旧客户端验证的表达。优先测试以下双兼容候选：

~~~lua
["0.0.81"] = {
    url = "XLINGS_RES",
    sha256 = {
        x86_64 = "<sha256-hex>",
        aarch64 = "<sha256-hex>",
    },
},
~~~

新客户端读取架构 hash 后继续把 `url` sentinel 展开为官方资源 URL；旧客户端仍有机会按原 `XLINGS_RES` 路径安装。是否成立必须用真实旧版本验证，不能仅靠静态推断。迁移建议：

1. 先给 `mcpp` 的 0.0.81、`latest` 解析后的目标版本及其他活跃镜像版本补 hash；`latest` 本身继续作为 `ref`，验收其最终 `DownloadTask::sha256` 非空。
2. `version-check.py` 的 `res_versioned` apply 路径不再写裸 sentinel。它应读取发布产物的 sidecar/manifest，生成带校验的 res 条目；缺任一受支持平台/架构的 hash 时 fail closed，不开不完整 PR。
3. `mirror_res.sh` 在上传后不仅检查 HTTP 200，还要下载或流式计算每个镜像制品的 SHA256，与上游值比较。
4. xlings 自身 release 当前没有 `.sha256` 资产。发布链应为每个归档生成 sidecar，或统一发布机器可读 manifest，再允许机器人生成带 hash 的 res 条目。
5. 审计其余 24 个含裸 `XLINGS_RES` 的配方，按“能取得权威 hash → 迁移；不能取得 → 保留兼容但走严格事务缓存”的顺序处理。不要让全量历史回填阻塞 mcpp 止血。

长期建议统一发布一个 manifest，字段至少为 `{version, platform, arch, filename, size, sha256}`。sidecar 可继续供人和简单脚本使用，索引机器人消费 manifest，避免在脚本中硬编码平台文件名。

## 5. 实施顺序

### P0：当天止血（xim-pkgindex / 发布脚本）

- 迁移 `mcpp@0.0.81` 和当前 `latest` 的三平台条目，使用上游已发布的 `.sha256`。
- 修改 mcpp 的 index bump 流程，使新版本不再生成裸 `XLINGS_RES`。
- 验证 GLOBAL 与 CN 镜像的 hash 都等于索引值。

这一阶段上线后，现有 114 KB 缓存会在 SHA cache path 被识别为 mismatch、删除并重新下载。

### P1：通用根因修复（mcpplibs/tinyhttps + xlings）

- tinyhttps 修正 chunked EOF 判定并扩充结果元数据。
- xlings 升级 tinyhttps 依赖。
- `download_one` 改为 staging → verify → atomic rename。
- sidecar 升级为 v2，删除无条件非空缓存 fallback。

### P2：自愈与生态收口

- 结构化区分归档输入错误与本地写入错误，输入错误时驱逐缓存。
- 为 xlings/其他官方 res 发布生成 checksum manifest。
- 分批迁移其余官方 `XLINGS_RES` 条目，并在索引 CI 中禁止新增无 SHA256 的官方二进制归档。

## 6. 测试计划

### 6.1 mcpplibs/tinyhttps

- `Content-Length=N`，服务端只发送 `<N` 字节后断开：失败，报告 expected/actual。
- chunked 正常终止：成功。
- chunked 在数据块中断开：失败。
- chunked 在下一个 size line 前断开：失败，不能把空行当 0 chunk。
- 非法 chunk size、缺少块尾 CRLF：失败。
- connection-close 响应：成功但 `expectedBytes` 为空，不宣称有长度证明。

### 6.2 xlings 单元/集成测试

- 预置 114 KB legacy 缓存，HEAD 失败：不得返回 cache hit。
- 预置 sidecar v2 完整缓存，HEAD 失败且大小一致：允许离线命中。
- sidecar v2 记录大小与文件不一致：驱逐并重新下载。
- 传输中取消/失败：最终 `destFile` 不存在或仍是旧的已提交文件，只有临时文件被清理。
- 模拟进程遗留 `.part.*`：下次安装忽略并重新下载。
- SHA256 candidate 失败后 fallback 成功：hash 校验读取 staging 文件，最终原子提交 fallback 内容，并记录实际 source URL。
- 两个进程并发请求同一目标：只有一个执行下载/提交，另一个等待后复用已提交缓存；不得互删 staging 或出现 Windows rename 失败。
- 解压报 `Truncated tar archive`：驱逐输入缓存；权限/磁盘写错误不驱逐。
- Windows：目标存在、rename 行为、临时文件清理均通过；不能依赖 POSIX rename-over-existing 语义。

测试应给 `download_one` 增加传输和 HEAD probe seam，或把缓存决策提取为纯函数。不要依赖公网或真实用户 `XLINGS_HOME`。

### 6.3 索引与 E2E

- 解析 mcpp `res=true` 条目后，断言各 host arch 的 `DownloadTask::sha256` 非空。
- 在隔离临时 `XLINGS_HOME` 中放入错误 mcpp 缓存，再安装 0.0.81：必须 hash mismatch → 重下 → 解压成功。
- GLOBAL/CN 两组资源 URL 下载后的 hash 均与索引一致。
- index CI lint：官方 `XLINGS_RES` 新版本若没有每个受支持架构的 SHA256，失败。

## 7. 验收标准

1. 下载被取消、进程异常退出或 HTTP 提前 EOF 后，最终缓存路径不出现新半文件。
2. 现有无 SHA256 legacy 半文件在 HEAD 失败时不再被接受。
3. 新版本仍可在离线环境复用由事务流程完整提交的无 SHA256 缓存，并显示降级警告。
4. `mcpp@0.0.81` 与 `latest` 在所有支持平台上都解析出非空 SHA256；错误缓存会自动修复。
5. chunked 提前 EOF 在 tinyhttps 0.2.x 的回归测试中稳定失败。
6. 两个进程并发下载同一目标时只有一次有效提交，且不会互删临时文件或破坏旧缓存。
7. 不对大型归档增加一次额外的完整解压预扫描。
8. Linux、macOS、Windows 单元测试与相关 E2E 全部通过，测试使用临时 `XLINGS_HOME`。

## 8. 不纳入本次修复

- 不把 1 KiB 阈值调成某个更大的固定值。合法包大小没有通用下限，阈值只适合识别极小错误页。
- 不通过文件扩展名或 MIME 类型证明归档完整。
- 不把 ETag 当内容 hash；弱 ETag、镜像间 ETag 不一致都很常见。
- 不要求本次一次性回填 336 个历史裸条目。先修复生成流程和活跃版本，再按可获得的权威校验数据迁移历史版本。
- 不在没有 SHA256 时激进地优先第三方镜像；现有 mirror 完整性门禁保持不变。

## 9. 实施拆分与动态状态

本节是本次跨仓实施的唯一状态台账。每个工作包必须独立形成可审查提交；只有其“完成证据”全部存在时才能标记完成。

| ID | 仓库 | 工作包 | 状态 | 完成证据 |
|---|---|---|---|---|
| X1 | `openxlings/xlings` | 缓存决策纯函数与 sidecar v2 | 已完成（本地） | `mcpp test`：legacy/v2/size/identity 回归通过；全套 10/10 测试二进制通过 |
| X2 | `openxlings/xlings` | 唯一 staging 文件、验收后可恢复提交 | 已完成（本地） | 失败/取消保留旧目标；hash fallback；backup 后失败恢复；全套 `mcpp test` 通过 |
| X3 | `openxlings/xlings` | 同目标并发锁与崩溃恢复 | 进行中 | POSIX/Windows 实现及本地测试通过；待双进程测试与三平台 CI |
| X4 | `openxlings/xlings` | 归档输入错误驱逐缓存 | 已完成（本地） | 非法/截断输入驱逐；本地目标写错误保留；全套 `mcpp test` 通过 |
| T1 | `mcpplibs/tinyhttps` | chunked EOF 严格判定与传输元数据 | 待开始 | 协议测试全部通过并发布新版本 |
| L1 | `mcpplibs/libxpkg` | `xpm.source` 解析、兼容、资源归一化 | 待开始 | 旧写法及新 xlings-res/URL template 契约测试 |
| L2 | `openxlings/xlings` | 删除 `load_platform_entries_()` 重复解析并接入 libxpkg | 待开始 | xpkg 解析只剩一个入口；旧索引安装回归通过 |
| I1 | `openxlings/xim-pkgindex` | mcpp 活跃版本补 hash，生成器禁止新裸条目 | 待开始 | GLOBAL/CN hash 一致；索引 CI 通过 |
| I2 | `openxlings/xim-pkgindex` | 官方资源分批迁移与新旧客户端兼容测试 | 待开始 | 旧客户端读取保留条目，新客户端读取推荐条目 |
| R1 | 全生态 | PR、三平台 CI、版本升级、release、索引更新 | 待开始 | PR 合并、release 资产、索引 PR、端到端安装记录 |

### 9.1 PR 边界与依赖关系

~~~text
X1 -> X2 -> X3 -> X4 ---------+--> xlings release -> index 更新 -> 生态 E2E
T1 -> tinyhttps release ------+
L1 -> libxpkg release -> L2 --+
I1 ---------------------------+
I2 依赖 L1/L2 的兼容矩阵通过，但不阻塞 I1 的 mcpp 止血
~~~

- `X1` 与 `T1`、`L1`、`I1` 可并行；同一仓库内部保持顺序，避免把根因修复、schema 和发布脚本揉成一个不可审查 PR。
- `X1`～`X4` 可以合并为一个 xlings PR，但提交按工作包分层，测试必须随对应实现提交。
- `L2` 必须等 `L1` 有可引用版本或 commit；不能在 xlings 中保留第二套“临时兼容解析器”。
- `I1` 只迁移有权威 hash 的活跃资源；`I2` 才做全量审计，不允许为了追求迁移率伪造或从不可信镜像推导 hash。

### 9.2 X1：缓存决策与 sidecar v2

**修改范围**：`src/core/xim/downloader.cppm`、`tests/unit/test_main.cpp`。

1. 先把缓存准入抽成不访问网络的决策函数，输入包含：是否声明 SHA256、文件存在/大小、HEAD 结果、远端大小/新鲜度字段、sidecar 版本/complete/size；输出为 `Hit`、`Redownload` 或 `OfflineUnverifiedHit`。
2. 先增加失败测试：HEAD 失败 + 114 KiB legacy 文件必须 `Redownload`；HEAD 失败 + v2 complete 且大小一致必须 `OfflineUnverifiedHit`；v2 大小不一致必须 `Redownload`。
3. sidecar reader 同时读取 legacy 与 v2；writer 只生成 v2，并以同目录临时文件 + rename 提交。
4. 删除 `HEAD failed && localSize > 0` 的旧准入分支；离线复用只接受 v2 完整提交证据。

**2026-07-12 实施记录**：

- 已为 `DownloadTask` 增加稳定 `cacheIdentity`，由解析后的包名、版本、平台、架构和资源键组成。
- sidecar reader 兼容 legacy，严格校验 v2 的 `format/complete/size/cache-identity`；writer 只写 v2。
- 已删除 HEAD 失败时任意非空缓存命中，只有 v2 的大小和 identity 同时匹配才返回 `OfflineUnverifiedHit`。
- RED 证据：`mcpp test` 曾因缺少 `CacheAdmissionInput_` / `decide_cache_admission_` 编译失败。
- GREEN 证据：`XimDownloaderTest` 7/7 通过；完整 `mcpp test` 10 个测试二进制全部通过。
- 测试门禁同时修复 `test_mirror` 对真实用户 `XLINGS_HOME` 的污染：测试进程现在使用独立临时 home，不再读取用户的 `github-mirrors.json`。

### 9.3 X2：文件下载事务

**修改范围**：`src/core/xim/downloader.cppm`、下载器单元/集成测试。

1. 每次 GET 使用 `<dest>.part.<pid>.<counter>`；`tinyhttps::DownloadOptions::destFile` 和 `onVerify` 都指向 staging。
2. 下载、候选校验、大小检查或取消失败时删除本次 staging，不删除已有已提交目标。
3. 验收后执行 `dest -> backup`、`staging -> dest`、删除 backup；第二步失败必须恢复旧目标。
4. 最终文件提交后再原子写 sidecar v2。文件已提交但 sidecar 未提交时，下次按保守 miss 处理。
5. 测试必须覆盖：候选一 hash 失败、候选二成功；取消；提交失败回滚；遗留 `.part.*` 不被当缓存。

**2026-07-12 实施记录**：

- tinyhttps 的目标和逐候选 `onVerify` 已统一改为唯一 sibling staging 文件，最终路径不再承接网络写入。
- SHA mismatch、传输失败、取消和极小错误页只删除本次 staging；旧目标保留到新内容验收成功。
- 提交使用 `live -> backup -> staging -> live`，第二次 rename 失败会恢复旧目标；该语义明确称为“可恢复提交”，不宣称两次 rename 之间最终路径始终存在。
- 已加入传输和 HEAD probe 测试 seam；失败保留旧目标、预取消零传输、候选 hash fallback、backup 后故障注入恢复测试均通过。
- 全套 `mcpp test`：10 passed，0 failed。

### 9.4 X3：跨进程并发

**修改范围**：`src/platform.cppm`、对应三平台实现、`src/core/xim/downloader.cppm`、测试。

1. 以最终目标路径派生锁文件；缓存判定、下载、提交和 sidecar 写入均在锁内。
2. Unix 使用 advisory file lock，Windows 使用不共享写入的文件句柄；等待检查取消并有明确超时错误。
3. 清理只处理锁持有者能够证明为过期的 staging/backup，不按通配符删除其他进程文件。
4. 并发测试断言只发生一次有效提交，等待者随后命中已提交缓存。

**2026-07-12 实施记录**：

- `platform::FileLock` 已实现：Linux/macOS 使用 `flock`，Windows 使用独占 `CreateFileW`；等待支持取消和 10 分钟超时。
- 锁已覆盖文件缓存判定、传输、提交和 sidecar 写入。
- 持锁后的恢复状态机已覆盖“仅 backup 时恢复 live”“live + backup 时保留 live”“清理同目标遗留 staging”。
- 同进程双句柄竞争、取消等待、延迟释放、两种崩溃恢复测试通过；完整 `mcpp test` 10/10 通过。
- 尚缺真实双进程竞争测试以及 macOS/Windows CI 证据，因此本项仍为“进行中”。

### 9.5 X4：解压失败自愈

**修改范围**：`src/core/xim/extract.cppm`、`src/core/xim/installer.cppm`、相关测试。

1. 解压结果区分 `InvalidInputArchive` 与 `LocalWriteFailure`。
2. 前者删除下载缓存及 sidecar，后者保留缓存并返回环境错误。
3. 不增加下载后的完整归档预扫描；测试直接构造截断归档和不可写目标。

**2026-07-12 实施记录**：

- 新增兼容 API `extract_archive_detailed()`，返回 `InvalidInputArchive`、`LocalWriteFailure` 或 `Internal`；原 `extract_archive()` 字符串接口保留。
- open/next-header/read-data/不安全归档路径归为输入错误；创建目录、write-header/write-data/finish-entry 归为本地写错误。
- installer 只在 `InvalidInputArchive` 时删除归档和 `.meta`；本地写错误保留缓存。
- 非法归档驱逐及“目标是普通文件”本地写失败保留缓存测试通过；未增加归档预扫描。
- 完整 `mcpp test`：10 passed，0 failed。

### 9.6 T1：tinyhttps 协议完整性

在独立仓库和独立 PR 中补协议级本地 HTTP server 测试，再修 chunk parser。成功结果公开 `bytesWritten`、可选 `expectedBytes`、`finalUrl`；xlings 在升级依赖后使用这些字段写 sidecar和日志。connection-close 响应的 `expectedBytes` 必须为空，不能被描述为长度已验证。

### 9.7 L1/L2：唯一 xpkg 入口

`libxpkg` 接受并归一化原有版本资源项以及 `xpm.source = "xlings-res" | <URL template>`，输出平台、架构、最终 URL、SHA256 和 fallback 的统一资源对象。兼容逻辑只位于 libxpkg 的 compat 模块。xlings 升级依赖并删除 `load_platform_entries_()`；删除前必须用同一组 fixture 对旧字符串 URL、`XLINGS_RES`、mirror table、`ref`、单 hash 和多架构 hash 做前后结果对比。

### 9.8 I1/I2：索引迁移与老客户端保护

新增 `xpm.source` 是可选字段，不会改变旧的 `xpm[platform][version]` 条目；因此迁移采用“双轨兼容”，而不是立即重写所有历史条目：

- 旧 `"XLINGS_RES"` 条目继续保留并可被旧客户端读取。
- 新版本推荐使用 `xpm.source = "xlings-res"`，版本项保留原平台/版本位置并补 SHA256。
- 在确认目标老版本客户端会把 `source` 当保留元数据而不是版本键之前，不对官方索引批量加入该字段；兼容测试必须实际运行至少当前稳定版和修复版解析同一 fixture。
- 如果旧客户端会误把 `source` 当平台或版本，官方索引只先落地旧语法的 `res = true + sha256-by-arch` 或显式资源表，待兼容版本覆盖后再迁移 `source`。
- 索引指针/制品发布机制可继续让旧用户固定读取旧 artifact，但这只能作为发布回滚手段，不能替代解析兼容测试。

### 9.9 发布门禁与最终验收

1. 每个仓库从干净的 `main` 建独立分支/worktree，不覆盖当前工作区已有改动。
2. PR 描述包含根因、行为变化、测试命令和跨仓依赖；所有 Linux、macOS、Windows 必需检查通过。
3. 先发布 tinyhttps/libxpkg，再升级 xlings 依赖与版本；xlings release 必须生成各平台资产和 SHA256/manifest。
4. release 自动或人工生成索引 PR；核对 GLOBAL、CN 资源 hash 与 manifest 一致后合并。
5. 用隔离 `XLINGS_HOME` 验证：旧稳定客户端仍能安装未迁移条目；新客户端能安装新旧条目；预置 114 KiB 错误缓存后安装 mcpp 会驱逐、重下并成功解压；离线 v2 完整缓存可复用。
6. 只有 Issue #356 关闭、所有跨仓 PR/release/index 证据链接回填本节后，`R1` 才能标记完成。

## 10. 最终建议

按 **P0 索引止血 → P1 事务缓存根治 → P2 发布生态收口** 的顺序实施。若只能先做一个改动，应先给 mcpp 补 SHA256，因为它能立即修复 Issue 中的用户；但关闭 Issue #356 应以 P1 完成并覆盖“HEAD 失败 + legacy 半文件”和“chunked 提前 EOF”回归测试为准，不能只以 mcpp 当前版本恢复安装为准。
