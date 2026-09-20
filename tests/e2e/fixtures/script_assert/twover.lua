-- Fixture: one package, three versions.
--   1.0  installs (no url, the hook writes the payload)
--   2.0  cannot be downloaded — the FAILURE case
--   3.0  installs — the positive control for the notice
--
-- The shape from openxlings/xlings#606: a requested version FAILS while a
-- different version of the same program is active. The installer used to print
--
--   twover@2.0 installed, but 'twover' still resolves to 1.0 — `xlings use ...`
--
-- for that, because the branch that prints it never asked whether the install
-- happened. The remedy it offered could not work: there was no 2.0 to switch to.
--
-- 127.0.0.1:1 is chosen so the failure is immediate and offline — a URL that
-- resolves but refuses, rather than a DNS timeout that makes the test slow and
-- its failure mode "the network".
package = {
    spec = "1",
    name = "twover",
    description = "Fixture: two versions, one of which cannot be fetched",
    licenses = {"MIT"},
    type = "package",
    repo = "https://example.com/twover",
    archs = {"x86_64", "aarch64"},
    xvm_enable = true,

    xpm = {
        linux = {
            ["latest"] = { ref = "1.0" },
            ["1.0"] = {},
            ["2.0"] = { url = "http://127.0.0.1:1/twover-2.0.tar.gz" },
            ["3.0"] = {},
        },
        macosx = {
            ["latest"] = { ref = "1.0" },
            ["1.0"] = {},
            ["2.0"] = { url = "http://127.0.0.1:1/twover-2.0.tar.gz" },
            ["3.0"] = {},
        },
        windows = {
            ["latest"] = { ref = "1.0" },
            ["1.0"] = {},
            ["2.0"] = { url = "http://127.0.0.1:1/twover-2.0.tar.gz" },
            ["3.0"] = {},
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, "bin"))
    local bin_path = path.join(dir, "bin", "twover")
    local f = io.open(bin_path, "w")
    if not f then return false end
    f:write("#!/bin/sh\necho 'twover-" .. pkginfo.version() .. "'\n")
    f:close()
    os.execute("chmod 755 '" .. bin_path .. "'")
    return true
end

function config()
    xvm.add("twover", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("twover", pkginfo.version())
    return true
end
