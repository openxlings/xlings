# 按场景查命令

[English](../09-commands-by-scenario.md) | **简体中文**

知道名词之后的速查。每个命令和参数都在[生成的参考](../generated/command-reference.md)里；
luban 的在[它自己的参考](../generated/luban-command-reference.md)里。

## 命令树

第一级是一个名词，或者适用于所有名词的动词；作用于某个名词的命令放在它下面；参数是最廉价、也最多的添加。

| 第一级 | 它指的是 |
|---|---|
| `install`、`remove`、`update`、`use`、`list`、`search`、`info`、`why` | 包与版本 |
| `config`、`index` | 包从哪里来，以及 xlings 的行为 |
| `subos …` | 环境 |
| `self …` | xlings 本身 |
| `interface`、`agent` | 驱动 xlings 的程序 |
| `profile`、`script` | profile 的历史；xlings 脚本 |

`luban` 是环境的第二个入口：它的每个环境命令就是它所对应的 `xlings subos` 命令（[Luban](luban/README.md)）。

## 包

| 想要 | 命令 |
|---|---|
| 安装，指定版本 | `xlings install gcc@16.1.0` |
| 装进另一个 SubOS | `xlings install gcc --subos work` |
| 给所有用户安装（root） | `sudo xlings install --system gcc` |
| 切换活跃版本 | `xlings use gcc 16.1.0` |
| 看装了什么 | `xlings list` |
| 看一个包有哪些版本 | `xlings info gcc --all-versions` |
| 知道依赖为什么解析成这样 | `xlings why glibc` |
| 卸载 | `xlings remove gcc`（`--all`、`--all-subos`、`--subos <n>`） |
| 刷新索引 | `xlings update` |

## 环境

| 想要 | 命令 |
|---|---|
| 创建一个 | `xlings subos new work` |
| 创建一个沙箱 | `xlings subos new work --sandbox=private` |
| 创建一个根（一个 Linux 系统） | `xlings subos new box --rootfs --from subos:luban-core` |
| 进入 | `xlings subos use work` |
| 在里面运行一条命令 | `xlings subos exec work -- make -j8` |
| 让它保持运行 | `xlings subos start work --ttl 30m` |
| 拷入拷出文件 | `xlings subos cp ./data.csv work:/tmp/` |
| 修改它的策略 | `xlings subos config work --net none` |
| 看它要求了什么、得到了什么 | `xlings subos status work` |
| 看里面发生过什么 | `xlings subos log work`、`xlings subos report work` |
| 升到它的 edition 的最新版 | `xlings subos upgrade work` |
| 根回滚 | `xlings subos rollback box` |
| 做成镜像 | `luban export box box.iso` |
| 删除 | `xlings subos remove work` |

## xlings 本身

| 想要 | 命令 |
|---|---|
| 升级 | `xlings self update` |
| 体检与修复 | `xlings self doctor`、`xlings self doctor --fix` |
| 看这台机器能怎样隔离 | `xlings self doctor --isolation` |
| 用中国大陆镜像 | `xlings config --mirror CN` |
| 添加索引 | `xlings config --index-repo myteam:https://example.com/idx.git` |
| 清理缓存 | `xlings self clean` |
| 卸载 | `xlings self uninstall` |

## 程序

| 想要 | 命令 |
|---|---|
| 从不被提问 | `--agent`，或 `XLINGS_AGENT_MODE=1` |
| 机器可读的输出 | 命令支持时用 `--json` |
| 稳定的程序化接口 | `xlings interface --list` |
