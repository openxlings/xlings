package = {
    spec = "1",
    name = "bzip2",
    description = "A freely available high-quality data compressor",
    licenses = {"bzip2-1.0.6"},
    repo = "https://sourceware.org/bzip2/",
    type = "package",

    xpm = {
        linux = {
            ["1.0.8"] = {
                url = "https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz",
                sha256 = "ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269",
            },
        },
        macosx = {
            ["1.0.8"] = {
                url = "https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz",
                sha256 = "ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269",
            },
        },
        windows = {
            ["1.0.8"] = {
                url = "https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz",
                sha256 = "ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269",
            },
        },
    },

    mcpp = {
        language = "c++23",
        import_std = false,
        c_standard = "c11",
        include_dirs = {"*"},
        sources = {
            "*/blocksort.c",
            "*/bzlib.c",
            "*/compress.c",
            "*/crctable.c",
            "*/decompress.c",
            "*/huffman.c",
            "*/randtable.c",
        },
        targets = { ["bzip2"] = { kind = "lib" } },
        deps = {},
    },
}
