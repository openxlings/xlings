package = {
    spec = "1",
    name = "zlib",
    description = "A compression library",
    licenses = {"Zlib"},
    repo = "https://github.com/madler/zlib",
    type = "package",

    xpm = {
        linux = {
            ["v1.3.2"] = {
                url = "https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz",
                sha256 = "b99a0b86c0ba9360ec7e78c4f1e43b1cbdf1e6936c8fa0f6835c0cd694a495a1",
            },
        },
        macosx = {
            ["v1.3.2"] = {
                url = "https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz",
                sha256 = "b99a0b86c0ba9360ec7e78c4f1e43b1cbdf1e6936c8fa0f6835c0cd694a495a1",
            },
        },
        windows = {
            ["v1.3.2"] = {
                url = "https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz",
                sha256 = "b99a0b86c0ba9360ec7e78c4f1e43b1cbdf1e6936c8fa0f6835c0cd694a495a1",
            },
        },
    },

    mcpp = {
        language = "c++23",
        import_std = false,
        c_standard = "c11",
        include_dirs = {"*"},
        sources = {
            "*/adler32.c",
            "*/compress.c",
            "*/crc32.c",
            "*/deflate.c",
            "*/gzclose.c",
            "*/gzlib.c",
            "*/gzread.c",
            "*/gzwrite.c",
            "*/inflate.c",
            "*/infback.c",
            "*/inftrees.c",
            "*/inffast.c",
            "*/trees.c",
            "*/uncompr.c",
            "*/zutil.c",
        },
        targets = { ["zlib"] = { kind = "lib" } },
        deps = {},
    },
}
