package = {
    spec = "1",
    namespace = "alpha",
    name = "explicit-consumer",
    description = "Consumer with an explicit namespace dependency",
    type = "package",
    xpm = {
        linux = {
            deps = { "alpha:demo" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {},
        },
        macosx = {
            deps = { "alpha:demo" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {},
        },
        windows = {
            deps = { "alpha:demo" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {},
        },
    },
}
