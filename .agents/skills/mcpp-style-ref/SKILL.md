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

### 接口与实现分离（推荐默认写法）

**新模块一律写成 `X.cppm` + `X.cpp` 一对。** 合并写法只留给极少数确实
只有声明、没有函数体的模块。

| | 文件 | 模块声明 | 产生 BMI? |
|---|---|---|---|
| 接口单元 | `X.cppm` | `export module M;` | 是 |
| 实现单元 | `X.cpp` | `module M;` | **否** |

```cpp
// error.cppm —— 接口:只有声明和文档
export module error;

import std;

export namespace mylib {

// 为什么存在、约束是什么、调用者必须知道什么 —— 写在这里。
struct Error {
    void test();
};

int add(int a, int b);

}  // namespace mylib
```

```cpp
// error.cpp —— 实现
module error;

import std;

namespace mylib {

void Error::test() { std::println("Hello"); }

int add(int a, int b) { return a + b; }

}  // namespace mylib
```

#### 为什么这是默认写法

不是风格偏好,是**编译期依赖**问题。函数体写在 `.cppm` 里,它就进了 BMI:
改一行函数体 → BMI 变 → **所有 import 它的 TU 全部重编**。

xlings 仓库实测(2026.8.13.1,110 个模块,32 核):

| | 合并写法 | 分离写法 | |
|---|---|---|---|
| 冷构建 | 56.40s | 26.09s | **2.16×** |
| 改一个实现(66 个下游 TU) | 54.04s | 4.35s | **12.4×** |
| 接口总行数 | 46,253 | 14,666 | **32%** |

#### 四类东西应当离开接口

1. **命名空间作用域的函数体** —— 最主要的一类。
2. **类成员函数体** —— 写在类内的函数体是隐式 `inline`,仍留在 BMI 里。
   声明留在类中,定义移到 `.cpp`。
3. **模块私有声明** —— 一个非 export 的 helper,只有当接口里**还有东西
   引用它**(导出的模板、`inline`/`constexpr` 函数体、类内函数体)时才需要
   留在接口。否则它应当整个搬进实现单元:留在接口就是留在 BMI,改签名会让
   所有 importer 白白重编。
4. **跟着函数体走的那些 `import`** —— 拆分工具会把 import 列表**复制**到
   实现单元,这对实现是对的、对接口是错的。接口会留下一堆只有被移走的函数体
   用到的 import,每一条都让接口的 BMI 依赖一个它并不需要的模块。
   **这一条编译器不会报错**,只能靠复查。

#### 分区的实现单元

分区(`module M:part;`)的实现要写成**主模块的实现单元**(`module M;`),
不能写 `module M:part;` —— 那是另一个分区,仍然产生 BMI。
一个模块可以有任意多个实现单元。

#### 拆分时会改变行为的四件事

搬代码不是纯粹的搬运,以下四点必须知道:

- **函数体搬出去会失去隐式 `inline`**。没有 LTO 时会损失跨 TU 内联。
  实测 `-O0` 下二进制大 0.10%。确实是热路径的小函数,可以显式留在接口里
  并标 `inline` —— 但要清楚这是在拿重编译代价换取内联。
- **命名空间作用域的 `static` 会丢掉 `static` 关键字**,从内部链接变成模块
  链接。定义仍然唯一,名字仍然是模块私有的。
- **无流参数的 `std::print` / `std::println` 在接口模板里会失败** ——
  clang 会把 `stdout` 推导成格式串。写全 `std::println(stdout, ...)`。
- **两个文件的文档注释要一致**。接口写「为什么」,实现顶部指回接口
  (`// See X.cppm for why this exists`),不要把同一段理由抄两遍 ——
  抄两遍就会各自漂移。

#### 什么时候可以合并

- 模块只有类型、常量、`concept`,没有函数体;
- 导出的**模板**(必须在接口里可见);
- `constexpr` / `consteval` 函数(调用者需要在编译期求值)。

除此之外:**分离**。

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
