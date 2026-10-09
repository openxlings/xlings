# Luban：环境、镜像与 agent 私有工作区

Luban 是一个最小 OS 模型：内核之上只有 xlings（包）和 luban（系统）是必需的，其余都可以选。`luban` 命令随 xlings 一起安装（同一个日期版本），在宿主上、在每个 Luban 环境里、在每台 Luban 真机上都是同一个程序。

> 设计：`.agents/docs/2026-10-09-luban-os-and-agent-private-design.md`

## 1. 常用（`luban --help`）

```bash
luban                         # 一屏概要：在哪里、有哪些环境、下一步
luban new box                 # 新建环境（默认 Luban Core）
luban new box tiny            # 指定 edition：nano / tiny / core / desktop / agent-workspace / ns:name
luban enter box               # 进入
luban run box -- make -j8     # 运行一个命令
luban ls                      # 列出环境
luban status [box]            # 这台宿主，或一个环境的状态
```

装软件仍然是 xlings：`xlings install <包>`（在环境里就装进这个环境）。

第一次在 Ubuntu 23.10+ 上新建环境时，`luban new` 会先检查宿主：AppArmor 限制了用户命名空间，需要一次系统设置（root 所有的 bwrap 和一个范围很窄的 AppArmor 规则）。它当场询问并请求 sudo 密码，之后所有用户都不再需要；也可以随时 `luban setup`。

## 2. 更多（`luban help --all`）

```bash
luban config box                      # 查看设置
luban config box proxy socks5h://127.0.0.1:7897
luban config box tz utc               # utc / proxy（代理出口）/ Asia/Tokyo
luban config box policy xim:agent-confined
luban history box                     # 代的历史
luban rollback box [--to 3]           # 回滚
luban export box box.iso              # live ISO（BIOS 和 UEFI，从内存运行）
luban export box box.img              # 驱动器镜像（GPT，BIOS 和 UEFI，ext4 根，持久）
luban export box box.qcow2            # 同上，qcow2
luban export box box.tar.zst          # rootfs（docker import / wsl --import）
luban write box.iso /dev/sdb          # 制作启动盘（会要求输入驱动器名确认）
luban write box /dev/sdb              # 直接把环境装到驱动器上
luban try box                         # 在本地虚拟机里试运行（qemu）
luban rm box                          # 删除（会先询问）
```

镜像需要内核：`xlings install linux-kernel --subos box`（edition 的 `boot.kernel` 会在提示里给出推荐的那个）。limine：`xlings install limine`。

`luban write` 只写整个驱动器，拒绝分区、已挂载或被占用的驱动器（包括系统所在的盘）。agent 模式下必须同时给出 `-y` 和 `--serial <驱动器序列号>`。

## 3. agent 私有工作区

```bash
luban new agent agent-workspace --proxy socks5h://127.0.0.1:7897
luban run agent -- claude
luban status agent
```

- 创建完成时就已经是私有的：edition 声明的策略（`agent-private`）在第一次进入之前就被选中并锁定。
- 网络只有那个代理（socks5h，域名由代理解析），代理不可用时不会改走直连。
- 一个固定的、中性的身份：主机名和 machine-id 在创建时随机一次，之后不变；时区默认跟随代理出口（经代理查询，查不到就用 UTC，绝不用宿主的）。
- 共享内核时无法隐藏的（内核版本、CPU 型号、绑定进来的宿主路径），`luban status` 如实列出。需要完全隔离时：`luban try agent --proxy socks5h://127.0.0.1:7897`——同一个环境运行在自己的内核上，网络只有代理。
- 只需要把 agent 和宿主隔开、不需要隐藏身份时：`luban config agent policy xim:agent-confined`。

## 4. 打造自己的发行版

从 `luban-nano`（只有 xlings 和 luban）出发，写一个模板，选你要的 libc、init、工具：

```json
{ "subos_kind": "rootfs", "from": "subos:luban-nano",
  "abi": "x86_64-linux-gnu",
  "packages": [ "xim:glibc@2.44.3", "xim:busybox@1.35.0" ],
  "boot": { "init": "/sbin/init", "kernel": "xim:linux-kernel@6.8.0-71" } }
```

用 `xlings subos pack` 打成 xpkg 放进自己的索引，别人就可以 `luban new box your-index:your-os`，然后导出成 ISO 或驱动器镜像。

## 5. 给 agent 用

所有命令都接受 `--json` 和 `--agent`（或 `XLINGS_AGENT_MODE=1`）：从不提问，需要回答的问题变成结构化错误（错误码、候选项和能回答它的确切参数），退出码 2。`luban <命令> --help` 的最后一行写出对应的 xlings 命令。
