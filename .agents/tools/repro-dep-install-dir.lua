-- Reproduction harness for openxlings/xlings#524.
--
-- Loads the REAL pkginfo.lua (path given as arg[1]) against a stubbed xmake
-- stdlib and a fake xpkgs store, then replays the exact call shapes the
-- published recipes use. No xlings, no network, no install.

local PKGINFO = arg[1] or error("usage: lua repro.lua <path-to-pkginfo.lua>")
local LABEL   = arg[2] or PKGINFO

-- ── fake filesystem ──────────────────────────────────────────────────
-- A cold home immediately after the resolver installed gcc's deps:
-- glibc 2.44 and binutils 2.42 are on disk, gcc's own dir exists.
local STORE = "/home/u/.xlings/data/xpkgs"
local DIRS = {
    [STORE] = true,
    [STORE .. "/xim-x-glibc"] = true,
    [STORE .. "/xim-x-glibc/2.44"] = true,
    [STORE .. "/xim-x-binutils"] = true,
    [STORE .. "/xim-x-binutils/2.42"] = true,
    [STORE .. "/xim-x-gcc"] = true,
    [STORE .. "/xim-x-gcc/16.1.0"] = true,
    [STORE .. "/xim-x-mesa"] = true,
    [STORE .. "/xim-x-mesa/25.0.7.2"] = true,
    [STORE .. "/xim-x-nvidia-gl-host-link"] = true,
    [STORE .. "/xim-x-nvidia-gl-host-link/0.1.1"] = true,
    [STORE .. "/xim-x-python"] = true,
    [STORE .. "/xim-x-python/3.12.4"] = true,
    [STORE .. "/xim-x-llvm-tools"] = true,
    [STORE .. "/xim-x-llvm-tools/20.1.0"] = true,
}
local CHILDREN = {
    [STORE] = { "xim-x-glibc", "xim-x-binutils", "xim-x-gcc", "xim-x-mesa",
                "xim-x-nvidia-gl-host-link", "xim-x-python", "xim-x-llvm-tools" },
    [STORE .. "/xim-x-glibc"] = { "2.44" },
    [STORE .. "/xim-x-binutils"] = { "2.42" },
    [STORE .. "/xim-x-mesa"] = { "25.0.7.2" },
    [STORE .. "/xim-x-nvidia-gl-host-link"] = { "0.1.1" },
    [STORE .. "/xim-x-python"] = { "3.12.4" },
    [STORE .. "/xim-x-llvm-tools"] = { "20.1.0" },
}

-- ── xmake-ish stdlib stubs ───────────────────────────────────────────
path = {
    join = function(...)
        local parts = {}
        for _, p in ipairs({...}) do
            if p and p ~= "" then parts[#parts + 1] = tostring(p) end
        end
        return (table.concat(parts, "/"):gsub("//+", "/"))
    end,
    filename  = function(p) return (tostring(p):gsub(".*/", "")) end,
    directory = function(p) return (tostring(p):gsub("/[^/]*$", "")) end,
}
local _os = os
os = setmetatable({
    isdir  = function(p) return DIRS[tostring(p)] == true end,
    isfile = function() return false end,
    host   = function() return "linux" end,
    dirs   = function(pattern)
        local base = tostring(pattern):gsub("/%*$", "")
        local out = {}
        for _, c in ipairs(CHILDREN[base] or {}) do
            out[#out + 1] = base .. "/" .. c
        end
        return out
    end,
    getenv = function() return nil end,
}, { __index = _os })

local MESSAGES = {}
local function record(level)
    return function(fmt, ...)
        local ok, msg = pcall(string.format, fmt, ...)
        MESSAGES[#MESSAGES + 1] = level .. ": " .. (ok and msg or tostring(fmt))
    end
end
_LIBXPKG_MODULES = {
    log = { debug = function() end, info = record("info"),
            warn = record("warn"), error = record("error") },
}

-- ── the runtime the xlings installer hands a config hook ─────────────
-- resolved_deps is keyed by the DECLARED spec, exactly as the resolver
-- records it. gcc declares `xim:glibc@>=2.39`; the resolver picked 2.44.
local function runtime_for(pkg, deps_list, resolved, with_roots)
    local rt = {
        pkg_name         = pkg,
        version          = "16.1.0",
        install_dir      = STORE .. "/xim-x-" .. pkg .. "/16.1.0",
        xpkg_dir         = "/home/u/.xlings/data/xim/pkgindex/pkgs/g",
        project_data_dir = "",
        deps_list        = deps_list,
        resolved_deps    = resolved,
    }
    -- xlings >= 2026.8.10.1 always sets this; earlier clients never did.
    if with_roots then rt.dependency_store_roots = { STORE } end
    return rt
end

local GCC_RESOLVED = {
    ["xim:glibc@>=2.39"] = {
        name = "xim:glibc", version = "2.44",
        install_dir = STORE .. "/xim-x-glibc/2.44",
    },
    ["xim:binutils@2.42"] = {
        name = "xim:binutils", version = "2.42",
        install_dir = STORE .. "/xim-x-binutils/2.42",
    },
}
local GODOT_RESOLVED = {
    ["xim:graphics@>=0.1"] = {
        name = "xim:graphics", version = "0.1.0",
        install_dir = STORE .. "/xim-x-graphics/0.1.0",
    },
}
local GRAPHICS_RESOLVED = {
    ["xim:nvidia-gl-host-link@>=0.1"] = {
        name = "xim:nvidia-gl-host-link", version = "0.1.1",
        install_dir = STORE .. "/xim-x-nvidia-gl-host-link/0.1.1",
    },
}

-- ── the cases, as the published recipes actually write them ──────────
local CASES = {
    { "gcc.lua:544  dep_install_dir('glibc')",
      "gcc", {"xim:glibc@>=2.39", "xim:binutils@2.42"}, GCC_RESOLVED,
      "glibc", nil, STORE .. "/xim-x-glibc/2.44" },
    { "llvm.lua:210 dep_install_dir('glibc')",
      "llvm", {"xim:glibc@>=2.39"}, GCC_RESOLVED,
      "glibc", nil, STORE .. "/xim-x-glibc/2.44" },
    { "graphics.lua:176 dep_install_dir('nvidia-gl-host-link')",
      "graphics", {"xim:nvidia-gl-host-link@>=0.1"}, GRAPHICS_RESOLVED,
      "nvidia-gl-host-link", nil, STORE .. "/xim-x-nvidia-gl-host-link/0.1.1" },
    { "godot.lua:351 dep_install_dir('mesa')  [transitive, not declared]",
      "godot", {"xim:graphics@>=0.1"}, GODOT_RESOLVED,
      "mesa", nil, STORE .. "/xim-x-mesa/25.0.7.2" },
    { "meson.lua:116 dep_install_dir('python')  [raise() on nil]",
      "meson", {"xim:python@>=3.9"},
      { ["xim:python@>=3.9"] = { name="xim:python", version="3.12.4",
          install_dir = STORE .. "/xim-x-python/3.12.4" } },
      "python", nil, STORE .. "/xim-x-python/3.12.4" },
    { "clangd:245 dep_install_dir('llvm-tools', ver)  [pkgmanager-installed]",
      "mcpp-vscode-clangd", {"xim:mcpp"}, {},
      "llvm-tools", "20.1.0", STORE .. "/xim-x-llvm-tools/20.1.0" },
    { "proposed fix: dep_install_dir('xim:glibc')",
      "gcc", {"xim:glibc@>=2.39", "xim:binutils@2.42"}, GCC_RESOLVED,
      "xim:glibc", nil, STORE .. "/xim-x-glibc/2.44" },
}

print(("═"):rep(78))
print("pkginfo under test: " .. LABEL)
print(("═"):rep(78))
print(string.format("%-52s %-11s %s", "call site", "no roots", "roots set"))
print(("─"):rep(78))

local fails = 0
for _, c in ipairs(CASES) do
    local desc, pkg, deps, resolved, name, ver, want = table.unpack(c)
    local results = {}
    for _, with_roots in ipairs({ false, true }) do
        MESSAGES = {}
        _RUNTIME = runtime_for(pkg, deps, resolved, with_roots)
        package.loaded["pkginfo_under_test"] = nil
        local chunk = assert(loadfile(PKGINFO))
        local M = chunk()
        local ok, got = pcall(M.dep_install_dir, name, ver)
        if not ok then
            results[#results + 1] = "RAISED"
        elseif got == want then
            results[#results + 1] = "ok"
        elseif got == nil then
            results[#results + 1] = "nil"
        else
            results[#results + 1] = "WRONG"
        end
        if with_roots then
            results.msgs = MESSAGES
            if got ~= want then fails = fails + 1 end
        end
    end
    print(string.format("%-52s %-11s %s", desc, results[1], results[2]))
    for _, m in ipairs(results.msgs or {}) do
        print("      └─ " .. m:sub(1, 120))
    end
end
print(("─"):rep(78))
print(string.format("with dependency_store_roots set: %d/%d call sites broken",
                    fails, #CASES))
