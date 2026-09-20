-- Fixture: a package that registers a shell script which does NOT parse.
--
-- This is the positive control for the install-time acceptance assertion
-- (openxlings/xlings#522). A real payload got into this state: the
-- `xim-x-glibc` 2.39 tarball's `bin/ldd` had its `RTLDLIST="` assignment eaten
-- by a hand-rolled path rewrite, the dangling quote swallowed the next nine
-- lines, and bash died on line 38 -- while the install reported success and
-- registered `ldd`.
--
-- The unterminated quote below reproduces that exact shape, deliberately:
-- everything after it is swallowed, so `bash -n` fails on a later line.
package = {
    spec = "1",
    name = "brokenscript",
    description = "Fixture: registers a shell script that does not parse",
    licenses = {"MIT"},
    type = "package",
    repo = "https://example.com/brokenscript",
    archs = {"x86_64", "aarch64"},
    xvm_enable = true,
    programs = { "brokenscript" },

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
    local bin_path = path.join(dir, "bin", "brokenscript")
    local f = io.open(bin_path, "w")
    if not f then return false end
    -- `RTLDLIST="` with no closing quote, then a line containing `(` -- the
    -- 2.39 failure, reproduced.
    f:write("#!/bin/bash\n")
    f:write("RTLDLIST=\"/some/path/ld.so\n")
    f:write("echo \"Copyright (C) 2026\"\n")
    f:close()
    os.execute("chmod 755 '" .. bin_path .. "'")
    return true
end

function config()
    xvm.add("brokenscript", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("brokenscript")
    return true
end
