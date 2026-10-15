# 文件系统能力：按实测选择策略

日期：2026-10-15　状态：已实现（2026.10.15.4）　缘起：mcpp-community/mcpp#809（home 在 NFSv3 上，任何包都无法配置）

## 1. 问题的类别

xlings 写 home 时依赖几个文件系统操作：

- 不覆盖地发布一个条目：materializer、root 生成、payload、导出都这样写；
- 原子交换两个名字；
- 硬链接；
- 符号链接：shim 和 SubOS 的链接都是它；
- 大小写敏感。

这些操作是否可用，由 home 所在的**文件系统**决定，不由平台决定。同一台 Linux 上，ext4 有 `renameat2(RENAME_NOREPLACE)`，NFSv3 没有，它返回 `EINVAL`；FAT 连符号链接都没有。macOS 的 SMB 或 NFS 挂载对 `renamex_np(RENAME_EXCL)` 返回 `ENOTSUP`。Windows 的 `MoveFileExW`（不带 REPLACE）在所有文件系统上都不覆盖，但没有原子交换。

修复之前，每个原语自己处理失败：

- `rename_no_replace` 失败就失败，NFS 上因此任何东西都发布不了；
- `exchange_paths` 遇到 `EINVAL` 就退回；
- `atomic_swap_paths` 失败就退回手工交换。

用 errno 猜测有两个问题。第一，`EINVAL` 也可能是别的错误（例如把目录改名进它自己的子目录），猜错就会掩盖真正的错误。第二，同一个问题每个调用点各修一次。

## 2. 设计

一处实测，各处按能力选择。

- **`platform::fs_caps(dir)`**（`modules/platform/src/fs/fs_caps.cppm`）：每个文件系统、每个进程只测一次，按设备号缓存（Windows 按卷序列号）。做法是在 `dir` 里建一个临时目录，真的去做一次：不覆盖的改名、交换、硬链接、符号链接、只有大小写不同的名字，做完删除。文件系统类型也一并读出：Linux 用 statfs 的 magic，macOS 用 `f_fstypename`，Windows 用卷名。目录不可写时，结果是平台的原生答案（`measured = false`），并且不缓存。
- **`platform::publish_no_replace(from, to, caps)`**：唯一的发布策略表。
  1. 有原生不覆盖改名：直接用；
  2. 没有，但有硬链接：文件和符号链接以硬链接发布（名字已存在时，`link(2)` 同样原子地失败；在 Linux 上它链接的是链接本身），然后删除临时名；
  3. 都没有，或者是目录：先确认目标名不存在，再改名过去。唯一的窗口是这期间有人在同一个名字上建了一个空目录；而这个名字属于调用方，它持有状态锁。

  `rename_no_replace(from, to)` 就是 `publish_no_replace(from, to, fs_caps(to 的目录))`。
- **`exchange_paths`**：先问 `fs_caps`，没有交换能力就返回 false，交给调用方不覆盖的受控移动；只有真正的错误才是错误。
- **home 的前提**：home 需要符号链接（Windows 用 junction 和复制，除外）。`ensure_home_layout` 在建 home 时实测，没有就拒绝，写明文件系统和怎么做，避免一个只能用一半的 home。
- **说出来**：`xlings self doctor` 在 home 的文件系统不是全部原生时，说明它是什么、xlings 在那里怎样写。

### 平台差异只在一处

原生调用放在 `os/fs_linux.cpp`、`os/fs_macos.cpp`、`os/fs_windows.cpp` 里，每个系统一份，函数名相同（`platform_impl::rename_no_replace_native`、`exchange_native`、`unsupported`、`device_of`、`filesystem_type`）。`fs_caps.cpp`、`asset_paths.cpp` 和 `platform.cpp` 的通用代码里没有平台宏。

## 3. 矩阵

| 文件系统 | 不覆盖改名 | 交换 | 硬链接 | 符号链接 | xlings 的做法 |
|---|---|---|---|---|---|
| ext4 / XFS / Btrfs / tmpfs / overlay / ZFS | 有 | 有 | 有 | 有 | 原生 |
| APFS | 有 | 有 | 有 | 有（默认大小写不敏感） | 原生 |
| NTFS / ReFS（Windows） | 有（MoveFileEx） | 无 | 有 | 需要权限 | 原生；交换走受控移动 |
| NFSv3 | 无（EINVAL） | 无 | 有 | 有 | 硬链接发布；目录改名到空闲的名字 |
| NFSv4 / SMB / CIFS / FUSE | 视实现 | 视实现 | 视实现 | 视实现 | 按实测 |
| 9p / virtiofs / WSL drvfs | 视实现 | 视实现 | 视实现 | 视实现 | 按实测 |
| FAT / exFAT | 视实现 | 无 | 无 | 无 | 不能作为 home，建立时拒绝 |

## 4. 验证

- **单元**（任何文件系统上）：`AtomicPublication.EveryStrategyPublishesAndNoneReplaces` 通过给定能力，逐一驱动三种策略：文件、链接、目录、已占用的名字、空目录。`AtomicPublication.AFilesystemIsMeasuredWhereItIs` 实测本机的文件系统，并检查临时目录已清理。
- **真实文件系统**（`xlings-ci-linux.yml` 的 filesystems 车道）：
  - 在 runner 上导出 NFSv3 并挂载（`vers=3,nolock,local_lock=all`）。`nfs_home_test.sh` 先确认那里 `renameat2(RENAME_NOREPLACE)` 确实返回 EINVAL，再把 home 放上去：安装 linux-headers 和 glibc（就是 #809 里失败的两个），检查 `usr/include/asm`、`lib/Scrt1.o` 已就位、没有残留的临时条目、删除后条目消失，以及 `self doctor` 的说明。
  - 用 loop 挂载一个 vfat 镜像。`fat_home_test.sh` 检查在那里建 home 会被拒绝，并写明文件系统和怎么做。
- 需求编号：`ATOMIC-PUBLICATION`。

## 5. 以后

- SMB 和 NFSv4 车道：需要 Samba 或 NFSv4 服务端，按需加。
- macOS 上的 SMB 或 NFS home：用的是同一张策略表，由实测决定，不需要再写代码。
