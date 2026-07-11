---
name: mcpp-style-ref
description: 为 mcpp 项目应用 Modern/Module C++ (C++23) 编码风格。适用于编写或审查带模块的 C++ 代码、命名标识符、组织 .cppm/.cpp 文件，或用户提及 mcpp、module C++、现代 C++ 风格时。
---

# mcpp-style-ref

mcpp 项目的 Modern/Module C++ 风格参考。C++23，使用 `import std`。

## 快速参考

### 命名

| 种类 | 风格 | 示例 |
|------|------|------|
| 类型/类 | PascalCase（大驼峰） | `StyleRef`, `HttpServer` |
| 对象/成员 | camelCase（小驼峰） | `fileName`, `configText` |
| 函数 | snake_case（下划线） | `load_config_file()`, `parse_()` |
| 私有 | `_` 后缀 | `fileName_`, `parse_()` |
| 常量 | UPPER_SNAKE | `MAX_SIZE`, `DEFAULT_TIMEOUT` |
| 全局 | `g` 前缀 | `gStyleRef` |
| 命名空间 | 全小写 | `mcpplibs`, `mylib` |

### 模块基础

- 使用 `import std` 替代 `#include <print>` 和 `#include <xxx>`
- 使用 `.cppm` 作为模块接口；分离实现时用 `.cpp`
- `export module module_name;` — 模块声明
- `export import :partition;` — 导出分区
- `import :partition;` — 内部分区（不导出）

### 模块结构

```
// .cppm
export module a;

export import a.b;
export import :a2;   // 可导出分区

import std;
import :a1;          // 内部分区
```

### 模块命名

- 模块：`topdir.subdir.filename`（如 `a.b`, `a.c`）
- 分区：`module_name:partition`（如 `a:a1`, `a.b:b1`）
- 用目录路径区分同名：`a/c.cppm` → `a.c`，`b/c.cppm` → `b.c`

### 类布局

```cpp
class StyleRef {
private:
    std::string fileName_;  // 数据成员带 _ 后缀

public:  // Big Five
    StyleRef() = default;
    StyleRef(const StyleRef&) = default;
    // ...

public:  // 公有接口
    void load_config_file(std::string fileName);  // 函数 snake_case，参数 camelCase

private:
    void parse_(std::string config);  // 私有函数以 _ 结尾
};
```

### 实践规则

- **初始化**：用 `{}` — `int n { 42 }`，`std::vector<int> v { 1, 2, 3 }`
- **字符串**：只读参数用 `std::string_view`
- **错误**：用 `std::optional` / `std::expected` 替代 int 错误码
- **内存**：用 `std::unique_ptr`、`std::shared_ptr`；避免裸 `new`/`delete`
- **RAII**：将资源与对象生命周期绑定
- **auto**：用于迭代器、lambda、复杂类型；需要明确表达意图时保留显式类型
- **宏**：优先用 `constexpr`、`inline`、`concept` 替代宏

### 接口与实现

两种写法均支持。

**写法 A：合并** — 接口与实现同在一个 `.cppm` 中：

```cpp
// mylib.cppm
export module mylib;

export int add(int a, int b) {
    return a + b;
}
```

**写法 B：分离** — 接口在 `.cppm`，实现在 `.cpp`（编译期隐藏实现）：

```cpp
// error.cppm（接口）
export module error;

export struct Error {
    void test();
};
```

```cpp
// error.cpp（实现）
module error;

import std;

void Error::test() {
    std::println("Hello");
}
```

简单模块用写法 A；需隐藏实现或减少编译依赖时用写法 B。

## 项目环境配置

安装 xlings 包管理器后，获取 GCC 15 工具链：

#### Linux/MacOS

```bash
curl -fsSL https://raw.githubusercontent.com/d2learn/xlings/refs/heads/main/tools/other/quick_install.sh | bash
```

#### Windows - PowerShell

```bash
irm https://raw.githubusercontent.com/d2learn/xlings/refs/heads/main/tools/other/quick_install.ps1 | iex
```

然后安装工具链(仅linux, 其中windows默认用msvc)：

```bash
xlings install gcc@15 -y
```

> xlings详细信息可参考 [xlings](https://github.com/d2learn/xlings) 文档。

## 示例项目创建

参考本仓库 `src/` 目录结构：

- `mcpp.toml`：声明 C++23 模块目标、工具链和依赖
- `add_files("main.cpp")`、`add_files("**.cppm")` 添加源文件
- 可执行目标与静态库目标分离（如 `mcpp-style-ref` 主程序、`error` 静态库）

构建：

```bash
mcpp build
mcpp test
```

## 适用场景

- 编写新的 C++ 模块代码（`.cppm`、`.cpp`）
- 审查或重构 mcpp 项目中的 C++ 代码
- 用户询问「mcpp 风格」「module C++ 风格」或「现代 C++ 惯例」

## 更多资源

- 完整参考：[reference.md](reference.md)
- mcpp-style-ref 仓库：[github.com/mcpp-community/mcpp-style-ref](https://github.com/mcpp-community/mcpp-style-ref)
    - 项目说明：[../../README.md](../../README.md)
    - 示例项目：[src/](../../../src)
- xlings 包管理器：[github.com/d2learn/xlings](https://github.com/d2learn/xlings)
