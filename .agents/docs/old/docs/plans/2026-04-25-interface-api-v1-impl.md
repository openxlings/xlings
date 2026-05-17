# Interface API v1 — Phase 1 实施细节

**Spec**: [2026-04-25-interface-api-v1.md](2026-04-25-interface-api-v1.md)
**Phase**: 1（协议骨架，PR-1）
**Branch**: `feat/interface-protocol-v1`

本文档列出 Phase 1 的文件级具体改动。Phase 2-5 的细节在每阶段开 PR 时另开 doc。

---

## 1. Phase 1 范围（同一 PR）

| ID | 内容 |
|----|------|
| F1.1 | wire 全部事件类型到 NDJSON：ProgressEvent / LogEvent / DataEvent / PromptEvent / ErrorEvent / CompletedEvent |
| F1.2 | 终止行改成 `{"kind":"result", "exitCode":N, ...}` |
| F1.3 | stdin 控制通道：异步线程读 NDJSON，识别 cancel / pause / resume / prompt-reply |
| F1.4 | CancellationToken 在 cli.cppm 创建，传给 bridge.execute；stdin 控制通道驱动它 |
| F1.5 | `xlings interface --version` 输出 `{"protocol_version":"1.0"}` |
| F1.6 | heartbeat：每 5s 检查最近一次 emit 时间，超过就 emit `{"kind":"heartbeat","ts":...}` |
| F1.7 | 单元测试：覆盖 wire 格式、cancel、prompt-reply |

**不在 Phase 1 范围**：
- `--protocol N` 协商（v1 起步只支持 1，留给 v1.1）
- 业务 capability 内部如何 emit ProgressEvent / ErrorEvent（属于 Phase 3）
- Schema 字段扩展（属于 Phase 2）

---

## 2. Wire 协议（v1.0 — Phase 1 落地形态）

### 2.1 stdout NDJSON 行（每行一个 JSON object，必带 `kind`）

| kind | 字段 | 来源 |
|------|------|------|
| `progress` | `phase, percent, message` | 来自 `ProgressEvent` |
| `log` | `level, message` | 来自 `LogEvent`（level: debug/info/warn/error） |
| `data` | `dataKind, payload` | 来自 `DataEvent`（dataKind = de.kind，payload = parsed de.json） |
| `prompt` | `id, question, options, defaultValue` | 来自 `PromptEvent` |
| `error` | `code, message, recoverable, hint?` | 来自 `ErrorEvent`（code 暂用 int，Phase 3 切枚举字符串） |
| `heartbeat` | `ts` (ISO8601) | cli.cppm 计时器自发 |
| `result` | `exitCode, summary?, data?` | cli.cppm 在 capability 返回后 emit，**最后一行** |

> Phase 1 不修改 capability 的 emit 逻辑（仍只通过 DataEvent）；本 phase 只改 cli.cppm 的 wire 部分。后续 capability 重构会逐步把内部 emit 切到对应事件类型。

### 2.2 stdin 控制通道

逐行 NDJSON。已识别 action：

```json
{"action":"cancel"}
{"action":"pause"}
{"action":"resume"}
{"action":"prompt-reply","id":"<prompt-id>","value":"<answer>"}
```

未知 action → emit `{"kind":"log","level":"warn","message":"unknown stdin action: ..."}`，不退出。

控制通道线程在 capability 开始执行前 spawn，capability 返回后 join（或被 stdin EOF 自然结束）。

---

## 3. 文件级改动

### 3.1 src/cli.cppm（主要改动）

#### 3.1.1 替换 `interface` 子命令的 action lambda

定位：当前文件 ~1615-1675 行。

新增 helper（cli.cppm 文件顶部 anonymous namespace 或 `cli::detail` 子模块）：

```cpp
// Convert any Event variant to NDJSON line. Returns "" for events not in wire protocol.
std::string event_to_ndjson_line(const Event& e) {
    nlohmann::json line;
    if (auto* p = std::get_if<ProgressEvent>(&e)) {
        line = {{"kind","progress"},{"phase",p->phase},{"percent",p->percent},{"message",p->message}};
    } else if (auto* l = std::get_if<LogEvent>(&e)) {
        const char* lvl = l->level==LogLevel::debug?"debug":
                          l->level==LogLevel::info ?"info":
                          l->level==LogLevel::warn ?"warn":"error";
        line = {{"kind","log"},{"level",lvl},{"message",l->message}};
    } else if (auto* d = std::get_if<DataEvent>(&e)) {
        auto payload = nlohmann::json::parse(d->json, nullptr, false);
        line = {{"kind","data"},{"dataKind",d->kind},
                {"payload", payload.is_discarded() ? nlohmann::json(d->json) : payload}};
    } else if (auto* pr = std::get_if<PromptEvent>(&e)) {
        line = {{"kind","prompt"},{"id",pr->id},{"question",pr->question},
                {"options",pr->options},{"defaultValue",pr->defaultValue}};
    } else if (auto* er = std::get_if<ErrorEvent>(&e)) {
        line = {{"kind","error"},{"code",er->code},{"message",er->message},
                {"recoverable",er->recoverable}};
    } else if (std::get_if<CompletedEvent>(&e)) {
        return ""; // CompletedEvent is internal, suppressed; result terminator is cli-emitted
    }
    return line.dump();
}
```

#### 3.1.2 新增 InterfaceSession 类（or function-scope helper）

封装 mutex、heartbeat 计时器、stdin reader 的生命周期。建议放在 cli.cppm 内部 namespace。

```cpp
class InterfaceSession {
    std::mutex io_mtx_;
    std::atomic<std::chrono::steady_clock::time_point> last_emit_;
    std::atomic<bool> shutting_down_{false};
    std::jthread heartbeat_thread_;
    std::jthread stdin_thread_;
    EventStream& stream_;
    CancellationToken& token_;

public:
    InterfaceSession(EventStream& stream, CancellationToken& token);
    ~InterfaceSession();   // 自动 stop threads

    void emit_line(std::string_view line);   // 加锁写 stdout，更新 last_emit_
    void emit_event(const Event& e);          // helper 调用 event_to_ndjson_line + emit_line
    void emit_result(int exitCode, std::string_view content); // emit kind:result，是终止行

private:
    void heartbeat_loop_();    // 每秒检查 last_emit_，>5s 则 emit heartbeat
    void stdin_loop_();        // 阻塞读 stdin，行 JSON 解析后驱动 token / stream
};
```

#### 3.1.3 接到事件流

```cpp
auto session = std::make_unique<InterfaceSession>(stream, token);
stream.on_event([&session](const Event& e) {
    session->emit_event(e);
});
auto result = bridge.execute(cap_name, cap_args, stream, &token);
session->emit_result(result.isError ? 1 : 0, result.content);
return result.isError ? 1 : 0;
```

注意：bridge.execute 的第 4 参数（CancellationToken*）当前在 ToolBridge 已有重载（参考 src/agent/tool_bridge.cppm:74）。如果原签名不带 cancel，要确认能调到带 cancel 的版本。

#### 3.1.4 新增 `--version` 元命令处理

```cpp
if (args.is_flag_set("version")) {
    std::cout << R"({"protocol_version":"1.0"})" << "\n" << std::flush;
    return 0;
}
```

放到 `--list` 检查之前。

#### 3.1.5 stdin reader 解析

```cpp
void InterfaceSession::stdin_loop_() {
    std::string line;
    while (!shutting_down_.load() && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.contains("action")) continue;
        auto action = j["action"].get<std::string>();
        if (action == "cancel")        token_.cancel();
        else if (action == "pause")    token_.pause();
        else if (action == "resume")   token_.resume();
        else if (action == "prompt-reply") {
            stream_.respond(j.value("id",""), j.value("value",""));
        } else {
            emit_event(LogEvent{LogLevel::warn, "unknown stdin action: " + action});
        }
    }
}
```

Note: 当 capability 跑完后主线程会析构 InterfaceSession；jthread 析构会 request_stop。但 `std::getline` 在阻塞读时不会响应 stop_token。Phase 1 简化：依赖 stdin EOF 或主线程退出后 OS 关闭 fd 让 getline 返回。可以用 `poll/select` 让 stdin reader 周期性醒来检查 shutting_down_。Phase 1 保留 100ms poll 实现，简单可靠。

### 3.2 src/runtime/event.cppm

不需要改动。现有 6 种 variant 已够 Phase 1。

### 3.3 src/runtime/event_stream.cppm

不需要改动。stream.respond() 已有，stdin 控制通道直接调用即可。

### 3.4 src/agent/tool_bridge.cppm

确认 execute 签名带可选 CancellationToken*。已有。

### 3.5 tests/unit/test_main.cpp（新测试套件）

新增 `InterfaceProtocol` 套件：

```cpp
TEST(InterfaceProtocol, VersionFlag) {
    // popen "xlings interface --version"
    // assert stdout 含 "protocol_version" 且 = "1.0"
}

TEST(InterfaceProtocol, ResultLineHasKindResult) {
    // popen "xlings interface system_status --args {}"
    // 解析 NDJSON，最后一行必须 kind=="result" 且 exitCode == 0
}

TEST(InterfaceProtocol, DataEventWiredAsKindData) {
    // popen "xlings interface list_packages --args {}"
    // 中间行（除 result 外）至少一行 kind=="data"
    // 字段：dataKind 非空、payload 是 object/array
}

TEST(InterfaceProtocol, UnknownCapabilityEmitsErrorAndExits1) {
    // popen "xlings interface __no_such__"
    // 期望：result kind 行 exitCode != 0；之前可能有 kind:error
}
```

CancellationToken 与 stdin 通道的测试需要双向 stdio，用 `popen` 不行；用 `posix_spawn` + pipe pair 或 boost.process。如果太重，Phase 1 可以先只测 wire 格式，cancellation 单独写一个 fork+pipe 的小 helper（约 50 行）。

### 3.6 docs/plans/2026-04-25-interface-api-v1.md

不动。spec 是协议契约，不需要因实现细节改。

---

## 4. 测试矩阵

| 测试 | 验证内容 | 必须通过 |
|------|---------|---------|
| existing 169 测试 | 不引入回归 | ✅ |
| InterfaceProtocol.VersionFlag | --version 输出 | ✅ |
| InterfaceProtocol.ResultLineHasKindResult | 终止行 kind=result | ✅ |
| InterfaceProtocol.DataEventWiredAsKindData | DataEvent 被包装 | ✅ |
| InterfaceProtocol.UnknownCapabilityEmitsErrorAndExits1 | 错误路径 | ✅ |
| InterfaceProtocol.HeartbeatAfterIdle (optional) | 5s+ idle 后有 heartbeat | ⏳ Phase 1 后置 |
| InterfaceProtocol.CancelViaStdin (optional) | stdin cancel 触发 exitCode=130 | ⏳ Phase 1 后置 |

---

## 5. 兼容性影响

⚠️ **这是 v0 → v1 的 breaking change**：

| 字段 | 现状（v0） | v1 |
|------|-----------|-----|
| 终止行 | `{exitCode}`（无 kind） | `{kind:"result", exitCode}` |
| DataEvent 行 | `{kind:"<de.kind>", data:...}` | `{kind:"data", dataKind:"<de.kind>", payload:...}` |

**xstore 适配方法**（在 PR-1 之后立即跟一个 xstore PR）：
- `parseNdjson` 把 `if (val.kind === "result")` 当结果，否则当事件
- 现有 `extractPackages` / `extractVersions` 等 helper 改吃 `payload` 而不是 `data`
- DataEvent 的 inner kind 在 `event.dataKind`，老代码读 `event.kind` 处要改

xstore 代码量改动评估：~30 行（types.ts + 3 个 extractor），改完整套行为不变。

xstore-ndjson 分支在 PR-1 合入 main 之前不影响（xstore 还是用旧 protocol）。PR-1 合入后，xstore 必须立刻跟一个 PR 同步切到 v1，否则 xstore 装包会断。

---

## 6. 工作顺序（按依赖）

1. ✅ Spec 文档 (`2026-04-25-interface-api-v1.md`) — 已完成
2. ✅ 本 impl 文档 — 当前
3. F1.1 + F1.2：wire 全部事件类型 + 终止 kind:result
4. F1.5：--version 元命令
5. F1.7：基础测试（VersionFlag / ResultLineHasKindResult / DataEventWiredAsKindData）
6. F1.3 + F1.4：stdin 控制通道 + CancellationToken 接通
7. F1.6：heartbeat
8. 跑全测试套件
9. （后续）xstore 适配 PR

---

## 7. 验收

- [ ] 全测试套件 pass（含 4 个新增 InterfaceProtocol 测试）
- [ ] `xlings interface --version` 输出 `{"protocol_version":"1.0"}`
- [ ] `xlings interface system_status --args {}` 末行有 `kind:"result"` 且 `exitCode:0`
- [ ] `xlings interface list_packages --args {}` 中间行有 `kind:"data"` 包裹的 `dataKind:"styled_list"` payload
- [ ] commit 消息引用 spec doc
