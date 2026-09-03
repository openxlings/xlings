# XVM | Xlings Version Manager

版本管理功能已融合到 xlings 单二进制中，不再需要独立的 xvm 程序。

---

## 基本用法

### 安装并注册版本

```bash
# 安装包后自动注册到版本数据库
xlings install gcc@15
xlings install node
```

安装完成后，版本信息自动写入 `~/.xlings/.xlings.json` 的 `versions` 段。

### 切换版本

```bash
# xlings use <target> <version>
xlings use gcc 15      # 模糊匹配: 15 → 15.1.0
xlings use gcc 14.2    # 精确匹配: 14.2 → 14.2.0
xlings use node 22
```

`use` 命令会：
1. 更新当前 subos 的 workspace（`~/.xlings/subos/<active>/.xlings.json`）
2. 同步该 subos 的**路由表**（`bin/` 目录），见下

### 路由表：文件表示「路由」，不表示「状态」

`subos/<n>/bin/` 里的一个文件**只表示**「这个名字由 xlings 分发」。它是 entry binary
的一个 symlink/hardlink，不携带版本、不携带来源 —— 所以它**不能**、也**不应该**被读成
「这个 subos 激活了某个版本」。状态只在 `subos/<n>/.xlings.json` 的 workspace 里。

2026.9.3.1 之前这两个含义共用一个目录、由两个 scope 写入:project 作用域的
`mirror_shim_to_global_bin` 往全局 bin 里塞路由条目(项目自己的 bin 从来不在 PATH 上,
而 cmd.exe 没有 cd 钩子能把它放上去),install/use 从全局作用域写状态条目。三个读者各按
不同含义读它,于是「项目装的工具在项目外报错」和「泄漏的名字永远不回收」同时成立。

现在:

- **唯一写者** `xself::sync_shim_tables()`,install / use / remove 都只调它。
- **期望集合** = 本 subos 的 active program ∪ 已知项目声明的命令名。项目命令名由
  `knownProjects`(只记路径)在重建时从项目自己的状态文件现算,不缓存 —— 缓存会过期,
  而过期的缓存会把已经不存在的名字重新建出来。
- **差集应用**,不清空重建:避免 PATH 空窗,且 Windows 上一个被占用的文件不会毁掉整次。
- **不是我们的文件**(不是 entry binary 的链接)只报告、绝不删除。
- **`doctor` 一条规则**(`shim table`)取代了原来互相打架的 Check 1 / Check 2。缺文件是
  Error(有程序 active 却跑不了),多文件是 Notice(删前删后都解析到空,且在老 home 上
  不是用户造成的)。

设计与实测:`.agents/docs/2026-09-03-project-shim-routing-vs-state-design.md`

### 查看版本

```bash
# 查看某个工具的所有已注册版本
xlings use gcc           # 不带版本号时列出所有版本
xlings info gcc          # 查看包详细信息
```

### Shim 机制

版本切换后，在 subos/bin/ 中创建的 shim 会硬链接到 xlings 二进制。当通过 shim 执行时：

```bash
gcc --version       # argv[0]=gcc → shim 模式 → 查版本 → exec 真实 gcc
g++ -o main main.cpp  # argv[0]=g++ → 查 bindings → exec 真实 g++-15
```

shim 分发流程：
1. 检测 `argv[0]` 提取程序名
2. 读取 effective workspace（项目 > subos > 全局）
3. 模糊匹配版本号
4. 展开路径变量 `${XLINGS_HOME}`
5. 设置环境变量
6. `execvp` 真实程序

### 宿主机透传

当解析到的 scope 对这个名字**毫无主张**时(既不 active,`installed[]` 里也没有),shim
把名字交还给 PATH,运行「如果 xlings 从没在那里放过文件、本来会跑的那个程序」。这正是
路由表存在的代价的对冲:项目的命令名必须出现在全局 bin 里,但它**不该改变项目之外的
任何行为**。

PATH 遍历会排除 xlings 自己的目录(按文件标识 + 路径前缀),`XLINGS_SHIM_DEPTH` 作兜底。
交互时(stderr 是 TTY)打印一行说明;`make` / CI 里静默,那里应该就是普通的 PATH 语义。

**另外两支不透传**,因为它们是这个 scope 提出的、但满足不了的**主张**:

| 状态 | 含义 | 行为 |
|---|---|---|
| `active` 无、`installed[]` 无 | 毫无主张 | **透传** |
| `active` 无、`installed[]` 有 | opt-in 了,只是没选版本 | 报错 |
| `active` 有、版本没装 | pin 了但没装 | 报错 |

后两支若也透传,就是 pyenv 至今带着的静默替换:pin 了一个没装的版本、系统上恰好有同名
命令,于是跑了系统的且一声不吭。

## 版本视图 (Subos)

每个 subos 维护独立的版本视图：

```bash
xlings subos new dev         # 创建 dev 环境
xlings subos use dev         # 切换到 dev
xlings use gcc 14            # 仅影响 dev 的版本视图
xlings subos use default     # 切回默认，gcc 版本不变
```

## 项目级配置

在项目目录创建 `.xlings.json` 可覆盖版本：

```json
{
  "workspace": {
    "gcc": "14.2.0"
  }
}
```

在此目录下执行 `gcc` 时，shim 会使用 14.2.0 版本（不影响全局设置）。

## 配置层级

```
优先级: 项目配置 > 当前 subos 配置 > 全局配置 > 硬编码默认值
```

| 文件 | 内容 |
|------|------|
| `~/.xlings/.xlings.json` | 全局 versions + lang + mirror + activeSubos |
| `~/.xlings/subos/<name>/.xlings.json` | workspace（版本视图） |
| `<project>/.xlings.json` | workspace 覆盖 + 本地 versions |

## 从旧版 xvm 迁移

| 旧 xvm 命令 | 新命令 |
|-------------|--------|
| `xvm add <target> <ver> --path <path>` | 通过 `.xlings.json` versions 段配置 |
| `xvm use <target> <ver>` | `xlings use <target> <ver>` |
| `xvm list <target>` | `xlings use <target>` (不带版本号) |
| `xvm current <target>` | `xlings use <target>` |
| `xvm remove <target> <ver>` | `xlings remove <target>` |
