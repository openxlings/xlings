# Hosting an Index

**English** | [简体中文](zh/31-hosting-an-index.md)

An index is a repository of recipes. Anyone can host one — public, private, or a
directory on disk — and users add it next to the official one
([05](05-indexes-and-mirrors.md)).

## Layout

```text
my-pkgindex/
├── pkgs/
│   ├── h/hello.lua          # one recipe per package, under its first letter
│   └── t/tool.lua
├── xim-indexrepos.lua       # optional: sub-indexes this index declares
└── README.md
```

A template to start from is
[openxlings/xim-pkgindex-template](https://github.com/openxlings/xim-pkgindex-template).

## Using it

```bash
xlings config --index-repo myteam:https://git.example.com/team/my-pkgindex.git
xlings update
xlings install myteam:tool
```

A local directory works too (`myteam:/srv/my-pkgindex`), which is how an index is
tested before it is pushed.

## Publishing snapshots as artifacts

A Git sync works for any index. An index that publishes snapshots as artifacts —
as the official one does — gives its users faster, retried downloads, with Git as
the fallback. The publisher provides:

1. a raw-readable `<repo>-pointers.json` at the root of the artifact base:
   `{"format_version":1,"indexes":{"<key>":{<manifest>}}}`;
2. one release per snapshot, tagged `v<index_version>`, carrying the tarball the
   manifest names;
3. a tarball with `pkgs/` at its root;
4. a manifest whose `artifact.sha256` matches the asset.

The pointer's key is matched to the entry's `name`; a pointer with one entry is
used as it is. Users declare the base as in [05](05-indexes-and-mirrors.md);
the format is [specs/index-distribution](specs/index-distribution.md), and which
clients a snapshot supports is [specs/index-version-contract](specs/index-version-contract.md).

## Sub-indexes

An index can declare others in `xim-indexrepos.lua`; a user who adds it gets them
too:

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

## A site for it

[xpkgindex](https://github.com/openxlings/xpkgindex) renders an index as a static
site — packages, versions, history, contributors, and the repository's own docs —
as [index.xlings.org](https://index.xlings.org) is rendered from the official index.
