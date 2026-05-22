# `cmd_install` 静默吞失败 — `interface install_packages` 假成功 bug 分析

**日期**: 2026-05-22
**状态**: Bug confirmed (隔离环境实测复现，见 §8a)，fix proposed (未实现)
**关联代码**: `src/core/xim/commands.cppm:327-383`、`src/core/xim/installer.cppm:1108/1194/1215`、`src/capabilities.cppm:47-74`、`src/core/config.cppm:84-88`
**影响版本**: 0.4.38 及之前所有版本
**发现场景**: mcpp 项目通过 `xlings interface install_packages` 安装 LLVM，xlings 报 `exitCode=0` 但 `~/.mcpp/registry/data/xpkgs/` 下 llvm verdir 不存在

---

## 1. 现象

mcpp 在自己内嵌的 xlings registry (`~/.mcpp/registry`) 下调用：

```
cd '~/.mcpp/registry' && env XLINGS_HOME='~/.mcpp/registry' \
  xlings interface install_packages \
    --args '{"targets":["xim:llvm@20.1.7"],"yes":true}' 2>/dev/null
→ exitCode = 0
→ ~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7  不存在
```

下载阶段输出可见：

```
Downloading xim:llvm@20.1.7  [====================] 9 B / 9 B
```

LLVM tarball 正常应该是 ~800 MB，这里"成功下载了 9 字节"。

## 2. Bug 链 — 三处缺陷的合力

### 2.1 `cmd_install` 退出时不查 `failedCount`

`src/core/xim/commands.cppm:327-383`：

```cpp
int successCount = 0;
int failedCount = 0;                  // ← 计数器声明

auto result = installer.execute(plan, dlConfig,
    [&, cancel](const InstallStatus& status) {
        switch (status.phase) {
            case InstallPhase::Done:
                ++successCount; break;
            case InstallPhase::Failed:
                log::error("[{}] failed: {}", status.name, status.message);
                ++failedCount;        // ← 失败被记下来
                break;
            // ...
        }
    },
    ...);

if (!result) {                        // ← 只看 installer 协调流程整体失败
    log::error("install failed: {}", result.error());
    return 1;
}

activate_requested_targets();
if (!allAlreadyInstalled) {
    nlohmann::json summaryPayload;
    summaryPayload["success"] = successCount;
    summaryPayload["failed"]  = failedCount;
    stream.emit(DataEvent{"install_summary", summaryPayload.dump()});  // ← failedCount 只进 event payload
}
return 0;                             // ← 即使 failedCount > 0 也照样 return 0
```

`failedCount` 进了 NDJSON 的 `install_summary` event 里，**但函数退出码硬编码 `0`**。

### 2.2 `installer.execute` 把单包失败转成 `continue`

`src/core/xim/installer.cppm:1090-1216`，三个失败点都是 `continue` 而不是 `return std::unexpected(...)`：

```cpp
// L1100-1108: executor 创建失败
auto execResult = mcpplibs::xpkg::create_executor(node.pkgFile);
if (!execResult) {
    log::error("failed to create executor for {}: {}", node.name, execResult.error());
    onStatus({ node.name, InstallPhase::Failed, 0.0f, execResult.error() });
    continue;                         // ← 不传播到 result
}

// L1189-1195: download 缺失
if (plannedDownloads.contains(planKey) && dlIt == downloadResults.end()) {
    log::error("download artifact missing for {}", node.name);
    onStatus({ node.name, InstallPhase::Failed, 0.0f, "download artifact missing" });
    continue;                         // ← 同上
}

// L1209-1216: extract 失败
auto extracted = extract_archive(dlIt->second.localFile, runtimeDir);
if (!extracted) {
    log::error("extract failed for {}: {}", node.name, extracted.error());
    onStatus({ node.name, InstallPhase::Failed, 0.0f, extracted.error() });
    continue;                         // ← 同上
}
```

`installer.execute` 的返回类型是 `std::expected<void, std::string>`。它**只在两种情况返 `unexpected`**：
- 整体被 cancel
- plan 本身有 errors（resolve 阶段）

任何单包安装失败都被 `continue` 吞掉，整体 `result` 保持 ok。这把责任完全推给了 caller — 但 caller (`cmd_install`) 又只看 `result` 不看 `failedCount` → bug 闭环。

### 2.3 downloader 在缺 sha256 时不查 size

`src/core/xim/downloader.cppm:345-359`：

```cpp
auto dlResult = tinyhttps::download_file(opts);
if (!dlResult.success) {
    result.error = dlResult.error;
    return result;                    // ← 网络层失败才返失败
}

// Verify SHA256 if provided
if (!task.sha256.empty()) {
    auto shaCmd = std::format("sha256sum \"{}\"", destFile.string());
    auto [shaRc, shaOut] = platform::run_command_capture(shaCmd);
    if (shaRc != 0 || shaOut.find(task.sha256) == std::string::npos) {
        result.error = std::format("SHA256 mismatch for {}", task.name);
        fs::remove(destFile, ec);
        return result;
    }
}
// else: 任由 9 字节通过
result.success = true;
return result;
```

LLVM 在 CN mirror 下命中的 URL（可能 gitee 重定向页 / 404 stub / 鉴权失败页）返回 Content-Length=9 的非归档内容。tinyhttps 完整收到 9 字节就报 success；xpm.lua 没声明 sha256 → 校验跳过 → 9 字节文件名仍叫 `llvm-*.tar.xz`。

## 3. 完整执行轨迹（LLVM via mcpp interface）

```
0. mcpp 调:  xlings interface install_packages --args '{"targets":["xim:llvm@20.1.7"],"yes":true}' 2>/dev/null
1. capabilities::InstallPackages::execute  →  xim::cmd_install([xim:llvm@20.1.7], yes=true, ...)
2. cmd_install 解析 → installer.execute
3. installer.execute Phase 1 下载：
     - download_all 返 [llvm: ok 9B, libxml2: ok, linux-headers: ok]
     - llvm 进 downloadResults map（成功条目）
4. installer.execute Phase 2 安装 llvm：
     - is_archive_("llvm-*.tar.xz")  →  true
     - extract_archive(9-byte-file)  →  libarchive 报错 "Unrecognized archive format"
     - log::error("extract failed for xim:llvm: Unrecognized archive format")    ← 进 stderr
     - onStatus({Failed, ...})  →  ++failedCount   ← cmd_install 的局部计数 +1
     - continue
5. installer.execute Phase 2 安装 libxml2、linux-headers：正常成功（successCount += 2）
6. installer.execute 全部 plan 跑完，return ok（void expected）
7. cmd_install:
     - if (!result) 分支不进
     - activate_requested_targets() — llvm 没注册到 xvm DB，跳过激活
     - emit install_summary {success: 2, failed: 1}    ← stdout NDJSON
     - return 0                                       ← 这里！
8. capabilities::InstallPackages 包装  exit_result(0)  →  {"exitCode": 0}
9. interface 框架打印  {"exitCode": 0}  到 stdout
10. mcpp 读到 exitCode=0 → 认为成功 → 后续操作发现 llvm verdir 不存在
```

## 4. 为什么 `xlings install llvm` 直接跑没遇到 — 4 个遮蔽机制

CLI 路径和 interface 路径**调用的是同一个 `cmd_install`**（cli.cppm:924 和 capabilities.cppm:71），bug 代码完全一样。但 CLI 用户从未"遇到"过，是被下面这些遮蔽机制**护住了**：

### 4.1 输出可见 — log::error 直接打到用户终端

CLI 默认 stderr 不重定向。`log::error("[xim:llvm@20.1.7] failed: extract failed: Unrecognized archive format")` **明晃晃打在终端上**：

```
[xlings] [xim:llvm@20.1.7] failed: extract failed: Unrecognized archive format
[xlings] install summary: 2 succeeded, 1 failed
$ echo $?
0                                             ← 退出码也是 0，但用户根本不会去看
```

用户看到 `failed: ...` 字样，立刻知道出错。"退出码=0"是个 silent fact，**人类用户根本不依赖它判断**。

mcpp 调用时 `2>/dev/null`，stderr 直接进黑洞，唯一信号源是 exitCode → 完全没机会感知。

### 4.2 信任契约不同 — mcpp 是程序，必须信 exitCode

- 人类终端用户：看眼睛 → 看见 `failed` 字样 → 自己判断
- 自动化调用方（mcpp / CI / agent）：只能信约定 → interface 的 outputSchema 写明 `exitCode` 是结果 → 0 即成功

xlings 的 interface 协议自己定的 schema 是：
```json
{"type":"object","properties":{"exitCode":{"type":"integer"}}}
```

外部消费方按这个 schema 行事 — 0 = 成功是契约。**xlings 违反了自己定义的契约**：声称 exitCode 反映结果，实际上无论失败几个包都给 0。

### 4.3 缓存命中 — 用户的 `~/.xlings/` 大概率已装过 llvm

很多用户的 `~/.xlings/data/xpkgs/xim-x-llvm/20.1.7/` 已经从过去的 install / mcpp 拷贝 / 手动操作里存在。`cmd_install` 在 commands.cppm:253-259 有 fast path：

```cpp
auto pending = plan.pending_count();
auto allAlreadyInstalled = (pending == 0);
if (allAlreadyInstalled) {
    for (auto& m : requestedMatches)
        log::println("{}@{} is already installed", m.canonicalName, m.version);
}
```

`alreadyInstalled` 标记来自 `catalog.resolve_target` 的 `match.installed` 字段，逻辑会扫 `data/xpkgs/<name>/<ver>` 是否存在非空目录。已存在 → fast path → 根本不进 Phase 1/2 → 没有 download、没有 extract、自然没有失败 → 巧合性地返 0。

mcpp 用的 `~/.mcpp/registry/data/xpkgs/` 是全新隔离 home，第一次跑 llvm 必然走完整下载流程，bug 立刻显形。

### 4.4 镜像差异 — CLI 默认 GLOBAL，mcpp registry 配 CN

用户截图里 `mcpp self config` 显示：

```
mirror          CN
index-repo      xim : https://gitee.com/sunrisepeak/xim-pkgindex.git
```

而用户的 `~/.xlings/.xlings.json` 大概率是 `mirror=GLOBAL`（默认）。同一个 xim:llvm@20.1.7：

| Mirror | xlings-res URL | 当前实际响应 |
|---|---|---|
| GLOBAL | `https://github.com/openxlings/xlings-res/.../llvm-...tar.xz` | 正常 800MB |
| CN | `https://gitee.com/.../llvm-...tar.xz` 或被 redirect | 命中 9 字节 stub（具体原因要看 pkgindex 配置 + gitee 状态） |

GLOBAL 走 GitHub 时一切正常 → 下载、解压、安装都 OK → CLI 用户从未触发 extract failure → bug 永远潜伏。

## 5. 四个机制叠加的稳健性

把 4 个机制摆在一起看：

| # | 遮蔽机制 | mcpp 路径打破 | CLI 路径保留 |
|---|---|---|---|
| 4.1 | log::error 到 stderr 可见 | `2>/dev/null` 切断 | 默认开放 |
| 4.2 | 程序消费方信契约 | 是程序消费 | 是人类消费 |
| 4.3 | 缓存 fast path | 全新 home 无缓存 | 老 home 大概率有缓存 |
| 4.4 | 镜像影响 URL | CN mirror 命中 9 字节 | GLOBAL 正常 |

CLI 用户要同时**翻车四次**才能撞见这个 bug：（1）有人把 `2>/dev/null` 接管输出；（2）有自动化在信 exitCode；（3）目标包从没装过；（4）当前 mirror 配置下 URL 有问题。换 mirror、清缓存、不重定向、人在终端 — 任一条件都能完全遮住。

mcpp 场景**同时把 4 条都搞坏了**，bug 在它面前赤裸裸地暴露。

## 6. 推荐修复（按优先级）

### 6.1 P0 — `cmd_install` 必须检查 `failedCount`（一行修复）

`src/core/xim/commands.cppm:383`：

```cpp
    if (!allAlreadyInstalled) {
        nlohmann::json summaryPayload;
        summaryPayload["success"] = successCount;
        summaryPayload["failed"]  = failedCount;
        stream.emit(DataEvent{"install_summary", summaryPayload.dump()});
    }
-   return 0;
+   return failedCount > 0 ? 1 : 0;
}
```

这一行立刻让 interface 契约自洽。

### 6.2 P1 — `installer.execute` 收集失败 list 并返结构化结果

把三个 `continue` 点的失败信息聚合到 `std::vector<std::string> failures`，整体改返：

```cpp
struct InstallExecuteResult {
    int success_count;
    std::vector<std::string> failures;   // {pkg_name + ": " + reason}
};
std::expected<InstallExecuteResult, std::string> execute(...);
```

`cmd_install` 据此决定 exitCode + emit 更详细的 summary。比 6.1 更彻底，让单包失败成为一等公民而不是仅靠 caller 的局部计数器。

### 6.3 P2 — downloader 在缺 sha256 时做最小合理性检查

`src/core/xim/downloader.cppm`：

```cpp
// 在 SHA256 校验前加：
if (task.sha256.empty() && is_archive_filename_(destFile)
    && fs::file_size(destFile, ec) < kMinArchiveBytes /* e.g. 1024 */) {
    result.error = std::format(
        "{} downloaded only {} bytes (no sha256 to verify) — "
        "likely a redirect / error page; refusing as archive",
        task.name, fs::file_size(destFile, ec));
    fs::remove(destFile, ec);
    return result;
}
```

挡住 9 字节 stub 类问题在到达 extract 之前。这是防御深度，跟 6.1/6.2 互补。

### 6.4 P3 — pkgindex 推动给所有 archive 包补齐 sha256

不是 xlings 仓库的事，但根因之一。`xim-pkgindex` 的 llvm.lua 等条目应该有 sha256，校验自然就挡住 9 字节响应。

## 7. 涉及面 — 还有哪些 capability 有同类风险

`src/capabilities.cppm` 里同样用 `exit_result(int)` 包装命令的 capability，都依赖底层 `cmd_*` 返码语义正确：

| Capability | 调用 | 是否同类风险 |
|---|---|---|
| `install_packages` | `cmd_install` | **是**（本 bug） |
| `plan_install` | `cmd_install(dryRun=true)` | 否（dryRun 路径无 download） |
| `remove_package` | `cmd_remove` | 待审 |
| `update_packages` | `cmd_update` | 待审 — `update` 内部也调 `cmd_install` (commands.cppm:971) |
| `list_packages` | `cmd_list` | 否（只读） |
| `info_package` | `cmd_info` | 否（只读） |

`update_packages` 必然受影响（共享 cmd_install），其他 cmd_* 需要单独检查是否有同样的"local counter not bubbling to return code"模式。

## 8. 验证 / 复现脚本

```bash
# 准备一个全新的隔离 home（模拟 mcpp registry 配置）
export TEST_HOME=/tmp/xlings-bug-repro
rm -rf "$TEST_HOME" && mkdir -p "$TEST_HOME/subos/default/bin" "$TEST_HOME/data"
cat > "$TEST_HOME/.xlings.json" <<JSON
{"activeSubos":"default","lang":"en","mirror":"CN","subos":{"default":{"dir":""}}}
JSON

# 触发：完全模拟 mcpp 的调用方式
env -i HOME="$HOME" PATH=/usr/bin:/bin XLINGS_HOME="$TEST_HOME" \
  xlings interface install_packages \
    --args '{"targets":["xim:llvm@20.1.7"],"yes":true}' 2>/dev/null
echo "interface exitCode: $?"
ls "$TEST_HOME/data/xpkgs/xim-x-llvm/" 2>&1 || echo "[verified] verdir absent"
ls -la "$TEST_HOME/data/runtimedir/llvm-"*.tar.gz 2>&1 | head -1
```

P0 修完后这段命令的 exitCode 应该变成非零，mcpp 也能拿到非零 exitCode。

## 8a. 实地复现结果（2026-05-22 测得）

按 §8 的脚本在 `/tmp/xlings-bug-repro/` 跑得，证据如下：

**xlings 自己知道 llvm 失败了**：

```
{"dataKind":"install_summary","kind":"data","payload":{"failed":1,"success":5}}
```

**但 interface 仍然返回 exitCode 0**：

```
{"exitCode":0,"kind":"result"}
interface exitCode = 0
```

**llvm 实际没装**：

```
=== STEP 3: did llvm actually install? ===
VERDIR ABSENT — confirms silent failure
scode-x-linux-headers    xim-x-glibc    xim-x-libxml2
xim-x-linux-headers      xim-x-zlib                       ← llvm 没有
```

**实际下载到的"llvm tarball" 9 字节，内容 `Not Found`**：

```
found: /tmp/xlings-bug-repro/data/runtimedir/llvm-20.1.7-linux-x86_64.tar.gz
size:  9 bytes
head:  Not Found
```

**根因外部确认 — gitcode CN 镜像 URL 实际是 401**：

```bash
$ curl -sSI -L "https://gitcode.com/xlings-res/llvm/releases/download/20.1.7/llvm-20.1.7-linux-x86_64.tar.gz"
HTTP/1.1 401 Unauthorized
Content-Length: 128
$ curl -sS -L  "https://gitcode.com/xlings-res/llvm/releases/download/20.1.7/llvm-20.1.7-linux-x86_64.tar.gz" | wc -c
9
```

这把 §1-§7 的全部推断变成了实测事实：mirror=CN → gitcode 资源服务器 → llvm release 在 gitcode 上不可用 → 服务器返 9 字节 "Not Found" → xlings 三处缺陷链式吞掉 → exitCode=0。

## 8b. 第三个独立 bug — `gitcode.com/xlings-res` 实际不可用

§8a 揭示了一个额外的、与本文档主 bug 无关但同源触发的问题：xlings 的 `config.cppm:84-88` 配置：

```cpp
static MirrorServerMap default_resource_servers_() {
    return {
        { "GLOBAL", { "https://github.com/xlings-res" } },
        { "CN",     { "https://gitcode.com/xlings-res" } },   // ← 实际返 200 OK + 9 字节假 body
    };
}
```

`gitcode.com/xlings-res/llvm` 没有可下载的 release artifacts。对所有 `mirror=CN` 的用户而言：xlings-res 系列包（任何 `xpm.lua` 写 `XLINGS_RES` 的包）都会下到 9 字节 stub。这跟 §6 的 P0/P1/P2 是**独立的第三层问题**，应该单独跟踪：

- **修复路径 1**：把 LLVM 等 xlings-res release 真的上传到 gitcode 对应仓库
- **修复路径 2**：把 `default_resource_servers_` 的 CN 条目改为已知可用的镜像（比如 gitee + gh-proxy）
- **修复路径 3**：临时把 CN 条目设为空数组，强制 fallback 到 GLOBAL（治标）

### 8b-1. gitcode CDN 长期缓存假文件的证据

跟踪完整 302 → file-cdn.gitcode.com 链路后，file-cdn 返回的 200 响应 header 揭示这不是临时网络抖动：

```
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Length: 9
ETag: "9d1ead73e678fa2f51a70a933b0bf017"        ← CDN 给假文件算了 ETag（要存一段时间）
Last-Modified: Tue, 19 May 2026 11:04:24 GMT    ← 3 天前就是这状态
Content-Disposition: attachment                  ← 还告诉客户端"当附件下载"
Age: 253400                                      ← 已被 CDN 缓存约 70 小时
X-CCDN-CacheTTL: 31536000                        ← CDN TTL 1 年（！）
X-CCDN-Expires: 31282600
Accept-Ranges: bytes
```

`X-CCDN-CacheTTL: 31536000` (= 365 × 24 × 3600 秒) 表示 gitcode 的 CDN 把 "Not Found" 这个 9 字节"假文件"**缓存一整年**。意味着所有走 `mirror=CN` 的用户在 release artifact 真的传上去 + CDN 主动 purge 之前（默认要等到 2027 年），都会拿到同一个 9 字节 stub。

更恶意的是 status code 是 **200 OK**（不是 404 / 401）。HTTP 客户端从协议层面**无法靠状态码 / Content-Type / Content-Length 判断这是垃圾**：

| 检测维度 | file-cdn 响应 | 客户端能否识别为失败 |
|---|---|---|
| HTTP status | `200 OK` | ❌ 200 就是成功 |
| Content-Type | `application/octet-stream` | ❌ 完全合法 |
| Content-Length | `9` | ✅ 唯一异常信号 |
| Content-Disposition | `attachment` | ❌ 告诉客户端"是个下载附件" |
| ETag | 实际算出来的 hash | ❌ 看起来像真文件 |
| body | `Not Found` 字面值 | ✅ 但需要解析才能知道 |

**唯一可用的防御就是 size sanity check** — 也就是 P1 修复的必要性源头。

## 9. 为什么直接 CLI `xlings install llvm` 在用户机器上从未失败 — 三条同时被破才能触发

用户实测：自己跑 `xlings install llvm --verbose` 从未失败。这跟 bug 链不矛盾，因为 bug **只有失败发生时才显形**，而 CLI 路径下"失败"本身就不会发生：

| 维度 | 用户 CLI (`xlings install llvm`) | mcpp registry interface 调用 |
|---|---|---|
| `XLINGS_HOME` | `~/.xlings/` (默认) | `~/.mcpp/registry/` (mcpp 设的) |
| `mirror` 字段 | `GLOBAL` (默认) | `CN` (mcpp 写死) |
| xlings 实际请求 URL | `https://github.com/xlings-res/llvm/...` | `https://gitcode.com/xlings-res/llvm/...` |
| 用户当前代理 | `https_proxy=http://127.0.0.1:7897/` 翻墙 | (mcpp 子进程继承同样代理) |
| github 实际响应 | 真 800MB tarball | — |
| gitcode 实际响应 | — | 9 字节 `Not Found` (CDN 缓存 1 年) |
| 解压 | 成功 | libarchive 报错 |
| `failedCount` 累计 | 0 | 1 |
| `cmd_install` 返码 | 0 (正确——真的成功了) | 0 (**错误——明明失败了**) |
| 用户视角 | "正常下载安装" | "exitCode=0 但 verdir 不存在" |

要在用户 CLI 上触发同一 bug，三件事必须同时发生：
1. 配置 `mirror=CN`（默认是 GLOBAL，不会自动切）
2. 当前代理路径绕开 / 不影响 gitcode（用户 7897 代理对 github 翻墙，对 gitcode 不一定起作用）
3. 装一个 `xlings-res` 系列且 gitcode 上 release 缺失的包（如 llvm）

任一条件不满足，CLI 都不会触发。mcpp registry 在自己 `.xlings.json` 里硬编码 `mirror=CN`（第 1 条），mcpp 安装环境通常不带翻墙代理（第 2 条），用户想装的 llvm 又恰好命中 release 缺失（第 3 条）——三条全占，bug 当场显形。

§4 (CLI 不显形的 4 重遮蔽) 是讲"即使三条全占，CLI 还有最后一道防线"——log::error 直接打到终端，人类用户用眼睛兜底。mcpp 把这道防线也用 `2>/dev/null` 切断了，第 5 重遮蔽也失效，bug 才完全暴露。

## 10. 状态更新（2026-05-22）

- **2026-05-22 PR #302**: GPU 透传（与本 bug 无关，借同一 session 处理），已 squash-merge 到 main (`cf647de`)，发布为 v0.4.39。
- **本文档主 bug (P0 + P1)**: 待开 PR。计划在 `fix-cmd-install-silent-failure` 分支同时修：
  - `commands.cppm:383` 一行：`return failedCount > 0 ? 1 : 0;`
  - `downloader.cppm` 加 archive 文件 minimum-size 校验
  - 两个 e2e 用例分别覆盖"failed → exit 1"和"小文件假装 archive → 拒收"
- **P2 (gitcode release 缺失)**: 跨仓库问题，需要 release 流程侧解决。本仓库可考虑临时移除 default_resource_servers 的 CN 条目，强制 fallback 到 GLOBAL，但这会让所有 CN 用户失去镜像加速能力 — 不在本次修复范围内。

## 9. 时间线建议

1. **立刻提 P0 PR**（一行改动 + 一个 e2e 用例覆盖"fail → exit 1"）
2. P1 跟在 P0 后面（重构 installer.execute 返回类型，影响 capabilities + cli 两边的调用点）
3. P2 单独 PR（需要先确定 archive 最小合理字节数阈值）
4. P3 跨仓库协作（xim-pkgindex）
