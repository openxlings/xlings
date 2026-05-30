package = {
    spec = "1",
    name = "node",
    description = "Offline fixture Node.js package for project-mode E2E tests",
    licenses = {"MIT"},
    type = "package",
    repo = "https://example.com/project-node-fixture",
    archs = {"x86_64"},
    xvm_enable = true,

    xpm = {
        linux = {
            ["latest"] = { ref = "22.17.1" },
            ["22.17.1"] = {},
            ["20.19.0"] = {},
        },
        macosx = {
            ["latest"] = { ref = "22.17.1" },
            ["22.17.1"] = {},
            ["20.19.0"] = {},
        },
        windows = {
            ["latest"] = { ref = "22.17.1" },
            ["22.17.1"] = {},
            ["20.19.0"] = {},
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

local function install_posix_bin(bindir, name, body)
    local file = path.join(bindir, name)
    if not write_file(file, "#!/bin/sh\n" .. body .. "\n") then
        return false
    end
    os.execute("chmod 755 '" .. file .. "'")
    return true
end

local function install_windows_bin(bindir, name, body)
    return write_file(path.join(bindir, name .. ".bat"), "@echo off\r\n" .. body .. "\r\n")
end

function install()
    local dir = pkginfo.install_dir()
    local bindir = path.join(dir, "bin")
    os.mkdir(bindir)

    local version = pkginfo.version()
    if os.host() == "windows" then
        if not install_windows_bin(bindir, "node", "echo v" .. version) then return false end
        if not install_windows_bin(bindir, "npm", "echo npm-for-node-" .. version) then return false end
        if not install_windows_bin(bindir, "npx", "echo npx-for-node-" .. version) then return false end
    else
        if not install_posix_bin(bindir, "node", "echo 'v" .. version .. "'") then return false end
        if not install_posix_bin(bindir, "npm", "echo 'npm-for-node-" .. version .. "'") then return false end
        if not install_posix_bin(bindir, "npx", "echo 'npx-for-node-" .. version .. "'") then return false end
    end

    return true
end

function config()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    local node_version = pkginfo.version()
    local npm_cfg = {
        bindir = bindir,
        version = "node-" .. node_version,
        binding = "node@" .. node_version,
    }
    local npx_cfg = {
        bindir = bindir,
        version = "node-" .. node_version,
        binding = "node@" .. node_version,
    }
    xvm.add("node", { bindir = bindir })
    xvm.add("npm", npm_cfg)
    xvm.add("npx", npx_cfg)
    return true
end

function uninstall()
    local node_version = pkginfo.version()
    xvm.remove("node")
    xvm.remove("npm", "node-" .. node_version)
    xvm.remove("npx", "node-" .. node_version)
    return true
end
