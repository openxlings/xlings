# Edition

[English](../../luban/02-editions.md) | **简体中文**

edition 是环境的模板：它带来的包、创建时所用的策略、怎样启动。edition 是索引里的包（`type = "subos"`，命名空间 `subos`）。

## 官方 edition

| Edition | `luban new box …` | 增加了什么 | x86_64 | aarch64 |
|---|---|---|---|---|
| `luban-nano` | `nano` | xlings 和 luban——别的都没有，连 shell 都没有 | 是 | 是 |
| `luban-tiny` | `tiny` | BusyBox、glibc、CA 证书、BusyBox init | 是 | 是 |
| `luban-tiny-musl` | `luban-tiny-musl` | 用 musl 代替 glibc 的 tiny | 是 | 是 |
| `luban-core` | `core`（默认） | bash、fish、vim、nvim、git、gh、mcpp、ninja、coreutils、gcc、binutils、make、openssl | 是 | 是 |
| `luban-agent-workspace` | `agent-workspace` | core、一个 agent（claude）和 `agent-private` 策略 | 是 | 是 |
| `luban-desktop` | `desktop` | core、xdesk 经典桌面，以及常用工具（node、python、uv、bun、ripgrep、fd、fzf、jq、tmux、htop、claude、codex、pi、opencode）；以机器方式运行（[桌面](08-desktop.md)） | 是 | 是（labwc） |
| `luban-desktop-xdesk` | `desktop-xdesk` | 同样的环境，桌面是 xdesk 画布（供探索） | 是 | 暂无 |

每个都 `from` 它下面的那一个（`core` 来自 `tiny`，`tiny` 来自 `nano`）；为另一种架构构建的 edition 在创建任何东西之前就会被拒绝。

## 版本

- edition 按发布日期编号（`2026.10.11.2`），已发布版本的输出永不改变。
- `luban new box --edition core` 取 edition 的 `latest`；`subos:luban-core@2026.10.11.1` 取那个版本。
- 需要比你更新的客户端的 edition 会说明这一点（`min_client`）并给出升级命令，而不是半残地运行。
- 环境记得它来自哪个 edition 的哪个版本，以及那个版本带来了什么；`luban upgrade` 把它升到最新（[03](03-environments.md)）。

## 什么是锁定的

edition 的包锁定到确切版本，所以今天和下个月用同一个版本创建的环境是相同的。`agent-workspace` 的 agent 是例外：
它每周都在变，所以创建时安装当时最新的版本并记录下来，`luban upgrade` 再更新它。

## 自己的 edition

edition 是一份 manifest：

```json
{ "subos_kind": "rootfs", "from": "subos:luban-nano",
  "abi": { "kernel": "linux", "libc": "gnu" },
  "packages": [ "xim:glibc@2.44.3", "xim:busybox@1.35.0" ],
  "boot": { "init": "/sbin/init", "profile": "xim:luban-boot-generic" } }
```

| 字段 | |
|---|---|
| `subos_kind` | `rootfs`：本身是一个根的环境 |
| `from` | 它下面的 edition |
| `abi` | 需要的内核和 libc（与架构无关的 edition 不写机器） |
| `packages` | 它带来的包，锁定版本 |
| `policy` | 创建时使用的策略（`xim:agent-private@…`） |
| `boot` | 它的 init，以及它的镜像使用的启动层 |
| `min_client` | 能理解它的最旧 xlings |

根里有两个文件说明它怎么运行，都不是代码：

- `usr/share/luban/settings.json`——edition 给 `luban config` 的默认值（`mode`、`persist`、`desktop`、
  `on_close`）；用户设置的优先，并且随升级更新。desktop edition 的是 `{ "mode": "machine", "desktop": "xdesk-classic" }`。
- `share/wayland-sessions/<id>.desktop`——一个桌面会话，由某个包带来（[桌面](08-desktop.md)）。

从它创建环境、调整、然后发布：

```bash
xlings subos new mydistro --rootfs --from subos:luban-nano
xlings subos pack mydistro --as myns:mydistro@2026.10.12.1
```

之后有你的索引的人就可以 `luban new box --edition myns:mydistro` 并导出它。把 edition、策略和启动层写成配方，
见索引的 [Luban 包](https://github.com/openxlings/xim-pkgindex/blob/main/docs/zh/12-luban-packages.md)一章。
