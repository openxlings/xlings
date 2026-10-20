# Edition 与发行版

[English](../../luban/02-editions.md) | **简体中文**

Luban 系统由 index 里的一个**系统包**创建：根的模板（带来的包、创建时采用的策略、怎样启动、预设什么）。Luban 自己的系统包是
**edition**；其他人的是基于 Luban 的发行版。桌面和其他组件是装进系统的包。一切都是 xpkg（系统包 `type = "subos"`）。

## 官方 edition

Luban 的 edition 位于官方 index 的 `luban:` 命名空间；luban 中的短名就是这里的名字（`--edition core` 即 `luban:core`）。

| Edition | 加入的内容 | 预设 | x86_64 | aarch64 |
|---|---|---|---|---|
| `nano` | 只有 xlings 与 luban，连 shell 都没有 | 控制台 | 是 | 是 |
| `tiny` | BusyBox、glibc、CA 证书、BusyBox init | 控制台、`/bin/sh` | 是 | 是 |
| `tiny-musl` | 以 musl 代替 glibc 的 tiny | 控制台、`/bin/sh` | 是 | 是 |
| `core`（默认） | bash、fish、vim、nvim、git、gh、mcpp、ninja、coreutils、gcc、binutils、make、openssl | 控制台、fish | 是 | 是 |
| `agent-workspace` | core、开发与 agent 工具集、`agent-private` 策略 | 控制台、fish | 是 | 是 |
| `desktop` | core、xdesk 桌面（aarch64 上为 labwc）、foot、Xwayland、命令行 / 开发 / agent 工具集 | 机器、桌面 `xdesk-classic`、fish | 是 | 是 |

每个都 `from` 它下面的那个（`core` 基于 `tiny`，`tiny` 基于 `nano`）；为其他架构构建的 edition 在创建前就被拒绝。
桌面不是 edition：edition 预设一个，`--desktop` 选另一个（[桌面](08-desktop.md)）。

2026.10.18.1 之前发布的 edition（`subos:luban-core`、`subos:luban-desktop-xdesk` 等）冻结：保留、照常解析、不再更新。
每个 `luban:` edition 声明它接替哪个，`luban upgrade` 把用旧 edition 创建的系统移到接替者，看到的内容不变
（desktop-xdesk 的系统启动时在 WuXian 视图里）。

## 任何 index 的系统：`--xpkg`

```bash
luban new box                                   # luban:core
luban new box --edition desktop --desktop lxqt  # Luban's desktop edition, with LXQt
luban new box --xpkg myindex:myos               # a distribution from an index you added
luban new box --xpkg myindex:myos --xpkg luban:kde --xpkg xim:htop   # and components into it
```

创建之前，luban 先读系统的链（`xlings subos new --dry-run`）：它是什么、基于什么、预设什么、需要什么。
来自官方以外 index 的系统或组件会标明来源，并经同意一次（`--yes` 即同意；agent 必须传它）；同意按包与版本记住。

```
 MyOS 1.0.0                                   from myindex
 Based on     Luban core 2026.10.18.1         official
 It will be   sandbox · login shell /usr/bin/fish · 12 packages
```

链上没有任何官方 edition 的系统被拒绝（"不是基于 Luban 的"）；需要本 luban 不具备的能力的系统会说明缺什么
（下文的 `luban.requires`），而不是半途失败。

## 版本

- edition 以发布日期为版本（`2026.10.18.1`），已发布版本的输出永不改变。
- `luban new box --edition core` 取 edition 的 `latest`；`--xpkg luban:core@2026.10.18.1` 取该版本。
- 需要更新客户端的 edition 会说明（`min_client`）并给出升级方式，而不是半途失败；不指定版本时取本客户端能用的最新版本，
  并说明跳过了哪个。
- 系统记得自己由什么创建——链上的每一层及各自的 index——`luban upgrade` 把它移到最新（[03](03-environments.md)）。

## 锁定什么

edition 的包锁定到确切版本，所以今天和下个月用同一版本创建的系统是一样的。agent 工具集里的 agent 例外：它们每周都在变，
创建时安装当时的最新版并记录下来，`luban upgrade` 再移动它们。

## 你自己的发行版

系统包是一个清单；基于 Luban 的发行版 `from` 一个官方 edition：

```json
{ "subos_kind": "rootfs", "from": "luban:core@2026.10.18.1",
  "abi": { "kernel": "linux", "libc": "gnu" },
  "packages": [ "xim:htop@3.5.3", "luban:lxqt@2.4.0-r2" ],
  "luban": { "os": { "name": "MyOS", "version": "1.0.0" },
             "settings": { "desktop": "lxqt", "enter": "desktop" },
             "requires": [ "root-layout>=2" ] } }
```

| 键 | |
|---|---|
| `subos_kind` | `rootfs`：作为根的系统 |
| `from` | 它下面的系统（基于 Luban 的发行版就是一个官方 edition） |
| `abi` | 需要的内核与 libc（与架构无关的系统不写具体机器） |
| `packages` | 带来的包，锁定版本 |
| `policy` | 创建时采用的策略（`xim:agent-private@…`） |
| `boot` | 它的 init，以及镜像使用的启动配置 |
| `min_client` | 能理解它的最旧 xlings |
| `replaces` | 它接替的包（只在同一 index 内认可） |
| `luban.os` | 它怎样称呼自己：`luban status` 显示 "MyOS 1.0.0（Luban core）"，`/etc/os-release` 是它自己的 |
| `luban.settings` | 它为 `luban config` 预设的值（`mode`、`enter`、`desktop`、`login_shell`、`persist`、`on_close`）：低于用户的设置 |
| `luban.requires` | 它对读取或运行它的一方要求的能力，按名字与级别（[opencaps](https://github.com/openxlings/xlings/tree/main/luban/specs/opencaps)）：不认识的名字即不满足 |

根里的 `usr/share/luban/settings.json` 为旧 luban 保存同样的预设，`share/wayland-sessions/<id>.desktop` 是它的包带来的桌面会话
（[桌面](08-desktop.md)）。

用它创建系统、调整，然后发布到你的 index：

```bash
xlings subos new mydistro --rootfs --from luban:core
xlings subos pack mydistro --as myns:mydistro@2026.10.18.1
```

之后任何添加了你的 index 的人都可以 `luban new box --xpkg myns:mydistro`。官方 index 保留 `luban`、`xim`、`subos` 与 `config`：
其他 index 在这些命名空间里的包被拒绝。index 的 [Luban 的包](https://github.com/openxlings/xim-pkgindex/blob/main/docs/zh/12-luban-packages.md)
一章把 edition、桌面组件、策略与启动配置作为配方说明。
