package = {
    spec = "1",
    namespace = "alpha",
    name = "bare-consumer",
    description = "Consumer with an intentionally bare dependency",
    type = "package",
    xpm = {
        linux = {
            deps = { "demo" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {},
        },
        macosx = {
            deps = { "demo" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {},
        },
        windows = {
            deps = { "demo" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {},
        },
    },
}
