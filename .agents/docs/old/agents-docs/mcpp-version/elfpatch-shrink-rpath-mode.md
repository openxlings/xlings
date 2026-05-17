# elfpatch 可选 shrink-rpath 模式

## 背景

当前 `elfpatch` 在安装后会为 ELF 写入：

- `INTERP`（默认使用 `subos` loader）
- `RUNPATH`（来自依赖闭包目录）

依赖闭包是“保守正确”的：通常会包含运行所需路径，但有时也会带入未实际使用的目录。  
这一般不影响可用性，但会增加动态加载搜索范围和调试复杂度。

## 目标

提供一个**可选**模式，在保持闭包正确性的前提下，进一步收缩 `RUNPATH` 到最小必要集合，行为接近 Nix 的 fixup/shrink 思路。

## 方案

在 `libxpkg.elfpatch` 中新增可选 shrink 流程：

1. 先按既有逻辑写入 `--set-rpath <closure paths>`
2. 若启用 shrink，再执行 `patchelf --shrink-rpath <file>`
3. 统计 shrink 成功/失败数量（不改变已有 patched/failed 语义）

`shrink-rpath` 只会删除当前 ELF 不需要的路径，保留满足 `DT_NEEDED` 解析所需目录。

## API 设计（兼容旧行为）

`elfpatch.auto()` 保持向后兼容：

- 旧用法（仍可用）：
  - `elfpatch.auto(true)`
- 新用法（可选 shrink）：
  - `elfpatch.auto({ enable = true, shrink = true })`

运行时状态扩展：

- `elfpatch_auto`
- `elfpatch_shrink`

`apply_auto()` 在 `opts.shrink` 未显式传入时，会读取 `runtime.pkginfo.elfpatch_shrink`。

## 默认策略

- 默认 **不启用 shrink**（`shrink = false`），避免改变已有包行为
- 包作者按需开启（例如 gcc 包）

## 示例（gcc）

在 `install()` 中启用：

```lua
elfpatch.auto({
    enable = true,
    shrink = true,
})
```

## 备注

- 即便不启用 shrink，闭包 rpath 通常也可正常运行
- 启用 shrink 后，`RUNPATH` 更精确，降低误命中和维护成本
