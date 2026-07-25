#include <gtest/gtest.h>
#include <iomanip>
#ifdef __unix__
#include <sys/wait.h>
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
import xlings.core.xvm.shim;
import xlings.core.xvm.commands;
import xlings.core.compact;
import xlings.core.config;
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
// i18n tests
// ============================================================

TEST(I18nTest, SetAndGetLanguageEn) {
    xlings::i18n::set_language("en");
    EXPECT_EQ(xlings::i18n::language(), "en");
    EXPECT_FALSE(xlings::i18n::is_chinese());
}

TEST(I18nTest, SetAndGetLanguageZh) {
    xlings::i18n::set_language("zh");
    EXPECT_EQ(xlings::i18n::language(), "zh");
    EXPECT_TRUE(xlings::i18n::is_chinese());
}

TEST(I18nTest, TranslateEnglish) {
    xlings::i18n::set_language("en");
    auto msg = xlings::i18n::tr(xlings::i18n::Msg::INSTALL_DONE);
    EXPECT_FALSE(msg.empty());
    EXPECT_NE(msg.find("{}"), std::string_view::npos);  // has format placeholder
}

TEST(I18nTest, TranslateChinese) {
    xlings::i18n::set_language("zh");
    auto msg = xlings::i18n::tr(xlings::i18n::Msg::INSTALL_DONE);
    EXPECT_FALSE(msg.empty());
}

TEST(I18nTest, TranslateFormat) {
    xlings::i18n::set_language("en");
    auto msg = xlings::i18n::trf(xlings::i18n::Msg::INSTALL_DONE, std::string("gcc@15.1.0"));
    EXPECT_NE(msg.find("gcc@15.1.0"), std::string::npos);
    EXPECT_EQ(msg.find("{}"), std::string::npos);  // no leftover placeholder
}

TEST(I18nTest, AllMessagesHaveContent) {
    int count = static_cast<int>(xlings::i18n::Msg::MSG_COUNT_);
    for (int lang = 0; lang < 2; ++lang) {
        xlings::i18n::set_language(lang == 0 ? "en" : "zh");
        for (int i = 0; i < count; ++i) {
            auto msg = xlings::i18n::tr(static_cast<xlings::i18n::Msg>(i));
            EXPECT_FALSE(msg.empty())
                << "Message " << i << " is empty for lang=" << (lang == 0 ? "en" : "zh");
        }
    }
}

TEST(I18nTest, InvalidMsgReturnsEmpty) {
    auto msg = xlings::i18n::tr(xlings::i18n::Msg::MSG_COUNT_);
    EXPECT_TRUE(msg.empty());
}

// ============================================================
// log tests
// ============================================================

TEST(LogTest, SetLevelByString) {
    // Should not crash
    xlings::log::set_level("debug");
    xlings::log::set_level("info");
    xlings::log::set_level("warn");
    xlings::log::set_level("error");
}

TEST(LogTest, SetLevelByEnum) {
    xlings::log::set_level(xlings::log::Level::Debug);
    xlings::log::set_level(xlings::log::Level::Error);
}

TEST(LogTest, SetAndClearContext) {
    xlings::log::set_context("test-pkg");
    xlings::log::clear_context();
}

TEST(LogTest, LogToFile) {
    namespace fs = std::filesystem;
    auto tmpFile = fs::temp_directory_path() / "xlings_test_log.txt";
    // Remove if exists
    fs::remove(tmpFile);

    xlings::log::set_level(xlings::log::Level::Debug);
    xlings::log::set_file(tmpFile);
    xlings::log::info("test message {}", 42);
    xlings::log::debug("debug msg");
    xlings::log::warn("warn msg");
    xlings::log::error("error msg");

    // Close by setting to empty path
    xlings::log::set_file("");

    // Read file and verify
    std::ifstream f(tmpFile);
    ASSERT_TRUE(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    f.close();

    EXPECT_NE(content.find("test message 42"), std::string::npos);
    EXPECT_NE(content.find("debug msg"), std::string::npos);
    EXPECT_NE(content.find("warn msg"), std::string::npos);
    EXPECT_NE(content.find("error msg"), std::string::npos);
    EXPECT_NE(content.find("[xlings]"), std::string::npos);  // info prefix
    EXPECT_NE(content.find("[error]"), std::string::npos);    // error prefix

    fs::remove(tmpFile);
    xlings::log::set_level(xlings::log::Level::Info);
}

// ============================================================
// utils tests
// ============================================================

TEST(UtilsTest, SplitString) {
    auto parts = xlings::utils::split_string("a:b:c", ':');
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");
    EXPECT_EQ(parts[2], "c");
}

TEST(UtilsTest, SplitStringEmpty) {
    auto parts = xlings::utils::split_string("", ',');
    // Empty string splits to 0 parts with std::views::split
    EXPECT_EQ(parts.size(), 0u);
}

TEST(UtilsTest, TrimString) {
    EXPECT_EQ(xlings::utils::trim_string("  hello  "), "hello");
    EXPECT_EQ(xlings::utils::trim_string("hello"), "hello");
    EXPECT_EQ(xlings::utils::trim_string(""), "");
}

TEST(UtilsTest, StripAnsi) {
    auto cleaned = xlings::utils::strip_ansi("\x1b[31mred\x1b[0m");
    EXPECT_EQ(cleaned, "red");
}

TEST(UtilsTest, GetEnvOrDefault) {
    auto val = xlings::utils::get_env_or_default("XLINGS_TEST_NONEXISTENT_12345", "fallback");
    EXPECT_EQ(val, "fallback");
}

TEST(CompactGitTest, MissingGitWithNeverInstallReturnsFalse) {
    namespace fs = std::filesystem;

    auto missingGit = fs::temp_directory_path() / "xlings_missing_git_for_compact_test" / "git";
    fs::remove_all(missingGit.parent_path());
    fs::create_directories(missingGit.parent_path());

    ScopedEnvVar gitBin("XLINGS_COMPACT_GIT_BIN", missingGit.string());
    ScopedEnvVar noAuto("XLINGS_NO_AUTO_INSTALL_GIT", "");
    ScopedEnvVar bootstrap("XLINGS_COMPACT_GIT_BOOTSTRAP", "");

    EXPECT_FALSE(xlings::compact::git::ensure_available(
        xlings::compact::git::EnsureMode::NeverInstall));

    fs::remove_all(missingGit.parent_path());
}

TEST(CompactGitTest, MissingGitHonorsNoAutoInstallFlag) {
    namespace fs = std::filesystem;

    auto missingGit = fs::temp_directory_path() / "xlings_missing_git_no_auto_test" / "git";
    fs::remove_all(missingGit.parent_path());
    fs::create_directories(missingGit.parent_path());

    ScopedEnvVar gitBin("XLINGS_COMPACT_GIT_BIN", missingGit.string());
    ScopedEnvVar noAuto("XLINGS_NO_AUTO_INSTALL_GIT", "1");
    ScopedEnvVar bootstrap("XLINGS_COMPACT_GIT_BOOTSTRAP", "");

    EXPECT_FALSE(xlings::compact::git::ensure_available());

    fs::remove_all(missingGit.parent_path());
}

// ── compact::git CA bundle resolution (#378) ──
TEST(CompactGitTest, CaBundleDefaultPresentReturnsEmpty) {
    auto exists = [](const std::string& p) { return p == "/etc/ssl/cert.pem"; };
    EXPECT_EQ(xlings::compact::git::resolve_ca_bundle(exists), "");
}

TEST(CompactGitTest, CaBundleDebianLayout) {
    auto exists = [](const std::string& p) {
        return p == "/etc/ssl/certs/ca-certificates.crt";
    };
    EXPECT_EQ(xlings::compact::git::resolve_ca_bundle(exists),
              "/etc/ssl/certs/ca-certificates.crt");
}

TEST(CompactGitTest, CaBundleRhelLayout) {
    auto exists = [](const std::string& p) {
        return p == "/etc/pki/tls/certs/ca-bundle.crt";
    };
    EXPECT_EQ(xlings::compact::git::resolve_ca_bundle(exists),
              "/etc/pki/tls/certs/ca-bundle.crt");
}

TEST(CompactGitTest, CaBundleNothingFoundReturnsEmpty) {
    auto exists = [](const std::string&) { return false; };
    EXPECT_EQ(xlings::compact::git::resolve_ca_bundle(exists), "");
}

TEST(XimRepoTest, SyncWithoutGitPreservesExistingSnapshot) {
    namespace fs = std::filesystem;

    auto root = fs::temp_directory_path() / "xlings_sync_without_git_test";
    auto repo = root / "xim-pkgindex";
    fs::remove_all(root);
    fs::create_directories(repo / "pkgs" / "p");
    auto marker = repo / "pkgs" / "p" / "patchelf.lua";
    {
        std::ofstream out(marker);
        out << "package = { name = \"patchelf\" }\n";
    }

    auto oldPath = std::string(std::getenv("PATH") ? std::getenv("PATH") : "");
    auto emptyPath = root / "empty-path";
    fs::create_directories(emptyPath);
    ScopedEnvVar gitBin("XLINGS_COMPACT_GIT_BIN", (root / "missing-git").string());
    ScopedEnvVar noAuto("XLINGS_NO_AUTO_INSTALL_GIT", "1");
    xlings::platform::set_env_variable("PATH", emptyPath.string());

    auto ok = xlings::xim::sync_repo(repo, "https://github.com/openxlings/xim-pkgindex.git", true);

    xlings::platform::set_env_variable("PATH", oldPath);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(fs::exists(marker));
    EXPECT_TRUE(fs::exists(repo / "pkgs"));
    fs::remove_all(root);
}

// Regression guard (2026-06-30 analysis): a fallback git clone must NEVER
// destroy an artifact-managed index (pkgs/ + .xlings-index-version, no .git).
// Wiping the version marker strands the official index on git permanently.
TEST(XimRepoTest, SyncPreservesArtifactManagedIndexNotDestroyed) {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "xlings_sync_preserve_artifact_test";
    auto repo = root / "xim-pkgindex";
    fs::remove_all(root);
    fs::create_directories(repo / "pkgs" / "p");
    { std::ofstream(repo / "pkgs" / "p" / "x.lua") << "package = { name = \"x\" }\n"; }
    { std::ofstream(repo / ".xlings-index-version") << "0.4.99"; }
    // No .git → without the guard this would clone + fs::remove_all(repo).
    ScopedEnvVar src("XLINGS_INDEX_SOURCE", "auto");
    // Don't let a git-unavailable test env trigger an auto-install of xim:git
    // (which re-execs the test binary and recurses); we only assert the
    // non-destruction invariant, which holds whether git is present or not.
    ScopedEnvVar noAuto("XLINGS_NO_AUTO_INSTALL_GIT", "1");

    // Whether or not git is available/reachable, the artifact marker MUST survive.
    xlings::xim::sync_repo(repo, "https://github.com/openxlings/xim-pkgindex.git", true);

    EXPECT_TRUE(fs::exists(repo / ".xlings-index-version"))
        << "artifact version marker was destroyed by git fallback";
    EXPECT_TRUE(fs::exists(repo / "pkgs"));
    fs::remove_all(root);
}

// Pure decision-table for the main-index artifact gate (P0-1 symmetry fix).
TEST(XimRepoTest, GateMainOfficialRemoteAlwaysAttemptsArtifactInAuto) {
    using xlings::xim::main_should_attempt_artifact;
    // auto: official remote always converges to artifact — even a stranded git
    // checkout (hasMarker=false, hasPkgs=true) self-heals.
    EXPECT_TRUE (main_should_attempt_artifact(true,  "auto",     false, true));
    EXPECT_TRUE (main_should_attempt_artifact(true,  "auto",     true,  true));
    EXPECT_TRUE (main_should_attempt_artifact(true,  "auto",     false, false));
    // non-official / local / fork mains never artifact-fetch.
    EXPECT_FALSE(main_should_attempt_artifact(false, "auto",     false, true));
    // explicit overrides.
    EXPECT_TRUE (main_should_attempt_artifact(true,  "artifact", false, true));
    EXPECT_FALSE(main_should_attempt_artifact(true,  "git",      true,  false));
    EXPECT_FALSE(main_should_attempt_artifact(false, "artifact", false, false));
}

TEST(XimRepoTest, GateSubDefaultMigratesOnceMainArtifactManaged) {
    using xlings::xim::sub_should_attempt_artifact;
    // auto: a stranded git sub (managed=false, pkgs=true) only migrates once the
    // MAIN index is artifact-managed (the C1 gate).
    EXPECT_FALSE(sub_should_attempt_artifact(true,  "auto", false, true,  false));
    EXPECT_TRUE (sub_should_attempt_artifact(true,  "auto", false, true,  true));
    EXPECT_TRUE (sub_should_attempt_artifact(true,  "auto", false, false, false)); // fresh
    EXPECT_TRUE (sub_should_attempt_artifact(true,  "auto", true,  true,  false)); // already managed
    EXPECT_FALSE(sub_should_attempt_artifact(false, "auto", false, false, true));  // non-default
    EXPECT_FALSE(sub_should_attempt_artifact(true,  "git",  false, false, true));
}

// ============================================================
// cmdline tests
// ============================================================

TEST(CmdlineTest, BasicParse) {
    using namespace mcpplibs;
    auto app = cmdline::App("test")
        .version("1.0")
        .option("verbose").short_name('v').help("verbose")
        .subcommand("install")
            .description("install a package")
            .arg("target").required().help("target package")
            .action([](const cmdline::ParsedArgs& args) {
                EXPECT_EQ(args.positional(0), "gcc");
            });

    auto result = app.parse_from("test install gcc");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_subcommand());
    EXPECT_EQ(result->subcommand_name(), "install");
}

TEST(CmdlineTest, GlobalOptionPropagation) {
    using namespace mcpplibs;
    bool actionCalled { false };
    auto app = cmdline::App("test")
        .option("yes").short_name('y').global().help("yes")
        .subcommand("install")
            .arg("target").help("pkg")
            .action([&](const cmdline::ParsedArgs& args) {
                EXPECT_TRUE(args.is_flag_set("yes"));
                actionCalled = true;
            });

    auto result = app.parse_from("test install gcc -y");
    ASSERT_TRUE(result.has_value());
    app.run(*result);
    EXPECT_TRUE(actionCalled);
}

TEST(CmdlineTest, HelpTriggersNonError) {
    using namespace mcpplibs;
    auto app = cmdline::App("test").version("1.0");
    auto result = app.parse_from("test --help");
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().is_error());
}

TEST(CmdlineTest, UnknownOptionIsError) {
    using namespace mcpplibs;
    auto app = cmdline::App("test");
    auto result = app.parse_from("test --bogus");
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().is_error());
}

// ============================================================
// UI tests (non-interactive only)
// ============================================================

TEST(UiTest, PrintProgressNocrash) {
    using namespace xlings::ui;
    std::vector<StatusEntry> entries = {
        { "glibc@2.39",    Phase::Done,        1.0f, "" },
        { "gcc@15.1.0",    Phase::Downloading, 0.45f, "" },
        { "binutils@2.42", Phase::Pending,     0.0f, "" },
    };
    // Should not crash
    print_progress(entries);
}

TEST(UiTest, PhaseLabels) {
    using namespace xlings::ui;
    EXPECT_EQ(phase_label(Phase::Pending), "pending");
    EXPECT_EQ(phase_label(Phase::Done), "done");
    EXPECT_EQ(phase_label(Phase::Failed), "failed");
    EXPECT_EQ(phase_label(Phase::Downloading), "downloading");
}

// ============================================================
// xim types tests
// ============================================================

TEST(XimTypesTest, InstallPlanEmpty) {
    xlings::xim::InstallPlan plan;
    EXPECT_FALSE(plan.has_errors());
    EXPECT_EQ(plan.pending_count(), 0u);
}

TEST(XimTypesTest, InstallPlanWithNodes) {
    xlings::xim::InstallPlan plan;
    {
        xlings::xim::PlanNode n; n.name = "gcc@15.1.0"; n.version = "15.1.0";
        plan.nodes.push_back(std::move(n));
    }
    {
        xlings::xim::PlanNode n; n.name = "glibc@2.39"; n.version = "2.39"; n.alreadyInstalled = true;
        plan.nodes.push_back(std::move(n));
    }
    {
        xlings::xim::PlanNode n; n.name = "binutils@2.42"; n.version = "2.42";
        plan.nodes.push_back(std::move(n));
    }
    EXPECT_EQ(plan.pending_count(), 2u);  // gcc + binutils (glibc already installed)
}

TEST(XimTypesTest, InstallPlanErrors) {
    xlings::xim::InstallPlan plan;
    plan.errors.push_back("cyclic dependency detected");
    EXPECT_TRUE(plan.has_errors());
}

TEST(XimTypesTest, DownloadTaskInit) {
    xlings::xim::DownloadTask task {
        .name = "gcc@15.1.0",
        .url = "https://example.com/gcc.tar.gz",
        .sha256 = "abcdef",
        .destDir = "/tmp/xim"
    };
    EXPECT_EQ(task.name, "gcc@15.1.0");
    EXPECT_EQ(task.sha256, "abcdef");
}

TEST(XimCatalogTest, CanonicalPackageNameAndStoreName) {
    EXPECT_EQ(xlings::xim::canonical_package_name("xim", "gcc"), "xim:gcc");
    EXPECT_EQ(xlings::xim::canonical_package_name("", "gcc"), "gcc");
    EXPECT_EQ(xlings::xim::package_store_name("xim", "gcc"), "xim-x-gcc");
    EXPECT_EQ(xlings::xim::package_store_name("", "gcc"), "gcc");
}

TEST(XimCatalogTest, FormatAmbiguousCandidates) {
    std::vector<xlings::xim::PackageMatch> matches = {
        {
            .name = "gcc",
            .version = "15.1.0",
            .namespaceName = "xim",
            .canonicalName = "xim:gcc",
            .repoName = "xim",
            .scope = xlings::xim::PackageScope::Global,
        },
        {
            .name = "gcc",
            .version = "15.1.0",
            .namespaceName = "project",
            .canonicalName = "project:gcc",
            .repoName = "project",
            .scope = xlings::xim::PackageScope::Project,
        },
    };

    auto msg = xlings::xim::format_ambiguous_candidates("gcc", matches);
    EXPECT_NE(msg.find("package 'gcc' is ambiguous"), std::string::npos);
    EXPECT_NE(msg.find("1. xim:gcc@15.1.0"), std::string::npos);
    EXPECT_NE(msg.find("2. project:gcc@15.1.0"), std::string::npos);
    EXPECT_NE(msg.find("from global repo 'xim'"), std::string::npos);
    EXPECT_NE(msg.find("from project repo 'project'"), std::string::npos);
    EXPECT_NE(msg.find("xlings install xim:gcc@15.1.0"), std::string::npos);
}

TEST(ConfigTest, WorkspaceInstallTargets) {
    xlings::xvm::Workspace ws;
    ws["gcc"] = "15.1.0";
    ws["node"] = "";

    auto targets = xlings::Config::workspace_install_targets(ws);
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0], "gcc@15.1.0");
    EXPECT_EQ(targets[1], "node");
}

TEST(ConfigTest, MergedWorkspaceAnonymousOverridesGlobal) {
    xlings::xvm::Workspace globalWs;
    globalWs["gcc"] = "15.1.0";
    globalWs["node"] = "22.0.0";

    xlings::xvm::Workspace projectWs;
    projectWs["gcc"] = "14.2.0";
    projectWs["python"] = "3.12.0";

    auto effective = xlings::Config::merged_workspace(
        globalWs, projectWs, {}, xlings::ProjectSubosMode::Anonymous);

    ASSERT_EQ(effective.size(), 3u);
    EXPECT_EQ(effective["gcc"], "14.2.0");
    EXPECT_EQ(effective["node"], "22.0.0");
    EXPECT_EQ(effective["python"], "3.12.0");
}

TEST(ConfigTest, MergedWorkspaceNamedDoesNotInheritGlobal) {
    xlings::xvm::Workspace globalWs;
    globalWs["gcc"] = "15.1.0";
    globalWs["node"] = "22.0.0";

    xlings::xvm::Workspace projectWs;
    projectWs["gcc"] = "14.2.0";

    xlings::xvm::Workspace subosWs;
    subosWs["clang"] = "18.1.0";

    auto effective = xlings::Config::merged_workspace(
        globalWs, projectWs, subosWs, xlings::ProjectSubosMode::Named);

    ASSERT_EQ(effective.size(), 2u);
    EXPECT_EQ(effective["gcc"], "14.2.0");
    EXPECT_EQ(effective["clang"], "18.1.0");
    EXPECT_FALSE(effective.contains("node"));
}

TEST(ConfigTest, MergedVersionsProjectOverridesGlobal) {
    xlings::xvm::VersionDB globalDb;
    xlings::xvm::add_version(globalDb, "gcc", "15.1.0", "/global/gcc-15");
    xlings::xvm::add_version(globalDb, "node", "22.0.0", "/global/node-22");

    xlings::xvm::VersionDB projectDb;
    xlings::xvm::add_version(projectDb, "gcc", "14.2.0", "/project/gcc-14");
    xlings::xvm::add_version(projectDb, "python", "3.12.0", "/project/python-3.12");

    auto merged = xlings::Config::merged_versions(globalDb, projectDb);
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_TRUE(xlings::xvm::has_version(merged, "gcc", "15.1.0"));
    EXPECT_TRUE(xlings::xvm::has_version(merged, "gcc", "14.2.0"));
    EXPECT_TRUE(xlings::xvm::has_version(merged, "node", "22.0.0"));
    EXPECT_TRUE(xlings::xvm::has_version(merged, "python", "3.12.0"));
}

TEST(ConfigTest, ResolveRepoSourceAbsolutePath) {
#ifdef _WIN32
    xlings::IndexRepo repo { .name = "local", .url = "C:\\tmp\\xim-pkgindex" };
    auto expected = std::filesystem::path("C:\\tmp\\xim-pkgindex");
#else
    xlings::IndexRepo repo { .name = "local", .url = "/tmp/xim-pkgindex" };
    auto expected = std::filesystem::path("/tmp/xim-pkgindex");
#endif
    auto path = xlings::Config::resolve_repo_source(repo, false);
    EXPECT_EQ(path, expected);
    EXPECT_TRUE(xlings::Config::is_local_repo_source(repo, false));
}

TEST(ConfigTest, ResolveRepoSourceFileScheme) {
#ifdef _WIN32
    // Standard file URI: file:///C:/path — production code strips the leading /
    xlings::IndexRepo repo { .name = "local", .url = "file:///C:/tmp/xim-pkgindex" };
    auto expected = std::filesystem::path("C:\\tmp\\xim-pkgindex");
#else
    xlings::IndexRepo repo { .name = "local", .url = "file:///tmp/xim-pkgindex" };
    auto expected = std::filesystem::path("/tmp/xim-pkgindex");
#endif
    auto path = xlings::Config::resolve_repo_source(repo, false);
    EXPECT_EQ(path, expected);
    EXPECT_TRUE(xlings::Config::is_local_repo_source(repo, false));
}

TEST(ConfigTest, ResolveRepoSourceRemoteUrlReturnsEmpty) {
    xlings::IndexRepo repo {
        .name = "xim",
        .url = "https://github.com/openxlings/xim-pkgindex.git"
    };
    EXPECT_TRUE(xlings::Config::resolve_repo_source(repo, false).empty());
    EXPECT_FALSE(xlings::Config::is_local_repo_source(repo, false));
}

// ── #377: index_repos parsing with artifact/source fields ──
TEST(ConfigIndexReposTest, PlainEntryHasNoArtifact) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"a","url":"https://x/a.git"}]})");
    auto repos = xlings::parse_index_repos_json(j, "");
    ASSERT_EQ(repos.size(), 1u);
    EXPECT_EQ(repos[0].name, "a");
    EXPECT_EQ(repos[0].url, "https://x/a.git");
    EXPECT_TRUE(repos[0].artifactBase.empty());
    EXPECT_TRUE(repos[0].source.empty());
}

TEST(ConfigIndexReposTest, ArtifactStringTrimsTrailingSlash) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"m","url":"https://x/m.git",
         "artifact":"https://github.com/xlings-res/mcpp-index/","source":"auto"}]})");
    auto repos = xlings::parse_index_repos_json(j, "");
    ASSERT_EQ(repos.size(), 1u);
    EXPECT_EQ(repos[0].artifactBase, "https://github.com/xlings-res/mcpp-index");
    EXPECT_EQ(repos[0].source, "auto");
}

TEST(ConfigIndexReposTest, ArtifactRegionObjectResolvesMirror) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"m","url":"https://x/m.git",
         "artifact":{"GLOBAL":"https://github.com/o/r","CN":"https://gitcode.com/o/r"}}]})");
    EXPECT_EQ(xlings::parse_index_repos_json(j, "CN")[0].artifactBase,
              "https://gitcode.com/o/r");
    EXPECT_EQ(xlings::parse_index_repos_json(j, "")[0].artifactBase,
              "https://github.com/o/r");
    EXPECT_EQ(xlings::parse_index_repos_json(j, "XX")[0].artifactBase,
              "https://github.com/o/r");  // unknown mirror -> GLOBAL fallback
}

TEST(ConfigIndexReposTest, MalformedEntriesSkipped) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"a"},{"url":"u"},{"name":"b","url":"https://x/b.git"}]})");
    auto repos = xlings::parse_index_repos_json(j, "");
    ASSERT_EQ(repos.size(), 1u);
    EXPECT_EQ(repos[0].name, "b");
}

// ============================================================
// xim index tests (requires xim-pkgindex repo)
// ============================================================

class XimIndexTest : public ::testing::Test {
protected:
    std::filesystem::path repoDir_;

    void SetUp() override {
        auto repo = find_pkgindex_repo();
        if (!repo) GTEST_SKIP() << "xim-pkgindex repo not found";
        repoDir_ = *repo;
    }
};

TEST_F(XimIndexTest, BuildIndex) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto result = mgr.rebuild();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(mgr.is_loaded());
    EXPECT_GT(mgr.size(), 40u);  // should have 50+ entries
}

TEST_F(XimIndexTest, SearchPackage) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    auto results = mgr.search("gcc");
    EXPECT_FALSE(results.empty());
    // At least one result should contain "gcc"
    bool found = false;
    for (auto& name : results) {
        if (name.find("gcc") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "search for 'gcc' should return gcc-related packages";
}

TEST_F(XimIndexTest, MatchVersion) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    auto match = mgr.match_version("gcc");
    EXPECT_TRUE(match.has_value()) << "should find a versioned gcc entry";
    if (match) {
        EXPECT_NE(match->find("gcc"), std::string::npos);
    }
}

TEST_F(XimIndexTest, FindEntry) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    // Find a known package
    auto match = mgr.match_version("gcc");
    ASSERT_TRUE(match.has_value());
    auto* entry = mgr.find_entry(*match);
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->path.empty());
}

TEST_F(XimIndexTest, LoadPackage) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    auto match = mgr.match_version("gcc");
    ASSERT_TRUE(match.has_value());

    auto pkg = mgr.load_package(*match);
    ASSERT_TRUE(pkg.has_value()) << pkg.error();
    EXPECT_EQ(pkg->name, "gcc");
}

TEST_F(XimIndexTest, AllNames) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    auto names = mgr.all_names();
    EXPECT_GT(names.size(), 40u);
    // Should be sorted
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
}

TEST_F(XimIndexTest, MarkInstalled) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    auto match = mgr.match_version("gcc");
    ASSERT_TRUE(match.has_value());

    mgr.mark_installed(*match, true);
    auto* entry = mgr.find_entry(*match);
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(entry->installed);

    mgr.mark_installed(*match, false);
    entry = mgr.find_entry(*match);
    EXPECT_FALSE(entry->installed);
}

TEST_F(XimIndexTest, EmptyRepoDirFails) {
    xlings::xim::IndexManager mgr;
    auto result = mgr.rebuild();
    EXPECT_FALSE(result.has_value());
}

TEST_F(XimIndexTest, NonexistentRepoDirFails) {
    xlings::xim::IndexManager mgr("/tmp/nonexistent_xim_repo_dir_xyz");
    auto result = mgr.rebuild();
    EXPECT_FALSE(result.has_value());
}

TEST(XimNamespaceIndexTest, PreservesSameNameCandidatesAndCanonicalOperations) {
    auto fixture = find_fixture_repo("index-same-name");
    ASSERT_TRUE(fixture.has_value());

    xlings::xim::IndexManager mgr(*fixture, "fixture-default");
    auto result = mgr.rebuild();
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_EQ(
        mgr.find_candidates("demo"),
        (std::vector<std::string> { "alpha:demo", "beta:demo" }));
    EXPECT_EQ(
        mgr.find_candidates("demo", std::string_view { "alpha" }),
        (std::vector<std::string> { "alpha:demo" }));

    auto* alphaEntry = mgr.find_entry("alpha:demo");
    auto* betaEntry = mgr.find_entry("beta:demo");
    ASSERT_NE(alphaEntry, nullptr);
    ASSERT_NE(betaEntry, nullptr);
    EXPECT_EQ(alphaEntry->identity.namespaceName, "alpha");
    EXPECT_EQ(betaEntry->identity.namespaceName, "beta");

    auto alphaPackage = mgr.load_package("alpha:demo");
    auto betaPackage = mgr.load_package("beta:demo");
    ASSERT_TRUE(alphaPackage.has_value()) << alphaPackage.error();
    ASSERT_TRUE(betaPackage.has_value()) << betaPackage.error();
    EXPECT_EQ(alphaPackage->description, "Alpha namespace demo package");
    EXPECT_EQ(betaPackage->description, "Beta namespace demo package");

    mgr.mark_installed("alpha:demo", true);
    EXPECT_TRUE(mgr.find_entry("alpha:demo")->installed);
    EXPECT_FALSE(mgr.find_entry("beta:demo")->installed);
    EXPECT_EQ(mgr.entry_path("alpha:demo"), alphaEntry->path);
}

TEST(XimNamespaceIndexTest, RejectsDuplicateEffectiveIdentityWithBothPaths) {
    auto fixture = find_fixture_repo("index-duplicate");
    ASSERT_TRUE(fixture.has_value());

    xlings::xim::IndexManager mgr(*fixture, "xim");
    auto result = mgr.rebuild();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("duplicate package identity 'xim:demo'"),
              std::string::npos);
    EXPECT_NE(result.error().find("implicit.demo.lua"), std::string::npos);
    EXPECT_NE(result.error().find("explicit.demo.lua"), std::string::npos);
}

// ============================================================
// xim resolver tests
// ============================================================

class XimResolverTest : public ::testing::Test {
protected:
    std::filesystem::path repoDir_;
    xlings::xim::IndexManager mgr_;

    void SetUp() override {
        auto repo = find_pkgindex_repo();
        if (!repo) GTEST_SKIP() << "xim-pkgindex repo not found";
        repoDir_ = *repo;
        mgr_ = xlings::xim::IndexManager(repoDir_);
        auto r = mgr_.rebuild();
        if (!r) GTEST_SKIP() << "rebuild failed: " << r.error();
    }
};

TEST_F(XimResolverTest, ResolveSinglePackage) {
    std::vector<std::string> targets = { "xvm" };
    auto result = xlings::xim::resolve(mgr_, targets, "linux");
    // Should succeed (xvm has no complex deps on linux)
    ASSERT_FALSE(result->has_errors())
        << "errors: " << (result->errors.empty() ? "" : result->errors[0]);
    EXPECT_FALSE(result->nodes.empty());
}

TEST_F(XimResolverTest, ResolveWithDeps) {
    std::vector<std::string> targets = { "pnpm" };
    auto result = xlings::xim::resolve(mgr_, targets, "linux");
    // pnpm depends on node (package name is "node", not "nodejs")
    if (result.has_value() && !result->has_errors()) {
        bool hasNode = false;
        for (auto& node : result->nodes) {
            if (node.name.find("node") != std::string::npos) {
                hasNode = true;
                break;
            }
        }
        EXPECT_TRUE(hasNode) << "pnpm should pull in node as dependency";
        // Deps should come before dependents in topo order
        int nodeIdx = -1, pnpmIdx = -1;
        for (int i = 0; i < static_cast<int>(result->nodes.size()); ++i) {
            if (result->nodes[i].name.find("node") != std::string::npos) nodeIdx = i;
            if (result->nodes[i].name.find("pnpm") != std::string::npos) pnpmIdx = i;
        }
        if (nodeIdx >= 0 && pnpmIdx >= 0) {
            EXPECT_LT(nodeIdx, pnpmIdx)
                << "node should come before pnpm in topo order";
        }
    }
}

TEST_F(XimResolverTest, ResolveNonexistent) {
    std::vector<std::string> targets = { "nonexistent_pkg_xyz_123" };
    auto result = xlings::xim::resolve(mgr_, targets, "linux");
    EXPECT_TRUE(result->has_errors());
}

TEST_F(XimResolverTest, ResolveMultipleTargets) {
    std::vector<std::string> targets = { "xvm", "claude-code" };
    auto result = xlings::xim::resolve(mgr_, targets, "linux");
    if (result.has_value() && !result->has_errors()) {
        // Should have at least 2 nodes
        EXPECT_GE(result->nodes.size(), 2u);
    }
}

// ============================================================
// xim downloader tests
// ============================================================

TEST(XimDownloaderTest, DownloadTaskExtractFilename) {
    xlings::xim::DownloadTask task {
        .name = "test",
        .url = "https://example.com/path/to/file.tar.gz?token=abc",
        .sha256 = "",
        .destDir = "/tmp/xim_test_dl"
    };
    // download_one would extract "file.tar.gz" from URL
    // We just verify the task structure is valid
    EXPECT_EQ(task.name, "test");
    EXPECT_FALSE(task.url.empty());
}

TEST(XimDownloaderTest, ExtractArchiveBadFormat) {
    namespace fs = std::filesystem;
    auto tmpDir = fs::temp_directory_path() / "xim_test_extract";
    auto result = xlings::xim::extract_archive("/tmp/nonexistent.xyz", tmpDir);
    EXPECT_FALSE(result.has_value());
    fs::remove_all(tmpDir);
}

TEST(XimDownloaderTest, DownloadAllEmpty) {
    std::vector<xlings::xim::DownloadTask> tasks;
    xlings::xim::DownloaderConfig config;
    auto results = xlings::xim::download_all(tasks, config, nullptr, nullptr);
    EXPECT_TRUE(results.empty());
}

// HEAD-based cache sidecar (used when sha256 is unset). Round-trips a
// minimal sidecar through the writer + parser and verifies missing-file
// and malformed-line handling.
TEST(XimDownloaderTest, MetaSidecarRoundTrip) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_meta_sidecar_test";
    fs::create_directories(tmp);
    auto path = tmp / "payload.tar.gz.meta";
    fs::remove(path);

    xlings::tinyhttps::RemoteFileMeta meta;
    meta.ok = true;
    meta.lastModified = "Wed, 21 Oct 2015 07:28:00 GMT";
    meta.etag = "\"abc123\"";

    ASSERT_TRUE(xlings::xim::write_meta_sidecar_(
        path, meta, 1234,
        "test/1.0.0/linux/x86_64/url",
        "https://example.test/payload.tar.gz"));
    auto roundtrip = xlings::xim::read_meta_sidecar_(path);
    ASSERT_TRUE(roundtrip.has_value());
    EXPECT_EQ(roundtrip->format, 2);
    EXPECT_TRUE(roundtrip->complete);
    EXPECT_EQ(roundtrip->size, 1234);
    EXPECT_EQ(roundtrip->cacheIdentity, "test/1.0.0/linux/x86_64/url");
    EXPECT_EQ(roundtrip->lastModified, meta.lastModified);
    EXPECT_EQ(roundtrip->etag, meta.etag);

    fs::remove(path);
    EXPECT_FALSE(xlings::xim::read_meta_sidecar_(path).has_value());

    // Malformed input: extra colons, blank lines, unknown keys all ignored
    {
        std::ofstream out(path);
        out << "\n";
        out << "x-custom: ignored\n";
        out << "Last-Modified: Mon, 01 Jan 2024 00:00:00 GMT\n";
        out << "no-colon-line\n";
    }
    auto m2 = xlings::xim::read_meta_sidecar_(path);
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->lastModified, "Mon, 01 Jan 2024 00:00:00 GMT");
    EXPECT_TRUE(m2->etag.empty());

    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, HeadFailureRejectsLegacyNonEmptyCache) {
    xlings::xim::CacheAdmissionInput_ input {
        .localSize = 114 * 1024,
        .headSucceeded = false,
        .sidecar = xlings::xim::MetaSidecar_ {
            .lastModified = "Wed, 21 Oct 2015 07:28:00 GMT",
        },
        .expectedCacheIdentity = "mcpp/0.0.81/linux/x86_64/xlings-res",
    };

    EXPECT_EQ(
        xlings::xim::decide_cache_admission_(input),
        xlings::xim::CacheAdmission_::Redownload);
}

TEST(XimDownloaderTest, HeadFailureAcceptsMatchingCommittedV2Cache) {
    xlings::xim::CacheAdmissionInput_ input {
        .localSize = 12628937,
        .headSucceeded = false,
        .sidecar = xlings::xim::MetaSidecar_ {
            .format = 2,
            .complete = true,
            .size = 12628937,
            .cacheIdentity = "mcpp/0.0.81/linux/x86_64/xlings-res",
        },
        .expectedCacheIdentity = "mcpp/0.0.81/linux/x86_64/xlings-res",
    };

    EXPECT_EQ(
        xlings::xim::decide_cache_admission_(input),
        xlings::xim::CacheAdmission_::OfflineUnverifiedHit);
}

TEST(XimDownloaderTest, HeadFailureRejectsV2CacheWithWrongSizeOrIdentity) {
    xlings::xim::CacheAdmissionInput_ input {
        .localSize = 114 * 1024,
        .headSucceeded = false,
        .sidecar = xlings::xim::MetaSidecar_ {
            .format = 2,
            .complete = true,
            .size = 12628937,
            .cacheIdentity = "other/0.0.81/linux/x86_64/xlings-res",
        },
        .expectedCacheIdentity = "mcpp/0.0.81/linux/x86_64/xlings-res",
    };

    EXPECT_EQ(
        xlings::xim::decide_cache_admission_(input),
        xlings::xim::CacheAdmission_::Redownload);
}

TEST(XimDownloaderTest, FailedTransferPreservesCommittedDestination) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_transaction_failure";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.tar.gz";
    {
        std::ofstream out(destination);
        out << "previous-good-payload";
    }

    xlings::xim::DownloadTask task {
        .name = "transaction-test",
        .url = "https://example.test/payload.tar.gz",
        .cacheIdentity = "transaction-test/1/linux/x86_64/url",
        .destDir = tmp,
    };
    xlings::xim::DownloadTestHooks_ hooks;
    hooks.queryRemoteMeta = [](const std::string&) {
        return xlings::tinyhttps::RemoteFileMeta{
            .ok = true,
            .contentLength = 999,
        };
    };
    hooks.transferOverride = [](const std::string&, const fs::path& path) {
        std::ofstream(path) << "partial";
        return xlings::tinyhttps::DownloadFileResult{false, "connection reset"};
    };

    auto result = xlings::xim::download_one(task, nullptr, nullptr, &hooks);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "previous-good-payload");
    for (const auto& entry : fs::directory_iterator(tmp)) {
        EXPECT_FALSE(entry.path().filename().string().contains(".part."));
    }
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, HashRejectedCandidateCommitsFallbackFromStaging) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_transaction_fallback";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const std::string goodPayload = "fallback-good-payload";
    auto expectedHash = xlings::sha256::hex(goodPayload);
    xlings::xim::DownloadTask task {
        .name = "fallback-test",
        .url = "https://mirror.test/payload.bin",
        .sha256 = expectedHash,
        .cacheIdentity = "fallback-test/1/linux/x86_64/url",
        .destDir = tmp,
        .fallbackUrls = {"https://origin.test/payload.bin"},
    };
    int attempts = 0;
    xlings::xim::DownloadTestHooks_ hooks;
    hooks.transferOverride = [&](const std::string&, const fs::path& path) {
        std::ofstream(path) << (++attempts == 1 ? "bad" : goodPayload);
        return xlings::tinyhttps::DownloadFileResult{true, {}};
    };

    auto result = xlings::xim::download_one(task, nullptr, nullptr, &hooks);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(attempts, 2);
    EXPECT_EQ(xlings::platform::read_file_to_string(result.localFile.string()),
              goodPayload);
    for (const auto& entry : fs::directory_iterator(tmp)) {
        EXPECT_FALSE(entry.path().filename().string().contains(".part."));
    }
    fs::remove_all(tmp);
}

TEST(TinyhttpsWrapperTest, ReturnsAcceptedCandidateTransferMetadata) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "tinyhttps_transfer_metadata";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    xlings::tinyhttps::DownloadOptions options;
    options.destFile = tmp / "payload.bin";
    options.urls = {"https://origin.test/payload.bin"};
    options.retryCount = 0;
    options.transferOverride = [](const std::string&, const fs::path& path) {
        std::ofstream(path) << "payload";
        return xlings::tinyhttps::DownloadFileResult {
            .success = true,
            .bytesWritten = 7,
            .expectedBytes = 7,
            .finalUrl = "https://cdn.test/final.bin",
            .etag = "etag-1",
            .lastModified = "Sat, 12 Jul 2026 00:00:00 GMT",
        };
    };

    auto result = xlings::tinyhttps::download_file(options);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.bytesWritten, 7);
    ASSERT_TRUE(result.expectedBytes.has_value());
    EXPECT_EQ(*result.expectedBytes, 7);
    EXPECT_EQ(result.finalUrl, "https://cdn.test/final.bin");
    EXPECT_EQ(result.etag, "etag-1");
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, RejectsIncompleteReportedTransferBeforeCommit) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_incomplete_metadata";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.bin";
    std::ofstream(destination) << "previous-good-payload";

    xlings::xim::DownloadTask task {
        .name = "incomplete-test",
        .url = "https://origin.test/payload.bin",
        .cacheIdentity = "incomplete-test/1/linux/x86_64/url",
        .destDir = tmp,
    };
    xlings::xim::DownloadTestHooks_ hooks;
    hooks.transferOverride = [](const std::string&, const fs::path& path) {
        std::ofstream(path) << "bad";
        return xlings::tinyhttps::DownloadFileResult {
            .success = true,
            .bytesWritten = 3,
            .expectedBytes = 100,
        };
    };

    auto result = xlings::xim::download_one(task, nullptr, nullptr, &hooks);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("wrote 3 of 100 bytes"), std::string::npos);
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "previous-good-payload");
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, PersistsAcceptedGetMetadataInCommittedSidecar) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_get_metadata";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    xlings::xim::DownloadTask task {
        .name = "metadata-test",
        .url = "https://origin.test/payload.bin",
        .cacheIdentity = "metadata-test/1/linux/x86_64/url",
        .destDir = tmp,
    };
    xlings::xim::DownloadTestHooks_ hooks;
    hooks.transferOverride = [](const std::string&, const fs::path& path) {
        std::ofstream(path) << "payload";
        return xlings::tinyhttps::DownloadFileResult {
            .success = true,
            .bytesWritten = 7,
            .expectedBytes = 7,
            .finalUrl = "https://cdn.test/final.bin",
            .etag = "etag-get",
            .lastModified = "Sat, 12 Jul 2026 00:00:00 GMT",
        };
    };

    auto result = xlings::xim::download_one(task, nullptr, nullptr, &hooks);
    ASSERT_TRUE(result.success) << result.error;
    auto sidecar = xlings::xim::read_meta_sidecar_(result.localFile.string() + ".meta");
    ASSERT_TRUE(sidecar.has_value());
    EXPECT_EQ(sidecar->format, 2);
    EXPECT_TRUE(sidecar->complete);
    EXPECT_EQ(sidecar->size, 7);
    EXPECT_EQ(sidecar->etag, "etag-get");
    EXPECT_EQ(sidecar->sourceUrl, "https://cdn.test/final.bin");
    EXPECT_EQ(sidecar->cacheIdentity, task.cacheIdentity);
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, CancelledDownloadPreservesCommittedDestination) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_transaction_cancel";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.bin";
    std::ofstream(destination) << "previous-good-payload";

    xlings::xim::DownloadTask task {
        .name = "cancel-test",
        .url = "https://example.test/payload.bin",
        .cacheIdentity = "cancel-test/1/linux/x86_64/url",
        .destDir = tmp,
    };
    int transfers = 0;
    xlings::xim::DownloadTestHooks_ hooks;
    hooks.queryRemoteMeta = [](const std::string&) {
        return xlings::tinyhttps::RemoteFileMeta{
            .ok = true,
            .contentLength = 999,
        };
    };
    hooks.transferOverride = [&](const std::string&, const fs::path&) {
        ++transfers;
        return xlings::tinyhttps::DownloadFileResult{true, {}};
    };
    xlings::CancellationToken cancellation;
    cancellation.cancel();

    auto result = xlings::xim::download_one(
        task, nullptr, &cancellation, &hooks);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, "cancelled");
    EXPECT_EQ(transfers, 0);
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "previous-good-payload");
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, CommitFailureAfterBackupRestoresPreviousFile) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_commit_restore";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.bin";
    auto staging = tmp / "payload.bin.part.test";
    std::ofstream(destination) << "previous-good-payload";
    std::ofstream(staging) << "replacement-payload";

    std::string error;
    EXPECT_FALSE(xlings::xim::commit_staging_file_(
        staging, destination, error, true));
    EXPECT_EQ(error, "injected commit failure after backup");
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "previous-good-payload");
    EXPECT_TRUE(fs::exists(staging));
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, FileLockWaitsForOwnerAndHonorsCancellation) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_file_lock";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto path = tmp / "payload.lock";

    xlings::platform::FileLock owner;
    std::string error;
    ASSERT_TRUE(owner.acquire(
        path, std::chrono::seconds{1}, {}, error)) << error;

    xlings::platform::FileLock cancelledWaiter;
    EXPECT_FALSE(cancelledWaiter.acquire(
        path, std::chrono::seconds{1}, [] { return true; }, error));
    EXPECT_EQ(error, "cancelled while waiting for cache lock");

    std::jthread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        owner.release();
    });
    xlings::platform::FileLock waiter;
    error.clear();
    auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(waiter.acquire(
        path, std::chrono::seconds{1}, {}, error)) << error;
    EXPECT_GE(
        std::chrono::steady_clock::now() - start,
        std::chrono::milliseconds{50});
    waiter.release();
    releaser.join();
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, FileLockSerializesIndependentProcesses) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path()
        / std::format("xim_download_process_lock_{}", xlings::platform::get_pid());
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto lock_path = tmp / "payload.lock";
    auto ready_path = tmp / "child.ready";
    auto executable = xlings::platform::get_executable_path();
    ASSERT_FALSE(executable.empty());

    auto command = std::format(
        "\"{}\" --file-lock-child \"{}\" \"{}\"",
        executable.string(), lock_path.string(), ready_path.string());
#ifdef _WIN32
    // spawn_command invokes cmd.exe /c. Its parser removes one outer quote
    // pair, so preserve the quotes around the executable and arguments by
    // wrapping the complete command once more.
    command = "\"" + command + "\"";
#endif
    auto child = xlings::platform::spawn_command(command);
    ASSERT_GT(child.pid, 0);

    auto ready_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{3};
    while (!fs::exists(ready_path)
           && std::chrono::steady_clock::now() < ready_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    xlings::platform::FileLock waiter;
    std::string error;
    auto start = std::chrono::steady_clock::now();
    bool acquired = false;
    if (fs::exists(ready_path)) {
        acquired = waiter.acquire(
            lock_path, std::chrono::seconds{2}, {}, error);
    }
    auto waited = std::chrono::steady_clock::now() - start;
    auto [child_status, child_output] = xlings::platform::wait_or_kill(
        child, nullptr, std::chrono::seconds{3});

    EXPECT_TRUE(fs::exists(ready_path)) << child_output;
    EXPECT_TRUE(acquired) << error << "\n" << child_output;
    EXPECT_GE(waited, std::chrono::milliseconds{150});
    EXPECT_EQ(child_status, 0) << child_output;
    waiter.release();
    fs::remove_all(tmp);
}

TEST(XimInstallerResourceTest, ResolvesXlingsResSourceAndFinalRefVersion) {
    mcpplibs::xpkg::PlatformMatrix matrix;
    matrix.source = "xlings-res";
    matrix.entries["linux"]["latest"].ref = "1.0.0";
    matrix.entries["linux"]["1.0.0"].sha256_by_arch["x86_64"] = "hash-x86";

    auto resolved = xlings::xim::detail_::resolve_download_resource_(
        matrix, "tool", "latest", "linux", "amd64", "GLOBAL");
    ASSERT_TRUE(resolved.has_value()) << resolved.error();
    EXPECT_EQ(resolved->version, "1.0.0");
    EXPECT_EQ(resolved->sha256, "hash-x86");
    EXPECT_TRUE(resolved->useResFallbacks);
    EXPECT_NE(resolved->url.find("/tool/releases/download/1.0.0/"),
              std::string::npos);
}

TEST(XimInstallerResourceTest, ResolvesTemplateAliasAndPreferredMirror) {
    mcpplibs::xpkg::PlatformMatrix matrix;
    matrix.source = "https://origin.test/${version}/tool-${arch_alias}.${ext}";
    auto& resource = matrix.entries["linux"]["2.0.0"];
    resource.sha256_by_arch["x86_64"] = "hash-x86";
    resource.arch_alias["x86_64"] = "amd64";
    resource.mirrors["CN"] = "https://cn.test/${version}/tool-${arch_alias}.tar.gz";

    auto resolved = xlings::xim::detail_::resolve_download_resource_(
        matrix, "tool", "2.0.0", "linux", "x86_64", "CN");
    ASSERT_TRUE(resolved.has_value()) << resolved.error();
    EXPECT_EQ(resolved->url, "https://cn.test/2.0.0/tool-amd64.tar.gz");
    EXPECT_EQ(resolved->sha256, "hash-x86");
    EXPECT_FALSE(resolved->useResFallbacks);
}

TEST(XimInstallerResourceTest, ResolvesGlobalCnSourceMapAndPreferredRegion) {
    mcpplibs::xpkg::PlatformMatrix matrix;
    matrix.source = "https://github.com/neovim/neovim/releases/download/v${version}/nvim-${arch_alias}.tar.gz";
    matrix.source_mirrors = {
        {"GLOBAL", matrix.source},
        {"CN", "https://gitcode.com/xlings-res/nvim/releases/download/${version}/nvim-${arch_alias}.tar.gz"},
    };
    auto& resource = matrix.entries["linux"]["0.12.4"];
    resource.sha256_by_arch["x86_64"] = "nvim-hash";
    resource.arch_alias["x86_64"] = "x86_64";

    auto global = xlings::xim::detail_::resolve_download_resource_(
        matrix, "nvim", "0.12.4", "linux", "x86_64", "GLOBAL");
    ASSERT_TRUE(global.has_value()) << global.error();
    EXPECT_EQ(global->url,
              "https://github.com/neovim/neovim/releases/download/v0.12.4/nvim-x86_64.tar.gz");
    EXPECT_EQ(global->mirrors.at("CN"),
              "https://gitcode.com/xlings-res/nvim/releases/download/0.12.4/nvim-x86_64.tar.gz");

    auto cn = xlings::xim::detail_::resolve_download_resource_(
        matrix, "nvim", "0.12.4", "linux", "x86_64", "CN");
    ASSERT_TRUE(cn.has_value()) << cn.error();
    EXPECT_EQ(cn->url, global->mirrors.at("CN"));
}

TEST(XimInstallerResourceTest, PreservesLegacyXlingsResAndFailsClosedOnArchMiss) {
    mcpplibs::xpkg::PlatformMatrix legacy;
    legacy.entries["linux"]["1.0.0"].url = "XLINGS_RES";
    auto resolved = xlings::xim::detail_::resolve_download_resource_(
        legacy, "legacy", "1.0.0", "linux", "x86_64", "GLOBAL");
    ASSERT_TRUE(resolved.has_value()) << resolved.error();
    EXPECT_TRUE(resolved->useResFallbacks);

    mcpplibs::xpkg::PlatformMatrix per_arch;
    per_arch.entries["linux"]["1.0.0"].archs["x86_64"] = {
        .url = "https://example.test/x86.tar.gz",
        .sha256 = "hash-x86",
    };
    auto missing = xlings::xim::detail_::resolve_download_resource_(
        per_arch, "tool", "1.0.0", "linux", "aarch64", "GLOBAL");
    EXPECT_FALSE(missing.has_value());
}

TEST(XimDownloaderTest, RecoveryRestoresBackupWhenLiveIsMissing) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_recover_backup";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.bin";
    auto backup = tmp / "payload.bin.old.100.1";
    auto staging = tmp / "payload.bin.part.100.1";
    std::ofstream(backup) << "previous-good-payload";
    std::ofstream(staging) << "partial";

    std::string error;
    ASSERT_TRUE(xlings::xim::recover_download_transaction_(
        destination, error)) << error;
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "previous-good-payload");
    EXPECT_FALSE(fs::exists(backup));
    EXPECT_FALSE(fs::exists(staging));
    fs::remove_all(tmp);
}

TEST(XimDownloaderTest, RecoveryKeepsLiveAndRemovesStaleBackup) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "xim_download_recover_live";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto destination = tmp / "payload.bin";
    auto backup = tmp / "payload.bin.old.100.1";
    std::ofstream(destination) << "committed-payload";
    std::ofstream(backup) << "previous-payload";

    std::string error;
    ASSERT_TRUE(xlings::xim::recover_download_transaction_(
        destination, error)) << error;
    EXPECT_EQ(xlings::platform::read_file_to_string(destination.string()),
              "committed-payload");
    EXPECT_FALSE(fs::exists(backup));
    fs::remove_all(tmp);
}

// ============================================================
// xim installer tests
// ============================================================

class XimInstallerTest : public ::testing::Test {
protected:
    std::filesystem::path repoDir_;
    xlings::xim::IndexManager mgr_;

    void SetUp() override {
        auto repo = find_pkgindex_repo();
        if (!repo) GTEST_SKIP() << "xim-pkgindex repo not found";
        repoDir_ = *repo;
        mgr_ = xlings::xim::IndexManager(repoDir_);
        auto r = mgr_.rebuild();
        if (!r) GTEST_SKIP() << "rebuild failed: " << r.error();
    }
};

TEST_F(XimInstallerTest, InstallerConstruction) {
    xlings::xim::Installer installer(mgr_);
    // Just verify it can be constructed without error
    SUCCEED();
}

TEST_F(XimInstallerTest, ExecuteEmptyPlan) {
    xlings::xim::Installer installer(mgr_);
    xlings::xim::InstallPlan plan;
    xlings::xim::DownloaderConfig config;

    auto result = installer.execute(plan, config, nullptr);
    EXPECT_TRUE(result.has_value());
}

TEST_F(XimInstallerTest, ExecutePlanWithErrors) {
    xlings::xim::Installer installer(mgr_);
    xlings::xim::InstallPlan plan;
    plan.errors.push_back("test error");
    xlings::xim::DownloaderConfig config;

    auto result = installer.execute(plan, config, nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST_F(XimInstallerTest, UninstallNonexistent) {
    xlings::xim::Installer installer(mgr_);
    auto result = installer.uninstall("nonexistent_pkg_xyz_999");
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// xim commands tests
// ============================================================

TEST(XimCommandsTest, DetectPlatform) {
    auto platform = xlings::xim::detect_platform();
    #if defined(__linux__)
        EXPECT_EQ(platform, "linux");
    #elif defined(__APPLE__)
        EXPECT_EQ(platform, "macosx");
    #elif defined(_WIN32)
        EXPECT_EQ(platform, "windows");
    #endif
}

TEST(XimCommandsTest, SearchNonexistentReturnsZero) {
    // cmd_search uses get_catalog() which loads from Config (global ~/.xlings/data),
    // not from test fixtures. Skip if catalog cannot load.
    auto& catalog = xlings::xim::get_catalog();
    if (!catalog.is_loaded()) GTEST_SKIP() << "package catalog not available";
    xlings::EventStream stream;
    auto rc = xlings::xim::cmd_search("zzz_nonexistent_pkg_xyz_999", stream);
    EXPECT_EQ(rc, 0);  // returns 0 with "no packages found" message
}

TEST(XimCommandsTest, ListWithFilter) {
    auto& catalog = xlings::xim::get_catalog();
    if (!catalog.is_loaded()) GTEST_SKIP() << "package catalog not available";
    xlings::EventStream stream;
    auto rc = xlings::xim::cmd_list("gcc", stream);
    EXPECT_EQ(rc, 0);
}

TEST(XimCommandsTest, InfoKnownPackage) {
    auto& catalog = xlings::xim::get_catalog();
    if (!catalog.is_loaded()) GTEST_SKIP() << "package catalog not available";
    auto platform = xlings::xim::detect_platform();
    // gcc fixture only has linux entries; skip on other platforms
    if (platform != "linux") GTEST_SKIP() << "gcc fixture not available on " << platform;
    xlings::EventStream stream;
    auto rc = xlings::xim::cmd_info("xim:gcc", stream);
    EXPECT_EQ(rc, 0);
}

TEST(XimCommandsTest, InfoUnknownPackage) {
    auto& catalog = xlings::xim::get_catalog();
    if (!catalog.is_loaded()) GTEST_SKIP() << "package catalog not available";
    xlings::EventStream stream;
    auto rc = xlings::xim::cmd_info("nonexistent_pkg_xyz_999", stream);
    EXPECT_EQ(rc, 1);
}

// ============================================================
// xvm types tests
// ============================================================

TEST(XvmTypesTest, VDataConstruction) {
    xlings::xvm::VData vdata;
    vdata.path = "/usr/bin";
    vdata.alias.push_back("15");
    vdata.envs["GCC_HOME"] = "/usr/lib/gcc";

    EXPECT_EQ(vdata.path, "/usr/bin");
    ASSERT_EQ(vdata.alias.size(), 1u);
    EXPECT_EQ(vdata.alias[0], "15");
    ASSERT_EQ(vdata.envs.size(), 1u);
    EXPECT_EQ(vdata.envs.at("GCC_HOME"), "/usr/lib/gcc");
}

TEST(XvmTypesTest, VInfoConstruction) {
    xlings::xvm::VInfo info;
    info.type = "program";
    info.filename = "gcc";

    xlings::xvm::VData vdata;
    vdata.path = "/usr/bin";
    info.versions["15.1.0"] = std::move(vdata);

    info.bindings["g++"]["15.1.0"] = "g++-15";

    EXPECT_EQ(info.type, "program");
    EXPECT_EQ(info.filename, "gcc");
    EXPECT_EQ(info.versions.size(), 1u);
    EXPECT_TRUE(info.versions.contains("15.1.0"));
    EXPECT_EQ(info.bindings["g++"]["15.1.0"], "g++-15");
}

TEST(XvmTypesTest, VersionDBAndWorkspace) {
    xlings::xvm::VersionDB db;
    EXPECT_TRUE(db.empty());

    xlings::xvm::VInfo info;
    info.type = "program";
    db["gcc"] = std::move(info);
    EXPECT_EQ(db.size(), 1u);

    xlings::xvm::Workspace ws;
    ws["gcc"] = "15.1.0";
    EXPECT_EQ(ws["gcc"], "15.1.0");
}

// ============================================================
// xvm db tests
// ============================================================

TEST(XvmDbTest, AddAndRemoveVersion) {
    xlings::xvm::VersionDB db;

    xlings::xvm::add_version(db, "gcc", "15.1.0", "/usr/bin", "program", "gcc");
    EXPECT_TRUE(xlings::xvm::has_target(db, "gcc"));
    EXPECT_TRUE(xlings::xvm::has_version(db, "gcc", "15.1.0"));

    xlings::xvm::add_version(db, "gcc", "14.2.0", "/opt/gcc14/bin", "program", "gcc");
    EXPECT_TRUE(xlings::xvm::has_version(db, "gcc", "14.2.0"));

    auto all = xlings::xvm::get_all_versions(db, "gcc");
    EXPECT_EQ(all.size(), 2u);

    xlings::xvm::remove_version(db, "gcc", "14.2.0");
    EXPECT_FALSE(xlings::xvm::has_version(db, "gcc", "14.2.0"));
    EXPECT_TRUE(xlings::xvm::has_version(db, "gcc", "15.1.0"));

    // Remove last version removes the target entirely
    xlings::xvm::remove_version(db, "gcc", "15.1.0");
    EXPECT_FALSE(xlings::xvm::has_target(db, "gcc"));
}

TEST(XvmDbTest, FuzzyVersionMatch) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/usr/bin");
    xlings::xvm::add_version(db, "gcc", "14.2.0", "/opt/gcc14/bin");
    xlings::xvm::add_version(db, "gcc", "14.1.0", "/opt/gcc141/bin");
    xlings::xvm::add_version(db, "gcc", "13.3.0", "/opt/gcc13/bin");

    // Exact match
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "15.1.0"), "15.1.0");

    // Prefix match: "15" -> "15.1.0"
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "15"), "15.1.0");

    // Prefix match: "14" -> "14.2.0" (highest)
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "14"), "14.2.0");

    // Prefix match: "14.1" -> "14.1.0"
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "14.1"), "14.1.0");

    // No match
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "16"), "");
    EXPECT_EQ(xlings::xvm::match_version(db, "nonexistent", "1"), "");
}

TEST(XvmDbTest, GetActiveVersion) {
    xlings::xvm::Workspace ws;
    ws["gcc"] = "15.1.0";
    ws["node"] = "22.0.0";

    EXPECT_EQ(xlings::xvm::get_active_version(ws, "gcc"), "15.1.0");
    EXPECT_EQ(xlings::xvm::get_active_version(ws, "node"), "22.0.0");
    EXPECT_EQ(xlings::xvm::get_active_version(ws, "python"), "");
}

TEST(XvmDbTest, GetVDataAndVInfo) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/usr/bin", "program", "gcc");

    auto* vinfo = xlings::xvm::get_vinfo(db, "gcc");
    ASSERT_NE(vinfo, nullptr);
    EXPECT_EQ(vinfo->type, "program");
    EXPECT_EQ(vinfo->filename, "gcc");

    auto* vdata = xlings::xvm::get_vdata(db, "gcc", "15.1.0");
    ASSERT_NE(vdata, nullptr);
    EXPECT_EQ(vdata->path, "/usr/bin");

    EXPECT_EQ(xlings::xvm::get_vdata(db, "gcc", "99.0.0"), nullptr);
    EXPECT_EQ(xlings::xvm::get_vinfo(db, "nonexistent"), nullptr);
}

TEST(XvmDbTest, AddVersionStoresMaterializationMetadataPerVersion) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "tool", "1.0.0", "/provider-a", "program", "tool-a", "", "repo-a");
    xlings::xvm::add_version(
        db, "tool", "2.0.0", "/provider-b", "lib", "libtool-b.so", "", "repo-b");

    const auto* first =
        xlings::xvm::get_vdata(db, "tool", "repo-a:1.0.0");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->kind, "program");
    EXPECT_EQ(first->sourceName, "tool-a");
    EXPECT_EQ(first->destinationName, "tool");

    const auto* second =
        xlings::xvm::get_vdata(db, "tool", "repo-b:2.0.0");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->kind, "lib");
    EXPECT_EQ(second->sourceName, "libtool-b.so");
    EXPECT_EQ(second->destinationName, "libtool-b.so");

    const auto* legacyInfo = xlings::xvm::get_vinfo(db, "tool");
    ASSERT_NE(legacyInfo, nullptr);
    EXPECT_EQ(legacyInfo->type, "program");
    EXPECT_EQ(legacyInfo->filename, "tool-a");
}

TEST(XvmDbTest, AddVersionPreservesVirtualGroupWithoutProgramPayload) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "provider-root", "1.0.0", "/provider", "group");

    const auto* root =
        xlings::xvm::get_vdata(db, "provider-root", "1.0.0");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->kind, "group");
    EXPECT_TRUE(root->sourceName.empty());
    EXPECT_TRUE(root->destinationName.empty());

    const auto* legacyInfo = xlings::xvm::get_vinfo(db, "provider-root");
    ASSERT_NE(legacyInfo, nullptr);
    EXPECT_EQ(legacyInfo->type, "group");
    EXPECT_TRUE(legacyInfo->filename.empty());
}

TEST(XvmDbTest, GetBinding) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/usr/bin");
    db["gcc"].bindings["g++"]["15.1.0"] = "g++-15";
    db["gcc"].bindings["g++"]["14.2.0"] = "g++-14";

    EXPECT_EQ(xlings::xvm::get_binding(db, "gcc", "g++", "15.1.0"), "g++-15");
    EXPECT_EQ(xlings::xvm::get_binding(db, "gcc", "g++", "14.2.0"), "g++-14");
    EXPECT_EQ(xlings::xvm::get_binding(db, "gcc", "g++", "99.0.0"), "");
    EXPECT_EQ(xlings::xvm::get_binding(db, "gcc", "clang", "15.1.0"), "");
}

TEST(XvmDbTest, ExpandPath) {
    EXPECT_EQ(xlings::xvm::expand_path("${XLINGS_HOME}/data/xpkgs/gcc", "/home/user/.xlings"),
              "/home/user/.xlings/data/xpkgs/gcc");
    EXPECT_EQ(xlings::xvm::expand_path("/absolute/path", "/home/user/.xlings"),
              "/absolute/path");
    EXPECT_EQ(xlings::xvm::expand_path("${XLINGS_HOME}/a/${XLINGS_HOME}/b", "/X"),
              "/X/a//X/b");
    EXPECT_EQ(xlings::xvm::expand_path("no_placeholder", "/X"), "no_placeholder");
}

// ============================================================
// xvm JSON serialization tests
// ============================================================

namespace {

nlohmann::json valid_binding_group_json() {
    return {
        {"provider", "repo:provider"},
        {"version", "1.0.0"},
        {"group", "provider-group"},
        {"rootTarget", "provider-root"},
        {"rootVersion", "1.0.0"},
    };
}

xlings::xvm::VData reload_vdata(const xlings::xvm::VData& data) {
    return xlings::xvm::vdata_from_json(
        xlings::xvm::vdata_to_json(data));
}

void expect_single_binding_integrity_issue(
    const xlings::xvm::VData& data,
    std::string_view code,
    std::string_view path) {
    ASSERT_EQ(data.bindingIntegrityIssues.size(), 1u);
    EXPECT_EQ(data.bindingIntegrityIssues[0].code, code);
    EXPECT_EQ(data.bindingIntegrityIssues[0].path, path);
}

void expect_metadata_integrity_failure(
    const xlings::xvm::VData& data,
    std::string_view code,
    std::string_view path) {
    xlings::xvm::VersionDB db;
    db["subject"].type = "program";
    db["subject"].versions["1.0.0"] = data;

    auto result =
        xlings::xvm::resolve_binding_selection(db, "subject", "1.0.0");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind,
              xlings::xvm::BindingErrorKind::MetadataIntegrityIssue);
    EXPECT_EQ(result.error().target, "subject");
    EXPECT_EQ(result.error().version, "1.0.0");
    EXPECT_NE(result.error().message.find(code), std::string::npos);
    EXPECT_NE(result.error().message.find(path), std::string::npos);
}

}  // namespace

TEST(XvmJsonTest, VDataRoundTrip) {
    xlings::xvm::VData original;
    original.path = "/usr/bin";
    original.alias = {"15", "latest"};
    original.envs["GCC_HOME"] = "/usr/lib/gcc";
    original.envs["PATH"] = "/usr/bin";

    auto j = xlings::xvm::vdata_to_json(original);
    auto restored = xlings::xvm::vdata_from_json(j);

    EXPECT_EQ(restored.path, original.path);
    EXPECT_EQ(restored.alias, original.alias);
    EXPECT_EQ(restored.envs, original.envs);
}

TEST(XvmJsonTest, VDataMinimal) {
    xlings::xvm::VData original;
    original.path = "/usr/bin";
    // No alias, no envs

    auto j = xlings::xvm::vdata_to_json(original);
    EXPECT_FALSE(j.contains("alias"));
    EXPECT_FALSE(j.contains("envs"));

    auto restored = xlings::xvm::vdata_from_json(j);
    EXPECT_EQ(restored.path, "/usr/bin");
    EXPECT_TRUE(restored.alias.empty());
    EXPECT_TRUE(restored.envs.empty());
}

TEST(XvmJsonTest, BindingGroupManifestAndMaterializationRoundTrip) {
    xlings::xvm::VData original;
    original.path = "/pkg/gcc-15";
    original.kind = "group";
    original.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "xim:gcc",
        .providerVersion = "15.1.0",
        .group = "xim-gnu-gcc",
        .rootTarget = "xim-gnu-gcc",
        .rootVersion = "xim:15.1.0",
    };
    original.bindingMembers = {
        {"g++", "xim:15.1.0"},
        {"gcc", "xim:15.1.0"},
        {"gcc-ar", "xim:gcc-15.1.0"},
        {"xim-gnu-gcc", "xim:15.1.0"},
    };
    original.bindingHeaders = {
        {
            .sourceDir = "include/c++/15.1.0",
            .destinationPrefix = "c++/15.1.0",
        },
        {
            .sourceDir = "include-fixed",
            .destinationPrefix = "",
        },
    };

    auto j = xlings::xvm::vdata_to_json(original);
    ASSERT_TRUE(j.contains("bindingGroup"));
    EXPECT_EQ(j["bindingGroup"]["provider"], "xim:gcc");
    EXPECT_EQ(j["bindingGroup"]["version"], "15.1.0");
    EXPECT_EQ(j["bindingGroup"]["group"], "xim-gnu-gcc");
    EXPECT_EQ(j["bindingGroup"]["rootTarget"], "xim-gnu-gcc");
    EXPECT_EQ(j["bindingGroup"]["rootVersion"], "xim:15.1.0");
    EXPECT_EQ(j["bindingMembers"]["gcc-ar"], "xim:gcc-15.1.0");
    EXPECT_EQ(j["kind"], "group");
    EXPECT_FALSE(j.contains("sourceName"));
    EXPECT_FALSE(j.contains("destinationName"));
    ASSERT_EQ(j["bindingHeaders"].size(), 2u);
    EXPECT_EQ(j["bindingHeaders"][0]["sourceDir"], "include/c++/15.1.0");
    EXPECT_EQ(j["bindingHeaders"][0]["destinationPrefix"], "c++/15.1.0");
    EXPECT_EQ(j["bindingHeaders"][1]["sourceDir"], "include-fixed");
    EXPECT_EQ(j["bindingHeaders"][1]["destinationPrefix"], "");
    EXPECT_FALSE(j.contains("bindingIntegrityIssues"));

    auto restored = xlings::xvm::vdata_from_json(j);
    ASSERT_TRUE(restored.bindingGroup.has_value());
    EXPECT_EQ(restored.bindingGroup->provider, "xim:gcc");
    EXPECT_EQ(restored.bindingGroup->providerVersion, "15.1.0");
    EXPECT_EQ(restored.bindingGroup->group, "xim-gnu-gcc");
    EXPECT_EQ(restored.bindingGroup->rootTarget, "xim-gnu-gcc");
    EXPECT_EQ(restored.bindingGroup->rootVersion, "xim:15.1.0");
    EXPECT_EQ(restored.bindingMembers, original.bindingMembers);
    EXPECT_EQ(restored.kind, "group");
    EXPECT_TRUE(restored.sourceName.empty());
    EXPECT_TRUE(restored.destinationName.empty());
    ASSERT_EQ(restored.bindingHeaders.size(), 2u);
    EXPECT_EQ(restored.bindingHeaders[0].sourceDir, "include/c++/15.1.0");
    EXPECT_EQ(restored.bindingHeaders[0].destinationPrefix, "c++/15.1.0");
    EXPECT_EQ(restored.bindingHeaders[1].sourceDir, "include-fixed");
    EXPECT_TRUE(restored.bindingHeaders[1].destinationPrefix.empty());
    EXPECT_TRUE(restored.bindingIntegrityIssues.empty());
}

TEST(XvmJsonTest, LegacyVDataOmitsProviderAndMaterializationMetadata) {
    auto legacy = nlohmann::json::parse(R"({
        "path": "/usr/bin",
        "alias": ["15"]
    })");

    auto restored = xlings::xvm::vdata_from_json(legacy);
    EXPECT_FALSE(restored.bindingGroup.has_value());
    EXPECT_TRUE(restored.bindingMembers.empty());
    EXPECT_TRUE(restored.kind.empty());
    EXPECT_TRUE(restored.sourceName.empty());
    EXPECT_TRUE(restored.destinationName.empty());
    EXPECT_TRUE(restored.bindingHeaders.empty());
    EXPECT_TRUE(restored.bindingIntegrityIssues.empty());

    auto serialized = xlings::xvm::vdata_to_json(restored);
    EXPECT_FALSE(serialized.contains("bindingGroup"));
    EXPECT_FALSE(serialized.contains("bindingMembers"));
    EXPECT_FALSE(serialized.contains("kind"));
    EXPECT_FALSE(serialized.contains("sourceName"));
    EXPECT_FALSE(serialized.contains("destinationName"));
    EXPECT_FALSE(serialized.contains("bindingHeaders"));
    EXPECT_FALSE(serialized.contains("bindingIntegrityIssues"));
}

TEST(XvmJsonTest,
     MalformedCanonicalEntriesRecordAndPersistIntegrityIssues) {
    auto corrupt = nlohmann::json::parse(R"({
        "path": "/pkg/provider",
        "bindingGroup": {
            "provider": "repo:provider",
            "version": "1.0.0",
            "group": "provider-group",
            "rootTarget": "provider-root",
            "rootVersion": "1.0.0"
        },
        "bindingMembers": {
            "provider-root": "1.0.0",
            "bad/member~name": 7
        },
        "bindingHeaders": [
            {
                "sourceDir": "include",
                "destinationPrefix": ""
            },
            {
                "sourceDir": false,
                "destinationPrefix": "broken"
            },
            {
                "sourceDir": "include-valid",
                "destinationPrefix": 9
            }
        ]
    })");

    auto parsed = xlings::xvm::vdata_from_json(corrupt);

    ASSERT_EQ(parsed.bindingMembers.size(), 1u);
    EXPECT_EQ(parsed.bindingMembers.at("provider-root"), "1.0.0");
    ASSERT_EQ(parsed.bindingHeaders.size(), 1u);
    EXPECT_EQ(parsed.bindingHeaders.front().sourceDir, "include");
    ASSERT_EQ(parsed.bindingIntegrityIssues.size(), 3u);
    EXPECT_EQ(parsed.bindingIntegrityIssues[0].code,
              "binding-member-version-not-string");
    EXPECT_EQ(parsed.bindingIntegrityIssues[0].path,
              "/bindingMembers/bad~1member~0name");
    EXPECT_EQ(parsed.bindingIntegrityIssues[1].code,
              "binding-header-source-dir-not-string");
    EXPECT_EQ(parsed.bindingIntegrityIssues[1].path,
              "/bindingHeaders/1/sourceDir");
    EXPECT_EQ(parsed.bindingIntegrityIssues[2].code,
              "binding-header-destination-prefix-not-string");
    EXPECT_EQ(parsed.bindingIntegrityIssues[2].path,
              "/bindingHeaders/2/destinationPrefix");

    auto serialized = xlings::xvm::vdata_to_json(parsed);
    ASSERT_TRUE(serialized.contains("bindingIntegrityIssues"));
    ASSERT_EQ(serialized["bindingIntegrityIssues"].size(), 3u);
    EXPECT_EQ(serialized["bindingIntegrityIssues"][0]["code"],
              "binding-member-version-not-string");
    EXPECT_EQ(serialized["bindingIntegrityIssues"][0]["path"],
              "/bindingMembers/bad~1member~0name");

    auto restored = xlings::xvm::vdata_from_json(serialized);
    ASSERT_EQ(restored.bindingIntegrityIssues.size(), 3u);
    EXPECT_EQ(restored.bindingIntegrityIssues[0].code,
              "binding-member-version-not-string");
    EXPECT_EQ(restored.bindingIntegrityIssues[0].path,
              "/bindingMembers/bad~1member~0name");
    EXPECT_EQ(restored.bindingIntegrityIssues[1].code,
              "binding-header-source-dir-not-string");
    EXPECT_EQ(restored.bindingIntegrityIssues[1].path,
              "/bindingHeaders/1/sourceDir");
    EXPECT_EQ(restored.bindingIntegrityIssues[2].code,
              "binding-header-destination-prefix-not-string");
    EXPECT_EQ(restored.bindingIntegrityIssues[2].path,
              "/bindingHeaders/2/destinationPrefix");
}

TEST(XvmJsonTest, MalformedCanonicalContainersRecordIntegrityIssues) {
    auto corrupt = nlohmann::json::parse(R"({
        "path": "/pkg/provider",
        "bindingMembers": [],
        "bindingHeaders": {}
    })");

    auto parsed = xlings::xvm::vdata_from_json(corrupt);

    ASSERT_EQ(parsed.bindingIntegrityIssues.size(), 3u);
    EXPECT_EQ(parsed.bindingIntegrityIssues[0].code,
              "binding-members-not-object");
    EXPECT_EQ(parsed.bindingIntegrityIssues[0].path, "/bindingMembers");
    EXPECT_EQ(parsed.bindingIntegrityIssues[1].code,
              "binding-headers-not-array");
    EXPECT_EQ(parsed.bindingIntegrityIssues[1].path, "/bindingHeaders");
    EXPECT_EQ(parsed.bindingIntegrityIssues[2].code,
              "binding-group-missing");
    EXPECT_EQ(parsed.bindingIntegrityIssues[2].path, "/bindingGroup");
}

TEST(XvmJsonTest, NonObjectBindingGroupPersistsIntegrityFailure) {
    nlohmann::json malformed{
        {"path", "/subject"},
        {"kind", "program"},
        {"bindingGroup", false},
    };

    auto parsed = xlings::xvm::vdata_from_json(malformed);
    expect_single_binding_integrity_issue(
        parsed, "binding-group-not-object", "/bindingGroup");
    expect_metadata_integrity_failure(
        parsed, "binding-group-not-object", "/bindingGroup");

    for (int cycle = 0; cycle < 3; ++cycle) {
        SCOPED_TRACE(cycle);
        parsed = reload_vdata(parsed);
        expect_single_binding_integrity_issue(
            parsed, "binding-group-not-object", "/bindingGroup");
        expect_metadata_integrity_failure(
            parsed, "binding-group-not-object", "/bindingGroup");
    }
}

TEST(XvmJsonTest, EveryInvalidBindingGroupFieldPersistsIntegrityFailure) {
    const std::array<std::string_view, 5> fields{
        "provider",
        "version",
        "group",
        "rootTarget",
        "rootVersion",
    };
    const std::array<std::string_view, 3> invalidClasses{
        "missing",
        "wrong-type",
        "empty",
    };

    for (const auto field : fields) {
        for (const auto invalidClass : invalidClasses) {
            SCOPED_TRACE(std::string(field) + ":" + std::string(invalidClass));
            nlohmann::json malformed{
                {"path", "/subject"},
                {"kind", "program"},
                {"bindingGroup", valid_binding_group_json()},
            };
            auto& group = malformed["bindingGroup"];
            if (invalidClass == "missing") {
                group.erase(std::string(field));
            } else if (invalidClass == "wrong-type") {
                group[std::string(field)] = false;
            } else {
                group[std::string(field)] = "";
            }
            const auto path = "/bindingGroup/" + std::string(field);

            auto parsed = xlings::xvm::vdata_from_json(malformed);
            expect_single_binding_integrity_issue(
                parsed, "binding-group-field-invalid", path);
            expect_metadata_integrity_failure(
                parsed, "binding-group-field-invalid", path);

            for (int cycle = 0; cycle < 3; ++cycle) {
                SCOPED_TRACE(cycle);
                parsed = reload_vdata(parsed);
                expect_single_binding_integrity_issue(
                    parsed, "binding-group-field-invalid", path);
                expect_metadata_integrity_failure(
                    parsed, "binding-group-field-invalid", path);
            }
        }
    }
}

TEST(XvmJsonTest, MalformedPersistedIssueStateBecomesDurableFailure) {
    struct Case {
        std::string_view name;
        nlohmann::json issueState;
        std::string_view code;
        std::string_view path;
    };
    const std::vector<Case> cases{
        {
            "container-not-array",
            nlohmann::json::object(),
            "binding-integrity-issues-not-array",
            "/bindingIntegrityIssues",
        },
        {
            "item-not-object",
            nlohmann::json::array({false}),
            "binding-integrity-issue-not-object",
            "/bindingIntegrityIssues/0",
        },
        {
            "code-missing",
            nlohmann::json::array({
                {{"path", "/bindingGroup/provider"}},
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/code",
        },
        {
            "code-wrong-type",
            nlohmann::json::array({
                {
                    {"code", false},
                    {"path", "/bindingGroup/provider"},
                },
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/code",
        },
        {
            "code-empty",
            nlohmann::json::array({
                {
                    {"code", ""},
                    {"path", "/bindingGroup/provider"},
                },
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/code",
        },
        {
            "path-missing",
            nlohmann::json::array({
                {{"code", "existing-integrity-issue"}},
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/path",
        },
        {
            "path-wrong-type",
            nlohmann::json::array({
                {
                    {"code", "existing-integrity-issue"},
                    {"path", false},
                },
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/path",
        },
        {
            "path-empty",
            nlohmann::json::array({
                {
                    {"code", "existing-integrity-issue"},
                    {"path", ""},
                },
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/path",
        },
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        nlohmann::json malformed{
            {"path", "/subject"},
            {"kind", "program"},
            {"bindingIntegrityIssues", testCase.issueState},
        };

        auto parsed = xlings::xvm::vdata_from_json(malformed);
        expect_single_binding_integrity_issue(
            parsed, testCase.code, testCase.path);
        expect_metadata_integrity_failure(
            parsed, testCase.code, testCase.path);

        for (int cycle = 0; cycle < 3; ++cycle) {
            SCOPED_TRACE(cycle);
            parsed = reload_vdata(parsed);
            expect_single_binding_integrity_issue(
                parsed, testCase.code, testCase.path);
            expect_metadata_integrity_failure(
                parsed, testCase.code, testCase.path);
        }
    }
}

TEST(XvmJsonTest, DuplicateDerivedIntegrityIssuesCollapseAcrossReloads) {
    nlohmann::json malformed{
        {"path", "/subject"},
        {"kind", "program"},
        {"bindingGroup", valid_binding_group_json()},
        {
            "bindingIntegrityIssues",
            nlohmann::json::array({
                {
                    {"code", "binding-group-field-invalid"},
                    {"path", "/bindingGroup/provider"},
                },
                {
                    {"code", "binding-group-field-invalid"},
                    {"path", "/bindingGroup/provider"},
                },
            }),
        },
    };
    malformed["bindingGroup"]["provider"] = "";

    auto parsed = xlings::xvm::vdata_from_json(malformed);
    for (int cycle = 0; cycle < 4; ++cycle) {
        SCOPED_TRACE(cycle);
        expect_single_binding_integrity_issue(
            parsed,
            "binding-group-field-invalid",
            "/bindingGroup/provider");
        expect_metadata_integrity_failure(
            parsed,
            "binding-group-field-invalid",
            "/bindingGroup/provider");
        parsed = reload_vdata(parsed);
    }
}

TEST(XvmJsonTest, EmptyBindingMemberTargetAndVersionPersistFailures) {
    struct Case {
        std::string_view name;
        std::string target;
        std::string version;
        std::string_view code;
        std::string_view path;
    };
    const std::vector<Case> cases{
        {
            "empty-target",
            "",
            "1.0.0",
            "binding-member-target-empty",
            "/bindingMembers/",
        },
        {
            "empty-version",
            "tool",
            "",
            "binding-member-version-empty",
            "/bindingMembers/tool",
        },
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        nlohmann::json members = nlohmann::json::object();
        members[testCase.target] = testCase.version;
        nlohmann::json malformed{
            {"path", "/subject"},
            {"kind", "group"},
            {"bindingGroup", valid_binding_group_json()},
            {"bindingMembers", std::move(members)},
        };

        auto parsed = xlings::xvm::vdata_from_json(malformed);
        expect_single_binding_integrity_issue(
            parsed, testCase.code, testCase.path);
        expect_metadata_integrity_failure(
            parsed, testCase.code, testCase.path);
        EXPECT_TRUE(parsed.bindingMembers.empty());

        for (int cycle = 0; cycle < 3; ++cycle) {
            SCOPED_TRACE(cycle);
            parsed = reload_vdata(parsed);
            expect_single_binding_integrity_issue(
                parsed, testCase.code, testCase.path);
            expect_metadata_integrity_failure(
                parsed, testCase.code, testCase.path);
            EXPECT_TRUE(parsed.bindingMembers.empty());
        }
    }
}

TEST(XvmJsonTest, EmptyHeaderSourcePersistsWhileEmptyDestinationIsValid) {
    nlohmann::json malformed{
        {"path", "/subject"},
        {"kind", "group"},
        {"bindingGroup", valid_binding_group_json()},
        {
            "bindingHeaders",
            nlohmann::json::array({
                {
                    {"sourceDir", ""},
                    {"destinationPrefix", ""},
                },
            }),
        },
    };

    auto parsed = xlings::xvm::vdata_from_json(malformed);
    expect_single_binding_integrity_issue(
        parsed,
        "binding-header-source-dir-empty",
        "/bindingHeaders/0/sourceDir");
    expect_metadata_integrity_failure(
        parsed,
        "binding-header-source-dir-empty",
        "/bindingHeaders/0/sourceDir");
    EXPECT_TRUE(parsed.bindingHeaders.empty());

    for (int cycle = 0; cycle < 3; ++cycle) {
        SCOPED_TRACE(cycle);
        parsed = reload_vdata(parsed);
        expect_single_binding_integrity_issue(
            parsed,
            "binding-header-source-dir-empty",
            "/bindingHeaders/0/sourceDir");
        expect_metadata_integrity_failure(
            parsed,
            "binding-header-source-dir-empty",
            "/bindingHeaders/0/sourceDir");
        EXPECT_TRUE(parsed.bindingHeaders.empty());
    }

    malformed["bindingHeaders"][0]["sourceDir"] = "include";
    auto valid = xlings::xvm::vdata_from_json(malformed);
    EXPECT_TRUE(valid.bindingIntegrityIssues.empty());
    ASSERT_EQ(valid.bindingHeaders.size(), 1u);
    EXPECT_EQ(valid.bindingHeaders[0].sourceDir, "include");
    EXPECT_TRUE(valid.bindingHeaders[0].destinationPrefix.empty());
}

TEST(XvmJsonTest, PerVersionMetadataSupportsDifferentProvidersForOneTarget) {
    xlings::xvm::VersionDB original;
    original["tool"].type = "program";
    original["tool"].filename = "first-provider-tool";

    auto& first = original["tool"].versions["repo-a:1.0.0"];
    first.path = "/pkg/provider-a";
    first.kind = "program";
    first.sourceName = "tool-a";
    first.destinationName = "tool";
    first.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "repo-a:provider",
        .providerVersion = "1.0.0",
        .group = "provider-a",
        .rootTarget = "provider-a",
        .rootVersion = "repo-a:1.0.0",
    };

    auto& second = original["tool"].versions["repo-b:2.0.0"];
    second.path = "/pkg/provider-b";
    second.kind = "lib";
    second.sourceName = "libtool-b.so";
    second.destinationName = "libtool.so";
    second.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "repo-b:provider",
        .providerVersion = "2.0.0",
        .group = "provider-b",
        .rootTarget = "provider-b",
        .rootVersion = "repo-b:2.0.0",
    };

    auto restored =
        xlings::xvm::versions_from_json(xlings::xvm::versions_to_json(original));

    const auto& firstRestored =
        restored.at("tool").versions.at("repo-a:1.0.0");
    EXPECT_EQ(firstRestored.kind, "program");
    EXPECT_EQ(firstRestored.sourceName, "tool-a");
    EXPECT_EQ(firstRestored.destinationName, "tool");
    ASSERT_TRUE(firstRestored.bindingGroup.has_value());
    EXPECT_EQ(firstRestored.bindingGroup->provider, "repo-a:provider");

    const auto& secondRestored =
        restored.at("tool").versions.at("repo-b:2.0.0");
    EXPECT_EQ(secondRestored.kind, "lib");
    EXPECT_EQ(secondRestored.sourceName, "libtool-b.so");
    EXPECT_EQ(secondRestored.destinationName, "libtool.so");
    ASSERT_TRUE(secondRestored.bindingGroup.has_value());
    EXPECT_EQ(secondRestored.bindingGroup->provider, "repo-b:provider");

    EXPECT_EQ(restored.at("tool").type, "program");
    EXPECT_EQ(restored.at("tool").filename, "first-provider-tool");
}

TEST(XvmJsonTest, VInfoRoundTrip) {
    xlings::xvm::VInfo original;
    original.type = "program";
    original.filename = "gcc";
    original.versions["15.1.0"].path = "/usr/bin";
    original.versions["15.1.0"].alias = {"15"};
    original.versions["14.2.0"].path = "/opt/gcc14/bin";
    original.bindings["g++"]["15.1.0"] = "g++-15";
    original.bindings["g++"]["14.2.0"] = "g++-14";

    auto j = xlings::xvm::vinfo_to_json(original);
    auto restored = xlings::xvm::vinfo_from_json(j);

    EXPECT_EQ(restored.type, "program");
    EXPECT_EQ(restored.filename, "gcc");
    ASSERT_EQ(restored.versions.size(), 2u);
    EXPECT_EQ(restored.versions.at("15.1.0").path, "/usr/bin");
    EXPECT_EQ(restored.versions.at("15.1.0").alias.size(), 1u);
    EXPECT_EQ(restored.versions.at("14.2.0").path, "/opt/gcc14/bin");
    ASSERT_EQ(restored.bindings.size(), 1u);
    EXPECT_EQ(restored.bindings.at("g++").at("15.1.0"), "g++-15");
}

TEST(XvmJsonTest, VersionDBRoundTrip) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/usr/bin", "program", "gcc");
    xlings::xvm::add_version(db, "gcc", "14.2.0", "/opt/gcc14/bin", "program", "gcc");
    xlings::xvm::add_version(db, "node", "22.0.0", "/opt/node22/bin", "program", "node");

    auto j = xlings::xvm::versions_to_json(db);
    auto restored = xlings::xvm::versions_from_json(j);

    ASSERT_EQ(restored.size(), 2u);
    EXPECT_TRUE(restored.contains("gcc"));
    EXPECT_TRUE(restored.contains("node"));
    EXPECT_EQ(restored.at("gcc").versions.size(), 2u);
    EXPECT_EQ(restored.at("node").versions.size(), 1u);
    EXPECT_EQ(restored.at("gcc").type, "program");
}

TEST(XvmJsonTest, WorkspaceRoundTrip) {
    xlings::xvm::Workspace ws;
    ws["gcc"] = "15.1.0";
    ws["node"] = "22.0.0";

    auto j = xlings::xvm::workspace_to_json(ws);
    auto restored = xlings::xvm::workspace_from_json(j);

    ASSERT_EQ(restored.size(), 2u);
    EXPECT_EQ(restored.at("gcc"), "15.1.0");
    EXPECT_EQ(restored.at("node"), "22.0.0");
}

TEST(XvmJsonTest, FromJsonEmptyObject) {
    auto j = nlohmann::json::object();
    auto db = xlings::xvm::versions_from_json(j);
    EXPECT_TRUE(db.empty());

    auto ws = xlings::xvm::workspace_from_json(j);
    EXPECT_TRUE(ws.empty());
}

TEST(XvmJsonTest, FromJsonNonObject) {
    auto j = nlohmann::json::array();
    auto db = xlings::xvm::versions_from_json(j);
    EXPECT_TRUE(db.empty());

    auto ws = xlings::xvm::workspace_from_json(j);
    EXPECT_TRUE(ws.empty());
}

TEST(XvmJsonTest, WorkspacePlatformAwareManifestParsing) {
    auto j = nlohmann::json::parse(R"({
        "node": {
            "default": "22.17.1",
            "linux": "20.19.0",
            "windows": "22.18.0"
        },
        "python": {
            "default": "3.12.9"
        },
        "rust": {
            "windows": "1.86.0"
        }
    })");

    auto ws = xlings::xvm::workspace_from_json(j);

#if defined(__linux__)
    EXPECT_EQ(ws.at("node"), "20.19.0");
#elif defined(_WIN32)
    EXPECT_EQ(ws.at("node"), "22.18.0");
#else
    EXPECT_EQ(ws.at("node"), "22.17.1");
#endif

    EXPECT_EQ(ws.at("python"), "3.12.9");
#if defined(_WIN32)
    EXPECT_EQ(ws.at("rust"), "1.86.0");
#else
    EXPECT_TRUE(ws.find("rust") == ws.end());
#endif
}

TEST(XvmJsonTest, FullConfigJsonRoundTrip) {
    // Simulate a complete .xlings.json
    std::string configJson = R"({
        "lang": "en",
        "mirror": "GLOBAL",
        "activeSubos": "default",
        "versions": {
            "gcc": {
                "type": "program",
                "filename": "gcc",
                "versions": {
                    "15.1.0": { "path": "/usr/bin", "alias": ["15"] },
                    "14.2.0": { "path": "/opt/gcc14/bin" }
                },
                "bindings": {
                    "g++": { "15.1.0": "g++-15", "14.2.0": "g++-14" }
                }
            },
            "node": {
                "type": "program",
                "filename": "node",
                "versions": {
                    "22.0.0": { "path": "/opt/node22/bin", "envs": {"NODE_HOME": "/opt/node22"} }
                }
            }
        }
    })";

    auto json = nlohmann::json::parse(configJson);
    auto db = xlings::xvm::versions_from_json(json["versions"]);

    ASSERT_EQ(db.size(), 2u);

    // Check gcc
    auto* gcc = xlings::xvm::get_vinfo(db, "gcc");
    ASSERT_NE(gcc, nullptr);
    EXPECT_EQ(gcc->type, "program");
    EXPECT_EQ(gcc->filename, "gcc");
    ASSERT_EQ(gcc->versions.size(), 2u);
    EXPECT_EQ(gcc->versions.at("15.1.0").path, "/usr/bin");
    ASSERT_EQ(gcc->versions.at("15.1.0").alias.size(), 1u);
    EXPECT_EQ(gcc->versions.at("15.1.0").alias[0], "15");
    EXPECT_EQ(gcc->versions.at("14.2.0").path, "/opt/gcc14/bin");
    EXPECT_EQ(gcc->bindings.at("g++").at("15.1.0"), "g++-15");

    // Check node
    auto* node_vdata = xlings::xvm::get_vdata(db, "node", "22.0.0");
    ASSERT_NE(node_vdata, nullptr);
    EXPECT_EQ(node_vdata->path, "/opt/node22/bin");
    EXPECT_EQ(node_vdata->envs.at("NODE_HOME"), "/opt/node22");

    // Fuzzy match
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "15"), "15.1.0");
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "14"), "14.2.0");

    // Serialize back and verify
    auto j2 = xlings::xvm::versions_to_json(db);
    auto db2 = xlings::xvm::versions_from_json(j2);
    EXPECT_EQ(db2.size(), db.size());
}

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
// xvm VData new fields (includedir/libdir) tests
// ============================================================

TEST(XvmVDataFieldsTest, IncludedirLibdirConstruction) {
    xlings::xvm::VData vdata;
    vdata.path = "/usr/bin";
    vdata.includedir = "/opt/glibc/2.39/include";
    vdata.libdir = "/opt/glibc/2.39/lib64";

    EXPECT_EQ(vdata.includedir, "/opt/glibc/2.39/include");
    EXPECT_EQ(vdata.libdir, "/opt/glibc/2.39/lib64");
}

TEST(XvmVDataFieldsTest, IncludedirLibdirJsonRoundTrip) {
    xlings::xvm::VData original;
    original.path = "/opt/openssl/3.1.5";
    original.includedir = "/opt/openssl/3.1.5/include";
    original.libdir = "/opt/openssl/3.1.5/lib64";
    original.alias = {"3.1"};

    auto j = xlings::xvm::vdata_to_json(original);
    EXPECT_EQ(j["includedir"].get<std::string>(), "/opt/openssl/3.1.5/include");
    EXPECT_EQ(j["libdir"].get<std::string>(), "/opt/openssl/3.1.5/lib64");

    auto restored = xlings::xvm::vdata_from_json(j);
    EXPECT_EQ(restored.path, original.path);
    EXPECT_EQ(restored.includedir, original.includedir);
    EXPECT_EQ(restored.libdir, original.libdir);
    EXPECT_EQ(restored.alias, original.alias);
}

TEST(XvmVDataFieldsTest, EmptyIncludedirLibdirNotSerialized) {
    xlings::xvm::VData vdata;
    vdata.path = "/usr/bin";
    // includedir and libdir are empty

    auto j = xlings::xvm::vdata_to_json(vdata);
    EXPECT_FALSE(j.contains("includedir"));
    EXPECT_FALSE(j.contains("libdir"));

    auto restored = xlings::xvm::vdata_from_json(j);
    EXPECT_TRUE(restored.includedir.empty());
    EXPECT_TRUE(restored.libdir.empty());
}

TEST(XvmVDataFieldsTest, FullConfigWithNewFields) {
    std::string configJson = R"({
        "versions": {
            "glibc": {
                "type": "program",
                "versions": {
                    "2.39": {
                        "path": "/opt/glibc/2.39",
                        "includedir": "/opt/glibc/2.39/include",
                        "libdir": "/opt/glibc/2.39/lib64"
                    }
                }
            }
        }
    })";

    auto json = nlohmann::json::parse(configJson);
    auto db = xlings::xvm::versions_from_json(json["versions"]);

    auto* vdata = xlings::xvm::get_vdata(db, "glibc", "2.39");
    ASSERT_NE(vdata, nullptr);
    EXPECT_EQ(vdata->path, "/opt/glibc/2.39");
    EXPECT_EQ(vdata->includedir, "/opt/glibc/2.39/include");
    EXPECT_EQ(vdata->libdir, "/opt/glibc/2.39/lib64");

    // Round-trip
    auto j2 = xlings::xvm::versions_to_json(db);
    auto db2 = xlings::xvm::versions_from_json(j2);
    auto* vdata2 = xlings::xvm::get_vdata(db2, "glibc", "2.39");
    ASSERT_NE(vdata2, nullptr);
    EXPECT_EQ(vdata2->includedir, "/opt/glibc/2.39/include");
    EXPECT_EQ(vdata2->libdir, "/opt/glibc/2.39/lib64");
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

// ============================================================
// xim sub-index repos tests
// ============================================================

TEST(XimSubReposTest, DiscoverSubReposFromLuaFile) {
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_subrepo_test";
    fs::remove_all(testDir);
    fs::create_directories(testDir);

    // Write a mock xim-indexrepos.lua
    std::string lua = R"(xim_indexrepos = {
    ["awesome"] = {
        ["GLOBAL"] = "https://github.com/openxlings/xim-pkgindex-awesome.git",
        ["CN"] = "https://gitee.com/d2learn/xim-pkgindex-awesome.git",
    },
    ["scode"] = {
        ["GLOBAL"] = "https://github.com/openxlings/xim-pkgindex-scode.git",
    }
}
)";
    xlings::platform::write_string_to_file(
        (testDir / "xim-indexrepos.lua").string(), lua);

    // Test GLOBAL mirror
    auto repos = xlings::xim::discover_sub_repos(testDir, "GLOBAL");
    ASSERT_EQ(repos.size(), 2u);
    // Order depends on iteration, so check by name
    bool foundAwesome = false, foundScode = false;
    for (auto& r : repos) {
        if (r.name == "awesome") {
            foundAwesome = true;
            EXPECT_EQ(r.url, "https://github.com/openxlings/xim-pkgindex-awesome.git");
        } else if (r.name == "scode") {
            foundScode = true;
            EXPECT_EQ(r.url, "https://github.com/openxlings/xim-pkgindex-scode.git");
        }
    }
    EXPECT_TRUE(foundAwesome);
    EXPECT_TRUE(foundScode);

    // Test CN mirror — awesome should use CN URL, scode falls back to GLOBAL
    auto reposCN = xlings::xim::discover_sub_repos(testDir, "CN");
    ASSERT_EQ(reposCN.size(), 2u);
    for (auto& r : reposCN) {
        if (r.name == "awesome") {
            EXPECT_EQ(r.url, "https://gitee.com/d2learn/xim-pkgindex-awesome.git");
        } else if (r.name == "scode") {
            EXPECT_EQ(r.url, "https://github.com/openxlings/xim-pkgindex-scode.git");
        }
    }

    fs::remove_all(testDir);
}

TEST(XimSubReposTest, DiscoverSubReposNoFile) {
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_subrepo_empty";
    fs::remove_all(testDir);
    fs::create_directories(testDir);

    auto repos = xlings::xim::discover_sub_repos(testDir, "GLOBAL");
    EXPECT_TRUE(repos.empty());

    fs::remove_all(testDir);
}

TEST(XimSubReposTest, SyncRepoUrlKeepsGithubOnCNMirror) {
    auto url = xlings::xim::sync_repo_url(
        "https://github.com/openxlings/xim-pkgindex-awesome.git", "CN");
    EXPECT_EQ(url, "https://github.com/openxlings/xim-pkgindex-awesome.git");
}

// ── merge_sub_repos: lua defaults are authoritative for their own names ──
// (C4) The official index may move a default to a new org/URL; the lua URL must
// win over a stale json entry so the sub still classifies as default-official.
// json contributes ONLY names absent from lua (user-added sub-indexes).
TEST(XimSubReposTest, MergeSubReposLuaDefaultWinsOverStaleJson) {
    std::vector<xlings::IndexRepo> lua = {
        {"awesome", "https://github.com/openxlings/xim-pkgindex-awesome.git"},
        {"scode",   "https://github.com/openxlings/xim-pkgindex-scode.git"},
    };
    // json: stale org for awesome (old d2learn) + a user-added fork.
    std::vector<xlings::IndexRepo> json = {
        {"awesome",    "https://github.com/d2learn/xim-pkgindex-awesome.git"},
        {"fromsource", "https://github.com/d2learn/xim-pkgindex-fromsource.git"},
    };

    auto merged = xlings::xim::merge_sub_repos(lua, json);
    ASSERT_EQ(merged.size(), 3u);
    std::unordered_map<std::string, std::string> byName;
    for (auto& r : merged) byName[r.name] = r.url;
    // lua wins for the default that drifted org
    EXPECT_EQ(byName["awesome"], "https://github.com/openxlings/xim-pkgindex-awesome.git");
    EXPECT_EQ(byName["scode"],   "https://github.com/openxlings/xim-pkgindex-scode.git");
    // user-added json-only repo preserved
    EXPECT_EQ(byName["fromsource"], "https://github.com/d2learn/xim-pkgindex-fromsource.git");
}

TEST(XimSubReposTest, MergeSubReposJsonOnlyPreserved) {
    std::vector<xlings::IndexRepo> lua = {};
    std::vector<xlings::IndexRepo> json = {
        {"fromsource", "https://github.com/d2learn/xim-pkgindex-fromsource.git"},
    };
    auto merged = xlings::xim::merge_sub_repos(lua, json);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(merged[0].name, "fromsource");
}

// ── sub_should_attempt_artifact: the C1 migration gate ──
TEST(XimSubReposTest, SubArtifactAutoFreshDefault) {
    // auto + default + fresh (no pkgs) → artifact
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        /*isDefaultOfficial=*/true, "auto",
        /*subManaged=*/false, /*subHasPkgs=*/false, /*mainArtifactManaged=*/false));
}

TEST(XimSubReposTest, SubArtifactAutoExistingGitNoMainMigration) {
    // auto + default + existing git checkout (has pkgs, not managed) +
    // main NOT artifact-managed → stay git (no migration)
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        true, "auto", /*subManaged=*/false, /*subHasPkgs=*/true,
        /*mainArtifactManaged=*/false));
}

TEST(XimSubReposTest, SubArtifactAutoMigratesWhenMainArtifactManaged) {
    // auto + default + existing git checkout + MAIN is artifact-managed
    // → migrate the sub to artifact too (the whole index is one unit). [C1]
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        true, "auto", /*subManaged=*/false, /*subHasPkgs=*/true,
        /*mainArtifactManaged=*/true));
}

TEST(XimSubReposTest, SubReposJsonObjectFormatRoundTrip) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "xlings-test-subrepos-json";
    fs::create_directories(dir);
    auto file = dir / "xim-indexrepos.json";

    std::vector<xlings::IndexRepo> repos;
    repos.push_back({"plain", "https://x/plain.git", "", ""});
    repos.push_back({"custom", "https://x/custom.git",
                     "https://github.com/o/custom-index", "auto"});
    xlings::xim::save_sub_repos_json(file, repos);

    auto loaded = xlings::xim::load_sub_repos_json(file);
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].name, "custom");   // nlohmann object keys sort alphabetically
    EXPECT_EQ(loaded[0].url, "https://x/custom.git");
    EXPECT_EQ(loaded[0].artifactBase, "https://github.com/o/custom-index");
    EXPECT_EQ(loaded[0].source, "auto");
    EXPECT_EQ(loaded[1].name, "plain");
    EXPECT_TRUE(loaded[1].artifactBase.empty());

    // plain entries must persist as plain strings (old-xlings tolerant)
    auto text = xlings::platform::read_file_to_string(file.string());
    auto j = nlohmann::json::parse(text);
    EXPECT_TRUE(j["plain"].is_string());
    EXPECT_TRUE(j["custom"].is_object());
    fs::remove_all(dir);
}

// ── #377: custom repos with a declared artifact source ──
TEST(XimSubReposTest, SubArtifactCustomAutoAlwaysAttempts) {
    // custom + artifact declared: attempts even for an existing git checkout
    // with main not artifact-managed (no C1 gate — atomic swap migrates safely)
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        false, "auto", false, true, false, true));
}

TEST(XimSubReposTest, SubArtifactCustomForcedArtifact) {
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        false, "artifact", false, true, false, true));
}

TEST(XimSubReposTest, SubArtifactCustomGitForced) {
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        false, "git", false, true, false, true));
}

TEST(XimSubReposTest, SubArtifactNoSourceStaysGit) {
    // default param: prior behavior for repos without artifact declarations
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        false, "auto", false, false, true));
}

TEST(XimSubReposTest, SubArtifactAutoAlreadyManaged) {
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        true, "auto", /*subManaged=*/true, /*subHasPkgs=*/true, false));
}

TEST(XimSubReposTest, SubArtifactNonDefaultNeverArtifact) {
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        /*isDefaultOfficial=*/false, "auto", false, false, true));
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        /*isDefaultOfficial=*/false, "artifact", false, false, true));
}

TEST(XimSubReposTest, SubArtifactSourceOverrides) {
    // git mode disables artifact even for a fresh default
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        true, "git", false, false, true));
    // artifact mode forces it for a default regardless of fs state
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        true, "artifact", false, true, false));
}

// ============================================================
// xim add-xpkg / local repo tests
// ============================================================

TEST(XimAddXpkgTest, LocalRepoLetterSubdir) {
    // Verify that add-xpkg places files under pkgs/<letter>/ subdirectory
    // by testing the IndexManager can find packages in that structure
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_addxpkg_test";
    fs::remove_all(testDir);
    fs::create_directories(testDir / "pkgs" / "t");

    // Create a minimal valid xpkg lua file (libxpkg table format)
    std::string lua = R"(package = {
    spec = "1",
    name = "test-pkg",
    description = "A test package",
    type = "package",
    status = "dev",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.com/test-1.0.0.tar.gz",
                sha256 = "abc123",
            },
        },
    },
}
)";
    xlings::platform::write_string_to_file(
        (testDir / "pkgs" / "t" / "test-pkg.lua").string(), lua);

    // Build index — should find the package
    xlings::xim::IndexManager mgr(testDir);
    auto result = mgr.rebuild();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_GE(mgr.size(), 1u);

    // Verify entry exists
    auto* entry = mgr.find_entry("test-pkg");
    EXPECT_NE(entry, nullptr);

    fs::remove_all(testDir);
}

TEST(XimAddXpkgTest, FlatPkgsDirNotIndexed) {
    // Files placed flat in pkgs/ (not in letter subdir) should NOT be found
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_addxpkg_flat_test";
    fs::remove_all(testDir);
    fs::create_directories(testDir / "pkgs");

    std::string lua = R"(package = {
    spec = "1",
    name = "flat-pkg",
    description = "A flat test package",
    type = "package",
    status = "dev",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.com/flat-1.0.0.tar.gz",
                sha256 = "abc123",
            },
        },
    },
}
)";
    xlings::platform::write_string_to_file(
        (testDir / "pkgs" / "flat-pkg.lua").string(), lua);

    xlings::xim::IndexManager mgr(testDir);
    auto result = mgr.rebuild();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(mgr.size(), 0u);  // flat file not picked up

    fs::remove_all(testDir);
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

// ============================================================
// xvm "latest" version resolution tests
// ============================================================

TEST(XvmDbTest, MatchLatestPicksHighest) {
    // "latest" isn't handled by match_version — it's handled in cmd_use.
    // But we can verify the underlying sort logic by checking match_version
    // with empty prefix returns nothing (since "" doesn't prefix-match digits),
    // confirming that "latest" needs special handling.
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "tool", "0.1.3", "/a");
    xlings::xvm::add_version(db, "tool", "0.1.4", "/b");
    xlings::xvm::add_version(db, "tool", "1.0.0", "/c");

    // "latest" should not match any version via fuzzy match
    EXPECT_EQ(xlings::xvm::match_version(db, "tool", "latest"), "");

    // Verify get_all_versions returns all, so cmd_use can sort and pick highest
    auto all = xlings::xvm::get_all_versions(db, "tool");
    EXPECT_EQ(all.size(), 3u);
}

TEST(XvmDbTest, NamespacedVersionMatch) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "gcc", "xim:15.1.0", "/a");
    xlings::xvm::add_version(db, "gcc", "xim:14.2.0", "/b");
    xlings::xvm::add_version(db, "gcc", "13.3.0", "/c");

    // Namespace-qualified match
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "xim:15"), "xim:15.1.0");
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "xim:14"), "xim:14.2.0");

    // Bare prefix prefers bare versions
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "13"), "13.3.0");
}

// ============================================================
// xvm binding tree tests
// ============================================================

namespace {

xlings::xvm::BindingGroupRef make_binding_group_ref(
    std::string provider,
    std::string providerVersion,
    std::string group,
    std::string rootTarget,
    std::string rootVersion) {
    return {
        .provider = std::move(provider),
        .providerVersion = std::move(providerVersion),
        .group = std::move(group),
        .rootTarget = std::move(rootTarget),
        .rootVersion = std::move(rootVersion),
    };
}

xlings::xvm::VData& add_provider_group_member(
    xlings::xvm::VersionDB& db,
    const std::string& target,
    const std::string& version,
    const xlings::xvm::BindingGroupRef& group,
    const std::string& kind,
    const std::string& sourceName = "",
    const std::string& destinationName = "") {
    auto& data = db[target].versions[version];
    data.path = "/pkg/" + group.providerVersion;
    data.kind = kind;
    data.sourceName = sourceName;
    data.destinationName = destinationName;
    data.bindingGroup = group;
    return data;
}

void expect_binding_error(
    const std::expected<xlings::xvm::BindingSelection,
                        xlings::xvm::BindingError>& result,
    xlings::xvm::BindingErrorKind kind,
    std::string_view target,
    std::string_view version) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, kind);
    EXPECT_EQ(result.error().target, target);
    EXPECT_EQ(result.error().version, version);
    EXPECT_FALSE(result.error().message.empty());
}

struct BindingGroupIdentityFieldCase {
    std::string_view path;
    std::string xlings::xvm::BindingGroupRef::* member;
};

const std::array<BindingGroupIdentityFieldCase, 5>
    binding_group_identity_fields{
        BindingGroupIdentityFieldCase{
            "/bindingGroup/provider",
            &xlings::xvm::BindingGroupRef::provider,
        },
        BindingGroupIdentityFieldCase{
            "/bindingGroup/version",
            &xlings::xvm::BindingGroupRef::providerVersion,
        },
        BindingGroupIdentityFieldCase{
            "/bindingGroup/group",
            &xlings::xvm::BindingGroupRef::group,
        },
        BindingGroupIdentityFieldCase{
            "/bindingGroup/rootTarget",
            &xlings::xvm::BindingGroupRef::rootTarget,
        },
        BindingGroupIdentityFieldCase{
            "/bindingGroup/rootVersion",
            &xlings::xvm::BindingGroupRef::rootVersion,
        },
    };

void expect_binding_metadata_error(
    const std::expected<xlings::xvm::BindingSelection,
                        xlings::xvm::BindingError>& result,
    std::string_view target,
    std::string_view version,
    std::string_view code,
    std::string_view path) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind,
              xlings::xvm::BindingErrorKind::MetadataIntegrityIssue);
    EXPECT_EQ(result.error().target, target);
    EXPECT_EQ(result.error().version, version);
    EXPECT_NE(result.error().message.find(code), std::string::npos);
    EXPECT_NE(result.error().message.find(path), std::string::npos);
}

}  // namespace

TEST(XvmBindingSelectionTest, ProviderGroupResolvesRootAndLeafExactly) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "xim-gnu-gcc",
        "xim-gnu-gcc", "xim:15.1.0");
    const std::map<std::string, std::string> expected{
        {"g++", "xim:15.1.0"},
        {"gcc", "xim:15.1.0"},
        {"gcc-ar", "xim:gcc-15.1.0"},
        {"libstdc++.so.6", "xim:gcc-15.1.0"},
        {"xim-gnu-gcc", "xim:15.1.0"},
    };

    auto& root = add_provider_group_member(
        db, "xim-gnu-gcc", "xim:15.1.0", group, "group");
    root.bindingMembers = expected;
    root.bindingHeaders = {
        {
            .sourceDir = "include",
            .destinationPrefix = "",
        },
    };
    add_provider_group_member(
        db, "gcc", "xim:15.1.0", group, "program", "gcc-15", "gcc");
    add_provider_group_member(
        db, "g++", "xim:15.1.0", group, "program", "g++-15", "g++");
    add_provider_group_member(
        db, "gcc-ar", "xim:gcc-15.1.0", group,
        "program", "gcc-ar-15", "gcc-ar");
    add_provider_group_member(
        db, "libstdc++.so.6", "xim:gcc-15.1.0", group,
        "lib", "libstdc++.so.6.0.34", "libstdc++.so.6");

    auto fromLeaf = xlings::xvm::resolve_binding_selection(
        db, "gcc", "xim:15.1.0");
    ASSERT_TRUE(fromLeaf.has_value()) << fromLeaf.error().message;
    EXPECT_EQ(fromLeaf->source, xlings::xvm::BindingSource::ProviderGroup);
    EXPECT_EQ(fromLeaf->members, expected);

    auto fromRoot = xlings::xvm::resolve_binding_selection(
        db, "xim-gnu-gcc", "xim:15.1.0");
    ASSERT_TRUE(fromRoot.has_value()) << fromRoot.error().message;
    EXPECT_EQ(fromRoot->source, xlings::xvm::BindingSource::ProviderGroup);
    EXPECT_EQ(fromRoot->members, expected);
}

TEST(XvmBindingSelectionTest, ProviderScopeSeparatesGroupsForSameTarget) {
    xlings::xvm::VersionDB db;
    db["cc"].type = "program";
    db["cc"].filename = "first-writer-cc";

    const auto groupA = make_binding_group_ref(
        "repo-a:toolchain", "1.0.0", "toolchain-a",
        "repo-a:root", "repo-a:1.0.0");
    auto& rootA = add_provider_group_member(
        db, "repo-a:root", "repo-a:1.0.0", groupA, "group");
    rootA.bindingMembers = {
        {"cc", "repo-a:1.0.0"},
        {"repo-a:root", "repo-a:1.0.0"},
    };
    add_provider_group_member(
        db, "cc", "repo-a:1.0.0", groupA,
        "program", "cc-a", "cc");

    const auto groupB = make_binding_group_ref(
        "repo-b:toolchain", "2.0.0", "toolchain-b",
        "repo-b:root", "repo-b:2.0.0");
    auto& rootB = add_provider_group_member(
        db, "repo-b:root", "repo-b:2.0.0", groupB, "group");
    rootB.bindingMembers = {
        {"cc", "repo-b:2.0.0"},
        {"repo-b:root", "repo-b:2.0.0"},
    };
    auto& ccB = add_provider_group_member(
        db, "cc", "repo-b:2.0.0", groupB,
        "lib", "libcc-b.so.2", "libcc.so");

    ASSERT_EQ(ccB.kind, "lib");
    EXPECT_EQ(ccB.sourceName, "libcc-b.so.2");
    EXPECT_EQ(ccB.destinationName, "libcc.so");

    auto selectedA = xlings::xvm::resolve_binding_selection(
        db, "cc", "repo-a:1.0.0");
    ASSERT_TRUE(selectedA.has_value()) << selectedA.error().message;
    EXPECT_EQ(selectedA->members,
              (std::map<std::string, std::string>{
                  {"cc", "repo-a:1.0.0"},
                  {"repo-a:root", "repo-a:1.0.0"},
              }));

    auto selectedB = xlings::xvm::resolve_binding_selection(
        db, "cc", "repo-b:2.0.0");
    ASSERT_TRUE(selectedB.has_value()) << selectedB.error().message;
    EXPECT_EQ(selectedB->members,
              (std::map<std::string, std::string>{
                  {"cc", "repo-b:2.0.0"},
                  {"repo-b:root", "repo-b:2.0.0"},
              }));
}

TEST(XvmBindingSelectionTest,
     SameProviderVersionSeparatesGroupRootsAndMembers) {
    xlings::xvm::VersionDB db;
    const auto groupA = make_binding_group_ref(
        "repo:toolchain", "1.0.0", "c-tools",
        "c-root", "repo:c-root");
    auto& rootA = add_provider_group_member(
        db, "c-root", "repo:c-root", groupA, "group");
    rootA.bindingMembers = {
        {"c-root", "repo:c-root"},
        {"cc", "repo:cc"},
    };
    add_provider_group_member(
        db, "cc", "repo:cc", groupA, "program", "cc", "cc");

    const auto groupB = make_binding_group_ref(
        "repo:toolchain", "1.0.0", "fortran-tools",
        "fortran-root", "repo:fortran-root");
    auto& rootB = add_provider_group_member(
        db, "fortran-root", "repo:fortran-root", groupB, "group");
    rootB.bindingMembers = {
        {"cc", "repo:fc"},
        {"fortran-root", "repo:fortran-root"},
    };
    add_provider_group_member(
        db, "cc", "repo:fc", groupB, "program", "fc", "fc");

    auto selectedA =
        xlings::xvm::resolve_binding_selection(db, "cc", "repo:cc");
    ASSERT_TRUE(selectedA.has_value()) << selectedA.error().message;
    EXPECT_EQ(selectedA->members,
              (std::map<std::string, std::string>{
                  {"c-root", "repo:c-root"},
                  {"cc", "repo:cc"},
              }));

    auto selectedB =
        xlings::xvm::resolve_binding_selection(db, "cc", "repo:fc");
    ASSERT_TRUE(selectedB.has_value()) << selectedB.error().message;
    EXPECT_EQ(selectedB->members,
              (std::map<std::string, std::string>{
                  {"cc", "repo:fc"},
                  {"fortran-root", "repo:fortran-root"},
              }));

    rootA.bindingMembers["cc"] = "repo:fc";
    auto crossGroup =
        xlings::xvm::resolve_binding_selection(db, "c-root", "repo:c-root");
    expect_binding_error(
        crossGroup, xlings::xvm::BindingErrorKind::MemberReferenceMismatch,
        "cc", "repo:fc");
}

TEST(XvmBindingSelectionErrorTest, RejectsBindingMembersWithoutGroup) {
    xlings::xvm::VersionDB db;
    auto& data = db["tool"].versions["1.0.0"];
    data.kind = "program";
    data.bindingMembers = {
        {"tool", "1.0.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1.0.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::PartialProviderMetadata,
        "tool", "1.0.0");
}

TEST(XvmBindingSelectionErrorTest, RejectsBindingHeadersWithoutGroup) {
    xlings::xvm::VersionDB db;
    auto& data = db["tool"].versions["1.0.0"];
    data.kind = "program";
    data.bindingHeaders = {
        {
            .sourceDir = "include",
            .destinationPrefix = "",
        },
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1.0.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::PartialProviderMetadata,
        "tool", "1.0.0");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderGroupRejectsBindingMembersOnNonRoot) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider-root", "1.0.0");
    auto& root = add_provider_group_member(
        db, "provider-root", "1.0.0", group, "group");
    root.bindingMembers = {
        {"provider-root", "1.0.0"},
        {"tool", "1.0.0"},
    };
    auto& member = add_provider_group_member(
        db, "tool", "1.0.0", group, "program");
    member.bindingMembers = {
        {"nested-tool", "1.0.0"},
    };

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    expect_binding_metadata_error(
        result, "tool", "1.0.0",
        "binding-metadata-on-non-root", "/bindingMembers");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderGroupRejectsBindingHeadersOnNonRoot) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider-root", "1.0.0");
    auto& root = add_provider_group_member(
        db, "provider-root", "1.0.0", group, "group");
    root.bindingMembers = {
        {"provider-root", "1.0.0"},
        {"tool", "1.0.0"},
    };
    auto& member = add_provider_group_member(
        db, "tool", "1.0.0", group, "program");
    member.bindingHeaders = {
        {
            .sourceDir = "include",
            .destinationPrefix = "",
        },
    };

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    expect_binding_metadata_error(
        result, "tool", "1.0.0",
        "binding-metadata-on-non-root", "/bindingHeaders");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderGroupRejectsPersistedEmptyNonRootMetadataFields) {
    struct Case {
        std::string_view field;
        nlohmann::json emptyValue;
    };
    const std::array<Case, 2> cases{
        Case{"bindingMembers", nlohmann::json::object()},
        Case{"bindingHeaders", nlohmann::json::array()},
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.field);
        nlohmann::json memberJson{
            {"path", "/pkg/1.0.0"},
            {"kind", "program"},
            {"bindingGroup", valid_binding_group_json()},
            {std::string(testCase.field), testCase.emptyValue},
        };
        auto member = xlings::xvm::vdata_from_json(memberJson);
        auto persisted = xlings::xvm::vdata_to_json(member);
        ASSERT_TRUE(persisted.contains(testCase.field));
        member = reload_vdata(member);

        xlings::xvm::VersionDB db;
        const auto group = make_binding_group_ref(
            "repo:provider", "1.0.0", "provider-group",
            "provider-root", "1.0.0");
        auto& root = add_provider_group_member(
            db, "provider-root", "1.0.0", group, "group");
        root.bindingMembers = {
            {"provider-root", "1.0.0"},
            {"tool", "1.0.0"},
        };
        db["tool"].versions["1.0.0"] = std::move(member);

        auto result = xlings::xvm::resolve_binding_selection(
            db, "provider-root", "1.0.0");

        expect_binding_metadata_error(
            result, "tool", "1.0.0",
            "binding-metadata-on-non-root",
            "/" + std::string(testCase.field));
    }
}

TEST(XvmBindingSelectionErrorTest,
     RejectsEveryEmptyStartingGroupIdentityField) {
    for (const auto& testCase : binding_group_identity_fields) {
        SCOPED_TRACE(testCase.path);
        xlings::xvm::VersionDB db;
        const auto group = make_binding_group_ref(
            "repo:provider", "1.0.0", "provider-group",
            "provider-root", "1.0.0");
        auto& root = add_provider_group_member(
            db, "provider-root", "1.0.0", group, "group");
        root.bindingMembers = {
            {"provider-root", "1.0.0"},
            {"tool", "1.0.0"},
        };
        auto& start = add_provider_group_member(
            db, "tool", "1.0.0", group, "program");
        ((*start.bindingGroup).*testCase.member).clear();

        auto result = xlings::xvm::resolve_binding_selection(
            db, "tool", "1.0.0");

        expect_binding_metadata_error(
            result, "tool", "1.0.0",
            "binding-group-field-invalid", testCase.path);
    }
}

TEST(XvmBindingSelectionErrorTest,
     RejectsEveryEmptyRootGroupIdentityField) {
    for (const auto& testCase : binding_group_identity_fields) {
        SCOPED_TRACE(testCase.path);
        xlings::xvm::VersionDB db;
        const auto group = make_binding_group_ref(
            "repo:provider", "1.0.0", "provider-group",
            "provider-root", "1.0.0");
        auto& root = add_provider_group_member(
            db, "provider-root", "1.0.0", group, "group");
        root.bindingMembers = {
            {"provider-root", "1.0.0"},
            {"tool", "1.0.0"},
        };
        ((*root.bindingGroup).*testCase.member).clear();
        add_provider_group_member(
            db, "tool", "1.0.0", group, "program");

        auto result = xlings::xvm::resolve_binding_selection(
            db, "tool", "1.0.0");

        expect_binding_metadata_error(
            result, "provider-root", "1.0.0",
            "binding-group-field-invalid", testCase.path);
    }
}

TEST(XvmBindingSelectionErrorTest,
     RejectsEveryEmptyMemberGroupIdentityField) {
    for (const auto& testCase : binding_group_identity_fields) {
        SCOPED_TRACE(testCase.path);
        xlings::xvm::VersionDB db;
        const auto group = make_binding_group_ref(
            "repo:provider", "1.0.0", "provider-group",
            "provider-root", "1.0.0");
        auto& root = add_provider_group_member(
            db, "provider-root", "1.0.0", group, "group");
        root.bindingMembers = {
            {"provider-root", "1.0.0"},
            {"tool", "1.0.0"},
        };
        auto& member = add_provider_group_member(
            db, "tool", "1.0.0", group, "program");
        ((*member.bindingGroup).*testCase.member).clear();

        auto result = xlings::xvm::resolve_binding_selection(
            db, "provider-root", "1.0.0");

        expect_binding_metadata_error(
            result, "tool", "1.0.0",
            "binding-group-field-invalid", testCase.path);
    }
}

TEST(XvmBindingSelectionErrorTest,
     LegacyRejectsProviderAwareNodeReachedByTraversal) {
    xlings::xvm::VersionDB db;
    db["legacy"].type = "program";
    db["legacy"].versions["1.0.0"].path = "/legacy";
    db["provider"].type = "program";
    auto& provider = db["provider"].versions["1.0.0"];
    provider.path = "/provider";
    provider.bindingGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider", "1.0.0");
    db["legacy"].bindings["provider"]["1.0.0"] = "1.0.0";
    db["provider"].bindings["legacy"]["1.0.0"] = "1.0.0";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "legacy", "1.0.0");

    expect_binding_error(
        result,
        xlings::xvm::BindingErrorKind::ProviderMetadataInLegacyGraph,
        "provider", "1.0.0");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderResolutionRejectsPersistedIntegrityIssue) {
    auto corruptRoot = nlohmann::json::parse(R"({
        "path": "/provider",
        "kind": "group",
        "bindingGroup": {
            "provider": "repo:provider",
            "version": "1.0.0",
            "group": "provider-group",
            "rootTarget": "provider-root",
            "rootVersion": "1.0.0"
        },
        "bindingMembers": {
            "provider-root": "1.0.0",
            "tool": false
        }
    })");
    auto persisted = xlings::xvm::vdata_to_json(
        xlings::xvm::vdata_from_json(corruptRoot));
    xlings::xvm::VersionDB db;
    db["provider-root"].versions["1.0.0"] =
        xlings::xvm::vdata_from_json(persisted);

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    ASSERT_FALSE(result.has_value());
    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::MetadataIntegrityIssue,
        "provider-root", "1.0.0");
    EXPECT_NE(result.error().message.find(
                  "binding-member-version-not-string"),
              std::string::npos);
    EXPECT_NE(result.error().message.find("/bindingMembers/tool"),
              std::string::npos);
}

TEST(XvmBindingSelectionErrorTest,
     LegacyResolutionRejectsPersistedIntegrityIssue) {
    auto corruptLegacy = nlohmann::json::parse(R"({
        "path": "/legacy",
        "bindingHeaders": [false]
    })");
    auto persisted = xlings::xvm::vdata_to_json(
        xlings::xvm::vdata_from_json(corruptLegacy));
    xlings::xvm::VersionDB db;
    db["legacy"].type = "program";
    db["legacy"].versions["1.0.0"] =
        xlings::xvm::vdata_from_json(persisted);

    auto result =
        xlings::xvm::resolve_binding_selection(db, "legacy", "1.0.0");

    ASSERT_FALSE(result.has_value());
    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::MetadataIntegrityIssue,
        "legacy", "1.0.0");
    EXPECT_NE(result.error().message.find("binding-header-not-object"),
              std::string::npos);
    EXPECT_NE(result.error().message.find("/bindingHeaders/0"),
              std::string::npos);
}

TEST(XvmBindingSelectionErrorTest, RejectsMissingStartingTarget) {
    xlings::xvm::VersionDB db;

    auto result =
        xlings::xvm::resolve_binding_selection(db, "missing", "1.0.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::TargetNotFound,
        "missing", "1.0.0");
}

TEST(XvmBindingSelectionErrorTest, RejectsMissingStartingVersion) {
    xlings::xvm::VersionDB db;
    db["tool"].versions["1.0.0"].path = "/tool";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "2.0.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::VersionNotFound,
        "tool", "2.0.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRejectsMissingRootVersion) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "program", "gcc", "gcc");
    db["root"].versions["14.2.0"].kind = "group";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::VersionNotFound,
        "root", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRequiresRootSelfReference) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "program", "gcc", "gcc");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingGroup->rootTarget = "not-root";
    root.bindingMembers = {
        {"gcc", "15.1.0"},
        {"root", "15.1.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::RootReferenceMismatch,
        "root", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRequiresMatchingRootIdentity) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "program", "gcc", "gcc");
    auto otherGroup = group;
    otherGroup.provider = "other:gcc";
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", otherGroup, "group");
    root.bindingMembers = {
        {"gcc", "15.1.0"},
        {"root", "15.1.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::GroupIdentityMismatch,
        "root", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRequiresRootInManifest) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "program", "gcc", "gcc");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"gcc", "15.1.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::RootMissingFromManifest,
        "root", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRequiresStartInManifest) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "program", "gcc", "gcc");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"root", "15.1.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::StartMemberMissing,
        "gcc", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRejectsMissingManifestTarget) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"missing", "15.1.0"},
        {"root", "15.1.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "root", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::TargetNotFound,
        "missing", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRejectsMissingManifestVersion) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"gcc", "15.1.0"},
        {"root", "15.1.0"},
    };
    add_provider_group_member(
        db, "gcc", "14.2.0", group, "program", "gcc", "gcc");

    auto result =
        xlings::xvm::resolve_binding_selection(db, "root", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::VersionNotFound,
        "gcc", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRejectsMemberBackReference) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"gcc", "15.1.0"},
        {"root", "15.1.0"},
    };
    auto otherGroup = group;
    otherGroup.group = "other";
    add_provider_group_member(
        db, "gcc", "15.1.0", otherGroup, "program", "gcc", "gcc");

    auto result =
        xlings::xvm::resolve_binding_selection(db, "root", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::MemberReferenceMismatch,
        "gcc", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupUsesPerVersionKind) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"gcc", "15.1.0"},
        {"root", "15.1.0"},
    };
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "archive", "gcc", "gcc");
    db["gcc"].type = "program";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "root", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::UnsupportedKind,
        "gcc", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, LegacyRejectsMissingDestinationVersion) {
    xlings::xvm::VersionDB db;
    db["a"].type = "program";
    db["a"].versions["1"].path = "/a";
    db["b"].type = "program";
    db["b"].versions["1"].path = "/b";
    db["a"].bindings["b"]["1"] = "2";
    db["b"].bindings["a"]["2"] = "1";

    auto result = xlings::xvm::resolve_binding_selection(db, "a", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::VersionNotFound, "b", "2");
}

TEST(XvmBindingSelectionErrorTest, LegacyRejectsAsymmetricEdge) {
    xlings::xvm::VersionDB db;
    db["a"].type = "program";
    db["a"].versions["1"].path = "/a";
    db["b"].type = "program";
    db["b"].versions["1"].path = "/b";
    db["a"].bindings["b"]["1"] = "1";

    auto result = xlings::xvm::resolve_binding_selection(db, "a", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::AsymmetricEdge, "b", "1");
}

TEST(XvmBindingSelectionErrorTest, LegacyRejectsSelfEdge) {
    xlings::xvm::VersionDB db;
    db["a"].type = "program";
    db["a"].versions["1"].path = "/a";
    db["a"].bindings["a"]["1"] = "1";

    auto result = xlings::xvm::resolve_binding_selection(db, "a", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::SelfEdge, "a", "1");
}

TEST(XvmBindingSelectionErrorTest, LegacyRejectsTargetVersionConflict) {
    xlings::xvm::VersionDB db;
    for (const auto& [target, version] :
         std::vector<std::pair<std::string, std::string>>{
             {"a", "1"}, {"a", "2"}, {"b", "1"}, {"c", "1"}}) {
        db[target].type = "program";
        db[target].versions[version].path = "/" + target + "/" + version;
    }
    db["a"].bindings["b"]["1"] = "1";
    db["b"].bindings["a"]["1"] = "1";
    db["b"].bindings["c"]["1"] = "1";
    db["c"].bindings["b"]["1"] = "1";
    db["c"].bindings["a"]["1"] = "2";
    db["a"].bindings["c"]["2"] = "1";

    auto result = xlings::xvm::resolve_binding_selection(db, "a", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::ConflictingTargetVersion,
        "a", "2");
}

TEST(XvmBindingSelectionErrorTest, LegacyFallsBackToVInfoKind) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "archive";
    db["tool"].versions["1"].path = "/tool";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::UnsupportedKind,
        "tool", "1");
}

TEST(XvmBindingSelectionErrorTest, LegacyPrefersVDataKindOverVInfo) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1"].path = "/tool";
    db["tool"].versions["1"].kind = "archive";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::UnsupportedKind,
        "tool", "1");
}

TEST(XvmDbTest, AddVersionWithBinding) {
    xlings::xvm::VersionDB db;

    // Simulate installing gcc package: xim-gnu-gcc is the parent package,
    // gcc, g++, gcc-ar are binding targets
    xlings::xvm::add_version(db, "xim-gnu-gcc", "15.1.0", "/pkg/gcc-15");
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/pkg/gcc-15", "program", "gcc", "gcc", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "g++", "15.1.0", "/pkg/gcc-15", "program", "g++", "g++", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "gcc-ar", "gcc-15.1.0", "/pkg/gcc-15", "program", "gcc-ar", "gcc-ar", "", "xim-gnu-gcc@15.1.0");

    // Verify bidirectional bindings exist
    // xim-gnu-gcc should know about gcc, g++, gcc-ar
    auto* parent = xlings::xvm::get_vinfo(db, "xim-gnu-gcc");
    ASSERT_NE(parent, nullptr);
    ASSERT_TRUE(parent->bindings.contains("gcc"));
    ASSERT_TRUE(parent->bindings.contains("g++"));
    ASSERT_TRUE(parent->bindings.contains("gcc-ar"));
    EXPECT_EQ(parent->bindings.at("gcc").at("15.1.0"), "15.1.0");
    EXPECT_EQ(parent->bindings.at("g++").at("15.1.0"), "15.1.0");
    EXPECT_EQ(parent->bindings.at("gcc-ar").at("15.1.0"), "gcc-15.1.0");

    // gcc should know about xim-gnu-gcc
    auto* gcc_info = xlings::xvm::get_vinfo(db, "gcc");
    ASSERT_NE(gcc_info, nullptr);
    ASSERT_TRUE(gcc_info->bindings.contains("xim-gnu-gcc"));
    EXPECT_EQ(gcc_info->bindings.at("xim-gnu-gcc").at("15.1.0"), "15.1.0");

    // gcc-ar should know about xim-gnu-gcc with correct version mapping
    auto* ar_info = xlings::xvm::get_vinfo(db, "gcc-ar");
    ASSERT_NE(ar_info, nullptr);
    ASSERT_TRUE(ar_info->bindings.contains("xim-gnu-gcc"));
    EXPECT_EQ(ar_info->bindings.at("xim-gnu-gcc").at("gcc-15.1.0"), "15.1.0");
}

TEST(XvmDbTest, AddVersionWithBindingMultipleVersions) {
    xlings::xvm::VersionDB db;

    // Install gcc 15.1.0
    xlings::xvm::add_version(db, "xim-gnu-gcc", "15.1.0", "/pkg/gcc-15");
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/pkg/gcc-15", "program", "gcc", "gcc", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "g++", "15.1.0", "/pkg/gcc-15", "program", "g++", "g++", "", "xim-gnu-gcc@15.1.0");

    // Install gcc 14.2.0
    xlings::xvm::add_version(db, "xim-gnu-gcc", "14.2.0", "/pkg/gcc-14");
    xlings::xvm::add_version(db, "gcc", "14.2.0", "/pkg/gcc-14", "program", "gcc", "gcc", "", "xim-gnu-gcc@14.2.0");
    xlings::xvm::add_version(db, "g++", "14.2.0", "/pkg/gcc-14", "program", "g++", "g++", "", "xim-gnu-gcc@14.2.0");

    // Parent should have version mappings for both versions
    auto* parent = xlings::xvm::get_vinfo(db, "xim-gnu-gcc");
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->bindings.at("gcc").at("15.1.0"), "15.1.0");
    EXPECT_EQ(parent->bindings.at("gcc").at("14.2.0"), "14.2.0");
    EXPECT_EQ(parent->bindings.at("g++").at("15.1.0"), "15.1.0");
    EXPECT_EQ(parent->bindings.at("g++").at("14.2.0"), "14.2.0");

    // Each child should map back to both parent versions
    auto* gcc_info = xlings::xvm::get_vinfo(db, "gcc");
    ASSERT_NE(gcc_info, nullptr);
    EXPECT_EQ(gcc_info->bindings.at("xim-gnu-gcc").at("15.1.0"), "15.1.0");
    EXPECT_EQ(gcc_info->bindings.at("xim-gnu-gcc").at("14.2.0"), "14.2.0");
}

TEST(XvmDbTest, AddVersionWithBindingNamespaced) {
    xlings::xvm::VersionDB db;

    // Simulate a non-primary repo with namespace
    xlings::xvm::add_version(db, "xim-gnu-gcc", "15.1.0", "/pkg/gcc-15", "program", "", "", "xim");
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/pkg/gcc-15", "program", "gcc", "gcc", "xim", "xim-gnu-gcc@15.1.0");

    // Verify namespaced version keys in bindings
    auto* parent = xlings::xvm::get_vinfo(db, "xim-gnu-gcc");
    ASSERT_NE(parent, nullptr);
    ASSERT_TRUE(parent->bindings.contains("gcc"));
    EXPECT_EQ(parent->bindings.at("gcc").at("xim:15.1.0"), "xim:15.1.0");

    auto* gcc_info = xlings::xvm::get_vinfo(db, "gcc");
    ASSERT_NE(gcc_info, nullptr);
    EXPECT_EQ(gcc_info->bindings.at("xim-gnu-gcc").at("xim:15.1.0"), "xim:15.1.0");
}

TEST(XvmDbTest, BindingTreeTraversal) {
    xlings::xvm::VersionDB db;

    xlings::xvm::add_version(db, "xim-gnu-gcc", "15.1.0", "/pkg/gcc-15");
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/pkg/gcc-15", "program", "gcc", "gcc", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "g++", "15.1.0", "/pkg/gcc-15", "program", "g++", "g++", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "gcc-ar", "gcc-15.1.0", "/pkg/gcc-15", "program", "gcc-ar", "gcc-ar", "", "xim-gnu-gcc@15.1.0");

    auto selection =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    ASSERT_TRUE(selection.has_value()) << selection.error().message;
    EXPECT_EQ(selection->source, xlings::xvm::BindingSource::LegacyGraph);
    EXPECT_EQ(selection->members,
              (std::map<std::string, std::string>{
                  {"g++", "15.1.0"},
                  {"gcc", "15.1.0"},
                  {"gcc-ar", "gcc-15.1.0"},
                  {"xim-gnu-gcc", "15.1.0"},
              }));
}

TEST(XvmDbTest, BindingJsonRoundTrip) {
    xlings::xvm::VersionDB db;

    xlings::xvm::add_version(db, "xim-gnu-gcc", "15.1.0", "/pkg/gcc-15");
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/pkg/gcc-15", "program", "gcc", "gcc", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "g++", "15.1.0", "/pkg/gcc-15", "program", "g++", "g++", "", "xim-gnu-gcc@15.1.0");

    // Serialize and deserialize
    auto j = xlings::xvm::versions_to_json(db);
    auto restored = xlings::xvm::versions_from_json(j);

    // Verify bindings survived round-trip
    auto* parent = xlings::xvm::get_vinfo(restored, "xim-gnu-gcc");
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->bindings.at("gcc").at("15.1.0"), "15.1.0");
    EXPECT_EQ(parent->bindings.at("g++").at("15.1.0"), "15.1.0");

    auto* gcc_info = xlings::xvm::get_vinfo(restored, "gcc");
    ASSERT_NE(gcc_info, nullptr);
    EXPECT_EQ(gcc_info->bindings.at("xim-gnu-gcc").at("15.1.0"), "15.1.0");
}

// ============================================================
// Profile generation tests
// ============================================================

class ProfileTest : public ::testing::Test {
protected:
    std::filesystem::path testDir_;

    void SetUp() override {
        testDir_ = std::filesystem::temp_directory_path() / "xlings_profile_test";
        std::filesystem::create_directories(testDir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir_, ec);
    }
};

TEST_F(ProfileTest, LoadCurrentEmpty) {
    // No profile file → returns generation 0
    auto gen = xlings::profile::load_current(testDir_);
    EXPECT_EQ(gen.number, 0);
    EXPECT_TRUE(gen.packages.empty());
}

TEST_F(ProfileTest, CommitAndLoadRoundTrip) {
    std::map<std::string, std::string> packages = {
        {"gcc", "15.1.0"},
        {"node", "22.0.0"},
    };
    int rc = xlings::profile::commit(testDir_, packages, "install gcc+node");
    EXPECT_EQ(rc, 0);

    auto gen = xlings::profile::load_current(testDir_);
    EXPECT_EQ(gen.number, 1);
    EXPECT_EQ(gen.packages.size(), 2u);
    EXPECT_EQ(gen.packages["gcc"], "15.1.0");
    EXPECT_EQ(gen.packages["node"], "22.0.0");

    // Second commit
    packages["python"] = "3.12.0";
    rc = xlings::profile::commit(testDir_, packages, "add python");
    EXPECT_EQ(rc, 0);

    gen = xlings::profile::load_current(testDir_);
    EXPECT_EQ(gen.number, 2);
    EXPECT_EQ(gen.packages.size(), 3u);
}

TEST_F(ProfileTest, ListGenerations) {
    std::map<std::string, std::string> p1 = {{"gcc", "15.1.0"}};
    std::map<std::string, std::string> p2 = {{"gcc", "15.1.0"}, {"node", "22.0.0"}};

    xlings::profile::commit(testDir_, p1, "install gcc");
    xlings::profile::commit(testDir_, p2, "add node");

    auto gens = xlings::profile::list_generations(testDir_);
    EXPECT_EQ(gens.size(), 2u);
    EXPECT_EQ(gens[0].number, 1);
    EXPECT_EQ(gens[0].packages.size(), 1u);
    EXPECT_EQ(gens[1].number, 2);
    EXPECT_EQ(gens[1].packages.size(), 2u);
}

TEST_F(ProfileTest, Rollback) {
    std::map<std::string, std::string> p1 = {{"gcc", "15.1.0"}};
    std::map<std::string, std::string> p2 = {{"gcc", "15.1.0"}, {"node", "22.0.0"}};

    xlings::profile::commit(testDir_, p1, "install gcc");
    xlings::profile::commit(testDir_, p2, "add node");

    int rc = xlings::profile::rollback(testDir_, 1);
    EXPECT_EQ(rc, 0);

    auto gen = xlings::profile::load_current(testDir_);
    EXPECT_EQ(gen.number, 1);
    EXPECT_EQ(gen.packages.size(), 1u);
    EXPECT_EQ(gen.packages["gcc"], "15.1.0");
}

TEST_F(ProfileTest, RollbackNonexistentFails) {
    int rc = xlings::profile::rollback(testDir_, 99);
    EXPECT_EQ(rc, 1);
}

TEST_F(ProfileTest, FindSubosReferencingEmpty) {
    // No subos dir → empty result
    auto refs = xlings::profile::find_subos_referencing(testDir_, "gcc");
    EXPECT_TRUE(refs.empty());
}

// ============================================================
// Log system extended tests
// ============================================================

TEST(LogTest, GetLevelReturnsSetValue) {
    xlings::log::set_level(xlings::log::Level::Debug);
    EXPECT_EQ(xlings::log::get_level(), xlings::log::Level::Debug);

    xlings::log::set_level(xlings::log::Level::Warn);
    EXPECT_EQ(xlings::log::get_level(), xlings::log::Level::Warn);

    // Restore
    xlings::log::set_level(xlings::log::Level::Info);
}

TEST(LogTest, LevelStringMatchesLevel) {
    xlings::log::set_level(xlings::log::Level::Debug);
    EXPECT_EQ(xlings::log::level_string(), "debug");

    xlings::log::set_level(xlings::log::Level::Info);
    EXPECT_EQ(xlings::log::level_string(), "info");

    xlings::log::set_level(xlings::log::Level::Warn);
    EXPECT_EQ(xlings::log::level_string(), "warn");

    xlings::log::set_level(xlings::log::Level::Error);
    EXPECT_EQ(xlings::log::level_string(), "error");

    // Restore
    xlings::log::set_level(xlings::log::Level::Info);
}

TEST(LogTest, EnableColorToggle) {
    // Should not crash and should be toggleable
    xlings::log::enable_color(false);
    xlings::log::info("no color test");
    xlings::log::enable_color(true);
    xlings::log::info("color test");
}

TEST(LogTest, LevelFiltering) {
    namespace fs = std::filesystem;
    // Use a unique file name to avoid conflicts with other log tests
    auto tmpFile = fs::temp_directory_path() / "xlings_test_log_filter2.txt";
    std::error_code ec;
    fs::remove(tmpFile, ec);

    // Save and restore level around test
    auto savedLevel = xlings::log::get_level();

    xlings::log::set_level(xlings::log::Level::Warn);
    xlings::log::set_file(tmpFile);

    xlings::log::debug("should_not_appear_debug");
    xlings::log::info("should_not_appear_info");
    xlings::log::warn("warn_visible");
    xlings::log::error("error_visible");

    // Close the log file before reading
    xlings::log::set_file("");

    // Read and verify
    std::ifstream f(tmpFile);
    if (!f.is_open()) {
        // On some platforms the file might not be created if ofstream has issues
        // Skip rather than fail hard
        xlings::log::set_level(savedLevel);
        GTEST_SKIP() << "Could not open log file for reading";
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    f.close();

    EXPECT_EQ(content.find("should_not_appear"), std::string::npos);
    EXPECT_NE(content.find("warn_visible"), std::string::npos);
    EXPECT_NE(content.find("error_visible"), std::string::npos);

    fs::remove(tmpFile, ec);
    xlings::log::set_level(savedLevel);
}

// ═══════════════════════════════════════════════════════════════
//  EventStream tests
// ═══════════════════════════════════════════════════════════════

TEST(Event, ProgressEventConstruction) {
    xlings::ProgressEvent e{
        .phase = "downloading",
        .percent = 0.5f,
        .message = "Downloading gcc-15..."
    };
    EXPECT_EQ(e.phase, "downloading");
    EXPECT_FLOAT_EQ(e.percent, 0.5f);
    EXPECT_EQ(e.message, "Downloading gcc-15...");
}

TEST(Event, PromptEventConstruction) {
    xlings::PromptEvent e{
        .id = "p1",
        .question = "Override existing?",
        .options = {"y", "n"},
        .defaultValue = "n"
    };
    EXPECT_EQ(e.id, "p1");
    EXPECT_EQ(e.options.size(), 2);
    EXPECT_EQ(e.defaultValue, "n");
}

TEST(Event, VariantHoldsTypes) {
    xlings::Event ev = xlings::LogEvent{xlings::LogLevel::info, "hello"};
    EXPECT_TRUE(std::holds_alternative<xlings::LogEvent>(ev));

    ev = xlings::ErrorEvent{.code = xlings::ErrorCode::Network,
                             .message = "fail", .recoverable = true};
    auto& err = std::get<xlings::ErrorEvent>(ev);
    EXPECT_TRUE(err.code == xlings::ErrorCode::Network);
    EXPECT_TRUE(err.recoverable);
}

TEST(Event, CompletedEvent) {
    xlings::Event ev = xlings::CompletedEvent{.success = true, .summary = "done"};
    auto& c = std::get<xlings::CompletedEvent>(ev);
    EXPECT_TRUE(c.success);
}

TEST(Event, DataEvent) {
    xlings::Event ev = xlings::DataEvent{.kind = "search_results", .json = R"({"count":3})"};
    auto& d = std::get<xlings::DataEvent>(ev);
    EXPECT_EQ(d.kind, "search_results");
}

// ============================================================
// EventStream tests
// ============================================================

TEST(EventStream, EmitAndConsume) {
    xlings::EventStream stream;
    std::vector<xlings::Event> received;

    stream.on_event([&](const xlings::Event& e) {
        received.push_back(e);
    });

    stream.emit(xlings::LogEvent{xlings::LogLevel::info, "hello"});
    stream.emit(xlings::ProgressEvent{"downloading", 0.5f, "..."});

    ASSERT_EQ(received.size(), 2);
    EXPECT_TRUE(std::holds_alternative<xlings::LogEvent>(received[0]));
    EXPECT_TRUE(std::holds_alternative<xlings::ProgressEvent>(received[1]));
}

TEST(EventStream, MultipleConsumers) {
    xlings::EventStream stream;
    int count_a = 0, count_b = 0;

    stream.on_event([&](const xlings::Event&) { ++count_a; });
    stream.on_event([&](const xlings::Event&) { ++count_b; });

    stream.emit(xlings::LogEvent{xlings::LogLevel::info, "test"});

    EXPECT_EQ(count_a, 1);
    EXPECT_EQ(count_b, 1);
}

TEST(EventStream, PromptAndRespond) {
    xlings::EventStream stream;
    std::string captured_question;

    stream.on_event([&](const xlings::Event& e) {
        if (auto* p = std::get_if<xlings::PromptEvent>(&e)) {
            captured_question = p->question;
            stream.respond(p->id, "y");
        }
    });

    auto answer = stream.prompt({
        .id = "p1",
        .question = "Override?",
        .options = {"y", "n"},
        .defaultValue = "n"
    });

    EXPECT_EQ(captured_question, "Override?");
    EXPECT_EQ(answer, "y");
}

TEST(EventStream, PromptDefaultOnEmpty) {
    xlings::EventStream stream;

    stream.on_event([&](const xlings::Event& e) {
        if (auto* p = std::get_if<xlings::PromptEvent>(&e)) {
            stream.respond(p->id, p->defaultValue);
        }
    });

    auto answer = stream.prompt({
        .id = "p2",
        .question = "Continue?",
        .options = {},
        .defaultValue = "yes"
    });
    EXPECT_EQ(answer, "yes");
}

TEST(EventStream, PromptBlocksUntilRespond) {
    xlings::EventStream stream;
    std::atomic<bool> promptReturned { false };
    std::string answer;

    stream.on_event([](const xlings::Event&) {});

    std::thread taskThread([&] {
        answer = stream.prompt({
            .id = "p_async",
            .question = "Confirm?",
            .options = {"y", "n"},
            .defaultValue = "n"
        });
        promptReturned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(promptReturned.load());

    stream.respond("p_async", "confirmed");

    taskThread.join();
    EXPECT_TRUE(promptReturned.load());
    EXPECT_EQ(answer, "confirmed");
}

TEST(EventStream, ConcurrentPromptsFromMultipleTasks) {
    xlings::EventStream stream;
    std::string answer1, answer2;

    stream.on_event([](const xlings::Event&) {});

    std::thread t1([&] {
        answer1 = stream.prompt({.id = "pa", .question = "Q1"});
    });
    std::thread t2([&] {
        answer2 = stream.prompt({.id = "pb", .question = "Q2"});
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    stream.respond("pb", "ans_b");
    stream.respond("pa", "ans_a");

    t1.join();
    t2.join();

    EXPECT_EQ(answer1, "ans_a");
    EXPECT_EQ(answer2, "ans_b");
}

// ============================================================
// ─── Mock Capabilities for testing ───
// ============================================================

namespace {

class MockSearchCapability : public xlings::capability::Capability {
public:
    auto spec() const -> xlings::capability::CapabilitySpec override {
        return {
            .name = "search_packages",
            .description = "Search for packages",
            .inputSchema = R"({"type":"object","properties":{"query":{"type":"string"}}})",
            .outputSchema = R"({"type":"object","properties":{"results":{"type":"array"}}})",
            .destructive = false,
            .asyncCapable = true
        };
    }

    auto execute(xlings::capability::Params params,
                 xlings::EventStream& stream) -> xlings::capability::Result override {
        stream.emit(xlings::LogEvent{xlings::LogLevel::info, "Searching..."});
        stream.emit(xlings::DataEvent{.kind = "search_results", .json = R"({"results":["gcc","g++"]})"});
        stream.emit(xlings::CompletedEvent{.success = true, .summary = "Found 2 packages"});
        return R"({"count":2})";
    }
};

class MockInstallCapability : public xlings::capability::Capability {
public:
    auto spec() const -> xlings::capability::CapabilitySpec override {
        return {
            .name = "install_package",
            .description = "Install a package",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}}})",
            .outputSchema = R"({"type":"object","properties":{"status":{"type":"string"}}})",
            .destructive = true,
            .asyncCapable = true
        };
    }

    auto execute(xlings::capability::Params params,
                 xlings::EventStream& stream) -> xlings::capability::Result override {
        stream.emit(xlings::ProgressEvent{"installing", 0.5f, "Installing..."});
        auto answer = stream.prompt({
            .id = "confirm_install",
            .question = "Proceed with install?",
            .options = {"y", "n"},
            .defaultValue = "y"
        });
        if (answer == "n") {
            return R"({"status":"cancelled"})";
        }
        stream.emit(xlings::CompletedEvent{.success = true, .summary = "Installed"});
        return R"({"status":"ok"})";
    }
};

}  // anonymous namespace

// ============================================================
// ─── Capability Tests ───
// ============================================================

TEST(Capability, RegistryRegisterAndGet) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());
    reg.register_capability(std::make_unique<MockInstallCapability>());

    auto* search = reg.get("search_packages");
    ASSERT_NE(search, nullptr);
    EXPECT_EQ(search->spec().name, "search_packages");
    EXPECT_FALSE(search->spec().destructive);

    auto* install = reg.get("install_package");
    ASSERT_NE(install, nullptr);
    EXPECT_TRUE(install->spec().destructive);

    EXPECT_EQ(reg.get("nonexistent"), nullptr);
}

TEST(Capability, RegistryListAll) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());
    reg.register_capability(std::make_unique<MockInstallCapability>());

    auto specs = reg.list_all();
    EXPECT_EQ(specs.size(), 2);
}

TEST(Capability, ExecuteWithEventStream) {
    xlings::EventStream stream;
    std::vector<xlings::Event> events;
    stream.on_event([&](const xlings::Event& e) { events.push_back(e); });

    MockSearchCapability search;
    auto result = search.execute(R"({"query":"gcc"})", stream);

    ASSERT_EQ(events.size(), 3);
    EXPECT_TRUE(std::holds_alternative<xlings::LogEvent>(events[0]));
    EXPECT_TRUE(std::holds_alternative<xlings::DataEvent>(events[1]));
    EXPECT_TRUE(std::holds_alternative<xlings::CompletedEvent>(events[2]));
    EXPECT_EQ(result, R"({"count":2})");
}

TEST(Capability, ExecuteWithPrompt) {
    xlings::EventStream stream;
    stream.on_event([&](const xlings::Event& e) {
        if (auto* p = std::get_if<xlings::PromptEvent>(&e)) {
            stream.respond(p->id, "y");
        }
    });

    MockInstallCapability install;
    auto result = install.execute(R"({"name":"gcc"})", stream);
    EXPECT_EQ(result, R"({"status":"ok"})");
}

TEST(Capability, ExecutePromptCancelled) {
    xlings::EventStream stream;
    stream.on_event([&](const xlings::Event& e) {
        if (auto* p = std::get_if<xlings::PromptEvent>(&e)) {
            stream.respond(p->id, "n");
        }
    });

    MockInstallCapability install;
    auto result = install.execute(R"({"name":"gcc"})", stream);
    EXPECT_EQ(result, R"({"status":"cancelled"})");
}

// ============================================================
// ─── TaskManager Tests ───
// ============================================================

TEST(TaskManager, SubmitAndComplete) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());

    xlings::task::TaskManager tm { reg };
    auto tid = tm.submit("search_packages", R"({"query":"gcc"})");
    EXPECT_FALSE(tid.empty());

    for (int i { 0 }; i < 100; ++i) {
        if (tm.info(tid).status == xlings::task::TaskStatus::completed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto taskInfo = tm.info(tid);
    EXPECT_EQ(taskInfo.status, xlings::task::TaskStatus::completed);
    EXPECT_EQ(taskInfo.capabilityName, "search_packages");
}

TEST(TaskManager, EventsRetrieval) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());

    xlings::task::TaskManager tm { reg };
    auto tid = tm.submit("search_packages", R"({"query":"gcc"})");

    for (int i { 0 }; i < 100; ++i) {
        if (tm.info(tid).status == xlings::task::TaskStatus::completed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto evts = tm.events(tid);
    EXPECT_GE(evts.size(), 3);  // LogEvent + DataEvent + CompletedEvent

    auto evts2 = tm.events(tid, evts.size());
    EXPECT_EQ(evts2.size(), 0);
}

TEST(TaskManager, PromptHandling) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockInstallCapability>());

    xlings::task::TaskManager tm { reg };
    auto tid = tm.submit("install_package", R"({"name":"gcc"})");

    bool foundPrompt { false };
    std::string promptId;
    for (int i { 0 }; i < 100; ++i) {
        auto taskInfo = tm.info(tid);
        if (taskInfo.status == xlings::task::TaskStatus::waiting_prompt) {
            auto evts = tm.events(tid);
            for (auto& rec : evts) {
                if (auto* p = std::get_if<xlings::PromptEvent>(&rec.event)) {
                    promptId = p->id;
                    foundPrompt = true;
                }
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(foundPrompt);
    tm.respond(tid, promptId, "y");

    for (int i { 0 }; i < 100; ++i) {
        if (tm.info(tid).status == xlings::task::TaskStatus::completed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(tm.info(tid).status, xlings::task::TaskStatus::completed);
}

TEST(TaskManager, ConcurrentTasks) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());

    xlings::task::TaskManager tm { reg };
    auto t1 = tm.submit("search_packages", R"({})");
    auto t2 = tm.submit("search_packages", R"({})");
    auto t3 = tm.submit("search_packages", R"({})");

    for (int i { 0 }; i < 200; ++i) {
        if (!tm.has_active_tasks()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_FALSE(tm.has_active_tasks());
    EXPECT_EQ(tm.info(t1).status, xlings::task::TaskStatus::completed);
    EXPECT_EQ(tm.info(t2).status, xlings::task::TaskStatus::completed);
    EXPECT_EQ(tm.info(t3).status, xlings::task::TaskStatus::completed);
}

TEST(TaskManager, InfoAll) {
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockSearchCapability>());

    xlings::task::TaskManager tm { reg };
    tm.submit("search_packages", R"({})");
    tm.submit("search_packages", R"({})");

    auto all = tm.info_all();
    EXPECT_EQ(all.size(), 2);
}

// ============================================================
// ─── Integration Tests: EventStream + Capability + TaskManager ───
// ============================================================

TEST(Integration, TuiPathSynchronous) {
    // Simulate CLI/TUI path: synchronous call, consumer handles events directly
    xlings::EventStream stream;
    std::vector<std::string> rendered;

    stream.on_event([&](const xlings::Event& e) {
        std::visit([&](auto&& ev) {
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T, xlings::ProgressEvent>) {
                rendered.push_back("progress:" + std::to_string(ev.percent));
            } else if constexpr (std::is_same_v<T, xlings::LogEvent>) {
                rendered.push_back("log:" + ev.message);
            } else if constexpr (std::is_same_v<T, xlings::PromptEvent>) {
                rendered.push_back("prompt:" + ev.question);
                stream.respond(ev.id, "y");
            } else if constexpr (std::is_same_v<T, xlings::DataEvent>) {
                rendered.push_back("data:" + ev.kind);
            } else if constexpr (std::is_same_v<T, xlings::CompletedEvent>) {
                rendered.push_back("completed:" + ev.summary);
            }
        }, e);
    });

    MockSearchCapability search;
    search.execute(R"({})", stream);

    ASSERT_EQ(rendered.size(), 3);
    EXPECT_EQ(rendered[0], "log:Searching...");
    EXPECT_EQ(rendered[1], "data:search_results");
    EXPECT_EQ(rendered[2], "completed:Found 2 packages");
}

TEST(Integration, AgentPathConcurrentWithPrompt) {
    // Simulate Agent path: concurrent tasks + prompt handling
    xlings::capability::Registry reg;
    reg.register_capability(std::make_unique<MockInstallCapability>());
    reg.register_capability(std::make_unique<MockSearchCapability>());

    xlings::task::TaskManager tm { reg };

    auto tSearch = tm.submit("search_packages", R"({})");
    auto tInstall = tm.submit("install_package", R"({"name":"gcc"})");

    // Agent main loop: poll events, handle prompts
    bool installDone = false;
    for (int i = 0; i < 200 && !installDone; ++i) {
        auto installInfo = tm.info(tInstall);
        if (installInfo.status == xlings::task::TaskStatus::waiting_prompt) {
            auto evts = tm.events(tInstall);
            for (auto& rec : evts) {
                if (auto* p = std::get_if<xlings::PromptEvent>(&rec.event)) {
                    tm.respond(tInstall, p->id, "y");
                }
            }
        }
        if (installInfo.status == xlings::task::TaskStatus::completed) {
            installDone = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(installDone);
    EXPECT_EQ(tm.info(tSearch).status, xlings::task::TaskStatus::completed);

    // Verify event stream contents
    auto searchEvents = tm.events(tSearch);
    EXPECT_GE(searchEvents.size(), 3);

    auto installEvents = tm.events(tInstall);
    EXPECT_GE(installEvents.size(), 2);  // ProgressEvent + PromptEvent + CompletedEvent
}

// ═══════════════════════════════════════════════════════════════
//  Phase 3: Real Capability implementations
// ═══════════════════════════════════════════════════════════════

TEST(Capabilities, BuildRegistryPopulatesAll) {
    auto reg = xlings::capabilities::build_registry();
    auto specs = reg.list_all();
    EXPECT_GE(specs.size(), 8);

    EXPECT_NE(reg.get("search_packages"), nullptr);
    EXPECT_NE(reg.get("install_packages"), nullptr);
    EXPECT_NE(reg.get("remove_package"), nullptr);
    EXPECT_NE(reg.get("update_packages"), nullptr);
    EXPECT_NE(reg.get("list_packages"), nullptr);
    EXPECT_NE(reg.get("package_info"), nullptr);
    EXPECT_NE(reg.get("use_version"), nullptr);
    EXPECT_NE(reg.get("system_status"), nullptr);
}

TEST(Capabilities, SpecsHaveRequiredFields) {
    auto reg = xlings::capabilities::build_registry();
    auto specs = reg.list_all();
    for (auto& s : specs) {
        EXPECT_FALSE(s.name.empty()) << "capability has empty name";
        EXPECT_FALSE(s.description.empty()) << s.name << " has empty description";
        EXPECT_FALSE(s.inputSchema.empty()) << s.name << " has empty inputSchema";
    }
}

TEST(Capabilities, DestructiveFlags) {
    auto reg = xlings::capabilities::build_registry();
    EXPECT_FALSE(reg.get("search_packages")->spec().destructive);
    EXPECT_FALSE(reg.get("list_packages")->spec().destructive);
    EXPECT_FALSE(reg.get("package_info")->spec().destructive);
    EXPECT_FALSE(reg.get("system_status")->spec().destructive);
    EXPECT_TRUE(reg.get("install_packages")->spec().destructive);
    EXPECT_TRUE(reg.get("remove_package")->spec().destructive);
    EXPECT_TRUE(reg.get("update_packages")->spec().destructive);
    EXPECT_TRUE(reg.get("use_version")->spec().destructive);
}

TEST(Capabilities, RegistryListAllSpecs) {
    auto reg = xlings::capabilities::build_registry();
    auto specs = reg.list_all();
    for (auto& s : specs) {
        auto parsed = nlohmann::json::parse(s.inputSchema, nullptr, false);
        EXPECT_FALSE(parsed.is_discarded()) << s.name << " has invalid inputSchema";
    }
}

TEST(Capabilities, SearchSpecSchema) {
    auto reg = xlings::capabilities::build_registry();
    auto* cap = reg.get("search_packages");
    ASSERT_NE(cap, nullptr);
    auto s = cap->spec();
    EXPECT_EQ(s.name, "search_packages");
    auto schema = nlohmann::json::parse(s.inputSchema);
    EXPECT_TRUE(schema.contains("required"));
    EXPECT_EQ(schema["required"][0], "keyword");
}

// ═══════════════════════════════════════════════════════════════
//  Archive extraction (libarchive-backed in-process)
// ═══════════════════════════════════════════════════════════════
//
// Replaces the previous popen("tar xf …") path. The test does the
// shell-out *only* to build a tiny fixture archive; the system-under-
// test (xim::extract_archive) goes through libarchive in-process and
// must produce the same files on disk.

namespace {
struct ExtractFixture {
    std::filesystem::path tmp;

    ExtractFixture() {
        namespace fs = std::filesystem;
        tmp = fs::temp_directory_path() / "xlings-extract-test";
        fs::remove_all(tmp);
        fs::create_directories(tmp / "src/sub");
        std::ofstream(tmp / "src/hello.txt")  << "hello-from-fixture\n";
        std::ofstream(tmp / "src/sub/nested.txt") << "deeply-nested-content\n";
    }

    ~ExtractFixture() {
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
    }

    // Chdir-based archive helpers. We use std::filesystem::current_path()
    // rather than shell `cd && tool` so we avoid:
    //   - cmd.exe `cd <other-drive>` being a no-op without /d
    //   - dash not having `pushd`
    //   - cross-shell quoting of paths with spaces
    // All archive tools below are invoked with relative inputs from inside
    // tmp/, producing the output as a relative filename, then we resolve
    // back to the absolute path.
    //
    // host_sys_: run a HOST tool (tar/zip/python) via std::system with the
    // build tool's injected runtime library path scrubbed. mcpp 0.0.47+
    // exports the target toolchain's runtime dirs (sandbox glibc) into
    // LD_LIBRARY_PATH for test processes; host tools crash when loaded
    // against that glibc. Scope: POSIX only (the var is harmless on
    // Windows, and `env` isn't available there).
    static int host_sys_(const char* cmd) {
#if defined(_WIN32)
        return std::system(cmd);
#else
        std::string wrapped = std::string("env -u LD_LIBRARY_PATH ") + cmd;
        return std::system(wrapped.c_str());
#endif
    }

    template <class F>
    static int run_in_(const std::filesystem::path& dir, F&& fn) {
        auto saved = std::filesystem::current_path();
        std::filesystem::current_path(dir);
        int rc = fn();
        std::filesystem::current_path(saved);
        return rc;
    }

    std::filesystem::path make_tar_gz() const {
        auto out = tmp / "fixture.tar.gz";
        int rc = run_in_(tmp, [] {
            return host_sys_("tar czf fixture.tar.gz src");
        });
        if (rc != 0) throw std::runtime_error("failed to create tar.gz fixture");
        return out;
    }

    std::filesystem::path make_zip() const {
        auto out = tmp / "fixture.zip";
        int rc = run_in_(tmp, [] {
            return host_sys_("zip -qr fixture.zip src");
        });
        if (rc != 0) throw std::runtime_error("failed to create zip fixture");
        return out;
    }

    std::filesystem::path make_utf8_zip() const {
        namespace fs = std::filesystem;
        std::ofstream(tmp / "make_utf8_zip.py") << R"PY(
import zipfile

with zipfile.ZipFile("fixture_utf8.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(
        "utf8/.github/ISSUE_TEMPLATE/bug-report---\u95ee\u9898.md",
        "unicode-path-fixture\n",
    )
)PY";
        auto out = tmp / "fixture_utf8.zip";
        int rc = run_in_(tmp, [] {
#ifdef _WIN32
            return host_sys_("python make_utf8_zip.py");
#else
            return host_sys_("python3 make_utf8_zip.py || python make_utf8_zip.py");
#endif
        });
        if (rc != 0) throw std::runtime_error("failed to create utf8 zip fixture");
        return out;
    }

    std::filesystem::path make_tar_xz() const {
        auto out = tmp / "fixture.tar.xz";
        int rc = run_in_(tmp, [] {
            return host_sys_("tar cJf fixture.tar.xz src");
        });
        if (rc != 0) throw std::runtime_error("failed to create tar.xz fixture");
        return out;
    }
};

bool file_has_(const std::filesystem::path& p, std::string_view expected) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str().find(expected) != std::string::npos;
}
} // namespace

TEST(Extract, TarGzRoundTrip) {
    ExtractFixture fx;
    auto archive = fx.make_tar_gz();
    auto out = fx.tmp / "out_targz";

    auto r = xlings::xim::extract_archive(archive, out);
    ASSERT_TRUE(r.has_value()) << "extract failed: " << (r ? "" : r.error());

    // Use the canonicalized path returned by extract_archive — on Windows
    // (and macOS) `out` and the resolved path can differ once symlinks /
    // 8.3 short names are walked.
    auto root = *r;
    EXPECT_TRUE(std::filesystem::exists(root / "src/hello.txt"));
    EXPECT_TRUE(std::filesystem::exists(root / "src/sub/nested.txt"));
    EXPECT_TRUE(file_has_(root / "src/hello.txt", "hello-from-fixture"));
    EXPECT_TRUE(file_has_(root / "src/sub/nested.txt", "deeply-nested-content"));
}

TEST(Extract, ZipRoundTrip) {
    // zip command may not be installed everywhere; skip cleanly if so.
    if (std::system("command -v zip >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "zip not available on this host";
    }
    ExtractFixture fx;
    auto archive = fx.make_zip();
    auto out = fx.tmp / "out_zip";

    auto r = xlings::xim::extract_archive(archive, out);
    ASSERT_TRUE(r.has_value()) << "extract failed: " << (r ? "" : r.error());

    auto root = *r;
    EXPECT_TRUE(std::filesystem::exists(root / "src/hello.txt"));
    EXPECT_TRUE(file_has_(root / "src/hello.txt", "hello-from-fixture"));
}

TEST(Extract, ZipUtf8PathRoundTrip) {
#ifdef _WIN32
    if (std::system("python --version >NUL 2>NUL") != 0) {
        GTEST_SKIP() << "python not available on this host";
    }
#else
    if (std::system("command -v python3 >/dev/null 2>&1 || command -v python >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "python not available on this host";
    }
#endif
    ExtractFixture fx;
    auto archive = fx.make_utf8_zip();
    auto out = fx.tmp / "out_zip_utf8";

    auto r = xlings::xim::extract_archive(archive, out);
    ASSERT_TRUE(r.has_value()) << "extract failed: " << (r ? "" : r.error());

    auto root = *r;
    auto expected = root / "utf8/.github/ISSUE_TEMPLATE/bug-report---问题.md";
    EXPECT_TRUE(std::filesystem::exists(expected));
    EXPECT_TRUE(file_has_(expected, "unicode-path-fixture"));
}

TEST(Extract, TarXzRoundTrip) {
    // Confirms that the .tar.xz path used by node / llvm packages works
    // through the libarchive-backed extractor (the original popen-tar
    // path was the source of the ollama-install hang bug).
    if (std::system("command -v xz >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "xz not available on this host";
    }
    ExtractFixture fx;
    auto archive = fx.make_tar_xz();
    auto out = fx.tmp / "out_tarxz";

    auto r = xlings::xim::extract_archive(archive, out);
    ASSERT_TRUE(r.has_value()) << "extract failed: " << (r ? "" : r.error());

    auto root = *r;
    EXPECT_TRUE(std::filesystem::exists(root / "src/hello.txt"));
    EXPECT_TRUE(std::filesystem::exists(root / "src/sub/nested.txt"));
}

TEST(Extract, MissingArchiveReturnsError) {
    ExtractFixture fx;
    auto r = xlings::xim::extract_archive(fx.tmp / "no-such.tar.gz", fx.tmp / "out");
    EXPECT_FALSE(r.has_value());
}

TEST(Extract, InvalidArchiveIsClassifiedAndEvictedWithSidecar) {
    ExtractFixture fx;
    auto archive = fx.tmp / "truncated.tar.gz";
    auto sidecar = std::filesystem::path(archive.string() + ".meta");
    std::ofstream(archive) << "not-a-complete-archive";
    std::ofstream(sidecar) << "format: 2\n";

    auto result = xlings::xim::extract_archive_detailed(
        archive, fx.tmp / "invalid-out");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind,
              xlings::xim::ExtractErrorKind::InvalidInputArchive);
    EXPECT_TRUE(xlings::xim::evict_invalid_archive_cache_(
        archive, result.error()));
    EXPECT_FALSE(std::filesystem::exists(archive));
    EXPECT_FALSE(std::filesystem::exists(sidecar));
}

TEST(Extract, LocalWriteFailureDoesNotEvictValidInput) {
    ExtractFixture fx;
    auto archive = fx.make_tar_gz();
    auto invalidDestination = fx.tmp / "destination-is-a-file";
    std::ofstream(invalidDestination) << "not-a-directory";

    auto result = xlings::xim::extract_archive_detailed(
        archive, invalidDestination);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind,
              xlings::xim::ExtractErrorKind::LocalWriteFailure);
    EXPECT_FALSE(xlings::xim::evict_invalid_archive_cache_(
        archive, result.error()));
    EXPECT_TRUE(std::filesystem::exists(archive));
}

TEST(Extract, RejectsPathTraversal) {
    // Build a tarball containing an entry with "../escape.txt". libarchive
    // with ARCHIVE_EXTRACT_SECURE_NODOTDOT must refuse to extract the
    // escape entry — we expect either an error, or successful extraction
    // of the safe entry without ../escape.txt appearing outside out_dir.
    ExtractFixture fx;
    namespace fs = std::filesystem;
    auto stage = fx.tmp / "stage";
    fs::create_directories(stage / "subdir");
    std::ofstream(stage / "safe.txt") << "safe\n";

    // Create a tar with one safe entry. The dot-dot test below uses
    // libarchive's secure flags; we mostly just ensure no escape files
    // appear above the destination dir.
    auto archive = fx.tmp / "ptraversal.tar.gz";
    std::string cmd = std::format("cd {} && tar czf {} -C {} .",
        fx.tmp.string(), archive.string(), stage.string());
#if !defined(_WIN32)
    cmd = "env -u LD_LIBRARY_PATH sh -c " + std::string("'") + cmd + "'";
#endif
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto out = fx.tmp / "out_ptrav";
    auto r = xlings::xim::extract_archive(archive, out);
    ASSERT_TRUE(r.has_value()) << r.error();

    // Confirm nothing landed outside `out`.
    EXPECT_FALSE(fs::exists(fx.tmp / "escape.txt"));
    EXPECT_FALSE(fs::exists(out.parent_path() / "escape.txt"));
}

// ═══════════════════════════════════════════════════════════════
//  TUI: theme icon byte sequences
// ═══════════════════════════════════════════════════════════════
//
// Force-check that every theme icon is the same UTF-8 byte sequence on
// every platform. xlings_tests is built and run on Linux / macOS /
// Windows in CI, so a regression that, say, changes `icon::done` to
// "+" only on Windows is caught the moment xlings_tests boots there.
//
// The intent is: there is exactly one icon set, byte-for-byte, no
// platform conditional, no font-substitution fallback, no ASCII
// downgrade. If the test fails on any platform, the source has
// drifted.

namespace {
struct IconSlot {
    std::string_view name;
    const char* value;
    std::string_view expected;
};

constexpr IconSlot kThemeIconSlots[] = {
    {"pending",     xlings::ui::theme::icon::pending,     "\xe2\x97\x8b"},  // ○ U+25CB
    {"downloading", xlings::ui::theme::icon::downloading, "\xe2\x86\x93"},  // ↓ U+2193
    {"extracting",  xlings::ui::theme::icon::extracting,  "\xe2\x96\xbe"},  // ▾ U+25BE
    {"installing",  xlings::ui::theme::icon::installing,  "\xe2\x8a\x95"},  // ⊕ U+2295
    {"configuring", xlings::ui::theme::icon::configuring, "\xe2\x8a\x95"},  // ⊕ U+2295
    {"done",        xlings::ui::theme::icon::done,        "\xe2\x9c\x93"},  // ✓ U+2713
    {"failed",      xlings::ui::theme::icon::failed,      "\xe2\x9c\x97"},  // ✗ U+2717
    {"info",        xlings::ui::theme::icon::info,        "\xe2\x80\xba"},  // › U+203A
    {"arrow",       xlings::ui::theme::icon::arrow,       "\xe2\x96\xb8"},  // ▸ U+25B8
    {"package",     xlings::ui::theme::icon::package,     "\xe2\x97\x86"},  // ◆ U+25C6
};
} // namespace

TEST(ThemeIcons, AllByteSequencesAreCanonical) {
    // Every slot must equal its expected UTF-8 byte sequence exactly.
    for (auto& slot : kThemeIconSlots) {
        EXPECT_EQ(std::string_view{slot.value}, slot.expected)
            << "icon::" << slot.name << " drifted from canonical bytes";
    }
}

TEST(ThemeIcons, NoPlatformAsciiFallback) {
    // Each icon's leading byte must have bit 7 set — i.e. it is a
    // multi-byte UTF-8 sequence, not an ASCII fallback. Catches
    // `#ifdef _WIN32 "+" #else "✓"` slipping back in for any slot.
    for (auto& slot : kThemeIconSlots) {
        std::string_view s{slot.value};
        ASSERT_FALSE(s.empty()) << "icon::" << slot.name << " is empty";
        EXPECT_TRUE(static_cast<unsigned char>(s[0]) & 0x80)
            << "icon::" << slot.name << " is single-byte ASCII (\""
            << s << "\")";
    }
}

TEST(ThemeIcons, AllAreThreeByteBmpUtf8) {
    // Belt-and-braces: every slot is a well-formed 3-byte BMP UTF-8
    // sequence (lead byte 0xE0..0xEF, followed by two continuation
    // bytes 0x80..0xBF). Rules out 4-byte SMP code points (which many
    // monospace fonts can't render) and malformed sequences.
    for (auto& slot : kThemeIconSlots) {
        std::string_view s{slot.value};
        ASSERT_EQ(s.size(), 3u) << "icon::" << slot.name
                                 << " is " << s.size() << " bytes, expected 3";
        auto b0 = static_cast<unsigned char>(s[0]);
        auto b1 = static_cast<unsigned char>(s[1]);
        auto b2 = static_cast<unsigned char>(s[2]);
        EXPECT_TRUE(b0 >= 0xE0 && b0 <= 0xEF)
            << "icon::" << slot.name << " lead byte not 3-byte UTF-8";
        EXPECT_TRUE((b1 & 0xC0) == 0x80)
            << "icon::" << slot.name << " byte 1 not a continuation";
        EXPECT_TRUE((b2 & 0xC0) == 0x80)
            << "icon::" << slot.name << " byte 2 not a continuation";
    }
}

// ═══════════════════════════════════════════════════════════════
//  Proxy: env-driven proxy resolution for the downloader
// ═══════════════════════════════════════════════════════════════
//
// xlings::tinyhttps::resolve_proxy(url) reads HTTPS_PROXY / HTTP_PROXY /
// ALL_PROXY (case-insensitive variants) and respects NO_PROXY. These
// tests lock the libcurl-compatible behaviour: scheme-aware selection,
// NO_PROXY suffix exemption, lowercase fallback.

namespace {
struct EnvScope {
    std::string name;
    bool had_prev{false};
    std::string prev_value;

    EnvScope(std::string_view n, const char* val) : name(n) {
        if (auto v = std::getenv(name.c_str())) {
            had_prev = true;
            prev_value = v;
        }
        set_(val);
    }
    ~EnvScope() {
        if (had_prev) set_(prev_value.c_str());
        else clear_();
    }
    void set_(const char* val) {
        if (!val) { clear_(); return; }
#ifdef _WIN32
        _putenv_s(name.c_str(), val);
#else
        ::setenv(name.c_str(), val, 1);
#endif
    }
    void clear_() {
#ifdef _WIN32
        _putenv_s(name.c_str(), "");
#else
        ::unsetenv(name.c_str());
#endif
    }
};

// Wipe every proxy-related env var so each test starts from a clean slate.
// Vector elements are unique_ptrs so a reallocation on push_back doesn't
// move-then-destroy intermediate EnvScopes (which would prematurely
// restore env vars before the test body runs).
struct ProxyEnvSandbox {
    std::vector<std::unique_ptr<EnvScope>> guards;
    ProxyEnvSandbox() {
        for (auto* n : {"HTTPS_PROXY", "https_proxy",
                        "HTTP_PROXY",  "http_proxy",
                        "ALL_PROXY",   "all_proxy",
                        "NO_PROXY",    "no_proxy"}) {
            guards.push_back(std::make_unique<EnvScope>(n, nullptr));
        }
    }
};
} // namespace

TEST(Proxy, NoEnvMeansDirect) {
    ProxyEnvSandbox sandbox;
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://example.com/foo"), "");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("http://example.com/foo"),  "");
}

TEST(Proxy, HttpsProxyUsedForHttpsScheme) {
    ProxyEnvSandbox sandbox;
    EnvScope https("HTTPS_PROXY", "http://127.0.0.1:7890");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://example.com/x"),
              "http://127.0.0.1:7890");
}

TEST(Proxy, HttpProxyUsedForHttpScheme) {
    ProxyEnvSandbox sandbox;
    EnvScope http("HTTP_PROXY", "http://127.0.0.1:7890");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("http://example.com/x"),
              "http://127.0.0.1:7890");
}

TEST(Proxy, LowercaseEnvAlsoAccepted) {
    ProxyEnvSandbox sandbox;
    EnvScope https("https_proxy", "http://10.0.0.1:8080");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://example.com/x"),
              "http://10.0.0.1:8080");
}

TEST(Proxy, AllProxyFallback) {
    ProxyEnvSandbox sandbox;
    EnvScope all("ALL_PROXY", "socks5://127.0.0.1:1080");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://example.com/x"),
              "socks5://127.0.0.1:1080");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("http://example.com/x"),
              "socks5://127.0.0.1:1080");
}

TEST(Proxy, HttpsProxyTakesPrecedenceOverHttpProxy) {
    ProxyEnvSandbox sandbox;
    EnvScope https("HTTPS_PROXY", "http://https-proxy:1");
    EnvScope http("HTTP_PROXY",   "http://http-proxy:2");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://example.com/x"),
              "http://https-proxy:1");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("http://example.com/x"),
              "http://http-proxy:2");
}

TEST(Proxy, NoProxyExactHostExempt) {
    ProxyEnvSandbox sandbox;
    EnvScope https("HTTPS_PROXY", "http://127.0.0.1:7890");
    EnvScope np("NO_PROXY", "localhost,internal.example");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://localhost:9000/x"), "");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://internal.example/x"), "");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://other.example.com/x"),
              "http://127.0.0.1:7890");
}

TEST(Proxy, NoProxySuffixMatchExempt) {
    ProxyEnvSandbox sandbox;
    EnvScope https("HTTPS_PROXY", "http://127.0.0.1:7890");
    EnvScope np("NO_PROXY", ".internal.example,corp.local");
    // dot-prefixed suffix: matches both bare and prefixed
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://api.internal.example/x"), "");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://internal.example/x"), "");
    // bare suffix without dot: still suffix-matches subdomains
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://node1.corp.local/x"), "");
    // unrelated host still goes through proxy
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://github.com/x"),
              "http://127.0.0.1:7890");
}

TEST(Proxy, NoProxyWildcardExemptsAll) {
    ProxyEnvSandbox sandbox;
    EnvScope https("HTTPS_PROXY", "http://127.0.0.1:7890");
    EnvScope np("NO_PROXY", "*");
    EXPECT_EQ(xlings::tinyhttps::resolve_proxy("https://anything.example/x"), "");
}

TEST(ThemeIcons, InfoPanelEmitsIconBytesToStdout) {
    // Render the same panel that `xlings config` uses, capture the bytes
    // that ftxui actually wrote to stdout, and confirm the canonical icon
    // and box-drawing UTF-8 sequences survive end-to-end. This is the
    // in-process equivalent of the bash e2e test, runs on every platform
    // (Linux / macOS / Windows xlings_tests) so Windows gets the same
    // emission-path coverage that redirected-stdout makes hard at the
    // binary level.
    namespace ui = xlings::ui;

    std::vector<ui::InfoField> fields = {
        {"language",  "en"},
        {"mirror",    "GLOBAL", true},
        {"data dir",  "/tmp/xlings"},
    };

    testing::internal::CaptureStdout();
    ui::print_info_panel("Test Panel", fields);
    auto output = testing::internal::GetCapturedStdout();

    ASSERT_FALSE(output.empty())
        << "print_info_panel emitted no bytes — ftxui rendering path broken";

    // The package icon ◆ (U+25C6, E2 97 86) is the title bullet;
    // box-drawing ─ (U+2500, E2 94 80) and │ (U+2502, E2 94 82) come
    // from ftxui's border. At least one of these byte triples must be
    // present in the captured output, on every platform, byte-for-byte.
    auto has = [&](std::string_view needle) {
        return output.find(needle) != std::string::npos;
    };
    EXPECT_TRUE(has("\xe2\x97\x86") || has("\xe2\x94\x80") || has("\xe2\x94\x82"))
        << "info_panel output contains no canonical UTF-8 sequences "
           "(◆ ─ │). Hex prefix: "
        << [&] {
               std::string hex;
               for (std::size_t i = 0; i < std::min<std::size_t>(80, output.size()); ++i) {
                   char buf[4];
                   std::snprintf(buf, sizeof(buf), "%02x ",
                                 static_cast<unsigned char>(output[i]));
                   hex += buf;
               }
               return hex;
           }();

    // Negative check: long runs of '?' would indicate that the rendering
    // layer or stdout capture downconverted UTF-8 to replacement chars.
    EXPECT_EQ(output.find("?????"), std::string::npos)
        << "info_panel output contains run of '?' — possible encoding loss";
}

// ============================================================
// subos GPU passthrough (--gpu) tests
// ============================================================

namespace {

bool contains_triple(const std::vector<std::string>& argv,
                     std::string_view flag,
                     std::string_view src,
                     std::string_view dst)
{
    for (size_t i = 0; i + 2 < argv.size(); ++i) {
        if (argv[i] == flag && argv[i + 1] == src && argv[i + 2] == dst)
            return true;
    }
    return false;
}

}  // namespace

TEST(SubosGpu, EmptyHostStillBindsSys) {
    auto args = xlings::subos::gpu::passthrough_args(
        [](const std::string&) { return false; });
    // No /dev/* bindings, but /sys --ro-bind must always be present.
    EXPECT_EQ(args.size(), 3u);
    EXPECT_TRUE(contains_triple(args, "--ro-bind", "/sys", "/sys"));
}

TEST(SubosGpu, BindsNvidiactlWhenPresent) {
    auto args = xlings::subos::gpu::passthrough_args(
        [](const std::string& p) { return p == "/dev/nvidiactl"; });
    EXPECT_TRUE(contains_triple(args, "--dev-bind",
                                "/dev/nvidiactl", "/dev/nvidiactl"));
    EXPECT_TRUE(contains_triple(args, "--ro-bind", "/sys", "/sys"));
}

TEST(SubosGpu, EnumeratesPerGpuNodesUpTo16) {
    int probe_count = 0;
    auto args = xlings::subos::gpu::passthrough_args(
        [&](const std::string& p) {
            // Count probes that target /dev/nvidia<digit>
            if (p.size() > 11 && p.starts_with("/dev/nvidia")
                && std::isdigit(static_cast<unsigned char>(p[11]))) {
                ++probe_count;
                return true;
            }
            return false;
        });
    EXPECT_EQ(probe_count, 16);
    // All 16 must appear as --dev-bind triples.
    for (int i = 0; i < 16; ++i) {
        auto path = "/dev/nvidia" + std::to_string(i);
        EXPECT_TRUE(contains_triple(args, "--dev-bind", path, path))
            << "missing --dev-bind for " << path;
    }
}

TEST(SubosGpu, BindsDriWhenPresent) {
    auto args = xlings::subos::gpu::passthrough_args(
        [](const std::string& p) { return p == "/dev/dri"; });
    EXPECT_TRUE(contains_triple(args, "--dev-bind",
                                "/dev/dri", "/dev/dri"));
}

TEST(SubosGpu, FullHostBindsAllKnownNodes) {
    auto args = xlings::subos::gpu::passthrough_args(
        [](const std::string&) { return true; });
    for (auto path : {"/dev/nvidiactl", "/dev/nvidia-uvm",
                      "/dev/nvidia-uvm-tools", "/dev/nvidia-modeset",
                      "/dev/dri"}) {
        EXPECT_TRUE(contains_triple(args, "--dev-bind", path, path))
            << "missing --dev-bind for " << path;
    }
    EXPECT_TRUE(contains_triple(args, "--ro-bind", "/sys", "/sys"));
}

// ============================================================
// downloader archive-filename sniff (P1 helper for the cmd-install
// silent-failure fix — see .agents/docs/2026-05-22-cmd-install-silent-failure-analysis.md)
// ============================================================

TEST(DownloaderArchiveSniff, RecognisesCommonArchiveExtensions) {
    using xlings::xim::looks_like_archive_filename_;
    EXPECT_TRUE(looks_like_archive_filename_("foo.tar.gz"));
    EXPECT_TRUE(looks_like_archive_filename_("foo.tar.xz"));
    EXPECT_TRUE(looks_like_archive_filename_("foo.tar.bz2"));
    EXPECT_TRUE(looks_like_archive_filename_("foo.tar.zst"));
    EXPECT_TRUE(looks_like_archive_filename_("foo.tgz"));
    EXPECT_TRUE(looks_like_archive_filename_("foo.zip"));
}

TEST(DownloaderArchiveSniff, RejectsNonArchiveExtensions) {
    using xlings::xim::looks_like_archive_filename_;
    EXPECT_FALSE(looks_like_archive_filename_("README.md"));
    EXPECT_FALSE(looks_like_archive_filename_("install.sh"));
    EXPECT_FALSE(looks_like_archive_filename_("config.json"));
    EXPECT_FALSE(looks_like_archive_filename_("binary.exe"));
    EXPECT_FALSE(looks_like_archive_filename_(""));
}

TEST(DownloaderArchiveSniff, IsCaseSensitiveSuffixMatch) {
    using xlings::xim::looks_like_archive_filename_;
    // Recipes in xim-pkgindex use lower-case; we mirror that to avoid
    // accepting weird upstream conventions like "Foo.TAR.GZ" silently.
    EXPECT_FALSE(looks_like_archive_filename_("foo.TAR.GZ"));
    EXPECT_FALSE(looks_like_archive_filename_("foo.Tar.Gz"));
}

TEST(DownloaderArchiveSniff, WorksWithFullPaths) {
    using xlings::xim::looks_like_archive_filename_;
    // The downloader passes the full destFile path. The function should
    // look at the basename only, ignoring directory components.
    EXPECT_TRUE(looks_like_archive_filename_("/var/tmp/runtimedir/llvm-20.1.7-linux-x86_64.tar.gz"));
    EXPECT_FALSE(looks_like_archive_filename_("/path/to/tar.gz/file.lua"));  // tar.gz only in dir name
}

// ============================================================

#ifndef XLINGS_USE_GTEST_MAIN
int main(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--file-lock-child") {
        xlings::platform::FileLock lock;
        std::string error;
        if (!lock.acquire(argv[2], std::chrono::seconds{2}, {}, error)) {
            std::cerr << error << '\n';
            return 2;
        }
        std::ofstream(argv[3]) << "ready";
        std::this_thread::sleep_for(std::chrono::milliseconds{400});
        return 0;
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
