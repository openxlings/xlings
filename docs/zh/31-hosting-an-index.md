# 自建索引

[English](../31-hosting-an-index.md) | **简体中文**

索引是一个装配方的仓库。任何人都可以托管一个——公开的、私有的，或者磁盘上的一个目录——用户把它加在官方索引旁边
（[05](05-indexes-and-mirrors.md)）。

## 布局

```text
my-pkgindex/
├── pkgs/
│   ├── h/hello.lua          # one recipe per package, under its first letter
│   └── t/tool.lua
├── xim-indexrepos.lua       # optional: sub-indexes this index declares
└── README.md
```

起步模板：[openxlings/xim-pkgindex-template](https://github.com/openxlings/xim-pkgindex-template)。

## 使用

```bash
xlings config --index-repo myteam:https://git.example.com/team/my-pkgindex.git
xlings update
xlings install myteam:tool
```

本地目录也可以（`myteam:/srv/my-pkgindex`），推送之前就是这样测试索引的。

## 以 artifact 发布快照

任何索引都可以 Git 同步。像官方索引那样把快照发布为 artifact 的索引，能给用户更快、可重试的下载，Git 作为回退。
发布方需要提供：

1. artifact base 根部一个可直接读取的 `<repo>-pointers.json`：`{"format_version":1,"indexes":{"<key>":{<manifest>}}}`；
2. 每个快照一个 release，tag 为 `v<index_version>`，附带 manifest 所写的 tarball；
3. tarball 根部含 `pkgs/`；
4. manifest 的 `artifact.sha256` 与资产一致。

指针的 key 按条目的 `name` 匹配；只有一个条目的指针直接采用。用户按 [05](05-indexes-and-mirrors.md) 声明 base；
格式见 [specs/index-distribution](../specs/index-distribution.md)，快照支持哪些客户端见
[specs/index-version-contract](../specs/index-version-contract.md)。

## 子索引

索引可以在 `xim-indexrepos.lua` 里声明其他索引；添加它的用户也会得到它们：

```lua
xim_indexrepos = {
    ["awesome"] = {
        ["GLOBAL"] = "https://github.com/openxlings/xim-pkgindex-awesome.git",
        ["CN"]     = "https://gitee.com/d2learn/xim-pkgindex-awesome.git",
        ["artifact"] = {
            ["GLOBAL"] = "https://github.com/xlings-res/awesome-index",
            ["CN"]     = "https://gitcode.com/xlings-res/awesome-index",
        },
        ["source"] = "auto",
    }
}
```

## 为它生成站点

[xpkgindex](https://github.com/openxlings/xpkgindex) 把索引渲染成静态站点——包、版本、历史、贡献者，以及仓库自己的文档——
[index.xlings.org](https://index.xlings.org) 就是这样由官方索引渲染出来的。
