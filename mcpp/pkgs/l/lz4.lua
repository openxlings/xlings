package = {
    spec = "1",
    name = "lz4",
    description = "LZ4 extremely fast compression",
    licenses = {"BSD-2-Clause"},
    repo = "https://github.com/lz4/lz4",
    type = "package",

    xpm = {
        linux = {
            ["v1.10.0"] = {
                url = "https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz",
                sha256 = "537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b",
            },
        },
        macosx = {
            ["v1.10.0"] = {
                url = "https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz",
                sha256 = "537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b",
            },
        },
        windows = {
            ["v1.10.0"] = {
                url = "https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz",
                sha256 = "537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b",
            },
        },
    },

    mcpp = {
        language = "c++23",
        import_std = false,
        c_standard = "c11",
        include_dirs = {"*/lib"},
        sources = {"*/lib/*.c"},
        targets = { ["lz4"] = { kind = "lib" } },
        deps = {},
    },
}
