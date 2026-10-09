# PR #650 自我 review（Luban OS、luban 工具、agent 私有工作区，2026-10-10）

- 范围：xlings PR #650（版本 2026.10.10.1）与 xim-pkgindex PR #945（重构后）。
- 依据：设计文档 `.agents/docs/2026-10-09-luban-os-and-agent-private-design.md`（含 §F 实施记录）、三平台 CI、本机 qemu 实测、`tests/requirements.toml`。
- 每条发现写明已修复还是保留，以及证据。

## 0. 结论

设计的 A（模型）、B（体验）、C（agent 私有工作区）、D（CI 与交付）都已落地。与设计不同的地方在 §F2 里逐条写明：
- 没有单独的 luban 包；
- ISO 用 initramfs 而不是 squashfs；
- 没有 `luban install`；
- 根视图的 store 仍是宿主路径；
- agent 仍以 root 运行；
- L3 用 `luban try --proxy`。

没有用文档代替实现的地方。

## 1. 自我 review 中发现并已修复的缺陷

| # | 发现 | 怎么发现的 | 修复 |
|---|---|---|---|
| S1 | 嵌套的 `xlings install` 在自己的进程组里探测终端，被 SIGTTOU 停住 30 分钟 | 用户实际试用 | 子进程组接管终端（X1），根本上去掉子进程（X2） |
| S2 | 新的 xvm 键比较把同一版本当成需要切换（"still resolves to subos:0.1.0"） | 试用输出 | `version_key_matches` |
| S3 | 根的预检查在没有 bwrap 的 root CI 里阻止了创建 | linux-root 车道 | 只阻止一次性设置能解决的情况，其余警告后继续 |
| S4 | `sigemptyset` 在 macOS 上是宏 | macOS 车道 | 不加 `::` |
| S5 | 终端测试在 Windows 上包含 POSIX 头 | Windows 车道 | 守卫 + 跳过 |
| S6 | `luban try` 检查 KVM 时用 `ofstream` 打开 `/dev/kvm`，会创建这个文件 | 本机运行，qemu 报错 | 只接受字符设备，从不创建 |
| S7 | `guestfwd=...-tcp:` 在 qemu 启动时就连接代理：代理不可用时机器起不来，而且只能有一个连接 | 镜像测试 | `luban __pipe`，每个连接一条，经 qemu 的 `cmd:` |
| S8 | live 根和两份 luban 放在内存里，1 GB 时 initramfs 解压失败 | 镜像测试 | 镜像里 luban-init 作为指向 luban 的链接；测试和 `try` 默认用 2 GB |
| S9 | 嵌套解析镜像里的链接只跟随最后一个路径分量 | 导出 ISO 时找不到内核 | 逐分量解析（与内核在 chroot 里的做法相同） |
| S10 | GPT 盘上 limine 的 BIOS 阶段需要 BIOS boot 分区 | `limine bios-install` 失败 | 布局加入 1 MiB 的 BIOS boot 分区 |
| S11 | `--tz` 加到了 `subos use` 而不是 `subos config` | linux-unit 车道 | 移到 config |
| S12 | 测试从第一行读主机名，而第一行是 "entering" 提示 | linux-unit 车道 | 按输出中的标记匹配 |
| S13 | Windows 和 macOS 上 `luban new` 会被拒绝为"不是 Linux" | 跨平台一致性检查 | Luban edition 默认使用该平台的承载（wsl2、vz） |
| S14 | agent-private 没有代理时，修复提示写的是字段名 | 拒绝路径检查 | 给出确切命令 `xlings subos config <n> --proxy ...` |
| S15 | 推送一直静默失败（分支没有 upstream，`git push -q`），CI 测的是旧提交 | CI 结果对不上本地 | 设置 upstream，推送时检查输出 |
| S16 | 别名经 `std::system`，即经 `/bin/sh`，在 nano 根里不可用 | 按 A4 的要求审查 | 普通单词直接 exec |

## 2. 分角度检查

**架构。**
- luban 只链接 `luban/` 和 `modules/`，由 `lint_layer_deps` 保证；SubOS 操作经 xlings 完成，只有一份实现。
- 命令规格抽成 `modules/cli`，xlings 和 luban 共用。
- 镜像格式（GPT、FAT）在 `luban.image`；归档格式在 `xim::write_archive`。
- 外部工具（qemu、xorriso、limine、curl、qemu-img）都经工具表解析，提权只经唯一的入口。

**稳定性。**
- 宿主预检查在下载之前。
- persona 文件读不出来时报错，而不是换一个身份。
- 写驱动器前检查分区、挂载和占用。
- 代理挂掉时没有直连回退（验收脚本覆盖）。

**简洁。**
- 用户可见的名词只有一个：环境。
- luban 的命令分三级展开，`--help` 只列 5 个常用命令。
- 去掉了 `agent-workspace-private` 那个 shell 入口：模板声明策略，`new --proxy` 应用。

**用户体验。**
- 一个计划，一个"created"，不卡住，模板对用户不可见。
- 拒绝时第一次就给出根本原因和一个修复办法。
- 拼错的命令有建议。
- agent 模式从不提问（退出码 2 和确切命令）。
- 写驱动器时，人要输入驱动器名，agent 要给出序列号。

**兼容性与无感升级。**
- `instance.json` 只追加字段（`root_abi`、`boot`）。
- 已发布的 edition `0.1.0` 不变。
- `--disk` 的含义不变，新增 `--drive`。
- 旧策略文件里显式写的 `"tz":"UTC"` 继续生效。
- 新策略文件的下限是 2026.10.10.1。降级后旧客户端会拒绝这些文件，这是有意的，提示是升级。
- 有意的可见变化：中性身份的主机名从实例名变成 persona（实例名可能就是用户名）。写进发布说明。
- `self update` 会一起更新 luban 和 luban-init。

**跨平台。**
- luban 在三个平台都发布。
- 在 Windows 和 macOS 上，Luban edition 走 wsl2 和 vz 承载，用法相同。vz 助手尚未发布时，拒绝信息如实说明。
- 镜像与驱动器只在 Linux 上制作；`luban write` 在其他平台给出路线。

**一致性。**
- luban 与 xlings、edition 与策略都用日期版本，单元测试保证 luban 和 xlings 的版本相同。
- `luban status` 与 `xlings subos status` 用同一份 JSON。

## 3. 保留的已知限制

见设计文档 §F2：
- 根视图的 store 仍是宿主路径（中性路径的镜像走 `--domain /xlings`）；
- 根视图里的 agent 以 root（用户命名空间内）运行；
- 没有 `luban install`；
- L3 用的是 generic 内核；
- recipe hook 对 sh 的依赖需要 edition 自己声明。
