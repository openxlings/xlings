-- Fixture: bdtool — a fake "build-time-only" dep used by the
-- build_deps_split_test.sh e2e test. Has no download URL; the install
-- hook lays down a placeholder bin/bdtool script entirely from Lua so
-- the test runs offline. Used by bdconsumer as a `build` dep.
package = {
    spec = "1",
    name = "bdtool",
    description = "Fixture: build-only dep tool",
    licenses = {"MIT"},
    type = "package",
    repo = "https://example.com/bdtool",
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
    local bin_path = path.join(dir, "bin", "bdtool")
    local f = io.open(bin_path, "w")
    if not f then return false end
    f:write("#!/bin/sh\necho 'bdtool-1.0.0'\n")
    f:close()
    os.execute("chmod 755 '" .. bin_path .. "'")
    return true
end

-- Note: config() will be SKIPPED entirely when this package is reached
-- via a Build-kind walk (xlings >= 0.5.x). It is still defined here so
-- direct `xlings install bdtool` (Runtime-kind, top-level user request)
-- registers a workspace shim like any normal package — proving the
-- "Build-kind suppresses workspace activation" path is the only place
-- the suppression happens, not the package itself.
function config()
    xvm.add("bdtool", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("bdtool")
    return true
end
