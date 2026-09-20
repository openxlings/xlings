-- Fixture: the control for brokenscript.lua — same shape, valid syntax.
--
-- Without this row the assertion could be "refuse every registered script" and
-- the broken-case test would still pass. Not-measured is never agreement.
package = {
    spec = "1",
    name = "goodscript",
    description = "Fixture: registers a shell script that parses",
    licenses = {"MIT"},
    type = "package",
    repo = "https://example.com/goodscript",
    archs = {"x86_64", "aarch64"},
    xvm_enable = true,
    programs = { "goodscript" },

    xpm = {
        linux   = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
        windows = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, "bin"))
    local bin_path = path.join(dir, "bin", "goodscript")
    local f = io.open(bin_path, "w")
    if not f then return false end
    f:write("#!/bin/bash\n")
    f:write("RTLDLIST=\"/some/path/ld.so\"\n")
    f:write("echo \"Copyright (C) 2026\"\n")
    f:close()
    os.execute("chmod 755 '" .. bin_path .. "'")
    return true
end

function config()
    xvm.add("goodscript", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("goodscript")
    return true
end
