-- Fixture: rttool — runtime-kind dep used by build_deps_split_test.sh.
-- Same shape as bdtool but pulled in via bdconsumer's `runtime` deps,
-- so the installer is expected to register it in the workspace and
-- create a PATH shim.
package = {
    spec = "1",
    name = "rttool",
    description = "Fixture: runtime dep tool",
    licenses = {"MIT"},
    type = "package",
    repo = "https://example.com/rttool",
    archs = {"x86_64", "aarch64"},
    xvm_enable = true,

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
    local bin_path = path.join(dir, "bin", "rttool")
    local f = io.open(bin_path, "w")
    if not f then return false end
    f:write("#!/bin/sh\necho 'rttool-1.0.0'\n")
    f:close()
    os.execute("chmod 755 '" .. bin_path .. "'")
    return true
end

function config()
    xvm.add("rttool", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("rttool")
    return true
end
