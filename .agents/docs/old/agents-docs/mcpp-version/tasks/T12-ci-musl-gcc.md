# T12 — CI/Release: 切换 Linux 构建到 musl-gcc@15.1.0

> **Wave**: 1（无前置依赖，可与 T11 并行）
> **预估改动**: ~30 行 YAML
> **设计文档**: [../release-static-build.md](../release-static-build.md)

---

## 1. 任务概述

更新 Linux CI 和 Release workflow，将编译工具链从 `gcc@15.1`（glibc）切换到 `musl-gcc@15.1.0`（musl），配合 T11 的 xmake.lua 改动，生成全静态二进制。

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| 无 | CI YAML 修改独立于 xmake.lua（T11 改动在构建时生效） |

注意：T11 和 T12 虽然逻辑上配合使用，但文件无冲突可并行实施。合并后首次 CI 运行时两者同时生效。

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `.github/workflows/xlings-ci-linux.yml` | 修改（替换 gcc 为 musl-gcc） |
| `.github/workflows/release.yml` | 修改（替换 Linux job 中的 gcc 为 musl-gcc） |

---

## 4. 实施步骤

### 4.1 更新 `xlings-ci-linux.yml`

#### 4.1.1 替换 "Install GCC 15.1" 步骤（第 41-49 行）

```yaml
# 当前:
      - name: Install GCC 15.1 with Xlings
        run: |
          export PATH=/home/xlings/.xlings_data/bin:$PATH
          xlings install gcc@15.1 -y
          GCC_BIN=$(which gcc)
          GCC_SDK=$(dirname "$(dirname "$GCC_BIN")")
          echo "GCC_SDK=$GCC_SDK" >> "$GITHUB_ENV"
          echo "PATH=$GCC_SDK/bin:$PATH" >> "$GITHUB_ENV"
          gcc --version

# 改为:
      - name: Install musl-gcc 15.1 with Xlings
        run: |
          export PATH=/home/xlings/.xlings_data/bin:$PATH
          xlings install musl-gcc@15.1.0 -y
          MUSL_SDK=/home/xlings/.xlings_data/xim/xpkgs/musl-gcc/15.1.0
          echo "MUSL_SDK=$MUSL_SDK" >> "$GITHUB_ENV"
          echo "PATH=$MUSL_SDK/bin:$PATH" >> "$GITHUB_ENV"
          x86_64-linux-musl-g++ --version
```

#### 4.1.2 替换 "Configure xmake" 步骤（第 64-66 行）

```yaml
# 当前:
      - name: Configure xmake
        run: |
          xmake f -p linux -m release --sdk="$GCC_SDK"

# 改为:
      - name: Configure xmake
        run: |
          xmake f -p linux -m release --sdk="$MUSL_SDK" --cross=x86_64-linux-musl-
```

#### 4.1.3 保留 Rust musl target 步骤

"Add Rust musl target" 步骤（第 61-62 行）保持不变，xvm 的 `release-build.sh` 仍需要它。

### 4.2 更新 `release.yml` Linux job

#### 4.2.1 替换 "Install GCC 15.1" 步骤（第 40-49 行）

```yaml
# 当前:
      - name: Install GCC 15.1 with Xlings
        run: |
          export PATH=/home/xlings/.xlings_data/bin:$PATH
          xlings install gcc@15.1 -y
          GCC_BIN=$(which gcc)
          GCC_SDK=$(dirname "$(dirname "$GCC_BIN")")
          echo "GCC_SDK=$GCC_SDK" >> "$GITHUB_ENV"
          echo "PATH=$GCC_SDK/bin:$PATH" >> "$GITHUB_ENV"
          gcc --version

# 改为:
      - name: Install musl-gcc 15.1 with Xlings
        run: |
          export PATH=/home/xlings/.xlings_data/bin:$PATH
          xlings install musl-gcc@15.1.0 -y
          MUSL_SDK=/home/xlings/.xlings_data/xim/xpkgs/musl-gcc/15.1.0
          echo "MUSL_SDK=$MUSL_SDK" >> "$GITHUB_ENV"
          echo "PATH=$MUSL_SDK/bin:$PATH" >> "$GITHUB_ENV"
          x86_64-linux-musl-g++ --version
```

#### 4.2.2 替换 "Configure xmake" 步骤（第 63-64 行）

```yaml
# 当前:
      - name: Configure xmake
        run: xmake f -p linux -m release --sdk="$GCC_SDK"

# 改为:
      - name: Configure xmake
        run: |
          xmake f -p linux -m release --sdk="$MUSL_SDK" --cross=x86_64-linux-musl-
```

#### 4.2.3 保留 Rust musl target 步骤

与 CI 相同，"Add Rust musl target" 保持不变。

---

## 5. 变更总结

| 项目 | 原来 | 改为 |
|------|------|------|
| 安装命令 | `xlings install gcc@15.1 -y` | `xlings install musl-gcc@15.1.0 -y` |
| SDK 环境变量 | `GCC_SDK` (自动探测) | `MUSL_SDK` (固定路径) |
| SDK 路径 | `$(dirname $(dirname $(which gcc)))` | `/home/xlings/.xlings_data/xim/xpkgs/musl-gcc/15.1.0` |
| xmake 配置 | `--sdk="$GCC_SDK"` | `--sdk="$MUSL_SDK" --cross=x86_64-linux-musl-` |
| 版本验证 | `gcc --version` | `x86_64-linux-musl-g++ --version` |

`--cross=x86_64-linux-musl-` 告诉 xmake 使用 `x86_64-linux-musl-gcc` / `x86_64-linux-musl-g++` 作为编译器前缀，而非系统默认的 `gcc` / `g++`。

---

## 6. 验收标准

### 6.1 CI 构建成功

GitHub Actions `xlings-ci-linux` workflow 显示绿色通过。

### 6.2 Release workflow 构建成功

手动触发 `Release` workflow，Linux job 成功完成并上传产物。

### 6.3 产物验证

CI 产生的 `xlings-*-linux-x86_64.tar.gz` 解压后：

```bash
file bin/xlings
# 期望包含: statically linked

readelf --dyn-syms bin/xlings | grep GLIBC
# 期望: 无输出（零 GLIBC 符号）
```

### 6.4 功能测试通过

CI 中现有的 Phase 3 测试（basic commands、subos 创建/切换/隔离/清理）全部通过。
