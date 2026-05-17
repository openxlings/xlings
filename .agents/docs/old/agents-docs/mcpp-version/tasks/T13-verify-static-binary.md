# T13 — 验证: musl 静态二进制构建与回归测试

> **Wave**: 2（依赖 T11 + T12 完成）
> **预估改动**: 无代码改动，纯验证
> **设计文档**: [../release-static-build.md](../release-static-build.md)

---

## 1. 任务概述

在 T11（xmake.lua 修改）和 T12（CI 切换 musl-gcc）合并后，执行本地构建和完整验证，确认静态二进制符合发布要求。

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| T11 | xmake.lua 已改为 `-static` |
| T12 | CI 已切换到 musl-gcc@15.1.0 |

---

## 3. 验证清单

### 3.1 本地构建验证

```bash
# 安装 musl-gcc (如未安装)
xlings install musl-gcc@15.1.0 -y

# 配置 xmake
MUSL_SDK=/home/xlings/.xlings_data/xim/xpkgs/musl-gcc/15.1.0
xmake f -p linux -m release --sdk=$MUSL_SDK --cross=x86_64-linux-musl-

# 构建
xmake clean
xmake build xlings
# 期望: [100%]: build ok
```

### 3.2 二进制类型检查

```bash
BIN=build/linux/x86_64/release/xlings

file $BIN
# 期望: ELF 64-bit LSB executable, x86-64, ... statically linked, stripped
# 关键词: "statically linked"

ldd $BIN
# 期望: not a dynamic executable
```

### 3.3 零 GLIBC 符号

```bash
readelf --dyn-syms $BIN | grep GLIBC
# 期望: 无任何输出

objdump -T $BIN | grep GLIBC
# 期望: 无任何输出
```

### 3.4 无 RUNPATH / RPATH

```bash
readelf -d $BIN | grep -E "(RUNPATH|RPATH)"
# 期望: 无任何输出
```

### 3.5 无动态依赖

```bash
readelf -d $BIN | grep NEEDED
# 期望: 无任何输出（全静态无 NEEDED 条目）
```

### 3.6 功能回归测试

```bash
# 设置环境（假设通过 linux_release.sh 构建了完整包）
SKIP_NETWORK_VERIFY=1 ./tools/linux_release.sh

PKG_DIR=$(ls -d build/xlings-*-linux-x86_64 | head -1)
export XLINGS_HOME=$PKG_DIR
export XLINGS_DATA=$PKG_DIR/data
export XLINGS_SUBOS=$PKG_DIR/subos/current
export PATH="$PKG_DIR/subos/current/bin:$PKG_DIR/bin:$PKG_DIR/tools/xmake/bin:$PATH"

# 基础命令
xlings -h                    # 期望: 正常输出帮助信息，包含 subos 命令
xlings config                # 期望: 输出 XLINGS_HOME/DATA/SUBOS 路径
xvm --version                # 期望: xvm 0.0.5
xlings subos list            # 期望: 显示 default 环境

# subos 操作
xlings subos new test-env
xlings subos use test-env
xlings subos list            # 期望: test-env 标记为当前
xlings subos use default
xlings subos remove test-env
xlings subos list            # 期望: 只剩 default
```

### 3.7 二进制大小对比

```bash
ls -lh $BIN
# 记录大小，与改动前对比

# 参考基线:
#   改动前 (gcc + glibc 动态): ~1.8 MB
#   改动后 (musl-gcc + static): 预期 ~2-4 MB
```

---

## 4. CI 验证

### 4.1 确认 CI 绿色

推送 T11 + T12 改动后，检查 GitHub Actions：

- `xlings-ci-linux` workflow 全部步骤通过
- Phase 3 测试（basic commands、subos 创建/切换/隔离/清理）通过

### 4.2 下载 CI 产物检查

从 CI 的 Artifacts 下载 `xlings-linux-x86_64`，在本地解压验证：

```bash
tar -xzf release.tar.gz
cd xlings-*-linux-x86_64

file bin/xlings
# 期望: statically linked

readelf --dyn-syms bin/xlings | grep GLIBC
# 期望: 无输出

readelf -d bin/xlings | grep RUNPATH
# 期望: 无输出
```

### 4.3 Release workflow 验证

手动触发 Release workflow，确认：
- Linux job 成功
- macOS / Windows job 不受影响
- Release 页面产物可下载

---

## 5. 验收标准汇总

| 检查项 | 期望结果 | 通过 |
|--------|---------|------|
| xmake build 成功 | `[100%]: build ok` | [ ] |
| `file` 输出 statically linked | 含 "statically linked" | [ ] |
| `ldd` 报告 | "not a dynamic executable" | [ ] |
| GLIBC 符号数 | 0 | [ ] |
| RUNPATH | 无 | [ ] |
| NEEDED 动态库数 | 0 | [ ] |
| `xlings -h` | 正常输出 | [ ] |
| `xlings config` | 正确路径 | [ ] |
| `xvm --version` | 版本号 | [ ] |
| `xlings subos list` | 显示 default | [ ] |
| subos 创建/切换/删除 | 全部成功 | [ ] |
| CI xlings-ci-linux | 绿色 | [ ] |
| CI Phase 3 测试 | 全部通过 | [ ] |
| Release workflow Linux job | 成功 | [ ] |
