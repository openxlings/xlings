add_rules("mode.debug", "mode.release")

set_languages("c++23")

-- GCC 15 ICE workaround: -O1/-O2 crash in tree-ssa-dce with C++23 modules
if is_plat("linux") then
    add_cxxflags("-Og", {force = true})
end

add_repositories("mcpplibs-index https://github.com/mcpplibs/mcpplibs-index.git")

-- Project-private package overrides. See xmake/packages/libarchive.lua
-- for why we ship our own libarchive definition (TL;DR: xmake-repo's
-- libarchive uses 7-Zip LZMA SDK as a dep, libarchive's cmake actually
-- needs xz-utils' liblzma → .tar.xz extraction silently degrades to a
-- fork-exec of the system `xz` binary, which doesn't exist on
-- musl-static minimal containers or Windows).
includes("xmake/packages/libarchive.lua")

-- Local-libxpkg override for cross-repo joint debugging.
-- Usage:
--   xmake f --local_libxpkg=/path/to/mcpplibs/libxpkg ...
-- When set, we treat the local checkout as the source of mcpplibs-xpkg
-- instead of fetching from the released xrepo package. Useful while
-- iterating on libxpkg + xlings together (e.g., schema split for
-- build/runtime deps). Empty string (default) → use released package.
option("local_libxpkg")
    set_default("")
    set_showmenu(true)
    set_description("Path to local mcpplibs/libxpkg checkout for dev builds")
option_end()

add_requires("cmdline 0.0.2")
add_requires("ftxui 6.1.9")
add_requires("mcpplibs-capi-lua")
-- mcpplibs-xpkg: prefer a local source checkout for joint debugging
-- when --local_libxpkg=/path/to/libxpkg is set. We pull the upstream
-- xmake.lua targets in via `includes()` so the consumer's build sees
-- our edits without going through xrepo's package cache (which keys
-- by released-version hash and ignores in-flight source changes).
-- Otherwise fall back to the released package from xrepo.
if has_config("local_libxpkg") and get_config("local_libxpkg") ~= "" then
    includes(path.join(get_config("local_libxpkg"), "xmake.lua"))
else
    add_requires("mcpplibs-xpkg 0.0.41")
end
add_requires("gtest 1.15.2")
add_requires("mcpplibs-tinyhttps 0.2.0")
-- libarchive's compression backends. Force `system = false` so xmake
-- builds them from source under our musl-cross toolchain instead of
-- picking up the host's glibc-built /usr/lib copies, which can't be
-- linked into a musl-static binary. Required for the linux release
-- build; harmless on macOS/Windows.
--
-- Note `xz` (xz-utils / liblzma) replaces the upstream
-- `lzma` (7-Zip LZMA SDK). See xmake/packages/libarchive.lua header
-- comment for why; the override of libarchive itself wires `xz` in as
-- a dep so libarchive's cmake actually finds liblzma.
add_requires("zlib", { system = false })
add_requires("lz4",  { system = false })
add_requires("bzip2", { system = false })
add_requires("zstd", { system = false })
add_requires("xz",   { system = false })
add_requires("libarchive-xlings 3.8.7")

-- C++23 main binary
target("xlings")
    set_kind("binary")
    add_files("src/main.cpp")
    add_files("src/**.cppm")
    add_includedirs("src/libs/json")
    add_packages("cmdline", "ftxui", "mcpplibs-capi-lua")
    if has_config("local_libxpkg") and get_config("local_libxpkg") ~= "" then
        add_deps("mcpplibs-xpkg", "mcpplibs-xpkg-loader",
                 "mcpplibs-xpkg-index", "mcpplibs-xpkg-lua-stdlib",
                 "mcpplibs-xpkg-executor")
    else
        add_packages("mcpplibs-xpkg")
    end
    add_packages("mcpplibs-tinyhttps", "libarchive-xlings")
    set_policy("build.c++.modules", true)

    if is_plat("macosx") then
        set_toolchains("llvm")
    elseif is_plat("linux") then
        add_ldflags("-static", {force = true})
    elseif is_plat("windows") then
        -- libarchive's XAR format parser uses xmllite (CreateXmlReader);
        -- xmake-repo's libarchive package_def only adds advapi32. Add
        -- xmllite here so the link step finds CreateXmlReader.
        add_syslinks("xmllite")
    end

-- Unit tests
target("xlings_tests")
    set_kind("binary")
    set_default(false)
    set_rundir("$(projectdir)")
    -- InterfaceProtocol tests spawn the real xlings binary via popen,
    -- so make sure it's available when `xmake build xlings_tests` runs.
    add_deps("xlings")
    add_files("tests/unit/*.cpp")
    add_files("src/**.cppm")
    add_includedirs("src/libs/json")
    add_packages("cmdline", "ftxui", "mcpplibs-capi-lua", "gtest")
    if has_config("local_libxpkg") and get_config("local_libxpkg") ~= "" then
        add_deps("mcpplibs-xpkg", "mcpplibs-xpkg-loader",
                 "mcpplibs-xpkg-index", "mcpplibs-xpkg-lua-stdlib",
                 "mcpplibs-xpkg-executor")
    else
        add_packages("mcpplibs-xpkg")
    end
    add_packages("mcpplibs-tinyhttps", "libarchive-xlings")
    set_policy("build.c++.modules", true)

    if is_plat("macosx") then
        set_toolchains("llvm")
    elseif is_plat("linux") then
        add_ldflags("-static", {force = true})
    elseif is_plat("windows") then
        -- libarchive's XAR format parser uses xmllite (CreateXmlReader);
        -- xmake-repo's libarchive package_def only adds advapi32. Add
        -- xmllite here so the link step finds CreateXmlReader.
        add_syslinks("xmllite")
    end
