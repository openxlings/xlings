// tests/unit/test_xvm_shim.cpp — shim creation, shim env, and the filesystem header symlink farm.
//
// Split out of the former single 12.7k-line test_main.cpp. Section order
// and contents are unchanged; only the file boundary is new.

#include <gtest/gtest.h>
#include <iomanip>
#ifdef __unix__
#include <sys/wait.h>
#endif
#if !defined(_WIN32)
#include <unistd.h>  // geteuid — AtomicWriteTest skips permission cases as root
#endif

import std;
import xlings.core.i18n;
import xlings.core.log;
import xlings.core.utils;
import xlings.ui;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.xim.installer;
import xlings.core.xim.commands;
import xlings.core.xim.repo;
import xlings.core.xim.extract;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.xvm.removal;
import xlings.core.xvm.registration;
import xlings.core.xvm.errors;
import xlings.core.xvm.inspect;
import xlings.core.xvm.lock;
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.shim;
import xlings.core.xvm.commands;
import xlings.core.compact;
import xlings.core.config;
import xlings.core.home_config;
import xlings.platform;
import xlings.libs.json;
import xlings.core.xself;
import xlings.core.profile;
import xlings.core.subos.gpu;
import xlings.core.xim.downloader;
import xlings.runtime;
import xlings.capabilities;
import xlings.libs.tinyhttps;
import xlings.libs.sha256;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
import mcpplibs.cmdline;

namespace {

struct ScopedEnvVar {
    std::string name;
    bool had_prev{false};
    std::string prev_value;

    ScopedEnvVar(std::string_view key, std::string_view value) : name(key) {
        if (auto* prev = std::getenv(name.c_str())) {
            had_prev = true;
            prev_value = prev;
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (had_prev) set(prev_value);
        else set("");
    }

    void set(std::string_view value) {
        xlings::platform::set_env_variable(name, std::string(value));
    }
};

std::optional<std::filesystem::path> find_pkgindex_repo() {
    namespace fs = std::filesystem;

    if (auto env = std::getenv("XIM_PKGINDEX_DIR")) {
        fs::path path(env);
        if (fs::exists(path / "pkgs")) return path;
    }

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures/xim-pkgindex",
        fs::current_path() / "../xim-pkgindex",
        fs::current_path() / "../d2learn/xim-pkgindex",
        fs::current_path() / "../../xim-pkgindex",
        fs::current_path() / "../../d2learn/xim-pkgindex",
    };

    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) return fs::weakly_canonical(path, ec);
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> find_fixture_repo(std::string_view name) {
    namespace fs = std::filesystem;

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures" / name,
        fs::current_path() / "../../tests/fixtures" / name,
    };
    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) {
            return fs::weakly_canonical(path, ec);
        }
    }
    return std::nullopt;
}

}  // namespace

// ============================================================
// xvm shim tests
// ============================================================

// ── shim env merge: scalar vars must not be blindly PATH-appended (#378) ──
TEST(XvmShimEnvTest, EmptyExistingUsesExpanded) {
    auto v = xlings::xvm::merge_shim_env_value("/etc/ssl/certs/ca.crt", "");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "/etc/ssl/certs/ca.crt");
}

TEST(XvmShimEnvTest, IdenticalExistingIsNoop) {
    // Both the caller (compact::git CA pin) and the shim's lua envs resolve
    // the same path; re-appending would corrupt it into "x:x".
    auto v = xlings::xvm::merge_shim_env_value("/etc/ssl/certs/ca.crt",
                                               "/etc/ssl/certs/ca.crt");
    EXPECT_FALSE(v.has_value());
}

TEST(XvmShimEnvTest, ComponentAlreadyPresentIsNoop) {
    std::string existing = std::string("/a/lib") + xlings::platform::PATH_SEPARATOR + "/b/lib";
    EXPECT_FALSE(xlings::xvm::merge_shim_env_value("/b/lib", existing).has_value());
}

TEST(XvmShimEnvTest, NewComponentPrepends) {
    auto v = xlings::xvm::merge_shim_env_value("/new/lib", "/old/lib");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, std::string("/new/lib") + xlings::platform::PATH_SEPARATOR + "/old/lib");
}

TEST(XvmShimTest, ExtractProgramName) {
    EXPECT_EQ(xlings::xvm::extract_program_name("/usr/bin/gcc"), "gcc");
    EXPECT_EQ(xlings::xvm::extract_program_name("./gcc"), "gcc");
    EXPECT_EQ(xlings::xvm::extract_program_name("gcc"), "gcc");
    EXPECT_EQ(xlings::xvm::extract_program_name("/home/user/.xlings/subos/default/bin/node"), "node");
    EXPECT_EQ(xlings::xvm::extract_program_name("/path/to/xlings"), "xlings");
}

TEST(XvmShimTest, ResolveExecutableFindsProgram) {
    // resolve_executable only looks up program_name as a file.
    // Alias handling is done separately in shim_dispatch via platform::exec.
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_env_alias_test";
    fs::remove_all(testDir);
    fs::create_directories(testDir / "bin");

    // Create a "gcc" binary but no "cc"
    auto gcc_path = testDir / "bin" / "gcc";
    xlings::platform::write_string_to_file(gcc_path.string(), "#!/bin/sh\n");

    // "cc" does not exist as a file → empty
    auto result1 = xlings::xvm::resolve_executable("cc", testDir.string(), "");
    EXPECT_TRUE(result1.empty());

    // "gcc" exists under bin/ → found
    auto result2 = xlings::xvm::resolve_executable("gcc", testDir.string(), "");
    EXPECT_FALSE(result2.empty());
    EXPECT_EQ(result2, testDir / "bin" / "gcc");

    fs::remove_all(testDir);
}

TEST(XvmShimTest, IsXlingsBinary) {
    // 0.4.8 collapsed the multicall surface to a single canonical name.
    // {xim, xvm, xself, xsubos, xinstall} are deprecated aliases that
    // main.cpp short-circuits to a migration error — they must NOT be
    // recognized as xlings here, otherwise they'd skip the error path.
    EXPECT_TRUE(xlings::xvm::is_xlings_binary("xlings"));
    EXPECT_FALSE(xlings::xvm::is_xlings_binary("xim"));
    EXPECT_FALSE(xlings::xvm::is_xlings_binary("xvm"));
    EXPECT_FALSE(xlings::xvm::is_xlings_binary("xself"));
    EXPECT_FALSE(xlings::xvm::is_xlings_binary("gcc"));
    EXPECT_FALSE(xlings::xvm::is_xlings_binary("node"));
    EXPECT_FALSE(xlings::xvm::is_xlings_binary("g++"));
    EXPECT_FALSE(xlings::xvm::is_xlings_binary(""));
}

TEST(XvmShimTest, ResolveAliasCommandToFullPath) {
    // Test: alias command's first word resolves to full path
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_alias_resolve_test";
    fs::remove_all(testDir);
    fs::create_directories(testDir / "bin");

    // Create a real gcc binary file
    auto gcc_path = testDir / "bin" / "gcc";
    xlings::platform::write_string_to_file(gcc_path.string(), "#!/bin/sh\n");

    // resolve_executable should find bin/gcc
    auto result = xlings::xvm::resolve_executable("gcc", testDir.string(), "");
    EXPECT_FALSE(result.empty());
    EXPECT_EQ(result, testDir / "bin" / "gcc");

    // Non-existent binary returns empty path
    auto result2 = xlings::xvm::resolve_executable("not-exist", testDir.string(), "");
    EXPECT_TRUE(result2.empty());

    fs::remove_all(testDir);
}

TEST(XvmShimTest, ResolveAliasDirectPath) {
    // Test: when path root directly contains the binary (no bin/ subdir)
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_alias_direct_test";
    fs::remove_all(testDir);
    fs::create_directories(testDir);

    // Create binary directly in path root
    auto gcc_path = testDir / "gcc";
    xlings::platform::write_string_to_file(gcc_path.string(), "#!/bin/sh\n");

    // resolve_executable should find path/gcc directly
    auto result = xlings::xvm::resolve_executable("gcc", testDir.string(), "");
    EXPECT_FALSE(result.empty());
    EXPECT_EQ(result, testDir / "gcc");

    fs::remove_all(testDir);
}

// ============================================================
// xvm config integration tests (filesystem-based)
// ============================================================

class XvmConfigTest : public ::testing::Test {
protected:
    std::filesystem::path testDir_;

    void SetUp() override {
        namespace fs = std::filesystem;
        testDir_ = fs::temp_directory_path() / "xlings_xvm_test";
        fs::remove_all(testDir_);
        fs::create_directories(testDir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(testDir_);
    }
};

TEST_F(XvmConfigTest, WriteAndReadGlobalConfig) {
    namespace fs = std::filesystem;

    // Build a VersionDB
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/usr/bin", "program", "gcc");
    xlings::xvm::add_version(db, "gcc", "14.2.0", "${XLINGS_HOME}/data/xpkgs/gcc/14.2.0/bin", "program", "gcc");
    db["gcc"].bindings["g++"]["15.1.0"] = "g++-15";
    db["gcc"].bindings["g++"]["14.2.0"] = "g++-14";

    // Build a config JSON
    nlohmann::json config;
    config["lang"] = "en";
    config["mirror"] = "GLOBAL";
    config["activeSubos"] = "default";
    config["versions"] = xlings::xvm::versions_to_json(db);
    config["subos"] = nlohmann::json::object();
    config["subos"]["default"] = nlohmann::json::object();

    // Write to file
    auto configPath = testDir_ / ".xlings.json";
    xlings::platform::write_string_to_file(configPath.string(), config.dump(2));

    ASSERT_TRUE(fs::exists(configPath));

    // Read back and verify
    auto content = xlings::platform::read_file_to_string(configPath.string());
    auto parsed = nlohmann::json::parse(content);

    EXPECT_EQ(parsed["lang"].get<std::string>(), "en");
    EXPECT_EQ(parsed["mirror"].get<std::string>(), "GLOBAL");
    EXPECT_EQ(parsed["activeSubos"].get<std::string>(), "default");

    auto restored_db = xlings::xvm::versions_from_json(parsed["versions"]);
    EXPECT_EQ(restored_db.size(), 1u);
    EXPECT_TRUE(xlings::xvm::has_version(restored_db, "gcc", "15.1.0"));
    EXPECT_TRUE(xlings::xvm::has_version(restored_db, "gcc", "14.2.0"));
    EXPECT_EQ(xlings::xvm::get_binding(restored_db, "gcc", "g++", "15.1.0"), "g++-15");
}

TEST_F(XvmConfigTest, WriteAndReadSubosWorkspace) {
    namespace fs = std::filesystem;

    // Create subos directory
    auto subosDir = testDir_ / "subos" / "default";
    fs::create_directories(subosDir);

    // Write workspace
    xlings::xvm::Workspace ws;
    ws["gcc"] = "15.1.0";
    ws["node"] = "22.0.0";

    nlohmann::json subosConfig;
    subosConfig["workspace"] = xlings::xvm::workspace_to_json(ws);

    auto configPath = subosDir / ".xlings.json";
    xlings::platform::write_string_to_file(configPath.string(), subosConfig.dump(2));
    ASSERT_TRUE(fs::exists(configPath));

    // Read back
    auto content = xlings::platform::read_file_to_string(configPath.string());
    auto parsed = nlohmann::json::parse(content);
    auto restored_ws = xlings::xvm::workspace_from_json(parsed["workspace"]);

    EXPECT_EQ(restored_ws.size(), 2u);
    EXPECT_EQ(restored_ws["gcc"], "15.1.0");
    EXPECT_EQ(restored_ws["node"], "22.0.0");
}

TEST_F(XvmConfigTest, ProjectConfigOverridesWorkspace) {
    // Simulate: subos workspace has gcc=15.1.0, project has gcc=14.2.0
    xlings::xvm::Workspace subosWs;
    subosWs["gcc"] = "15.1.0";
    subosWs["node"] = "22.0.0";

    xlings::xvm::Workspace projectWs;
    projectWs["gcc"] = "14.2.0";

    // Manual merge (simulating Config::effective_workspace logic)
    xlings::xvm::Workspace effective = subosWs;
    for (auto it = projectWs.begin(); it != projectWs.end(); ++it) {
        effective[it->first] = it->second;
    }

    EXPECT_EQ(effective["gcc"], "14.2.0");   // project overrides
    EXPECT_EQ(effective["node"], "22.0.0");  // subos preserved
}

TEST_F(XvmConfigTest, CreateSubosDirectoryStructure) {
    namespace fs = std::filesystem;

    auto subosDir = testDir_ / "subos" / "dev";
    fs::create_directories(subosDir / "bin");
    fs::create_directories(subosDir / "lib");
    fs::create_directories(subosDir / "usr");
    fs::create_directories(subosDir / "generations");

    // Write empty workspace
    nlohmann::json subosConfig;
    subosConfig["workspace"] = nlohmann::json::object();
    auto configPath = subosDir / ".xlings.json";
    xlings::platform::write_string_to_file(configPath.string(), subosConfig.dump(2));

    EXPECT_TRUE(fs::exists(subosDir / "bin"));
    EXPECT_TRUE(fs::exists(subosDir / "lib"));
    EXPECT_TRUE(fs::exists(subosDir / "usr"));
    EXPECT_TRUE(fs::exists(subosDir / "generations"));
    EXPECT_TRUE(fs::exists(configPath));

    // Verify config content
    auto content = xlings::platform::read_file_to_string(configPath.string());
    auto parsed = nlohmann::json::parse(content);
    EXPECT_TRUE(parsed.contains("workspace"));
    EXPECT_TRUE(parsed["workspace"].is_object());
    EXPECT_TRUE(parsed["workspace"].empty());
}

// ============================================================
// xvm header symlink tests (filesystem-based)
// ============================================================

class XvmHeaderSymlinkTest : public ::testing::Test {
protected:
    std::filesystem::path testDir_;

    void SetUp() override {
        namespace fs = std::filesystem;
        testDir_ = fs::temp_directory_path() / "xlings_xvm_header_test";
        fs::remove_all(testDir_);
        fs::create_directories(testDir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(testDir_);
    }
};

TEST_F(XvmHeaderSymlinkTest, InstallAndRemoveHeaders) {
    namespace fs = std::filesystem;

    // Create a fake include directory with headers
    auto srcInclude = testDir_ / "pkg" / "include";
    fs::create_directories(srcInclude / "bits");
    xlings::platform::write_string_to_file((srcInclude / "stdio.h").string(), "/* stdio */");
    xlings::platform::write_string_to_file((srcInclude / "bits" / "types.h").string(), "/* types */");

    // Install headers
    auto sysrootInclude = testDir_ / "sysroot" / "usr" / "include";
    xlings::xvm::install_headers(srcInclude.string(), sysrootInclude);

    // Verify links created (symlinks on Unix, hard links/copies on Windows)
    EXPECT_TRUE(fs::exists(sysrootInclude / "stdio.h"));
    EXPECT_TRUE(fs::exists(sysrootInclude / "bits"));
#if !defined(_WIN32)
    EXPECT_TRUE(fs::is_symlink(sysrootInclude / "stdio.h"));
    EXPECT_EQ(fs::read_symlink(sysrootInclude / "stdio.h").string(),
              (srcInclude / "stdio.h").string());
#endif

    // Remove headers
    xlings::xvm::remove_headers(srcInclude.string(), sysrootInclude);

    // Verify links removed
    EXPECT_FALSE(fs::exists(sysrootInclude / "stdio.h"));
    EXPECT_FALSE(fs::exists(sysrootInclude / "bits"));
}

TEST_F(XvmHeaderSymlinkTest, InstallHeadersOverwrite) {
    namespace fs = std::filesystem;

    auto srcInclude1 = testDir_ / "pkg1" / "include";
    auto srcInclude2 = testDir_ / "pkg2" / "include";
    fs::create_directories(srcInclude1);
    fs::create_directories(srcInclude2);
    xlings::platform::write_string_to_file((srcInclude1 / "common.h").string(), "/* v1 */");
    xlings::platform::write_string_to_file((srcInclude2 / "common.h").string(), "/* v2 */");

    auto sysrootInclude = testDir_ / "sysroot" / "usr" / "include";

    // Install first, then overwrite with second
    xlings::xvm::install_headers(srcInclude1.string(), sysrootInclude);
    EXPECT_TRUE(fs::exists(sysrootInclude / "common.h"));
#if !defined(_WIN32)
    EXPECT_TRUE(fs::is_symlink(sysrootInclude / "common.h"));
    EXPECT_EQ(fs::read_symlink(sysrootInclude / "common.h").string(),
              (srcInclude1 / "common.h").string());
#endif

    xlings::xvm::install_headers(srcInclude2.string(), sysrootInclude);
    EXPECT_TRUE(fs::exists(sysrootInclude / "common.h"));
#if !defined(_WIN32)
    EXPECT_TRUE(fs::is_symlink(sysrootInclude / "common.h"));
    EXPECT_EQ(fs::read_symlink(sysrootInclude / "common.h").string(),
              (srcInclude2 / "common.h").string());
#endif
}

TEST_F(XvmHeaderSymlinkTest, RemoveHeadersNonexistentDir) {
    namespace fs = std::filesystem;
    auto sysrootInclude = testDir_ / "sysroot" / "usr" / "include";
    // Should not crash with nonexistent source dir
    xlings::xvm::remove_headers("/tmp/nonexistent_dir_xyz_999", sysrootInclude);
    xlings::xvm::remove_headers("", sysrootInclude);
}

// A header asset's destination prefix decides the subdirectory it appears
// under. The source directory's own name is not always the name the compiler
// looks for: a toolchain's `include/c++/15.1.0` has to show up as
// `c++/15.1.0`, not as its contents flattened into the include root.
//
// No recipe can set the prefix today -- libxpkg's `xvm.setup` takes a bare
// `includedir` -- but the field is serialized and round-trips through the
// version database, so materialization honoring it is what keeps it from
// being another persisted-and-ignored value.
TEST_F(XvmHeaderSymlinkTest, ADestinationPrefixDecidesWhereHeadersLand) {
    namespace fs = std::filesystem;

    auto srcInclude = testDir_ / "pkg" / "include" / "c++" / "15.1.0";
    fs::create_directories(srcInclude);
    xlings::platform::write_string_to_file(
        (srcInclude / "vector").string(), "/* vector */");

    auto sysrootInclude = testDir_ / "sysroot" / "usr" / "include";
    const xlings::xvm::HeaderAsset asset{
        .sourceDir = srcInclude.string(),
        .destinationPrefix = "c++/15.1.0",
    };

    xlings::xvm::install_headers(asset, sysrootInclude);
    EXPECT_TRUE(fs::exists(sysrootInclude / "c++" / "15.1.0" / "vector"));
    EXPECT_FALSE(fs::exists(sysrootInclude / "vector"))
        << "the prefix was ignored and the headers were flattened into the "
           "include root";

    xlings::xvm::remove_headers(asset, sysrootInclude);
    EXPECT_FALSE(fs::exists(sysrootInclude / "c++" / "15.1.0" / "vector"));
    // The prefix directory only ever held this release's links, so it goes
    // too rather than accumulating as litter.
    EXPECT_FALSE(fs::exists(sysrootInclude / "c++" / "15.1.0"));
}

TEST_F(XvmHeaderSymlinkTest, APrefixDirectoryWithOtherContentSurvivesRemoval) {
    namespace fs = std::filesystem;

    auto srcInclude = testDir_ / "pkg" / "include";
    fs::create_directories(srcInclude);
    xlings::platform::write_string_to_file(
        (srcInclude / "vector").string(), "/* vector */");

    auto sysrootInclude = testDir_ / "sysroot" / "usr" / "include";
    const xlings::xvm::HeaderAsset asset{
        .sourceDir = srcInclude.string(),
        .destinationPrefix = "c++",
    };
    xlings::xvm::install_headers(asset, sysrootInclude);

    // Something else lives under the same prefix -- another package's
    // headers, or a file the user put there.
    xlings::platform::write_string_to_file(
        (sysrootInclude / "c++" / "keep.h").string(), "/* not ours */");

    xlings::xvm::remove_headers(asset, sysrootInclude);
    EXPECT_FALSE(fs::exists(sysrootInclude / "c++" / "vector"));
    EXPECT_TRUE(fs::exists(sysrootInclude / "c++" / "keep.h"))
        << "removing a header asset deleted a directory it did not own";
}

// ============================================================
// create_shim / is_builtin_shim tests
// ============================================================

class ShimCreateTest : public ::testing::Test {
protected:
    std::filesystem::path testDir_;

    void SetUp() override {
        namespace fs = std::filesystem;
        testDir_ = fs::temp_directory_path() / "xlings_shim_create_test";
        fs::remove_all(testDir_);
        fs::create_directories(testDir_ / "src");
        fs::create_directories(testDir_ / "dst");
        // Create a small source file to act as the "binary"
        xlings::platform::write_string_to_file(
            (testDir_ / "src" / "xlings").string(), "fake-binary-content");
    }

    void TearDown() override {
        std::filesystem::remove_all(testDir_);
    }
};

TEST_F(ShimCreateTest, CreatesShimOnUnix) {
    namespace fs = std::filesystem;
    auto src = testDir_ / "src" / "xlings";
    auto dst = testDir_ / "dst" / "gcc";
    auto result = xlings::xself::create_shim(src, dst);
#if !defined(_WIN32)
    EXPECT_EQ(result, xlings::xself::LinkResult::Symlink);
    EXPECT_TRUE(fs::is_symlink(dst));
#else
    // On Windows: hardlink or copy
    EXPECT_TRUE(result == xlings::xself::LinkResult::Hardlink ||
                result == xlings::xself::LinkResult::Copy);
    EXPECT_TRUE(fs::exists(dst));
#endif
}

TEST_F(ShimCreateTest, SymlinkIsRelative) {
    namespace fs = std::filesystem;
    auto src = testDir_ / "src" / "xlings";
    auto dst = testDir_ / "dst" / "gcc";
    auto result = xlings::xself::create_shim(src, dst);
#if !defined(_WIN32)
    ASSERT_EQ(result, xlings::xself::LinkResult::Symlink);
    auto link_target = fs::read_symlink(dst);
    EXPECT_TRUE(link_target.is_relative())
        << "symlink should be relative, got: " << link_target;
#endif
}

TEST_F(ShimCreateTest, OverwritesExisting) {
    namespace fs = std::filesystem;
    auto src = testDir_ / "src" / "xlings";
    auto dst = testDir_ / "dst" / "gcc";
    // Create an existing file at dst
    xlings::platform::write_string_to_file(dst.string(), "old-content");
    ASSERT_TRUE(fs::exists(dst));

    auto result = xlings::xself::create_shim(src, dst);
    EXPECT_NE(result, xlings::xself::LinkResult::Failed);
#if !defined(_WIN32)
    EXPECT_TRUE(fs::is_symlink(dst));
#else
    EXPECT_TRUE(fs::exists(dst));
#endif
}

TEST_F(ShimCreateTest, OverwritesExistingSymlink) {
    namespace fs = std::filesystem;
    auto src = testDir_ / "src" / "xlings";
    auto dst = testDir_ / "dst" / "gcc";
#if !defined(_WIN32)
    // Create a dangling symlink at dst
    fs::create_symlink("/nonexistent/path", dst);
    ASSERT_TRUE(fs::is_symlink(dst));

    auto result = xlings::xself::create_shim(src, dst);
    EXPECT_EQ(result, xlings::xself::LinkResult::Symlink);
    // Should now point to the real source
    EXPECT_TRUE(fs::exists(dst));
#endif
}

TEST_F(ShimCreateTest, SourceNotExistReturnsFailed) {
    auto dst = testDir_ / "dst" / "gcc";
    auto result = xlings::xself::create_shim(testDir_ / "nonexistent", dst);
    EXPECT_EQ(result, xlings::xself::LinkResult::Failed);
    EXPECT_FALSE(std::filesystem::exists(dst));
}

TEST_F(ShimCreateTest, IsBuiltinShimCoversAll) {
    // 0.4.8: only the canonical `xlings` is a builtin shim. The legacy
    // aliases (xim/xinstall/xsubos/xself) were removed.
    EXPECT_TRUE(xlings::xself::is_builtin_shim("xlings"));
    EXPECT_FALSE(xlings::xself::is_builtin_shim("xim"));
    EXPECT_FALSE(xlings::xself::is_builtin_shim("xvm"));
    EXPECT_FALSE(xlings::xself::is_builtin_shim("xinstall"));
    EXPECT_FALSE(xlings::xself::is_builtin_shim("xsubos"));
    EXPECT_FALSE(xlings::xself::is_builtin_shim("xself"));
    EXPECT_FALSE(xlings::xself::is_builtin_shim("xmake"));
    EXPECT_FALSE(xlings::xself::is_builtin_shim("gcc"));
    EXPECT_FALSE(xlings::xself::is_builtin_shim("node"));
    EXPECT_FALSE(xlings::xself::is_builtin_shim(""));
}

TEST_F(ShimCreateTest, EnsureSubosShimsCreatesAll) {
    namespace fs = std::filesystem;
    auto src = testDir_ / "src" / "xlings";
    auto binDir = testDir_ / "dst";

    xlings::xself::ensure_subos_shims(binDir, src, fs::path{});

    // 0.4.8: only the canonical `xlings` shim is created.
    auto xlings_shim = binDir / "xlings";
    EXPECT_TRUE(fs::exists(xlings_shim));
#if !defined(_WIN32)
    EXPECT_TRUE(fs::is_symlink(xlings_shim));
#endif

    // Legacy alias shims must NOT be created.
    for (auto name : {"xim", "xvm", "xinstall", "xsubos", "xself"}) {
        EXPECT_FALSE(fs::exists(binDir / name))
            << "legacy alias shim '" << name << "' should not be created in 0.4.8+";
    }
}

// COMPAT(0.4.8 → drop in 0.6.0): tests for xself::compat::cleanup_legacy_alias_shims.
// Delete this whole TEST_F block when the compat module is removed.
TEST_F(ShimCreateTest, CleanupLegacyAliasShimsRemovesOnlyMatchingSymlinks) {
#if defined(_WIN32)
    GTEST_SKIP() << "symlink semantics differ on Windows";
#else
    namespace fs = std::filesystem;
    auto src = testDir_ / "src" / "xlings";
    auto binDir = testDir_ / "dst";
    fs::create_directories(binDir);

    // Layout under test:
    //   xvm, xself, xsubos, xinstall — symlinks → bootstrap (must be removed)
    //   xim                          — regular user file with colliding name
    //                                  (must survive — gate is "is symlink")
    for (auto name : {"xvm", "xself", "xsubos", "xinstall"}) {
        fs::create_symlink(src, binDir / name);
    }
    auto userFile = binDir / "xim";
    std::ofstream(userFile) << "user data\n";

    xlings::xself::compat::v0_4_8::cleanup_legacy_alias_shims(binDir, src);

    // Regular user file with a colliding name must survive.
    EXPECT_TRUE(fs::exists(userFile));
    EXPECT_FALSE(fs::is_symlink(userFile));

    // Matching symlinks must be removed.
    for (auto name : {"xvm", "xself", "xsubos", "xinstall"}) {
        EXPECT_FALSE(fs::exists(binDir / name))
            << "legacy alias symlink '" << name << "' should have been removed";
    }
#endif
}

// ── subos path normalization (2026-07-30) ────────────────────────────
//
// An install-time absolute subos path baked into an alias survives every
// `subos use`, because the versions DB is shared by the whole home and nothing
// rewrites it: the user switches to `default` and their g++ keeps compiling
// against `dev-hello`. These pin the rewrite that happens at exec time.

namespace {
std::string norm(const std::string& text,
                 const std::string& home,
                 const std::string& active) {
    return xlings::xvm::normalize_subos_paths(text, home, active);
}
}  // namespace

TEST(SubosPathNormalizeTest, RewritesBakedSysrootToActiveSubos) {
    EXPECT_EQ(norm("g++ --sysroot=/home/u/.xlings/subos/dev-hello",
                   "/home/u/.xlings", "/home/u/.xlings/subos/default"),
              "g++ --sysroot=/home/u/.xlings/subos/default");
}

TEST(SubosPathNormalizeTest, KeepsSuffixAfterSubosName) {
    EXPECT_EQ(norm("cc -I/home/u/.xlings/subos/dev/usr/include -O2",
                   "/home/u/.xlings", "/home/u/.xlings/subos/default"),
              "cc -I/home/u/.xlings/subos/default/usr/include -O2");
}

TEST(SubosPathNormalizeTest, LeavesForeignSubosPathsAlone) {
    // A user's own /opt/subos/... is not ours. Byte-identical passthrough.
    const std::string cmd = "tool --root=/opt/subos/foo/bar";
    EXPECT_EQ(norm(cmd, "/home/u/.xlings", "/home/u/.xlings/subos/default"),
              cmd);
}

TEST(SubosPathNormalizeTest, AcceptsProjectSubosByDotXlingsSuffix) {
    // Project mode bakes <projectDir>/.xlings/subos/<name>, which is not under
    // homeDir at all -- the `.xlings` suffix rule is what catches it.
    EXPECT_EQ(norm("g++ --sysroot=/w/proj/.xlings/subos/anon",
                   "/home/u/.xlings", "/w/proj/.xlings/subos/dev"),
              "g++ --sysroot=/w/proj/.xlings/subos/dev");
}

TEST(SubosPathNormalizeTest, IsIdempotent) {
    const std::string cmd = "g++ --sysroot=/home/u/.xlings/subos/default";
    auto once = norm(cmd, "/home/u/.xlings", "/home/u/.xlings/subos/default");
    EXPECT_EQ(once, cmd);
    EXPECT_EQ(norm(once, "/home/u/.xlings", "/home/u/.xlings/subos/default"),
              cmd);
}

TEST(SubosPathNormalizeTest, RewritesEveryOccurrence) {
    EXPECT_EQ(norm("g++ --sysroot=/h/.xlings/subos/a -B/h/.xlings/subos/a/usr/lib",
                   "/h/.xlings", "/h/.xlings/subos/b"),
              "g++ --sysroot=/h/.xlings/subos/b -B/h/.xlings/subos/b/usr/lib");
}

TEST(SubosPathNormalizeTest, EmptyActiveDirIsNoOp) {
    // Never rewrite to nothing: an unresolvable active subos must leave the
    // alias as it was, so any failure names the real path.
    const std::string cmd = "g++ --sysroot=/h/.xlings/subos/a";
    EXPECT_EQ(norm(cmd, "/h/.xlings", ""), cmd);
}

TEST(SubosPathNormalizeTest, HandlesPathListSeparators) {
    // A colon-separated list is two tokens, and only the ours-prefixed one
    // may move.
    EXPECT_EQ(norm("/h/.xlings/subos/a/bin:/opt/subos/x/bin",
                   "/h/.xlings", "/h/.xlings/subos/b"),
              "/h/.xlings/subos/b/bin:/opt/subos/x/bin");
}

TEST(SubosPathNormalizeTest, HandlesWindowsSeparators) {
    // The drive letter must survive: ':' is a token boundary, so without the
    // drive-letter step-back the prefix would be `\Users\u\.xlings` and the
    // result would splice a second drive spec onto the surviving `C:`.
    EXPECT_EQ(norm("g++ --sysroot=C:\\Users\\u\\.xlings\\subos\\dev",
                   "C:\\Users\\u\\.xlings", "C:\\Users\\u\\.xlings\\subos\\default"),
              "g++ --sysroot=C:\\Users\\u\\.xlings\\subos\\default");
}

TEST(SubosPathNormalizeTest, LeavesTrailingSubosMarkerAlone) {
    // A path that ends at the marker has no name segment to replace.
    const std::string cmd = "tool --dir=/h/.xlings/subos/";
    EXPECT_EQ(norm(cmd, "/h/.xlings", "/h/.xlings/subos/b"), cmd);
}
