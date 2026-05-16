# T09 — CI/CD: 补齐 macOS arm64 + Windows x86_64 多平台

> **Wave**: 5（依赖全部其他任务完成）
> **预估改动**: ~200 行 YAML，2 个新文件 + 1 个文件更新

---

## 1. 任务概述

在现有 Linux CI（`.github/workflows/xlings-ci-linux.yml`）的基础上，新增：

- `.github/workflows/xlings-ci-macos.yml` — macOS arm64（Apple M 系列）
- `.github/workflows/xlings-ci-windows.yml` — Windows x86_64（MSVC / clang-cl）

并更新 Linux CI 中与新目录结构相关的路径（`XLINGS_DATA` → `envs/default`）。

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| T01-T08 全部 | CI 需要代码功能完整（三平台编译 + 多环境路径） |

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `.github/workflows/xlings-ci-linux.yml` | 更新路径（旧 `XLINGS_DATA` → 新 envs/default） |
| `.github/workflows/xlings-ci-macos.yml` | 新建 |
| `.github/workflows/xlings-ci-windows.yml` | 新建 |

---

## 4. 实施步骤

### 4.1 更新 Linux CI 路径

当前 Linux CI 第 97-98 行使用旧路径（T09 执行时需确认是否已因 T02 更新而自动适配）：

```yaml
# 旧写法（如存在则替换）:
export XLINGS_DATA="$XLINGS_HOME/data"
export PATH="$XLINGS_HOME/bin:$XLINGS_DATA/bin:$PATH"

# 新写法（多环境结构）:
export XLINGS_DATA="$XLINGS_HOME/envs/default"
export PATH="$XLINGS_HOME/bin:$XLINGS_DATA/bin:$PATH"
```

同时更新第 105 行的硬编码路径（如有），改为通过 `xlings` 命令或 `$XLINGS_DATA` 变量动态解析。

### 4.2 新建 `xlings-ci-macos.yml`

```yaml
name: xlings-ci-macos

on:
  push:
    branches: [main, master]
  pull_request:
    branches: [main, master]

env:
  GIT_TERMINAL_PROMPT: 0

jobs:
  build-and-test:
    runs-on: macos-15   # Apple M1 arm64

    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Install system deps (Homebrew)
        run: |
          brew install curl git

      - name: Install xmake (bundled)
        run: |
          XMAKE_URL="https://github.com/xmake-io/xmake/releases/download/v3.0.7/xmake-bundle-v3.0.7.macos.arm64"
          curl -fsSL -o xmake.bin "$XMAKE_URL"
          chmod +x xmake.bin
          mkdir -p xmake/bin
          mv xmake.bin xmake/bin/xmake
          echo "PATH=$GITHUB_WORKSPACE/xmake/bin:$PATH" >> "$GITHUB_ENV"

      - name: Install Clang / Xcode tools
        run: |
          # macOS runner 自带 Xcode Command Line Tools
          clang --version
          # C++23 Modules 需要 Clang 18+ 或 Apple Clang 16+
          # macos-15 (Sonoma) 配有 Apple Clang 16+
          clang++ -std=c++23 -x c++ /dev/null -o /dev/null 2>/dev/null || \
            (echo "ERROR: Clang does not support C++23" && exit 1)

      - name: Install Rust
        uses: dtolnay/rust-toolchain@stable

      - name: Cache Rust
        uses: Swatinem/rust-cache@v2
        with:
          workspaces: core/xvm

      - name: Configure xmake (Apple Clang)
        run: |
          cd "$GITHUB_WORKSPACE"
          xmake f -p macosx -a arm64 -m release --toolchain=clang
          xmake f --cxxflags="-std=c++23" || true

      - name: Build xvm (Rust)
        run: |
          cd core/xvm
          cargo build --release --target aarch64-apple-darwin

      - name: Build xlings (C++23)
        run: |
          cd "$GITHUB_WORKSPACE"
          xmake build xlings

      - name: Package release
        run: |
          cd "$GITHUB_WORKSPACE"
          VERSION=$(grep 'VERSION' core/config.cppm | grep -o '"[0-9.]*"' | tr -d '"' | head -1)
          PKG_NAME="xlings-${VERSION}-macosx-arm64"
          mkdir -p "build/${PKG_NAME}/bin"
          cp build/macosx/arm64/release/xlings "build/${PKG_NAME}/bin/.xlings.real"
          cp core/xvm/target/aarch64-apple-darwin/release/xvm "build/${PKG_NAME}/bin/"
          cp core/xvm/target/aarch64-apple-darwin/release/xvm-shim "build/${PKG_NAME}/bin/"
          # 入口脚本
          cp tools/xlings.sh "build/${PKG_NAME}/bin/xlings" 2>/dev/null || \
            printf '#!/bin/sh\nDIR=$(dirname "$0"); XLINGS_HOME="$(dirname "$DIR")"; "$DIR/.xlings.real" "$@"\n' > "build/${PKG_NAME}/bin/xlings"
          chmod +x "build/${PKG_NAME}/bin/xlings"
          # xim
          cp -r xim "build/${PKG_NAME}/"
          cd build && tar -czf "${PKG_NAME}.tar.gz" "${PKG_NAME}"
          cp "${PKG_NAME}.tar.gz" release.tar.gz

      - name: Smoke test
        run: |
          cd "$GITHUB_WORKSPACE"
          tar -xzf build/release.tar.gz -C build
          PKG_DIR=$(ls -d build/xlings-*-macosx-arm64 | head -1)
          export XLINGS_HOME="$GITHUB_WORKSPACE/$PKG_DIR"
          export PATH="$XLINGS_HOME/bin:$PATH"
          xlings --version || xlings -h
          xlings env list

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        if: success()
        with:
          name: xlings-macosx-arm64
          path: build/release.tar.gz
```

### 4.3 新建 `xlings-ci-windows.yml`

```yaml
name: xlings-ci-windows

on:
  push:
    branches: [main, master]
  pull_request:
    branches: [main, master]

env:
  GIT_TERMINAL_PROMPT: 0

jobs:
  build-and-test:
    runs-on: windows-2022   # x86_64, MSVC 2022

    defaults:
      run:
        shell: pwsh   # PowerShell

    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Install xmake (winget)
        run: |
          winget install xmake-io.xmake --accept-source-agreements --accept-package-agreements
          $env:PATH = "C:\Program Files\xmake;$env:PATH"
          echo "PATH=C:\Program Files\xmake;$env:PATH" >> $env:GITHUB_ENV
          xmake --version

      - name: Install Rust
        uses: dtolnay/rust-toolchain@stable
        with:
          targets: x86_64-pc-windows-msvc

      - name: Cache Rust
        uses: Swatinem/rust-cache@v2
        with:
          workspaces: core/xvm

      - name: Configure xmake (MSVC + C++23)
        run: |
          xmake f -p windows -a x64 -m release --toolchain=msvc
          xmake f --cxxflags="/std:c++latest"

      - name: Build xvm (Rust / MSVC)
        run: |
          cd core/xvm
          cargo build --release --target x86_64-pc-windows-msvc

      - name: Build xlings (C++23 / MSVC)
        run: |
          xmake build xlings

      - name: Package release
        run: |
          $VERSION = (Select-String -Path "core/config.cppm" -Pattern '"(\d+\.\d+\.\d+)"').Matches.Groups[1].Value
          $PKG = "xlings-$VERSION-windows-x86_64"
          New-Item -ItemType Directory -Force "build/$PKG/bin" | Out-Null
          Copy-Item "build/windows/x64/release/xlings.exe" "build/$PKG/bin/.xlings.real.exe"
          Copy-Item "core/xvm/target/x86_64-pc-windows-msvc/release/xvm.exe" "build/$PKG/bin/"
          Copy-Item "core/xvm/target/x86_64-pc-windows-msvc/release/xvm-shim.exe" "build/$PKG/bin/"
          # 入口脚本 (bat)
          '@echo off`r`nsetlocal`r`nset "DIR=%~dp0"`r`nset "XLINGS_HOME=%DIR%.."`r`n"%DIR%.xlings.real.exe" %*' | Out-File "build/$PKG/bin/xlings.bat" -Encoding ASCII
          Copy-Item -Recurse xim "build/$PKG/"
          Compress-Archive -Path "build/$PKG" -DestinationPath "build/$PKG.zip"
          Copy-Item "build/$PKG.zip" "build/release.zip"

      - name: Smoke test
        run: |
          Expand-Archive "build/release.zip" -DestinationPath build
          $pkg = (Get-ChildItem "build" -Filter "xlings-*-windows-x86_64" -Directory | Select-Object -First 1).FullName
          $env:XLINGS_HOME = $pkg
          $env:PATH = "$pkg\bin;$env:PATH"
          xlings.bat -h
          xlings.bat env list

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        if: success()
        with:
          name: xlings-windows-x86_64
          path: build/release.zip
```

---

## 5. xmake.lua 跨平台配置更新

结合 [../main.md §6.4](../main.md) 的配置，确认 `xmake.lua` 覆盖三平台：

```lua
target("xlings")
    set_kind("binary")
    add_files("core/main.cpp", "core/**.cppm")
    add_includedirs("core/json")
    set_policy("build.c++.modules", true)

    if is_plat("linux") then
        add_ldflags("-static-libstdc++", "-static-libgcc", {force = true})
    elseif is_plat("macosx") then
        -- Apple Clang 使用 libc++ 默认静态链接
        add_cxxflags("-std=c++23", {force = true})
    elseif is_plat("windows") then
        add_cxflags("/std:c++latest", "/MT", {force = true})  -- 静态 CRT
    end
```

---

## 6. 验收标准

### 6.1 三平台 CI 全绿

GitHub Actions 页面确认三个 workflow 均显示 ✅ pass：
- `xlings-ci-linux`
- `xlings-ci-macos`
- `xlings-ci-windows`

### 6.2 产物文件存在

| 平台 | 产物格式 | 文件名示例 |
|------|---------|-----------|
| Linux x86_64 | `.tar.gz` | `xlings-0.2.0-linux-x86_64.tar.gz` |
| macOS arm64 | `.tar.gz` | `xlings-0.2.0-macosx-arm64.tar.gz` |
| Windows x86_64 | `.zip` | `xlings-0.2.0-windows-x86_64.zip` |

### 6.3 各平台 smoke test 通过

每个平台解压后执行：

```bash
# Linux / macOS
./xlings-*/bin/xlings --version
./xlings-*/bin/xlings env list   # 显示 [default](*)

# Windows
.\xlings-*\bin\xlings.bat --version
.\xlings-*\bin\xlings.bat env list
```

### 6.4 自包含验证（无系统 xlings 依赖）

```bash
# CI 中不预装 xlings，直接测试新构建的产物
unset XLINGS_HOME
./xlings-*/bin/xlings config   # 期望: 自动检测 XLINGS_HOME 为解压目录
```

### 6.5 构建时间基准

| 平台 | 期望构建时间 |
|------|------------|
| Linux | < 10 min |
| macOS | < 12 min |
| Windows | < 15 min |
