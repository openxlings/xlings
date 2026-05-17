# T01 — platform: 新增 `get_executable_path()`

> **Wave**: 1（无前置依赖，可立即执行）
> **预估改动**: ~30 行 C++，分布于 3 个文件

---

## 1. 任务概述

在三个平台分区模块中分别实现 `get_executable_path()` 函数，返回当前进程可执行文件的绝对路径（**不跟随符号链接**）。此函数是 T02（config 自包含检测）的前置依赖。

---

## 2. 依赖前置

无（Wave 1，独立任务）

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/platform/linux.cppm` | 新增函数实现 |
| `core/platform/macos.cppm` | 新增函数实现 |
| `core/platform/windows.cppm` | 新增函数实现 |
| `core/platform.cppm` | 新增导出声明（`using platform_impl::get_executable_path;`） |

---

## 4. 实施步骤

### 4.1 Linux 实现（`core/platform/linux.cppm`）

```cpp
// 在 platform_impl namespace 内新增
export std::filesystem::path get_executable_path() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n == -1) return {};
    buf[n] = '\0';
    return std::filesystem::path(buf);
}
```

全局模块片段需要加入：
```cpp
module;
#include <unistd.h>   // readlink
```

### 4.2 macOS 实现（`core/platform/macos.cppm`）

```cpp
// 在 platform_impl namespace 内新增
export std::filesystem::path get_executable_path() {
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    // 解析符号链接得到真实路径
    char real[4096];
    if (::realpath(buf, real) == nullptr) return std::filesystem::path(buf);
    return std::filesystem::path(real);
}
```

全局模块片段需要加入：
```cpp
module;
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#include <stdlib.h>        // realpath
```

### 4.3 Windows 实现（`core/platform/windows.cppm`）

```cpp
// 在 platform_impl namespace 内新增
export std::filesystem::path get_executable_path() {
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return {};
    return std::filesystem::path(buf);
}
```

全局模块片段需要加入：
```cpp
module;
#include <windows.h>
```

### 4.4 主模块导出（`core/platform.cppm`）

在 `export namespace xlings::platform` 块中新增：

```cpp
using platform_impl::get_executable_path;
```

---

## 5. 注意事项

- Linux 的 `/proc/self/exe` 始终返回真实文件路径（已跟随符号链接），符合需求
- macOS 的 `_NSGetExecutablePath` 可能返回相对路径或含符号链接，需 `realpath()` 规范化
- Windows 的 `GetModuleFileNameW` 直接返回真实路径，无需额外处理
- 返回类型统一为 `std::filesystem::path`，空路径表示获取失败

---

## 6. 验收标准

### 6.1 编译验证

```bash
xmake build xlings   # 三平台均无编译错误
```

### 6.2 功能验证（Linux）

在 `main.cpp` 中临时添加打印，验证路径符合预期：

```cpp
auto exePath = xlings::platform::get_executable_path();
// 期望输出: /home/user/.xlings/bin/.xlings.real
// 或自包含模式: /path/to/extracted/xlings-0.2.0/bin/.xlings.real
```

### 6.3 符号链接验证

```bash
ln -s /path/to/.xlings.real /tmp/test-link
/tmp/test-link   # get_executable_path() 应返回 /path/to/.xlings.real，而非 /tmp/test-link
```

### 6.4 后置检查

T02 可以直接调用 `xlings::platform::get_executable_path()` 无编译错误。
