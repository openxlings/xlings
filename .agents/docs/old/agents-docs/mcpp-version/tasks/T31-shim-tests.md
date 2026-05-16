# T31 — shim 统一策略测试（单元 + E2E）

> **Wave**: 依赖 T29, T30
> **预估改动**: ~80 行测试代码
> **设计文档**: [../shim-optimization-design.md](../../shim-optimization-design.md)

---

## 1. 任务概述

为统一的 `create_shim()` 函数和整体 shim 创建流程添加测试覆盖。

---

## 2. 涉及文件

| 文件 | 操作 |
|------|------|
| `tests/unit/test_main.cpp` | 新增：ShimCreateTest 系列单元测试 |
| `tests/e2e/shim_link_test.sh` | 新建：验证 shim 为 symlink 的 E2E 测试 |

---

## 3. 单元测试用例

| 测试名 | 验证内容 |
|--------|----------|
| `ShimCreateTest.CreatesSymlinkOnUnix` | create_shim() 在 Unix 上创建 symlink |
| `ShimCreateTest.SymlinkIsRelative` | symlink 使用相对路径 |
| `ShimCreateTest.OverwritesExisting` | 已存在的 shim 被正确替换 |
| `ShimCreateTest.SourceNotExist` | 源文件不存在时返回 Failed |
| `ShimCreateTest.IsBuiltinShimCoverage` | is_builtin_shim 覆盖所有基础和可选 shim |

---

## 4. E2E 测试用例

| 测试 | 验证内容 |
|------|----------|
| `shim_link_test.sh` | self init 后基础 shim 为 symlink 且可执行 |
| 扩展 bootstrap_home_test.sh | 验证 portable 模式下 shim 类型 |

---

## 5. 验收

```bash
# 单元测试
xmake build xlings_tests && xmake run xlings_tests --gtest_filter="ShimCreate*"

# E2E 测试
bash tests/e2e/shim_link_test.sh
```
