package = {
    spec = "1",
    name = "candidate-helper",
    description = "Offline candidate lifecycle fixture",
    licenses = {"MIT"},
    type = "script",
    repo = "https://example.com/candidate-helper",
    archs = {"x86_64", "aarch64"},

    xpm = {
        linux = { ["0.0.1"] = {} },
        macosx = { ["0.0.1"] = {} },
        windows = { ["0.0.1"] = {} },
    },
}

function xpkg_main()
    print("candidate-helper")
end
