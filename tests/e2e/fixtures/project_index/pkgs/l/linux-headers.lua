package = {
    spec = "1",
    name = "linux-headers",
    description = "Offline fixture Linux headers package for sysroot E2E tests",
    licenses = {"MIT"},
    type = "package",
    repo = "https://example.com/project-linux-headers-fixture",
    archs = {"x86_64"},
    xvm_enable = true,

    xpm = {
        linux = {
            ["latest"] = { ref = "5.11.1" },
            ["5.11.1"] = {},
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

local function write_file(file, content)
    local f = io.open(file, "w")
    if not f then return false end
    f:write(content)
    f:close()
    return true
end

function install()
    local include_dir = path.join(pkginfo.install_dir(), "include", "linux")
    os.mkdir(include_dir)
    return write_file(path.join(include_dir, "errno.h"), "#define EPERM 1\n")
end

function config()
    xvm.setup("linux-headers", { includedir = "include" })
    return true
end

function uninstall()
    xvm.teardown("linux-headers", { includedir = "include" })
    return true
end
