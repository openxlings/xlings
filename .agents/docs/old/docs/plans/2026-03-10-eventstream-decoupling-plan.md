# EventStream 全解耦：消除 core→ui 依赖

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 5 个 core 模块不再 `import xlings.ui`，改为通过 EventStream 发射事件，CLI 层消费事件并渲染 UI。

**Architecture:** core 发事件 → EventStream → CLI consumer 调 ui:: 函数渲染。层级依赖从 core→ui 变为 core→runtime，cli→ui+runtime。

---

## 设计概要

### 事件映射

| 旧调用 | 新事件 | 事件类型 |
|--------|--------|----------|
| `ui::print_info_panel(title, fields)` | `DataEvent{kind="info_panel", json}` | DataEvent |
| `ui::print_styled_list(title, items, numbered)` | `DataEvent{kind="styled_list", json}` | DataEvent |
| `ui::print_install_plan(packages)` | `DataEvent{kind="install_plan", json}` | DataEvent |
| `ui::print_install_summary(s, f)` | `DataEvent{kind="install_summary", json}` | DataEvent |
| `ui::print_remove_summary(target)` | `DataEvent{kind="remove_summary", json}` | DataEvent |
| `ui::print_subos_list(entries)` | `DataEvent{kind="subos_list", json}` | DataEvent |
| `ui::print_subcommand_help(...)` | `DataEvent{kind="help", json}` | DataEvent |
| `ui::confirm(question, default)` | `stream.prompt({id, question, options=["y","n"]})` | PromptEvent |
| `ui::select_package(items)` | `stream.prompt({id, question, options=[names]})` | PromptEvent |
| downloader TUI (render_progress_) | `DataEvent{kind="download_progress", json}` | DataEvent |

### CLI Consumer 模式

```cpp
// cli.cppm 创建 EventStream，注册 consumer
EventStream stream;
stream.on_event([&stream](const Event& e) {
    if (auto* d = std::get_if<DataEvent>(&e)) {
        dispatch_data_event(*d);  // 解析 JSON，调 ui:: 函数
    }
    if (auto* p = std::get_if<PromptEvent>(&e)) {
        handle_prompt(stream, *p); // 调 ui::confirm/select → stream.respond()
    }
});
// 然后传给 cmd_install(targets, yes, noDeps, stream)
```

### JSON Schema

**info_panel:**
```json
{"title": "...", "fields": [{"label": "...", "value": "...", "highlight": false}],
 "extra_fields": [...]}
```

**styled_list:**
```json
{"title": "...", "items": ["..."], "numbered": true}
```

**install_plan:**
```json
{"packages": [["name", "version"], ...]}
```

**install_summary / remove_summary:**
```json
{"success": 3, "failed": 1}
{"target": "gcc"}
```

**subos_list:**
```json
{"entries": [{"name":"default","active":true,"dir":"...","pkgCount":5}]}
```

**help:**
```json
{"name":"self","description":"...","args":[],"opts":[{"name":"install","desc":"..."}]}
```

**download_progress:**
```json
{"files": [{"name":"gcc","totalBytes":0,"downloadedBytes":0,"started":false,"finished":false,"success":false}],
 "elapsedSec": 12.5}
```

---

## 执行步骤

### Task 1: 扩展命令签名 + CLI EventStream 骨架

给所有命令函数加 `EventStream& stream` 参数。CLI 创建 EventStream 并传入。命令暂不使用 stream。

**Files:**
- 修改: `src/cli.cppm` — 创建 EventStream，传给命令调用
- 修改: `src/core/xim/commands.cppm` — 所有 cmd_* 加 stream 参数
- 修改: `src/core/xvm/commands.cppm` — cmd_use, cmd_list_versions 加 stream
- 修改: `src/core/subos.cppm` — run() 加 stream，内部 cmd 传递
- 修改: `src/core/xself.cppm` — run() 加 stream，内部 cmd 传递

### Task 2: CLI consumer + 简单 DataEvent 处理

实现 CLI consumer 的 dispatch_data_event() 和 handle_prompt()。先支持 info_panel 和 help 两种最简单的 kind。

**Files:**
- 修改: `src/cli.cppm` — 添加 consumer 逻辑
- 需要 `import xlings.ui` 在 cli.cppm（cli 已经导入）

### Task 3: 解耦 xvm/commands.cppm（最简单，2 处调用）

替换 `ui::InfoField` + `ui::print_info_panel` → `DataEvent`。移除 `import xlings.ui`。

### Task 4: 解耦 subos.cppm（3 处调用）

替换 `ui::print_subos_list` + `ui::InfoField`/`print_info_panel` → DataEvent。
CLI consumer 增加 subos_list 处理。移除 `import xlings.ui`。

### Task 5: 解耦 xself.cppm（4 处调用）

替换 `ui::InfoField`/`print_info_panel` + `ui::HelpOpt`/`print_subcommand_help` → DataEvent。
CLI consumer 增加 help 处理。移除 `import xlings.ui`。

### Task 6: 解耦 xim/commands.cppm — 数据展示部分

替换 `print_styled_list`, `print_install_plan`, `print_install_summary`,
`print_remove_summary`, `print_info_panel` → DataEvent。
CLI consumer 增加对应处理。

### Task 7: 解耦 xim/commands.cppm — 交互部分

替换 `ui::confirm()` → `stream.prompt()`。
替换 `ui::select_package()` → `stream.prompt()`。
CLI consumer 的 handle_prompt 处理。
移除 `import xlings.ui`。

### Task 8: 解耦 downloader.cppm — 提取 TUI 渲染

将 `render_progress_()` 移到 `src/ui/progress.cppm`。
downloader 的 TUI 刷新线程改为调用回调（ProgressRenderer）而非直接渲染。
`download_all()` 新增 `ProgressRenderer` 回调参数。
移除 downloader 的 `import xlings.ui`。

**新签名:**
```cpp
using ProgressSnapshot = std::vector<TaskProgress>;
using ProgressRenderer = std::function<void(const ProgressSnapshot&, double elapsedSec)>;

std::vector<DownloadResult>
download_all(std::span<const DownloadTask> tasks,
             const DownloaderConfig& config,
             ProgressRenderer onRender = nullptr,
             std::function<void(std::string_view, float)> onProgress = nullptr);
```

### Task 9: 连接 downloader → EventStream → UI

在 xim/commands.cppm 的 cmd_install 中，将 download_all 的 render callback 连接到 EventStream。
CLI consumer 收到 download_progress DataEvent 后调用 ui 渲染。

### Task 10: 全量验证

- `rm -rf build && xmake build`
- 运行 139 tests
- 验证 CLI: `xlings --help`, `xlings install`, `xlings search`
- 确认 core/ 无任何文件 import xlings.ui
