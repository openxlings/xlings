# T18 — 项目级 .xlings.json 依赖声明与批量安装

> **优先级**: P2
> **Wave**: 3（独立于 T14-T17，但建议在 T14 完成后实施以获得更好体验）
> **预估改动**: ~60 行 C++
> **设计文档**: [../xim-issues-design.md](../xim-issues-design.md) §五

---

## 1. 任务概述

在项目根目录引入 `.xlings.json` 作为项目级配置文件，声明项目依赖。通过 `xlings install`（无参数）一键安装所有项目依赖。

复用已有的 `.xlings.json` 文件名，通过位置区分作用域：

| 位置 | 作用域 | 内容 |
|------|--------|------|
| `$XLINGS_HOME/.xlings.json` | 全局 | activeSubos, mirror, xim 配置 |
| `<project>/.xlings.json` | 项目级 | 项目名、语言、依赖列表 |

---

## 2. 问题分析

### 2.1 现状

- 老版本 `config.xlings`（Lua 格式）仅在文档中提到，从未实现依赖声明
- 当前 `xlings install` 必须指定包名，不带参数会进入 xim 帮助页面
- `$XLINGS_HOME/.xlings.json` 已作为全局配置使用，C++ 侧已有 `nlohmann::json` 库

### 2.2 用户需求场景

```
项目仓库/
├── .xlings.json       ← 项目级配置（声明依赖）
├── src/
├── exercises/
└── ...
```

新用户克隆项目后，运行 `xlings install` 即可安装所有项目依赖，无需逐个手动安装。

---

## 3. 依赖前置

| 依赖 | 原因 |
|------|------|
| T14 (建议) | xpkgs 复用机制到位后，批量安装性能更好 |

---

## 4. 实现方案

### 4.1 项目级 .xlings.json 格式设计

```json
{
    "name": "clings",
    "lang": "c",
    "deps": [
        "gcc@15.1.0",
        "cmake",
        "d2x"
    ]
}
```

字段说明：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 否 | 项目名 |
| `lang` | string | 否 | 项目语言 |
| `deps` | array | 否 | 依赖列表，每项格式同 `xlings install` 参数 |

**与全局 .xlings.json 的区分**：项目级文件包含 `deps` 字段且**不**包含 `activeSubos` / `xim` / `mirror` 等全局字段。查找时排除 `$XLINGS_HOME` 目录。

### 4.2 C++ 侧：install 命令无参分支

文件: `core/cmdprocessor.cppm`

当 `xlings install` 不带包名参数时，查找并解析项目级 `.xlings.json`：

```cpp
.add("install", "install package/tool",
    [](int argc, char* argv[]) {
        if (argc <= 2) {
            return install_from_project_config();
        }
        return xim_exec("", argc, argv);
    },
    "xlings install [package[@version]]")
```

```cpp
static int install_from_project_config() {
    auto configPath = find_project_xlings_json();
    if (configPath.empty()) {
        std::println("[xlings] no project .xlings.json found");
        std::println("  usage: xlings install <package[@version]>");
        return 1;
    }

    std::println("[xlings] found project config: {}", configPath.string());

    auto content = platform::read_file_to_string(configPath.string());
    auto json = nlohmann::json::parse(content, nullptr, false);
    if (json.is_discarded() || !json.contains("deps") || !json["deps"].is_array()) {
        std::println("[xlings] no deps found in {}", configPath.string());
        return 0;
    }

    auto& deps = json["deps"];
    auto name = json.value("name", configPath.parent_path().filename().string());
    std::println("[xlings] install deps for project: {} ({} packages)", name, deps.size());

    int failed = 0;
    for (size_t i = 0; i < deps.size(); ++i) {
        auto pkg = deps[i].get<std::string>();
        std::println("\n[xlings] [{}/{}] {}", i + 1, deps.size(), pkg);

        // 构造 argv: xlings install <pkg> -y
        std::vector<const char*> args = {"xlings", "install", pkg.c_str(), "-y"};
        int ret = xim_exec("", static_cast<int>(args.size()),
                           const_cast<char**>(args.data()));
        if (ret != 0) ++failed;
    }

    if (failed == 0) {
        std::println("\n[xlings] all {} deps installed", deps.size());
    } else {
        std::println("\n[xlings] {} of {} deps failed", failed, deps.size());
    }
    return failed > 0 ? 1 : 0;
}
```

### 4.3 项目级 .xlings.json 查找逻辑

从当前目录向上查找 `.xlings.json`，排除 `$XLINGS_HOME` 目录以避免误读全局配置：

```cpp
static std::filesystem::path find_project_xlings_json() {
    namespace fs = std::filesystem;
    auto& p = Config::paths();
    auto homeDir = fs::canonical(p.homeDir);
    auto dir = fs::current_path();

    while (true) {
        auto candidate = dir / ".xlings.json";
        if (fs::exists(candidate)) {
            // 排除全局配置文件
            auto candidateDir = fs::canonical(dir);
            if (candidateDir != homeDir) {
                return candidate;
            }
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}
```

---

## 5. 验收标准

### 5.1 基础功能验证

```bash
mkdir /tmp/test-project && cd /tmp/test-project

cat > .xlings.json << 'EOF'
{
    "name": "test-project",
    "lang": "c",
    "deps": [
        "cmake"
    ]
}
EOF

xlings install
# 期望输出:
#   [xlings] found project config: /tmp/test-project/.xlings.json
#   [xlings] install deps for project: test-project (1 packages)
#   [xlings] [1/1] cmake
#   ...
#   [xlings] all 1 deps installed

cmake --version
# 期望: 正常输出
```

### 5.2 多依赖验证

```bash
cat > .xlings.json << 'EOF'
{
    "name": "multi-deps",
    "deps": ["cmake", "d2x"]
}
EOF

xlings install
# 期望: 按顺序安装两个包
# 已安装的包跳过（显示 already installed）
```

### 5.3 子目录查找验证

```bash
cd /tmp/test-project
mkdir -p src && cd src

xlings install
# 期望: 向上查找到 /tmp/test-project/.xlings.json 并安装
```

### 5.4 不误读全局配置

```bash
cd ~/.xlings
xlings install
# 期望: 不将全局 .xlings.json 当作项目配置
#   [xlings] no project .xlings.json found
```

### 5.5 无配置文件时提示

```bash
cd /tmp
xlings install
# 期望:
#   [xlings] no project .xlings.json found
#   usage: xlings install <package[@version]>
```

### 5.6 无 deps 字段

```bash
echo '{"name": "empty"}' > /tmp/test-empty/.xlings.json
cd /tmp/test-empty
xlings install
# 期望: [xlings] no deps found in .xlings.json
```

### 5.7 验收检查表

| 检查项 | 期望结果 | 通过 |
|--------|---------|------|
| `xlings install` 无参数找到项目 `.xlings.json` | 正确解析 | [ ] |
| 批量安装所有依赖 | 每个依赖安装成功 | [ ] |
| 已安装依赖跳过 | 显示 already installed | [ ] |
| 子目录向上查找 `.xlings.json` | 正确找到 | [ ] |
| 排除 `$XLINGS_HOME/.xlings.json` | 不误读全局配置 | [ ] |
| 无 `.xlings.json` 时友好提示 | 输出使用说明 | [ ] |
| 无 `deps` 字段时不报错 | 正常退出 | [ ] |
| `xlings install <pkg>` 单包安装兼容 | 行为不变 | [ ] |
