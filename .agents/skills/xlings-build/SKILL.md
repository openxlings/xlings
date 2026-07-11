---
name: xlings-build
description: 构建 xlings 项目的三平台操作指南，覆盖 Linux、macOS、Windows 上的工具链准备、`mcpp` 构建、release 脚本和构建后验证。
---

# XLINGS Build

## Overview

Use this skill as the build source of truth for xlings. Prefer repository CI and release scripts over ad-hoc command invention.

This repo has two build products:
- C++23 core via `mcpp`
- Rust `xvm` / `xvm-shim` via `cargo`

Prefer the platform release scripts for final packaging:
- Linux: `tools/linux_release.sh`
- macOS: `tools/macos_release.sh`
- Windows: `tools/windows_release.ps1`

## Standard Workflow

Follow this order unless the user explicitly wants only one phase:

1. Install `xlings` first so `.xlings.json` can provide mcpp.
2. Install the platform toolchain.
3. Confirm `mcpp.toml` and the selected target.
4. Build the repo or run the release script.
5. Verify the built binary with `-h`.
6. Run a network smoke test with `xlings install d2x -y` when the environment allows network access.

## Linux Build

Use this path when building the static Linux binary with `musl-gcc@15.1.0`.

### 1) Install xlings

```bash
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/refs/heads/main/tools/other/quick_install.sh | bash
source ~/.bashrc
```

### 2) Install musl toolchain with xlings

```bash
xlings install musl-gcc@15.1.0 -y
xlings info musl-gcc
```

Current repo CI and release scripts use this SDK root:

```bash
MUSL_SDK="${XLINGS_HOME:-$HOME/.xlings}/data/xpkgs/musl-gcc/15.1.0"
export CC=x86_64-linux-musl-gcc
export CXX=x86_64-linux-musl-g++
export PATH="$MUSL_SDK/bin:$PATH"
```

If the toolchain cannot run because of a missing musl loader path, create the loader symlink used by this repo:

```bash
sudo mkdir -p /home/xlings/.xlings_data/lib
sudo chown "$(id -u):$(id -g)" /home/xlings/.xlings_data/lib
ln -sfn "$MUSL_SDK/x86_64-linux-musl/lib/libc.so" /home/xlings/.xlings_data/lib/ld-musl-x86_64.so.1
```

### 3) Build with mcpp

`mcpp.toml` declares the musl target and toolchain.

### 4) Build

Quick local build:

```bash
mcpp build --target x86_64-linux-musl
cargo build --manifest-path core/xvm/Cargo.toml --release
```

Release-package build aligned with CI:

```bash
chmod +x ./tools/linux_release.sh
SKIP_NETWORK_VERIFY=1 ./tools/linux_release.sh
```

### 5) Verify

```bash
./build/linux/x86_64/release/xlings -h
./build/linux/x86_64/release/xlings install d2x -y
```

If using the packaged release tree, prefer `./bin/xlings -h` inside the assembled output directory.

## macOS Build

The checked-in CI uses the toolchain declared by `mcpp.toml` and builds the C++ project with `mcpp`. Installing xlings first only provides the project-declared development tools; xlings does not implicitly install xmake.

### 1) Install xlings

```bash
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/refs/heads/main/tools/other/quick_install.sh | bash
source ~/.bashrc
```

### 2) Install toolchain

CI-aligned toolchain:

```bash
xlings install
```

Then build with the toolchain declared by `mcpp.toml`:

```bash
mcpp build
```

### 3) Build

Quick local build:

```bash
mcpp build
cargo build --manifest-path core/xvm/Cargo.toml --release
```

Release-package build aligned with CI:

```bash
chmod +x ./tools/macos_release.sh
SKIP_NETWORK_VERIFY=1 ./tools/macos_release.sh
```

### 4) Verify

```bash
./build/macosx/arm64/release/xlings -h
./build/macosx/arm64/release/xlings install d2x -y
```

Also keep the release-script expectation in mind: the final binary should not retain LLVM dylib runtime dependency.

## Windows Build

Use the LLVM toolchain declared by `mcpp.toml`, matching Windows CI.

### 1) Prepare tools

Install xlings/mcpp and ensure Rust is available.

Optional: install xlings as a runtime/package-manager smoke-test target after build.

### 3) Build

Quick local build:

```powershell
mcpp build
cargo build --manifest-path core/xvm/Cargo.toml --release
```

Release-package build aligned with CI:

```powershell
pwsh ./tools/windows_release.ps1
```

### 4) Verify

```powershell
.\build\windows\x64\release\xlings.exe -h
.\build\windows\x64\release\xlings.exe install d2x -y
```

## Verification Rules

After any successful build, run at least:

```bash
xlings -h
xlings config
xvm --version
```

When testing package-manager behavior, also run:

```bash
xlings install d2x -y
```

If network access is not available, say that the smoke test was skipped rather than claiming success.

## Practical Notes

- On Linux, `musl-gcc@15.1.0` is required for `import std` in the C++23 build.
- `mcpp.toml` is the only C++ project definition.
- Prefer release scripts for packaging because they also assemble the xlings binary, `xvm`, config files, package-index data, and post-build verification. The release does not bundle xmake as an internal runtime dependency.
- When the user asks for exact CI parity, read the workflow file first and mirror it exactly rather than relying on memory.

## References

- Read [references/links.md](references/links.md) for workflow, release script, and project links.
