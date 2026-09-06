> 编写日期: 2026-07-29 | 版本: 2026.7.29.0

# 自我管理与修复

`xlings self` 管理 xlings 自身：升级客户端、检查并修复 home 的状态、清理缓存。
日常几乎用不到，但升级之后、或者某个命令开始报奇怪的错时，从这里开始。

## 升级

```bash
xlings self update
```

它做两件事：刷新包索引，然后把 xlings 自己作为一个包安装到最新版。
升级完成后可能会看到这一行：

```
packages installed by the previous client may still be registered in its format
  run  xlings self doctor --fix
```

这不是错误。它的意思是：这个 home 里的包是更早的客户端注册的，记录格式还是旧的。
旧格式仍然能用，但新客户端读它的时候会看到一些"看起来坏了、其实没坏"的条目。
跑一次 `--fix` 就迁移完了，之后这行提示不再出现。

## 体检

```bash
xlings self doctor
```

只读，不改任何东西。它检查四层状态是否一致：

| 层 | 检查什么 |
|----|----------|
| workspace | 当前 SubOS 声明的活跃版本 |
| 版本数据库 | 每个 `包@版本` 的注册记录 |
| shim | `bin/` 下的转发文件 |
| payload | 记录指向的目录和里面的可执行文件 |
| sysroot | `lib/`、`lib64/`、`usr/`、`etc/`、`share/` 下的链接是否还指得到东西 |

退出码 0 表示当前 SubOS 健康；非 0 表示有需要处理的问题。
只有告警（例如"某个 alias 可能是系统命令"）不会让它变红。

默认输出是收敛过的：一个丢失的 payload 只报一行，而不是它注册的每个程序报一行；
"没问题但值得知道"的条目（例如只用来锚定发布的包）合并成一行计数。想看全部：

```bash
xlings self doctor --all
```

## 修复

```bash
xlings self doctor --fix
```

一次跑完，把能修的都修掉。它会：

- 重建缺失的 shim，删除多余的 shim
- 丢掉指向不存在版本的绑定边（这类边会让 `use` 和 `install` 都失败）
- 把指向未注册版本的活跃项取消激活 —— 包括**其他 SubOS** 的
- 对 payload 坏掉的包重新注册；不行就卸载重装
- 把断掉的 sysroot 链接**重新指回去**（下面单独说），指不回去的才删除
- 把**应该在、却不在**的 sysroot 链接放回去（被更早的客户端删掉的那些）
- 如果这个 home 被整体挪过位置，把记录和链接**重新指到当前根**（下面单独说）
- 上面都救不回来、且任何索引都提供不了的记录，直接清除

修复结束后会**重新体检一遍**，你看到的报告是**修复之后**的状态，不是修复之前的。
所以"报了一堆问题又说修好了"不会发生；剩下的行就是真的还没解决的。

想先看看它打算做什么：

```bash
xlings self doctor --fix --dry-run
```

列出将要执行的动作，一个字节都不改。

### 关于"清除记录"

`--fix` 会删除注册记录，但只在同时满足这些条件时：

- payload 目录已经不存在了（或者存在但里面没有可执行文件、也不是发布锚点）
- 重新注册、卸载重装都失败了
- 没有任何索引能重新提供它
- **没有别的 SubOS 还在用它**

删掉的是一个指向虚空的指针 —— payload 早就没了，留着只会让每次体检变红。
包本身、payload 本身，都不会被 `--fix` 删除。

**这一步会丢信息**（"这个包曾经装过"这条事实没有了），所以它不会被算作"修好了"：
一次清除过记录的运行，结尾不会说 `OK`，而是明说清除了几条。
**删除 sysroot 链接同样算丢信息**，同样不会说 `OK` —— 从 sysroot 里删掉一个文件，
不比丢掉一条记录轻。

退出码仍然是 0。退出码回答的是"这个 home 还需不需要人来处理"，
合法清除之后它不需要 —— 把它变成非 0 等于把"丢了东西"和"坏了"混成一个信号。

### 断掉的 sysroot 链接

SubOS 的 `lib/`、`usr/include/` 这些目录里是指向 payload 的链接。
包被换版本、被移除之后，可能留下指不到东西的链接。

`--fix` 对它们的处理顺序是**先问版本数据库**：
如果数据库知道这个库当前该指向哪里，就**重新指过去**；
只有在没有任何记录能说明它该指向哪里时，才删除它。

"目标不存在"和"目标救不回来"不是同一件事 —— 前者只是链接过期了。

### 如果你把整个 home 挪过位置

用 `mv` 搬 home **不是受支持的操作**，以后也不会是：payload 里的可执行文件把旧路径
写死在 `PT_INTERP`/`RPATH` 里（内核按字面读它，`$ORIGIN` 在那里不展开），linker script
里也是绝对路径。这些东西不是 xlings 的簿记，是 payload 的内容，谁都改不回来。

但**"不修"不等于"可以毁"**。挪过之后 `self doctor` 会先说出这件事：

```
✗ home relocated   this home is at <新路径>, but its records were written for <旧路径>
                   2901 registration(s) still name the old path and 2901 of them
                   have their payload present here
  → run            xlings self doctor --fix
  • note           re-points the versions database and the sysroot links;
                   binaries that baked the old path into PT_INTERP/RPATH are
                   NOT repaired — re-provision the home if the toolchain fails
```

`--fix` 会把 xlings 自己的簿记指回当前根：版本数据库、索引缓存、SubOS 清单里的
绝对路径，以及**所有 SubOS** 的 sysroot 链接。它**不会**因为记录里的路径不存在
就删链接、清记录 —— 判据是"payload 在不在当前根下"，不是"记录里的路径存在不存在"。

它也不会因此宣布这个 home 好了。上面那条 `note` 是认真的：用旧路径编译出来的东西
仍然可能起不来。真正的答案还是重新 provision（`$MCPP_HOME` 下的 registry 是可重建
缓存，删掉重来就是）。

被更早的客户端删掉过链接的 home 也能救回来：`--fix` 会把当前选择声明、却不在位的
链接重新放回去。单个包也可以自己来 —— `xlings install <包>@<版本>`，payload 还在
的话它不下载、只重新注册并重新放置链接。

### 命令给的建议一定能跑

报告里 `→ run` 后面的命令是可以直接粘贴执行的完整命令。
它给的是**包**的坐标，不是程序名 —— `nm` 是 llvm 装出来的程序，
索引里没有叫 `nm` 的包，所以这里不会出现 `xlings install nm`。
如果没有任何包能提供某个条目，它会直说没有办法，而不是给一条跑不通的命令。

### 元数据重置

```bash
xlings self doctor --reset-metadata
```

单独一个开关，`--fix` 不包含它。原因是它**会丢信息**：把读不出来的发布元数据
（成员列表、头文件资产）整个丢掉，条目退回成一个可以单独切换的版本。
只有在这些元数据已经损坏、且重装也救不回来时才用它。

## 清理

```bash
xlings self clean            # 清缓存 + 回收没人引用的包
xlings self clean --dry-run  # 先看看会删什么
```

payload 是按引用计数共享的，`clean` 只回收所有 SubOS 都不再引用的那些。

## 其他

```bash
xlings self config     # 显示 home 路径、当前 SubOS、镜像等配置
xlings self install    # 从 release 包安装 xlings（一般由安装脚本调用）
xlings self init       # 创建 home / data / subos 目录
xlings self migrate    # 把旧版本的目录布局迁移到 subos/default
```

## 出问题时的顺序

```
xlings self doctor              # 先看清楚是什么问题
xlings self doctor --fix --dry-run   # 再看它打算怎么修
xlings self doctor --fix        # 修
xlings self doctor              # 确认干净了（退出码 0）
```

如果 `--fix` 之后还有剩余问题，报告里会写明每一条为什么没修好。
属于其他 SubOS 的 payload 问题不会在这里修 —— 修它需要在那个 SubOS 里安装，
会把包拉进当前环境，所以报告只会告诉你去哪个 SubOS 处理。

## 相关文档

- [多版本管理](multi-version.md) —— 版本视图、shim、坐标写法
- [SubOS 与 Agent](subos-and-agent.md) —— 多环境的隔离与共享
- [xvm 版本管理](../design/xvm-version-management.md) —— 内部实现
