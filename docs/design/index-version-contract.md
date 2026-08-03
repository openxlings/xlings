> 更新日期：2026-08-04

# 索引版本契约 —— 发布方须知

一个索引快照可以声明它需要什么版本的客户端。客户端读到后，会**自动路由到自己能用的
最新快照**，而不是硬失败。

本文是**发布方**要实现的契约。实现它不需要读 xlings 源码。

设计背景与取舍：`.agents/docs/2026-08-03-index-snapshot-selection-design.md`

---

## 1. 为什么需要它

索引一旦开始使用新客户端才有的能力（新的 xvm 注册 node kind、新的 `spec` 版本），
老客户端就会硬失败：

```
error: unsupported registration node kind 'files'
```

声明契约后，老客户端会**自动退到它还能用的那个快照**，并被告知如何升级：

```
[index] xim: using 20e53c6 instead of e2aad0b
[index]   e2aad0b requires xlings >= 2026.8.4.1, this is 2026.8.2.1
[index]   upgrade to 2026.8.4.1: xlings self update
```

---

## 2. 在索引树里声明

索引树根目录放 `index-compat.json`（可选，缺失即无约束）：

```json
{
  "requires": {
    "xlings": { "min": "2026.8.4.1" }
  }
}
```

- `requires` 是 **consumer 名 → 约束** 的映射。
- `min` **含**，`max` **不含**：满足条件是 `min <= 客户端版本 < max`。两个都可选。
- xlings 只解释 `"xlings"` 这个键；其余键**原样透传**，供对应消费者自己判断
  （通过 `xlings index list --json` 读取）。
- 序列化后不得超过 **4 KiB** —— 它会进入每次 `xlings update` 都要抓的指针文件。

**没有范围表达式**。没有 `^`、`~`、`||`。两个边界就是范围；这条契约一旦被误读，
后果是整个索引对某类客户端不可用，所以语法故意贫乏到无法误读。

版本比较按分量做数值比较，段数任意 —— `2026.8.4.1` 这样的 4 段版本可以正确比较，
且 `2026.x` 恒大于 `0.4.x`。

---

## 3. 什么时候该抬 floor

**当索引开始依赖只有新客户端才有的能力时。**

抬 floor 会把老客户端冻在旧快照上，所以它是**安全网，不是首选**。先看能不能用
配方级能力探测让单个配方优雅降级；只有当索引整体依赖新能力时才抬。

| | 粒度 | 适用 |
|---|---|---|
| 配方能力探测 | 单个配方 | 少数配方尝鲜新能力 |
| 索引版本契约 | 整个快照 | 索引整体开始依赖新能力 |

---

## 4. 指针文件格式

客户端读的是**指针文件**（`<name>-pointers.json`，仓库文件，可 git push 覆盖），
里面除了当前快照，还要有**可寻址的历史**：

```json
{
  "format_version": 2,
  "indexes": {
    "mcpplibs": {
      "format_version": 1,
      "index_version": "e2aad0b",
      "index_name": "mcpplibs",
      "generated_at": "2026-08-04T14:52:11Z",
      "artifact": { "name": "mcpp-index-e2aad0b.tar.gz", "sha256": "…", "size": 371707 },
      "requires": { "xlings": { "min": "2026.8.4.1" } },
      "history": [
        { "index_version": "e2aad0b", "generated_at": "…",
          "requires": { "xlings": { "min": "2026.8.4.1" } },
          "artifact": { "name": "…", "sha256": "…", "size": 371707 } },
        { "index_version": "0adb288", "generated_at": "…",
          "artifact": { "name": "…", "sha256": "…", "size": 369884 } }
      ],
      "history_truncated": false
    }
  },
  "client_latest": { "xlings": "2026.8.4.1" }
}
```

要点：

- **`history` 倒序，`history[0]` 就是当前快照**（与外层重复是刻意的：客户端只过滤一个列表）。
- **每条 history 自带 `artifact.sha256`**。客户端选中哪条就校验哪条 —— 这是安全性质，
  不是可选项。**因此只有 `history` 里列出的快照可被选择**：不在列表里就没有哈希可校验。
- **`requires` 缺失 = 无约束**，任何客户端都可用。
- **`history_truncated`** 让客户端能区分"没有兼容快照"和"兼容的被截断了"。
- **`client_latest`** 是最新的客户端版本，与快照无关。**这一项不能省**：被路由到旧快照的
  客户端读到的是旧快照里的 xlings 配方（指向那个时代的 `latest`），没有这一项它
  永远不知道有更新的客户端存在，也就永远升不上去。

### `history` 该留哪些

不是"最近 N 个"。契约变动极少而快照产出频繁（xlings-res/xim-index 有 313 个 artifact、
但只有个位数的不同契约）。按**并集**留：

```
A) 最近 8 个快照（不论 requires）      → 让"回滚一个版本"可行
B) 每个不同 requires 值的最新那个      → 让每个契约世代都可达
上限 32，超出丢最老的并置 history_truncated
```

只按 A 留会让老客户端需要的那个世代掉出窗口，而窗口里躺着 8 个契约相同的快照。

xlings 提供参考实现：`tools/merge_index_pointer.py`（可直接复用，读-改-写地合并，
保留兄弟索引的 key）。

---

## 5. 客户端行为（发布方需要知道的部分）

```
快照集 = history（无 history 时即当前快照这一条）
候选   = 客户端版本满足 requires.xlings 的那些
候选非空 → 取最新的一条
候选为空 → 报错并列出可选集，且不动本地已有索引树
```

- 用户可用 `xlings index use <name> <version>` 钉住某个快照；钉子**绕过契约检查**
  （用户明确要求），但**不绕过 sha256 校验**。
- 钉到 `history` 之外的版本 → 报错，不回退到最新。
- `xlings self update` **不受路由约束**（否则被路由到旧快照的客户端将永远无法升级）。

---

## 6. 检查清单

- [ ] 索引树里有 `index-compat.json`（需要约束时）
- [ ] 构建产出的 manifest 含 `requires`
- [ ] 指针含 `history`（倒序、每条带 sha256）与 `history_truncated`
- [ ] 指针含 `client_latest`
- [ ] `format_version` 为 `2`
- [ ] 合并指针时保留了兄弟索引的 key
- [ ] CI 检查：配方里用到的新能力，都被声明的 `min` 覆盖
