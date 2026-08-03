package = {
    spec = "1",
    name = "ninja",
    description = "XLINGS_RES fixture Ninja package for project-mode E2E tests",
    licenses = {"Apache-2.0"},
    type = "package",
    repo = "https://github.com/ninja-build/ninja",
    archs = {"x86_64", "aarch64"},
    xvm_enable = true,

    xpm = {
        linux = {
            ["latest"] = { ref = "1.12.1" },
            ["1.12.1"] = "XLINGS_RES",
        },
        macosx = {
            ["latest"] = { ref = "1.12.1" },
            ["1.12.1"] = "XLINGS_RES",
        },
        windows = {
            ["latest"] = { ref = "1.12.1" },
            ["1.12.1"] = "XLINGS_RES",
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local exe = os.host() == "windows" and "ninja.exe" or "ninja"
    os.mv(exe, pkginfo.install_dir())
    return true
end

function config()
    xvm.add("ninja")
    return true
end

function uninstall()
    xvm.remove("ninja")
    return true
end
