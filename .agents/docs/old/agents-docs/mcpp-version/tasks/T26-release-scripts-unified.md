# T26 — 发布脚本三平台统一

> **Wave**: 依赖 T24
> **预估改动**: ~80 行（bash + ps1）
> **设计文档**: [../shim-unified-design.md](../shim-unified-design.md)

---

## 1. 任务概述

统一 linux_release.sh、macos_release.sh、windows_release.ps1：
1. 三平台均打包 xmake
2. xvm 配置改为复制 config/xvm 两次（config/xvm 与 subos/default/xvm）
3. 删除手写 shim 列表的 for 循环
4. 组装完成后调用 `xlings self init` 创建 shim

---

## 2. 依赖

| 依赖 | 原因 |
|------|------|
| T24 | 需要 config/xvm 模板；需要 xlings self init 能创建 shim |

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| tools/linux_release.sh | 修改：xvm 复制、删除 shim for、调用 init |
| tools/macos_release.sh | 修改：新增 xmake 下载、xvm 复制、shim 改为 8 个、调用 init |
| tools/windows_release.ps1 | 修改：新增 xmake 下载、xvm 复制、调用 init |

---

## 4. 实施步骤

### 4.1 xmake 三平台统一

| 平台 | URL | 目标 |
|------|-----|------|
| Linux | xmake-bundle-v3.0.7.linux.x86_64 | bin/xmake |
| macOS | xmake-bundle-v3.0.7.macos.arm64 | bin/xmake |
| Windows | xmake-bundle-v3.0.7.win64.exe | bin/xmake.exe |

支持 `SKIP_XMAKE_BUNDLE=1` 跳过下载。

### 4.2 xvm 配置复制

```bash
# Linux/macOS
mkdir -p "$OUT_DIR/config/xvm"
cp config/xvm/versions.xvm.yaml config/xvm/.workspace.xvm.yaml "$OUT_DIR/config/xvm/"
cp config/xvm/versions.xvm.yaml config/xvm/.workspace.xvm.yaml "$OUT_DIR/subos/default/xvm/"
```

```powershell
# Windows
New-Item -ItemType Directory -Force "$OUT_DIR\config\xvm" | Out-Null
Copy-Item config\xvm\* "$OUT_DIR\config\xvm\"
Copy-Item config\xvm\* "$OUT_DIR\subos\default\xvm\"
```

### 4.3 删除 shim for 循环

删除各脚本中 `for shim in ...` 复制 xvm-shim 的循环。

### 4.4 删除内联 xvm yaml

删除 cat/heredoc 或 @" "@ 写入 versions.xvm.yaml、.workspace.xvm.yaml 的代码。

### 4.5 调用 xlings self init

在组装完成、验证之前：

```bash
# Linux/macOS
XLINGS_HOME="$OUT_DIR" XLINGS_DATA="$OUT_DIR/data" XLINGS_SUBOS="$OUT_DIR/subos/default" \
  "$OUT_DIR/bin/xlings" self init
```

```powershell
# Windows
$env:XLINGS_HOME=$OUT_DIR
$env:XLINGS_DATA="$OUT_DIR\data"
$env:XLINGS_SUBOS="$OUT_DIR\subos\default"
& "$OUT_DIR\bin\xlings.exe" self init
```

### 4.6 验证步骤更新

验证列表中增加 subos/default/bin 下 8 个 shim 的检查（xlings, xvm, xvm-shim, xmake, xim, xinstall, xsubos, xself）。

---

## 5. 验收

```bash
# 三平台分别执行发布脚本
./tools/linux_release.sh
./tools/macos_release.sh
pwsh ./tools/windows_release.ps1

# 解压后检查
tar -xzf build/xlings-*-linux-x86_64.tar.gz
ls xlings-*/subos/default/bin/   # 8 个 shim
ls xlings-*/config/xvm/          # versions.xvm.yaml, .workspace.xvm.yaml
```
