# 实现计划：Luban 进入之后、Luban OS 应用、mcpp 对 xlings 的依赖

日期：2026-10-16　状态：执行中
依据：`2026-10-16-luban-inside-experience-and-ecosystem-design.md`（下称 **L**）与
`mcpp-community/mcpp:.agents/docs/2026-10-16-xlings-provider-port-and-shared-store-design.md`（下称 **M**）。

每个仓库一个 PR；版本：xlings 2026.10.16.1、mcpp 2026.10.16.1；xim-pkgindex 以 edition 新版本承载。

## 1. 依赖关系

```
xim-pkgindex PR (P)  ──┐  terminfo、adwaita-cursors、luban-fonts、edition 新版本（min_client = xlings 2026.10.16.1）
                       │
xlings PR (X)  ────────┼─▶ xlings 2026.10.16.1 发布 ──▶ P 合并（edition 的 latest 指向新版本）
  ├ X-A 终端与 broker ─┤
  ├ X-B 路径与 share ──┤
  ├ X-C 系统内的 index、用户、当前系统、主机名
  ├ X-D 自检
  ├ X-E mcpp 需要的能力（M 的 X1–X6）──▶ mcpp PR (C) 钉 xlings 2026.10.16.1 ──▶ mcpp 2026.10.16.1 发布
  ├ X-F aarch64（#657）
  ├ X-G #652、#672
  ├ X-H Luban OS 应用
  └ X-I 测试矩阵与文档
```

P 中的数据包（terminfo、光标、字体）与 X 并行准备；P 中的 edition 新版本声明 `min_client`，在 X 发布前不会被旧客户端选中。
C 依赖 X-E 的接口与 X 的发布。

## 2. 任务

| 编号 | 仓库 | 任务 | 依据 | 验证 |
|---|---|---|---|---|
| X-A1 | X | `platform::terminal_relay`；会话主命令即第一个加入者；加入会话改用它 | L §4 | 单元：raw/恢复、窗口大小、断开结束、字节透明；e2e：pty 中 `jobs`、fish |
| X-A2 | X | broker：不加 `-y`、不设非交互、监视客户端、走中继 | L §4 | e2e：install 确认与 `n`；下载中 Ctrl-C |
| X-A3 | X | 域内交互命令与 carrier guest 走中继 | L §4 | e2e：域内 upgrade 在终端 |
| X-A4 | X | `domain_producer::run_relayed`；创建也转发事件 | L §5 | e2e：`luban new --json` 有 progress |
| X-B1 | X | `/usr` 挂载点；generation 目录条目 `mounts.tsv`；换代 `MS_MOVE` | L §6.2 | 单元：generation 结构；e2e：`cd /usr/bin; pwd -P` |
| X-B2 | X | session-init 能力保留与丢弃 | L §6.3 | e2e：用户进程 CapEff 为 0 |
| X-B3 | X | payload `share` 投影；`XDG_DATA_DIRS` | L §6.4 | 单元：合并与冲突；e2e：desktop 有 applications |
| X-C1 | X | `ensure_catalog`；建系统带入 index 快照 | L §7 | e2e：系统内 list/info/search |
| X-C2 | X | 系统内普通用户的用户层 home | L §8 | e2e：普通用户 install 无需 root |
| X-C3 | X | `/etc/xlings/root.json` 决定当前系统；主机名同一规则 | L §9 | e2e：machine 中 vim 可用、主机名 |
| X-C4 | X | 同名创建报"已存在"；edition 场景按宿主分支断言；`bare.sh` 的 apk 与 `--allowerasing` | L §1 U17–U19 | host-matrix |
| X-D1 | X | edition 与包的 `checks`/`check` 读取；`luban status --check`；建成、升级、进入、启动四个时机 | L §11 | 单元与 e2e |
| X-E1 | X | `index_repos` 的 `dir`、`store`；`sync_index` 能力带 `dest` | M §13 X1、X2 | 单元与契约 |
| X-E2 | X | home 的只读上游：身份与完整性、闭包、链接条目、借用方配置、只删链接、悬空自愈 | M X3 | 单元与 e2e |
| X-E3 | X | 完整性与查询能力（`complete`、`origin`） | M X4 | 单元 |
| X-E4 | X | 握手（版本、协议、能力、layout） | M X5 | 单元 |
| X-E5 | X | Windows 长路径：manifest、可增长缓冲、如实报告 | M X6 | Windows CI |
| X-F1 | X | 工具表、每架构一张表、aarch64 的 `--drive` 与 `try`、ISO 的 UEFI El Torito | L §14 | 镜像 e2e（x86_64 BIOS+UEFI、aarch64 UEFI） |
| X-G1 | X | #652 原因进入 `InstallStatus.message` | L §15 | 单元 |
| X-G2 | X | #672 WSL 导入检查 | L §15 | Windows CI |
| X-H1 | X | 主循环"有事就连续画"；`ui::px` | L §13.3 | LA：点击后下一帧；多缩放 |
| X-H2 | X | 终端：一个有效字号、默认 16、缩放快捷键、整数网格、宽字符、粗体、程序化框线 | L §13.7 | LA：多缩放字形与行距 |
| X-H3 | X | 像素控件库与资产；首页、新建、进度、运行命令、管理、设置、关闭对话框 | L §13.2–§13.6、§13.10、§13.11、§13.13 | LA：结构断言与基准图 |
| X-H4 | X | 托盘：Windows/macOS 用 SDL；Linux 自实现 StatusNotifierItem 与 DBusMenu；通知 | L §13.12 | 单元（DBus 编解码）与 CI 抓屏 |
| X-H5 | X | 窗口契约：`Cursor` 接口、文件拖放（virtiofs）| L §13.9 | e2e |
| X-I1 | X | `tests/host-matrix/pty.py`、`inside.sh`、腿扩展、需求登记 | L §12 | CI |
| X-I2 | X | 文档与规范、CHANGELOG、版本号 | — | lint |
| P1 | P | `terminfo`、`adwaita-cursors`、`luban-fonts` 包（资源经 res-build 发布到 xlings-res 与 GitCode） | L §10 | 包测试 |
| P2 | P | edition 新版本：terminfo、光标、`login_shell`、`checks`、mcpp 配置（写明目标三元组） | L §10 | 包测试 |
| P3 | P | `pkgs/luban/` 迁移；`version-bump.yml` 与测试路径 | L §16 | CI |
| P4 | P | aarch64 musl 与 tiny-musl | L §14 | res-build |
| C1 | C | `mcpp.provider` 端口、假实现、适配器迁入、lint | M §4 | 单元、契约 |
| C2 | C | 调用方迁移、删除布局知识 | M §1.3 | 构建与全部测试 |
| C3 | C | index 与 store 位置、暂存交换、存量迁移 | M §5–§6 | e2e 迁移 |
| C4 | C | payload 不可变（删除 W1–W6）、运行库钉版本 | M §7 | e2e 自包含 |
| C5 | C | 上游复用、信任边界、dedupe、系统级安装 | M §8–§9 | e2e 复用与系统级 |
| C6 | C | 文件系统能力（#809 同类）、握手、长路径 | M §10–§12 | NFS、Windows e2e |
| C7 | C | 文档、规范、CHANGELOG、版本号 | M §16 | lint |

## 3. 顺序

1. X-G1、X-C1、X-C3、X-C4：小而独立，先落地并在 CI 上建立基线。
2. X-A1–X-A4：终端与转发（影响 e2e 的写法，先做）。
3. X-B1–X-B3：路径与 share。
4. X-C2、X-D1。
5. X-E1–X-E5（与 C1、C2 交替：端口的形状以 X-E 的接口为准）。
6. X-F1、X-G2。
7. X-H1–X-H5。
8. X-I1、X-I2；P1–P4 并行。
9. X 的 CI 全绿 → 生态自审 → 合并与发布 → P 合并 → C 的 CI 全绿 → 合并与发布 → 真实环境验证。
