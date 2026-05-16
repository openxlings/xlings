# 方案：xlings 下载器自动检测 + 应用系统代理

**目标：** 用户在配过 `clash`/`v2ray`/`mihomo` 等代理（或在企业网设了 HTTP 代理）的环境下，`xlings install`/`update` 等下载命令应该自动走代理，而不是穷尽重试 + 失败。**程序自己读，用户零配置**。

## 一、当前现状

### 1.1 下载链路（已确认）
- `src/core/xim/downloader.cppm:82-194` `download_one()` → 调 `tinyhttps::download_file`
- `src/libs/tinyhttps.cppm:36-45` `make_client()` 构造 `mcpplibs::tinyhttps::HttpClient`，目前只设 `connectTimeoutMs / readTimeoutMs / verifySsl / keepAlive / maxRedirects`
- 整仓 grep `proxy/PROXY/HTTP_PROXY` —— **零结果**：没读 env、没配置项、没 fallback

### 1.2 上游能力（**重要发现**）
mcpplibs::tinyhttps 0.2.0 **已经有完整的 HTTP CONNECT 隧道支持**：

```cpp
// mcpplibs/tinyhttps/src/http.cppm:35
struct HttpClientConfig {
    std::optional<std::string> proxy;   // ← 设这个就走代理
    // ...
};

// http.cppm:278-280, 558-560, 807-809：HttpClient 内部已经
//   if (config_.proxy.has_value()) {
//       auto pc = parse_proxy_url(config_.proxy.value());
//       auto tunnel = proxy_connect(pc.host, pc.port, ...);
//       // 用 tunnel socket 继续 TLS / 普通 HTTP 通信
//   }
```

**结论：本方案只需要在 xlings 这一侧把 `proxy` 字段填上**，无需改 mcpplibs，无需升级版本。CONNECT 隧道、HTTPS over proxy、错误处理上游都做好了。

### 1.3 平台原生代理来源
| 平台 | 来源（命令行/GUI 用户最常见的两条） |
|---|---|
| Linux | `HTTP_PROXY`/`HTTPS_PROXY`/`ALL_PROXY` env，**就这一个**，GUI 也是写到 systemd-environment 里最终走 env |
| macOS | env 同上；GUI 在 System Settings → Network → Proxies 设的会写进 SystemConfiguration，命令行可以用 `scutil --proxy` 拿到，C 层可以用 `SCDynamicStoreCopyProxies()` / `CFNetworkCopySystemProxySettings()` |
| Windows | env 同上（Git Bash / PowerShell 用户）；GUI 在「Internet 选项 → 连接 → 局域网设置」写进注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings\ProxyEnable + ProxyServer`；WinHTTP 也提供 `WinHttpGetProxyForUrlEx` |

### 1.4 不做 WPAD/PAC
libcurl 都没原生支持 PAC（[Daniel Stenberg 解释](https://daniel.haxx.se/blog/2022/08/12/the-dream-of-auto-detecting-proxies/)：要嵌一个 JS 引擎跑 `FindProxyForURL(url, host)`），生态里只有 `libproxy` 这种重量级方案。**xlings 不做 PAC**，不值得。

## 二、设计

### 2.1 三层职责（与现有架构对齐）

```
┌──────────────────────────────────────────────────────────┐
│ src/platform/{linux,macos,windows}.cppm                  │
│   detect_system_proxy(target_url) -> ProxyConfig         │  ← 新增
│   读 env + 平台原生 store，按 NO_PROXY 过滤               │
└──────────────────┬───────────────────────────────────────┘
                   │ 一次调用，结果可缓存
                   ▼
┌──────────────────────────────────────────────────────────┐
│ src/libs/tinyhttps.cppm                                  │
│   make_client() 在 HttpClientConfig.proxy 上填好          │  ← 新增 ~3 行
└──────────────────┬───────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────┐
│ mcpplibs::tinyhttps  (已支持 HTTP CONNECT 隧道)           │  ← 不动
└──────────────────────────────────────────────────────────┘
```

### 2.2 `ProxyConfig` 数据结构（新建 `src/platform/proxy.cppm` 或并入 `platform.cppm`）

```cpp
struct ProxyConfig {
    std::string http_proxy;     // empty = direct
    std::string https_proxy;    // empty = direct
    std::vector<std::string> no_proxy;  // host suffix list (libcurl 兼容)
    std::string source;         // "env" / "scutil" / "winhttp" / "registry" / "none" — 给 --verbose 看
};

// 给 URL 解析合适的代理；"none" / 命中 NO_PROXY 时返回空。
std::string resolve_proxy_for(const ProxyConfig& cfg, std::string_view url);
```

### 2.3 平台实现优先级

每个平台都按以下顺序探测，**第一个命中就用**（兼容 libcurl/curl/git 的常规感知）：

1. **`HTTPS_PROXY` / `HTTP_PROXY` env**（大小写都试，libcurl 兼容）
2. **`ALL_PROXY` env**
3. 平台原生 store：
   - macOS: `SCDynamicStoreCopyProxies()` —— 读 `kSCPropNetProxiesHTTPProxy/Port`、`HTTPSProxy/Port`、`ExceptionsList`
   - Windows: `WinHttpGetIEProxyConfigForCurrentUser()` 拿 IE/系统代理（这个比直接读注册表更稳）
   - Linux: 没有"平台原生 store"，停在 env 即可

4. 都没有 → `source = "none"`，下载走 direct

### 2.4 NO_PROXY 处理
libcurl 兼容语法："comma-separated host suffix"，例如 `localhost,127.0.0.1,.example.com,*.internal`。`resolve_proxy_for(cfg, url)` 在选中代理前先检查 url 的 host：
- 完全匹配
- 后缀匹配（`.example.com` 命中 `foo.example.com` / `example.com`）
- `*` 通配（可选）

### 2.5 在 tinyhttps.cppm 里的注入点（极小改动）

```cpp
// src/libs/tinyhttps.cppm:36
auto make_client(int connectTimeoutSec, int readTimeoutSec, std::string_view url)
    -> mcpplibs::tinyhttps::HttpClient {
    mcpplibs::tinyhttps::HttpClientConfig cfg;
    cfg.connectTimeoutMs = connectTimeoutSec * 1000;
    cfg.readTimeoutMs    = readTimeoutSec * 1000;

    static const auto proxy_cfg = platform::detect_system_proxy();    // ← cache
    auto proxy = platform::resolve_proxy_for(proxy_cfg, url);
    if (!proxy.empty()) cfg.proxy = proxy;                            // ← 关键 1 行

    return mcpplibs::tinyhttps::HttpClient(std::move(cfg));
}
```

调用点 `download_once / probe_latency / fetch_to_file / query_content_length` 把 `url` 传过来即可。

### 2.6 用户可见的覆盖通道

| 优先级 | 来源 | 备注 |
|---|---|---|
| 1（最高）| `XLINGS_PROXY=...` env | 用户显式覆盖 |
| 2 | `XLINGS_NO_PROXY=1` | 强制不走代理（哪怕系统配了） |
| 3 | `.xlings.json` 里 `proxy: "..."` 字段（可选 Phase 2） | 工程级覆盖 |
| 4 | 自动检测（HTTPS_PROXY / scutil / WinHTTP） | 默认 |

把 `xlings config` 的输出加一行显示当前生效代理 + source，让用户能 debug。

## 三、实现拆解

| Phase | 文件 | 改动 | 估算 LOC |
|---|---|---|---|
| 1 | `src/platform.cppm` (`platform/proxy.cppm`) | 加 `ProxyConfig` + `resolve_proxy_for` + `detect_system_proxy` 接口 | ~40 |
| 1 | `src/platform/linux.cppm` | env-only 实现 | ~30 |
| 1 | `src/libs/tinyhttps.cppm` | make_client 注入 + 各调用点透传 url | ~15 |
| 1 | `src/cli.cppm` `xlings config` panel | 显示当前 proxy + source | ~8 |
| 1 | `tests/unit/test_main.cpp` `Proxy.*` | env 解析 / NO_PROXY 匹配 / XLINGS_PROXY override | ~80 |
| 2 | `src/platform/macos.cppm` | SCDynamicStoreCopyProxies | ~50 |
| 2 | `src/platform/windows.cppm` | WinHttpGetIEProxyConfigForCurrentUser | ~50 |
| 2 | `tests/e2e/proxy_smoke_test.sh` | mock proxy（用 `nc -l` 或 mitmproxy）+ 跑 `xlings install` 看连过来 | ~100 |
| 3（可选）| `docs/quick-install.md` 加代理章节 | 文档 | ~30 |

**Phase 1 ≈ 170 LOC，能让 90% 用户（设了 env 的）开箱即用。** Phase 2 ≈ 100 LOC，覆盖 macOS GUI / Windows GUI 两条剩下的路径。

## 四、风险与边界

1. **HTTPS_PROXY 不带 scheme 的写法** —— `127.0.0.1:7890` vs `http://127.0.0.1:7890`，libcurl 容忍，我们的 `parse_proxy_url` 也容忍（已经处理了）。
2. **认证代理** `http://user:pass@host:port` —— mcpplibs::tinyhttps 当前的 `proxy_connect` **不支持 Basic Auth**（看代码无 user/pass 解析），如果用户用带认证的代理，会失败。**Phase 2 内增加**或推一个上游 PR。
3. **HTTPS proxy（TLS-to-proxy）** —— 现在 mcpplibs 只做 plain HTTP CONNECT；如果用户设 `https://proxy.example:443`（通过 TLS 连代理本身），现版不支持。Phase 2 / 上游推。
4. **代理脏快照** —— `detect_system_proxy()` 缓存为 static，进程内不刷。如果用户跑 xlings 时改代理设置不会被感知（一次 install 命令周期内不刷可接受）。
5. **noproxy 包冲突** —— xim:noproxy（"Run commands bypassing system proxy"）是给 *外部* 命令清 env 用的，和 xlings 内部下载不冲突。但如果用户在 noproxy shell 里 `xlings install`，env 里没 HTTP_PROXY，xlings 会走 direct，**这是预期行为**。

## 五、需要你确认的设计抉择

1. **Phase 1 范围** —— 我建议只做 env（HTTPS_PROXY/HTTP_PROXY/ALL_PROXY/NO_PROXY + XLINGS_PROXY 覆盖），不动 macOS scutil 和 Windows registry。Phase 2 再补。这样能用最小代码立刻给 80%+ 用户解决，同意吗？
2. **缓存策略** —— `detect_system_proxy()` 进程内 cached（static once），还是每次下载都重读 env？我倾向 cached。
3. **认证代理 / TLS-to-proxy** —— 不在 Phase 1。同意还是要一起？（要的话需要先推上游 mcpplibs）
4. **`xlings config` 是否显示 proxy 状态？** —— 我建议显示，方便用户 debug "为什么没走代理"。
5. **CLI 子命令 `xlings proxy`** —— 显示当前生效 proxy + 来源 + 测试连通性。是要还是过度设计？

要我开始按 Phase 1 写代码吗？
