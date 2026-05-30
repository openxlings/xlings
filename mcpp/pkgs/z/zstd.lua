package = {
    spec = "1",
    name = "zstd",
    description = "Zstandard real-time compression algorithm",
    licenses = {"BSD-3-Clause"},
    repo = "https://github.com/facebook/zstd",
    type = "package",

    xpm = {
        linux = {
            ["v1.5.7"] = {
                url = "https://github.com/facebook/zstd/archive/refs/tags/v1.5.7.tar.gz",
                sha256 = "37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3",
            },
        },
        macosx = {
            ["v1.5.7"] = {
                url = "https://github.com/facebook/zstd/archive/refs/tags/v1.5.7.tar.gz",
                sha256 = "37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3",
            },
        },
        windows = {
            ["v1.5.7"] = {
                url = "https://github.com/facebook/zstd/archive/refs/tags/v1.5.7.tar.gz",
                sha256 = "37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3",
            },
        },
    },

    mcpp = {
        language = "c++23",
        import_std = false,
        c_standard = "c11",
        include_dirs = {
            "*/lib",
            "*/lib/common",
            "*/lib/compress",
            "*/lib/decompress",
        },
        -- libarchive only needs the public zstd compression/decompression API.
        -- dictBuilder pulls in qsort_r, which is not portable to Windows.
        sources = {
            "*/lib/common/*.c",
            "*/lib/compress/*.c",
            "*/lib/decompress/*.c",
            "!*/lib/common/zstd_trace.c",
        },
        targets = { ["zstd"] = { kind = "lib" } },
        deps = {},
    },
}
