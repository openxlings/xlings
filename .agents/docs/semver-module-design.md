# xlings 语义化版本模块设计方案

> 日期: 2026-05-12
> 状态: 实施中

## 一、问题

版本处理散落在 6+ 个文件中，存在三套不同的比较逻辑：

| 位置 | 比较方式 | 问题 |
|------|---------|------|
| `catalog.cppm:select_version_()` | 字典序 `ver > best` | `"9.0" > "15.0"` 结果错误 |
| `db.cppm:pick_highest_version()` | 按 `.` 拆分数值比较 | 正确但重复实现 |
| `commands.cppm:cmd_use()` | 按 `.` 拆分数值比较 | 与 db.cppm 重复 |
| `resolver.cppm` | 字典序 | 同 catalog 问题 |

且不支持任何版本范围表达式（`>=`、`^`、`~`）。

## 二、目标

1. 新增 `xlings::semver` 模块，统一版本解析、比较、匹配
2. 支持核心 semver range 语法（对齐 npm/Cargo 规范）
3. 替换用户面对的版本处理代码，使用统一接口

## 三、作用域

### semver 模块处理的（用户面对的版本号）

- xpm 中定义的包版本: `"15.1.0"`, `"2.39"`, `"2026.5.7"`
- 用户 CLI 输入: `pkg@15`, `pkg@^15.1.0`, `pkg@>=22.0`
- .xlings.json workspace 中的版本约束: `"^15.1.0"`, `">=2.0"`
- 依赖解析时的版本选择

### semver 模块不处理的（xvm 内部版本键）

- xvm 注册版本键: `"gcc-15.1.0"`, `"glibc-2.39"`, `"node-24.15.0"`
- 这些是不透明的绑定标签，格式由包脚本作者决定，无规范
- 保持现有的精确匹配 + 前缀匹配逻辑，不走 semver
- `db.cppm` 中的 `match_version()` 和 `pick_highest_version()` 不改造

## 四、支持的语法

```
精确:     1.2.3          → == 1.2.3
前缀:     15             → >= 15.0.0 && < 16.0.0（现有行为保留）
          15.1           → >= 15.1.0 && < 15.2.0
范围:     >=1.2.0        → >= 1.2.0
          >1.0.0         → > 1.0.0
          <=2.0.0        → <= 2.0.0
          <3.0.0         → < 3.0.0
兼容:     ^1.2.3         → >= 1.2.3 && < 2.0.0（同主版本）
          ^0.2.3         → >= 0.2.3 && < 0.3.0（主版本为0时锁次版本）
近似:     ~1.2.3         → >= 1.2.3 && < 1.3.0（同主+次版本）
通配:     1.2.*          → >= 1.2.0 && < 1.3.0
          1.*            → >= 1.0.0 && < 2.0.0
区间:     >=1.0.0 <2.0.0 → 空格分隔表示 AND
```

不支持（避免过度复杂）：`||`（OR）、build metadata（`+build`）。

prerelease 标签（`-beta.1`）：支持解析和比较，`1.0.0-beta.1 < 1.0.0`。

## 五、模块接口

### 文件：`src/core/semver.cppm`

```cpp
export module xlings.core.semver;
import std;

export namespace xlings::semver {

// ── 版本号 ──
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    int components = 0;       // 用户写了几段: "15"→1, "15.1"→2, "15.1.0"→3
    std::string prerelease;   // "beta.1", "" 表示正式版

    // 比较: 1.3.3-beta.1 < 1.3.3 < 1.3.4
    std::strong_ordering operator<=>(const Version& o) const;
    bool operator==(const Version& o) const;
};

// 解析版本字符串
// "15.1.0"        → Version{15, 1, 0, 3, ""}
// "15"            → Version{15, 0, 0, 1, ""}
// "1.3.3-beta.1"  → Version{1, 3, 3, 3, "beta.1"}
std::optional<Version> parse(std::string_view s);

// 格式化输出
std::string to_string(const Version& v);

// ── 版本范围 ──
enum class Op { Eq, Gt, Gte, Lt, Lte };

struct Constraint {
    Op op;
    Version ver;
};

struct Range {
    std::vector<Constraint> constraints;  // 所有约束取交集 (AND)
};

// 解析范围表达式
// "^1.2.3"          → [Gte{1,2,3}, Lt{2,0,0}]
// ">=1.0.0 <2.0.0"  → [Gte{1,0,0}, Lt{2,0,0}]
// "~1.2.3"          → [Gte{1,2,3}, Lt{1,3,0}]
// "15"              → [Gte{15,0,0}, Lt{16,0,0}]
// "15.1.0"          → [Eq{15,1,0}]
// "1.2.*"           → [Gte{1,2,0}, Lt{1,3,0}]
std::optional<Range> parse_range(std::string_view expr);

// 版本是否满足范围
bool satisfies(const Version& ver, const Range& range);

// ── 实用函数 ──

// 从版本字符串列表中选择满足范围的最高版本
// 跳过 "latest" 标签，返回空字符串表示无匹配
std::string select_best(std::span<const std::string> available,
                        std::string_view range_expr);

// 比较两个版本字符串 (-1, 0, 1)
// 无法解析的版本回退到字典序比较
int compare(std::string_view a, std::string_view b);

// 排序：降序（最高版本在前）
void sort_desc(std::vector<std::string>& versions);

} // namespace xlings::semver
```

## 六、改造点清单

### Phase 1: semver 核心模块

新增 `src/core/semver.cppm`，实现上述接口。

### Phase 2: 替换用户面对的版本处理

| 文件 | 函数 | 当前逻辑 | 改为 |
|------|------|---------|------|
| `catalog.cppm:105-141` | `select_version_()` | 字典序前缀匹配 | `semver::select_best(available, hint)` |
| `catalog.cppm:120` | 版本比较 | `ver > best` | `semver::compare(ver, best) > 0` |
| `resolver.cppm:81-95` | 版本选择 | 字典序 | `semver::select_best()` |
| `resolver.cppm:131-150` | 版本选择 | `ver > best` | `semver::compare(ver, best) > 0` |
| `commands.cppm:160-188` | `cmd_use()` latest | 手写 numeric sort | `semver::sort_desc()` |

### Phase 3: 扩展支持

- `.xlings.json` workspace 版本值支持 range 表达式
- CLI `pkg@^15` / `pkg@>=1.0` 语法

### 不改动的部分

| 文件 | 函数 | 原因 |
|------|------|------|
| `db.cppm:99-129` | `pick_highest_version()` | xvm 内部键，非用户版本 |
| `db.cppm:136-219` | `match_version()` | xvm 内部键，非用户版本 |

## 七、.xlings.json 新增支持写法

```json
{
  "workspace": {
    "gcc": "^15.1.0",
    "node": ">=22.0.0",
    "npm": "~11.2.0",
    "glibc": "2.39",
    "cmake": ">=3.28 <4.0"
  }
}
```

## 八、向后兼容

- `"15.1.0"` → 解析为精确匹配 `Eq{15,1,0}`，行为不变
- `"15"` → 解析为前缀范围 `[Gte{15,0,0}, Lt{16,0,0}]`，行为不变
- `"latest"` → 特殊处理保留，不经过 semver 解析
- 现有 `.xlings.json` 零改动即兼容

## 九、职责边界

```
┌──────────────────────────────────────────┐
│  semver 模块                              │
│  只处理规范版本号: "15.1.0", "^2.39"      │
│  用于: install, workspace, 依赖解析       │
└──────────────────────────────────────────┘

┌──────────────────────────────────────────┐
│  xvm 版本键 (不改动)                      │
│  不透明字符串: "gcc-15.1.0", "glibc-2.39" │
│  保持现有的精确匹配 + 前缀匹配            │
│  不走 semver                              │
└──────────────────────────────────────────┘
```
