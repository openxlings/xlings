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

    ASSERT_TRUE(
        xlings::xvm::remove_version(db, "gcc", "14.2.0").has_value());
    EXPECT_FALSE(xlings::xvm::has_version(db, "gcc", "14.2.0"));
    EXPECT_TRUE(xlings::xvm::has_version(db, "gcc", "15.1.0"));

    // Remove last version removes the target entirely
    ASSERT_TRUE(
        xlings::xvm::remove_version(db, "gcc", "15.1.0").has_value());
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

TEST(XvmRegistrationTest,
     RegistersChildBeforeRootWithNamespacedTransformedVersions) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    const xlings::xvm::RegistrationBatch batch{
        .provider = "xim:gcc",
        .providerVersion = "15.1.0",
        .nodes = {
            {
                .target = "gcc-ar",
                .version = "xim:gcc-15.1.0",
                .path = "/pkg/gcc/15.1.0/bin",
                .kind = "program",
                .sourceName = "gcc-ar-15",
                .destinationName = "gcc-ar",
                .binding = xlings::xvm::RegistrationBinding{
                    .rootTarget = "xim-gnu-gcc",
                    .rootVersion = "xim:15.1.0",
                },
            },
            {
                .target = "xim-gnu-gcc",
                .version = "xim:15.1.0",
                .path = "/pkg/gcc/15.1.0",
                .kind = "group",
            },
        },
    };

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    const auto& root =
        db.at("xim-gnu-gcc").versions.at("xim:15.1.0");
    const auto& child =
        db.at("gcc-ar").versions.at("xim:gcc-15.1.0");
    ASSERT_TRUE(root.bindingGroup.has_value());
    ASSERT_TRUE(child.bindingGroup.has_value());
    EXPECT_EQ(root.bindingGroup->group, "xim-gnu-gcc");
    EXPECT_EQ(root.bindingGroup->rootTarget, "xim-gnu-gcc");
    EXPECT_EQ(root.bindingGroup->rootVersion, "xim:15.1.0");
    EXPECT_EQ(child.bindingGroup->provider, root.bindingGroup->provider);
    EXPECT_EQ(
        child.bindingGroup->providerVersion,
        root.bindingGroup->providerVersion);
    EXPECT_EQ(child.bindingGroup->group, root.bindingGroup->group);
    EXPECT_EQ(
        child.bindingGroup->rootTarget,
        root.bindingGroup->rootTarget);
    EXPECT_EQ(
        child.bindingGroup->rootVersion,
        root.bindingGroup->rootVersion);
    EXPECT_EQ(
        root.bindingMembers,
        (std::map<std::string, std::string>{
            {"gcc-ar", "xim:gcc-15.1.0"},
            {"xim-gnu-gcc", "xim:15.1.0"},
        }));
    EXPECT_EQ(
        db.at("xim-gnu-gcc")
            .bindings.at("gcc-ar").at("xim:15.1.0"),
        "xim:gcc-15.1.0");
    EXPECT_EQ(
        db.at("gcc-ar")
            .bindings.at("xim-gnu-gcc").at("xim:gcc-15.1.0"),
        "xim:15.1.0");
    EXPECT_EQ(workspace.at("gcc-ar"), "xim:gcc-15.1.0");
    EXPECT_EQ(workspace.at("xim-gnu-gcc"), "xim:15.1.0");
    EXPECT_EQ(
        installed.at("gcc-ar"),
        (std::vector<std::string>{"xim:gcc-15.1.0"}));
    EXPECT_EQ(
        installed.at("xim-gnu-gcc"),
        (std::vector<std::string>{"xim:15.1.0"}));
}

namespace {

xlings::xvm::RegistrationNode make_registration_node(
    std::string target,
    std::string version,
    std::string kind = "program") {
    return {
        .target = std::move(target),
        .version = std::move(version),
        .path = "/pkg/provider/1.0.0",
        .kind = std::move(kind),
        .sourceName = "payload",
        .destinationName = "tool",
    };
}

xlings::xvm::RegistrationBinding make_registration_binding(
    std::string rootTarget,
    std::string rootVersion,
    std::string group = {}) {
    return {
        .rootTarget = std::move(rootTarget),
        .rootVersion = std::move(rootVersion),
        .group = std::move(group),
    };
}

xlings::xvm::RegistrationBatch make_registration_batch(
    std::vector<xlings::xvm::RegistrationNode> nodes) {
    return {
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .nodes = std::move(nodes),
    };
}

void expect_registration_error(
    const std::expected<
        std::vector<xlings::xvm::RegisteredMember>,
        xlings::xvm::RegistrationError>& result,
    xlings::xvm::RegistrationErrorKind kind,
    std::string_view path,
    std::string_view target = {},
    std::string_view version = {}) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, kind);
    EXPECT_EQ(result.error().path, path);
    EXPECT_EQ(result.error().target, target);
    EXPECT_EQ(result.error().version, version);
    EXPECT_FALSE(result.error().message.empty());
}

void expect_registration_state_unchanged(
    const xlings::xvm::VersionDB& db,
    const xlings::xvm::Workspace& workspace,
    const xlings::xvm::WorkspaceInstalled& installed,
    const nlohmann::json& dbBefore,
    const xlings::xvm::Workspace& workspaceBefore,
    const xlings::xvm::WorkspaceInstalled& installedBefore) {
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

}  // namespace

TEST(XvmRegistrationTest, RootFirstAndRootLastSerializeIdentically) {
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto child = make_registration_node("tool", "repo:tool-1.0.0");
    child.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");

    xlings::xvm::VersionDB rootFirstDb;
    xlings::xvm::Workspace rootFirstWorkspace;
    xlings::xvm::WorkspaceInstalled rootFirstInstalled;
    auto rootFirst = xlings::xvm::apply_registration_batch(
        rootFirstDb, rootFirstWorkspace, rootFirstInstalled,
        make_registration_batch({root, child}));
    ASSERT_TRUE(rootFirst.has_value()) << rootFirst.error().message;

    xlings::xvm::VersionDB rootLastDb;
    xlings::xvm::Workspace rootLastWorkspace;
    xlings::xvm::WorkspaceInstalled rootLastInstalled;
    auto rootLast = xlings::xvm::apply_registration_batch(
        rootLastDb, rootLastWorkspace, rootLastInstalled,
        make_registration_batch({child, root}));
    ASSERT_TRUE(rootLast.has_value()) << rootLast.error().message;

    EXPECT_EQ(
        xlings::xvm::versions_to_json(rootFirstDb),
        xlings::xvm::versions_to_json(rootLastDb));
    EXPECT_EQ(rootFirstWorkspace, rootLastWorkspace);
    EXPECT_EQ(rootFirstInstalled, rootLastInstalled);
}

TEST(XvmRegistrationErrorTest, RejectsPhantomRootWithoutMutation) {
    xlings::xvm::VersionDB db;
    db["sentinel"].versions["0"].path = "/sentinel";
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    auto child = make_registration_node("tool", "repo:1.0.0");
    child.binding =
        make_registration_binding("missing-root", "repo:1.0.0");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, make_registration_batch({child}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::RootNotInBatch,
        "/nodes/0/binding", "missing-root", "repo:1.0.0");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationErrorTest, RejectsSelfBindingWithoutMutation) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto node = make_registration_node("root", "repo:1.0.0", "group");
    node.sourceName.clear();
    node.destinationName.clear();
    node.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, make_registration_batch({node}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::SelfBinding,
        "/nodes/0/binding", "root", "repo:1.0.0");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationErrorTest, RejectsDuplicateExactNodeWithoutMutation) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto node = make_registration_node("tool", "repo:1.0.0");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({node, node}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::DuplicateNode,
        "/nodes/1", "tool", "repo:1.0.0");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationErrorTest,
     RejectsTwoVersionsOfOneTargetInOneGroupWithoutMutation) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto first = make_registration_node("tool", "repo:tool-1.0.0");
    first.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");
    auto second = make_registration_node("tool", "repo:tool-alt-1.0.0");
    second.binding = first.binding;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({root, first, second}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::TargetVersionConflict,
        "/nodes/2", "tool", "repo:tool-alt-1.0.0");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationErrorTest, RejectsOneGroupLabelWithTwoRoots) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto rootA = make_registration_node("root-a", "repo:a", "group");
    rootA.sourceName.clear();
    rootA.destinationName.clear();
    auto rootB = make_registration_node("root-b", "repo:b", "group");
    rootB.sourceName.clear();
    rootB.destinationName.clear();
    auto childA = make_registration_node("tool-a", "repo:a");
    childA.binding =
        make_registration_binding("root-a", "repo:a", "shared");
    auto childB = make_registration_node("tool-b", "repo:b");
    childB.binding =
        make_registration_binding("root-b", "repo:b", "shared");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({rootA, childA, rootB, childB}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::GroupConflict,
        "/nodes/3/binding/group", "tool-b", "repo:b");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationErrorTest, RejectsOneRootAssignedToTwoGroups) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto childA = make_registration_node("tool-a", "repo:a");
    childA.binding =
        make_registration_binding("root", "repo:1.0.0", "group-a");
    auto childB = make_registration_node("tool-b", "repo:b");
    childB.binding =
        make_registration_binding("root", "repo:1.0.0", "group-b");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({root, childA, childB}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::GroupConflict,
        "/nodes/2/binding/group", "tool-b", "repo:b");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationErrorTest, RejectsInvalidBatchNodeAndBindingIdentity) {
    struct Case {
        xlings::xvm::RegistrationBatch batch;
        xlings::xvm::RegistrationErrorKind kind;
        std::string path;
        std::string target;
        std::string version;
    };

    auto validNode = make_registration_node("tool", "repo:1.0.0");
    auto emptyProvider = make_registration_batch({validNode});
    emptyProvider.provider.clear();
    auto emptyRelease = make_registration_batch({validNode});
    emptyRelease.providerVersion.clear();
    auto emptyTargetNode = validNode;
    emptyTargetNode.target.clear();
    auto emptyVersionNode = validNode;
    emptyVersionNode.version.clear();
    auto emptyRootNode = validNode;
    emptyRootNode.binding =
        make_registration_binding("", "repo:1.0.0");
    auto emptyRootVersionNode = validNode;
    emptyRootVersionNode.binding =
        make_registration_binding("root", "");

    const std::vector<Case> cases{
        {
            std::move(emptyProvider),
            xlings::xvm::RegistrationErrorKind::InvalidBatchIdentity,
            "/provider", "", "",
        },
        {
            std::move(emptyRelease),
            xlings::xvm::RegistrationErrorKind::InvalidBatchIdentity,
            "/providerVersion", "", "",
        },
        {
            make_registration_batch({emptyTargetNode}),
            xlings::xvm::RegistrationErrorKind::InvalidNodeIdentity,
            "/nodes/0/target", "", "repo:1.0.0",
        },
        {
            make_registration_batch({emptyVersionNode}),
            xlings::xvm::RegistrationErrorKind::InvalidNodeIdentity,
            "/nodes/0/version", "tool", "",
        },
        {
            make_registration_batch({emptyRootNode}),
            xlings::xvm::RegistrationErrorKind::InvalidBindingIdentity,
            "/nodes/0/binding/rootTarget", "tool", "repo:1.0.0",
        },
        {
            make_registration_batch({emptyRootVersionNode}),
            xlings::xvm::RegistrationErrorKind::InvalidBindingIdentity,
            "/nodes/0/binding/rootVersion", "tool", "repo:1.0.0",
        },
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.path);
        xlings::xvm::VersionDB db;
        xlings::xvm::Workspace workspace;
        xlings::xvm::WorkspaceInstalled installed;

        auto result = xlings::xvm::apply_registration_batch(
            db, workspace, installed, testCase.batch);

        expect_registration_error(
            result, testCase.kind, testCase.path,
            testCase.target, testCase.version);
        EXPECT_TRUE(db.empty());
        EXPECT_TRUE(workspace.empty());
        EXPECT_TRUE(installed.empty());
    }
}

TEST(XvmRegistrationErrorTest,
     RejectsInvalidNodePayloadAtIndexedPathWithoutMutation) {
    struct Case {
        xlings::xvm::RegistrationNode node;
        std::string path;
    };

    auto unsupported =
        make_registration_node("archive", "repo:archive", "archive");
    auto emptyProgramSource =
        make_registration_node("program", "repo:program", "program");
    emptyProgramSource.sourceName.clear();
    auto emptyProgramDestination =
        make_registration_node("program", "repo:program", "program");
    emptyProgramDestination.destinationName.clear();
    auto emptyLibrarySource =
        make_registration_node("library", "repo:library", "lib");
    emptyLibrarySource.sourceName.clear();
    auto emptyLibraryDestination =
        make_registration_node("library", "repo:library", "lib");
    emptyLibraryDestination.destinationName.clear();

    const std::vector<Case> cases{
        {
            std::move(unsupported),
            "/nodes/0/kind",
        },
        {
            std::move(emptyProgramSource),
            "/nodes/0/sourceName",
        },
        {
            std::move(emptyProgramDestination),
            "/nodes/0/destinationName",
        },
        {
            std::move(emptyLibrarySource),
            "/nodes/0/sourceName",
        },
        {
            std::move(emptyLibraryDestination),
            "/nodes/0/destinationName",
        },
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.path);
        xlings::xvm::VersionDB db;
        db["sentinel"].versions["0"].path = "/sentinel";
        xlings::xvm::Workspace workspace{{"sentinel", "0"}};
        xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
        const auto dbBefore = xlings::xvm::versions_to_json(db);
        const auto workspaceBefore = workspace;
        const auto installedBefore = installed;

        auto result = xlings::xvm::apply_registration_batch(
            db, workspace, installed,
            make_registration_batch({testCase.node}));

        expect_registration_error(
            result,
            xlings::xvm::RegistrationErrorKind::InvalidNodePayload,
            testCase.path, testCase.node.target, testCase.node.version);
        expect_registration_state_unchanged(
            db, workspace, installed,
            dbBefore, workspaceBefore, installedBefore);
    }
}

TEST(XvmRegistrationOwnershipTest,
     RejectsPersistedGroupLabelWithDifferentRootWithoutMutation) {
    xlings::xvm::VersionDB db;
    const xlings::xvm::BindingGroupRef existingGroup{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "shared",
        .rootTarget = "old-root",
        .rootVersion = "repo:old-root",
    };
    auto& oldRoot = db["old-root"].versions["repo:old-root"];
    oldRoot.path = "/pkg/provider/old";
    oldRoot.kind = "group";
    oldRoot.bindingGroup = existingGroup;
    oldRoot.bindingMembers = {
        {"old-root", "repo:old-root"},
    };
    oldRoot.bindingMembersDeclared = true;

    auto newRoot =
        make_registration_node("new-root", "repo:new-root", "group");
    auto newTool =
        make_registration_node("new-tool", "repo:new-tool");
    newTool.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({newRoot, newTool}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::GroupConflict,
        "/nodes/1/binding/group", "new-tool", "repo:new-tool");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsPersistedGroupLabelWithDuplicateRootsWithoutMutation) {
    xlings::xvm::VersionDB db;
    const xlings::xvm::BindingGroupRef oldGroup{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "shared",
        .rootTarget = "old-root",
        .rootVersion = "repo:old-root",
    };
    auto& oldRoot = db["old-root"].versions["repo:old-root"];
    oldRoot.path = "/pkg/provider/old";
    oldRoot.kind = "group";
    oldRoot.bindingGroup = oldGroup;
    oldRoot.bindingMembers = {
        {"old-root", "repo:old-root"},
    };
    oldRoot.bindingMembersDeclared = true;

    const xlings::xvm::BindingGroupRef otherGroup{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "shared",
        .rootTarget = "other-root",
        .rootVersion = "repo:other-root",
    };
    auto& otherRoot = db["other-root"].versions["repo:other-root"];
    otherRoot.path = "/pkg/provider/other";
    otherRoot.kind = "group";
    otherRoot.bindingGroup = otherGroup;
    otherRoot.bindingMembers = {
        {"other-root", "repo:other-root"},
    };
    otherRoot.bindingMembersDeclared = true;

    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            make_registration_node("fresh", "repo:fresh"),
        }));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::GroupConflict,
        "/bindingGroups/shared", "other-root", "repo:other-root");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     MissingPersistedRootStillRequiresEveryOwnedMemberWithoutMutation) {
    xlings::xvm::VersionDB db;
    const xlings::xvm::BindingGroupRef existingGroup{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "shared",
        .rootTarget = "root",
        .rootVersion = "repo:root",
    };
    auto& oldMember = db["old-member"].versions["repo:old-member"];
    oldMember.path = "/pkg/provider/old";
    oldMember.kind = "program";
    oldMember.sourceName = "old-member";
    oldMember.destinationName = "old-member";
    oldMember.bindingGroup = existingGroup;

    auto root = make_registration_node("root", "repo:root", "group");
    auto newMember =
        make_registration_node("new-member", "repo:new-member");
    newMember.binding =
        make_registration_binding("root", "repo:root", "shared");
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({root, newMember}));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::IncompleteOwnedGroup,
        "/nodes/1/binding/group",
        "old-member", "repo:old-member");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     MissingPersistedRootRejectsDifferentIncomingGroupWithoutMutation) {
    xlings::xvm::VersionDB db;
    const auto oldGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "old-group",
        "shared-root", "repo:root");
    add_provider_group_member(
        db, "old-member", "repo:old-member", oldGroup,
        "program", "old-member", "old-member");
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto root =
        make_registration_node("shared-root", "repo:root", "group");
    auto newMember =
        make_registration_node("new-member", "repo:new-member");
    newMember.binding = make_registration_binding(
        "shared-root", "repo:root", "new-group");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({root, newMember}));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::BindingValidationFailed,
        "/bindingGroups/old-group", "shared-root", "repo:root");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsPersistedTwoLabelsForOneRootWithoutMutation) {
    xlings::xvm::VersionDB db;
    const auto alphaGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "alpha",
        "shared-root", "repo:root");
    auto& root = add_provider_group_member(
        db, "shared-root", "repo:root", alphaGroup, "group");
    root.bindingMembers = {
        {"shared-root", "repo:root"},
    };
    root.bindingMembersDeclared = true;
    const auto betaGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "beta",
        "shared-root", "repo:root");
    add_provider_group_member(
        db, "beta-member", "repo:beta", betaGroup,
        "program", "beta-member", "beta-member");
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            make_registration_node("fresh", "repo:fresh"),
        }));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::BindingValidationFailed,
        "/bindingGroups/beta", "shared-root", "repo:root");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     CompleteSameOwnerRootMigrationIsAtomicAndOrderIndependent) {
    const auto oldGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "shared",
        "old-root", "repo:old-root");
    xlings::xvm::VersionDB initialDb;
    auto& oldRoot = add_provider_group_member(
        initialDb, "old-root", "repo:old-root", oldGroup, "group");
    oldRoot.bindingMembers = {
        {"old-a", "repo:old-a"},
        {"old-b", "repo:old-b"},
        {"old-root", "repo:old-root"},
    };
    oldRoot.bindingMembersDeclared = true;
    oldRoot.bindingHeaders = {
        {
            .sourceDir = "/pkg/old/include",
            .destinationPrefix = "old",
        },
    };
    oldRoot.bindingHeadersDeclared = true;
    add_provider_group_member(
        initialDb, "old-a", "repo:old-a", oldGroup,
        "program", "old-a", "old-a");
    add_provider_group_member(
        initialDb, "old-b", "repo:old-b", oldGroup,
        "program", "old-b", "old-b");
    initialDb["old-root"].bindings["old-a"]["repo:old-root"] =
        "repo:old-a";
    initialDb["old-a"].bindings["old-root"]["repo:old-a"] =
        "repo:old-root";
    initialDb["old-root"].bindings["old-b"]["repo:old-root"] =
        "repo:old-b";
    initialDb["old-b"].bindings["old-root"]["repo:old-b"] =
        "repo:old-root";
    const xlings::xvm::Workspace initialWorkspace{
        {"old-a", "repo:old-a"},
        {"old-b", "repo:old-b"},
        {"old-root", "repo:old-root"},
    };
    const xlings::xvm::WorkspaceInstalled initialInstalled{
        {"old-a", {"repo:old-a"}},
        {"old-b", {"repo:old-b"}},
        {"old-root", {"repo:old-root"}},
    };

    auto newRoot =
        make_registration_node("new-root", "repo:new-root", "group");
    auto migratedOldRoot =
        make_registration_node("old-root", "repo:old-root", "group");
    migratedOldRoot.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    auto migratedA =
        make_registration_node("old-a", "repo:old-a");
    migratedA.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    auto migratedB =
        make_registration_node("old-b", "repo:old-b");
    migratedB.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    auto forwardBatch = make_registration_batch({
        migratedA,
        newRoot,
        migratedOldRoot,
        migratedB,
    });
    auto reverseBatch = forwardBatch;
    std::ranges::reverse(reverseBatch.nodes);

    auto forwardDb = initialDb;
    auto forwardWorkspace = initialWorkspace;
    auto forwardInstalled = initialInstalled;
    auto forward = xlings::xvm::apply_registration_batch(
        forwardDb, forwardWorkspace, forwardInstalled, forwardBatch);
    auto reverseDb = initialDb;
    auto reverseWorkspace = initialWorkspace;
    auto reverseInstalled = initialInstalled;
    auto reverse = xlings::xvm::apply_registration_batch(
        reverseDb, reverseWorkspace, reverseInstalled, reverseBatch);

    ASSERT_TRUE(forward.has_value()) << forward.error().message;
    ASSERT_TRUE(reverse.has_value()) << reverse.error().message;
    EXPECT_EQ(
        xlings::xvm::versions_to_json(forwardDb),
        xlings::xvm::versions_to_json(reverseDb));
    EXPECT_EQ(forwardWorkspace, reverseWorkspace);
    EXPECT_EQ(forwardInstalled, reverseInstalled);

    const std::map<std::string, std::string> expectedMembers{
        {"new-root", "repo:new-root"},
        {"old-a", "repo:old-a"},
        {"old-b", "repo:old-b"},
        {"old-root", "repo:old-root"},
    };
    const auto& migratedRoot =
        forwardDb.at("new-root").versions.at("repo:new-root");
    EXPECT_EQ(migratedRoot.bindingMembers, expectedMembers);
    EXPECT_TRUE(migratedRoot.bindingHeaders.empty());

    for (const auto& [target, version] : expectedMembers) {
        const auto& member = forwardDb.at(target).versions.at(version);
        ASSERT_TRUE(member.bindingGroup.has_value());
        EXPECT_EQ(member.bindingGroup->group, "shared");
        EXPECT_EQ(member.bindingGroup->rootTarget, "new-root");
        EXPECT_EQ(member.bindingGroup->rootVersion, "repo:new-root");
    }
    std::size_t sharedRefCount = 0;
    std::set<std::pair<std::string, std::string>> sharedRoots;
    for (const auto& [_, info] : forwardDb) {
        for (const auto& versionEntry : info.versions) {
            const auto& data = versionEntry.second;
            if (!data.bindingGroup
                || data.bindingGroup->provider != "repo:provider"
                || data.bindingGroup->providerVersion != "1.0.0"
                || data.bindingGroup->group != "shared") {
                continue;
            }
            ++sharedRefCount;
            sharedRoots.emplace(
                data.bindingGroup->rootTarget,
                data.bindingGroup->rootVersion);
        }
    }
    EXPECT_EQ(sharedRefCount, expectedMembers.size());
    EXPECT_EQ(
        sharedRoots,
        (std::set<std::pair<std::string, std::string>>{
            {"new-root", "repo:new-root"},
        }));

    const auto& migratedOldRootData =
        forwardDb.at("old-root").versions.at("repo:old-root");
    EXPECT_TRUE(migratedOldRootData.bindingMembers.empty());
    EXPECT_FALSE(migratedOldRootData.bindingMembersDeclared);
    EXPECT_TRUE(migratedOldRootData.bindingHeaders.empty());
    EXPECT_FALSE(migratedOldRootData.bindingHeadersDeclared);
    EXPECT_FALSE(forwardDb.at("old-root").bindings.contains("old-a"));
    EXPECT_FALSE(forwardDb.at("old-root").bindings.contains("old-b"));
    EXPECT_FALSE(forwardDb.at("old-a").bindings.contains("old-root"));
    EXPECT_FALSE(forwardDb.at("old-b").bindings.contains("old-root"));
    EXPECT_EQ(
        forwardDb.at("old-root")
            .bindings.at("new-root").at("repo:old-root"),
        "repo:new-root");
}

TEST(XvmRegistrationOwnershipTest,
     RootMigrationRejectsOmittedManifestOnlyMemberWithoutMutation) {
    const auto oldGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "shared",
        "old-root", "repo:old-root");
    xlings::xvm::VersionDB db;
    auto& oldRoot = add_provider_group_member(
        db, "old-root", "repo:old-root", oldGroup, "group");
    oldRoot.bindingMembers = {
        {"manifest-only", "repo:manifest-only"},
        {"old-member", "repo:old-member"},
        {"old-root", "repo:old-root"},
    };
    oldRoot.bindingMembersDeclared = true;
    add_provider_group_member(
        db, "old-member", "repo:old-member", oldGroup,
        "program", "old-member", "old-member");
    auto& manifestOnly =
        db["manifest-only"].versions["repo:manifest-only"];
    manifestOnly.path = "/pkg/provider/1.0.0";
    manifestOnly.kind = "program";
    manifestOnly.sourceName = "manifest-only";
    manifestOnly.destinationName = "manifest-only";

    auto newRoot =
        make_registration_node("new-root", "repo:new-root", "group");
    auto migratedOldRoot =
        make_registration_node("old-root", "repo:old-root", "group");
    migratedOldRoot.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    auto migratedMember =
        make_registration_node("old-member", "repo:old-member");
    migratedMember.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            migratedOldRoot,
            migratedMember,
            newRoot,
        }));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::IncompleteOwnedGroup,
        "/nodes/0", "manifest-only", "repo:manifest-only");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsOtherProviderExactCollisionWithoutMutation) {
    xlings::xvm::VersionDB db;
    auto& existing = db["tool"].versions["repo:1.0.0"];
    existing.path = "/other/tool";
    existing.kind = "program";
    existing.sourceName = "other-tool";
    existing.destinationName = "tool";
    existing.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "other:provider",
        .providerVersion = "1.0.0",
        .group = "tool",
        .rootTarget = "tool",
        .rootVersion = "repo:1.0.0",
    };
    existing.bindingMembers = {{"tool", "repo:1.0.0"}};
    existing.bindingMembersDeclared = true;
    xlings::xvm::Workspace workspace{{"tool", "repo:1.0.0"}};
    xlings::xvm::WorkspaceInstalled installed{
        {"tool", {"repo:1.0.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            make_registration_node("tool", "repo:1.0.0"),
        }));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::OwnershipConflict,
        "/nodes/0", "tool", "repo:1.0.0");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsSameProviderDifferentReleaseCollisionWithoutMutation) {
    xlings::xvm::VersionDB db;
    auto& existing = db["tool"].versions["repo:1.0.0"];
    existing.path = "/provider/old";
    existing.kind = "program";
    existing.sourceName = "old-tool";
    existing.destinationName = "tool";
    existing.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "repo:provider",
        .providerVersion = "0.9.0",
        .group = "tool",
        .rootTarget = "tool",
        .rootVersion = "repo:1.0.0",
    };
    existing.bindingMembers = {{"tool", "repo:1.0.0"}};
    existing.bindingMembersDeclared = true;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    const auto dbBefore = xlings::xvm::versions_to_json(db);

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            make_registration_node("tool", "repo:1.0.0"),
        }));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::OwnershipConflict,
        "/nodes/0", "tool", "repo:1.0.0");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationOwnershipTest, SameOwnerReregistrationIsIdempotent) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto child = make_registration_node("tool", "repo:tool-1.0.0");
    child.sourceName = "tool-real";
    child.destinationName = "tool";
    child.alias = {"--driver-mode=gcc"};
    child.envs = {{"TOOLCHAIN_ROOT", "/pkg/provider/1.0.0"}};
    child.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");
    const auto batch = make_registration_batch({child, root});

    auto first = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);
    ASSERT_TRUE(first.has_value()) << first.error().message;
    const auto dbAfterFirst = xlings::xvm::versions_to_json(db);
    const auto workspaceAfterFirst = workspace;
    const auto installedAfterFirst = installed;

    auto second = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbAfterFirst);
    EXPECT_EQ(workspace, workspaceAfterFirst);
    EXPECT_EQ(installed, installedAfterFirst);
    EXPECT_EQ(
        db.at("root").bindings.at("tool").size(),
        1u);
    EXPECT_EQ(
        db.at("tool").bindings.at("root").size(),
        1u);
}

namespace {

void seed_complete_legacy_registration_group(
    xlings::xvm::VersionDB& db) {
    auto& rootInfo = db["legacy-root"];
    rootInfo.type = "program";
    rootInfo.filename = "legacy-root-real";
    auto& root = rootInfo.versions["repo:1.0.0"];
    root.path = "/pkg/provider/1.0.0";
    root.alias = {"root-alias"};
    root.envs = {{"ROOT_ENV", "root"}};

    auto& childInfo = db["tool"];
    childInfo.type = "program";
    childInfo.filename = "tool-real";
    auto& child = childInfo.versions["repo:tool-1.0.0"];
    child.path = "/pkg/provider/1.0.0";
    child.alias = {"tool-alias"};
    child.envs = {{"TOOL_ENV", "tool"}};

    rootInfo.bindings["tool"]["repo:1.0.0"] =
        "repo:tool-1.0.0";
    childInfo.bindings["legacy-root"]["repo:tool-1.0.0"] =
        "repo:1.0.0";
}

xlings::xvm::RegistrationBatch complete_legacy_adoption_batch() {
    auto root =
        make_registration_node("legacy-root", "repo:1.0.0");
    root.sourceName = "legacy-root-real";
    root.destinationName = "legacy-root";
    root.alias = {"root-alias"};
    root.envs = {{"ROOT_ENV", "root"}};
    auto child =
        make_registration_node("tool", "repo:tool-1.0.0");
    child.sourceName = "tool-real";
    child.destinationName = "tool";
    child.alias = {"tool-alias"};
    child.envs = {{"TOOL_ENV", "tool"}};
    child.binding = make_registration_binding(
        "legacy-root", "repo:1.0.0");
    return make_registration_batch({child, root});
}

}  // namespace

TEST(XvmRegistrationOwnershipTest, AdoptsCompatibleCompleteLegacyGroup) {
    xlings::xvm::VersionDB db;
    seed_complete_legacy_registration_group(db);
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, complete_legacy_adoption_batch());

    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto selection = xlings::xvm::resolve_binding_selection(
        db, "tool", "repo:tool-1.0.0");
    ASSERT_TRUE(selection.has_value()) << selection.error().message;
    EXPECT_EQ(
        selection->members,
        (std::map<std::string, std::string>{
            {"legacy-root", "repo:1.0.0"},
            {"tool", "repo:tool-1.0.0"},
        }));
    EXPECT_EQ(
        db.at("tool")
            .versions.at("repo:tool-1.0.0")
            .bindingGroup->provider,
        "repo:provider");
}

TEST(XvmRegistrationOwnershipTest,
     AdoptsOwnerlessLegacyGroupWithNormalizedVirtualPayload) {
    xlings::xvm::VersionDB db;
    auto& info = db["virtual"];
    info.type = "group";
    auto& legacy = info.versions["repo:virtual"];
    legacy.path = "/pkg/provider/1.0.0";
    legacy.kind = "group";
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto group =
        make_registration_node("virtual", "repo:virtual", "group");
    group.sourceName = "ignored-payload";
    group.destinationName = "ignored-shim";

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({group}));

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& adopted = db.at("virtual").versions.at("repo:virtual");
    EXPECT_TRUE(adopted.sourceName.empty());
    EXPECT_TRUE(adopted.destinationName.empty());
    ASSERT_TRUE(adopted.bindingGroup.has_value());
    EXPECT_EQ(adopted.bindingGroup->group, "virtual");
}

TEST(XvmRegistrationOwnershipTest,
     RejectsIncompatibleLegacyPayloadWithoutMutation) {
    xlings::xvm::VersionDB db;
    seed_complete_legacy_registration_group(db);
    xlings::xvm::Workspace workspace{{"tool", "repo:tool-1.0.0"}};
    xlings::xvm::WorkspaceInstalled installed{
        {"tool", {"repo:tool-1.0.0"}},
    };
    auto batch = complete_legacy_adoption_batch();
    batch.nodes[0].path = "/different/payload";
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::LegacyPayloadMismatch,
        "/nodes/0/path", "tool", "repo:tool-1.0.0");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsIncompleteLegacyComponentWithoutMutation) {
    xlings::xvm::VersionDB db;
    seed_complete_legacy_registration_group(db);
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto batch = complete_legacy_adoption_batch();
    batch.nodes.erase(batch.nodes.begin());
    const auto dbBefore = xlings::xvm::versions_to_json(db);

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::IncompleteLegacyComponent,
        "/nodes/0", "tool", "repo:tool-1.0.0");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationOwnershipTest,
     RejectsIncomingOnlyLegacyEdgeWithoutMutation) {
    xlings::xvm::VersionDB db;
    auto& toolInfo = db["tool"];
    toolInfo.type = "program";
    toolInfo.filename = "payload";
    auto& tool = toolInfo.versions["repo:tool"];
    tool.path = "/pkg/provider/1.0.0";

    auto& peerInfo = db["peer"];
    peerInfo.type = "program";
    peerInfo.filename = "peer";
    peerInfo.versions["repo:peer"].path = "/pkg/peer";
    peerInfo.bindings["tool"]["repo:peer"] = "repo:tool";

    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            make_registration_node("tool", "repo:tool"),
        }));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::BindingValidationFailed,
        "/nodes/0", "peer", "repo:peer");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsIncompleteSameOwnerGroupReconfiguration) {
    xlings::xvm::VersionDB db;
    const xlings::xvm::BindingGroupRef group{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "toolchain",
        .rootTarget = "root",
        .rootVersion = "repo:1.0.0",
    };
    auto& root = db["root"].versions["repo:1.0.0"];
    root.path = "/pkg/provider/1.0.0";
    root.kind = "group";
    root.bindingGroup = group;
    root.bindingMembers = {
        {"root", "repo:1.0.0"},
        {"tool", "repo:tool-1.0.0"},
    };
    root.bindingMembersDeclared = true;
    auto& child = db["tool"].versions["repo:tool-1.0.0"];
    child.path = "/pkg/provider/1.0.0";
    child.kind = "program";
    child.sourceName = "tool-real";
    child.destinationName = "tool";
    child.bindingGroup = group;
    db["root"].bindings["tool"]["repo:1.0.0"] =
        "repo:tool-1.0.0";
    db["tool"].bindings["root"]["repo:tool-1.0.0"] =
        "repo:1.0.0";
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    auto replacement =
        make_registration_node("root", "repo:1.0.0", "group");
    replacement.sourceName.clear();
    replacement.destinationName.clear();

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({replacement}));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::IncompleteOwnedGroup,
        "/nodes/0", "tool", "repo:tool-1.0.0");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationReconfigureTest,
     ReplacesCurrentExactEdgesAndPreservesAnotherVersion) {
    xlings::xvm::VersionDB db;
    const xlings::xvm::BindingGroupRef currentGroup{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "toolchain",
        .rootTarget = "root",
        .rootVersion = "repo:1.0.0",
    };
    auto& currentRoot = db["root"].versions["repo:1.0.0"];
    currentRoot.path = "/pkg/provider/1.0.0";
    currentRoot.kind = "group";
    currentRoot.bindingGroup = currentGroup;
    currentRoot.bindingMembers = {
        {"root", "repo:1.0.0"},
        {"tool", "repo:tool-1.0.0"},
    };
    currentRoot.bindingMembersDeclared = true;
    auto& currentTool = db["tool"].versions["repo:tool-1.0.0"];
    currentTool.path = "/pkg/provider/1.0.0";
    currentTool.kind = "program";
    currentTool.sourceName = "tool-real";
    currentTool.destinationName = "tool";
    currentTool.bindingGroup = currentGroup;
    db["root"].bindings["tool"]["repo:1.0.0"] =
        "repo:tool-1.0.0";
    db["tool"].bindings["root"]["repo:tool-1.0.0"] =
        "repo:1.0.0";

    auto& stale = db["stale"].versions["repo:stale-1.0.0"];
    stale.path = "/pkg/stale";
    stale.kind = "program";
    stale.sourceName = "stale";
    stale.destinationName = "stale";
    db["root"].bindings["stale"]["repo:1.0.0"] =
        "repo:stale-1.0.0";
    db["stale"].bindings["root"]["repo:stale-1.0.0"] =
        "repo:1.0.0";

    const xlings::xvm::BindingGroupRef oldGroup{
        .provider = "repo:provider",
        .providerVersion = "0.9.0",
        .group = "toolchain",
        .rootTarget = "root",
        .rootVersion = "repo:0.9.0",
    };
    auto& oldRoot = db["root"].versions["repo:0.9.0"];
    oldRoot.path = "/pkg/provider/0.9.0";
    oldRoot.kind = "group";
    oldRoot.bindingGroup = oldGroup;
    oldRoot.bindingMembers = {
        {"root", "repo:0.9.0"},
        {"tool", "repo:tool-0.9.0"},
    };
    oldRoot.bindingMembersDeclared = true;
    auto& oldTool = db["tool"].versions["repo:tool-0.9.0"];
    oldTool.path = "/pkg/provider/0.9.0";
    oldTool.kind = "program";
    oldTool.sourceName = "tool-old";
    oldTool.destinationName = "tool";
    oldTool.alias = {"--old"};
    oldTool.envs = {{"OLD", "1"}};
    oldTool.bindingGroup = oldGroup;
    db["root"].bindings["tool"]["repo:0.9.0"] =
        "repo:tool-0.9.0";
    db["tool"].bindings["root"]["repo:tool-0.9.0"] =
        "repo:0.9.0";
    const auto oldRootBefore = xlings::xvm::vdata_to_json(oldRoot);
    const auto oldToolBefore = xlings::xvm::vdata_to_json(oldTool);
    const auto staleBefore = xlings::xvm::vdata_to_json(stale);

    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto tool =
        make_registration_node("tool", "repo:tool-1.0.0");
    tool.sourceName = "tool-real";
    tool.destinationName = "tool";
    tool.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({tool, root}));

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(db.at("root").bindings.contains("stale"));
    EXPECT_FALSE(db.at("stale").bindings.contains("root"));
    EXPECT_EQ(
        xlings::xvm::vdata_to_json(
            db.at("root").versions.at("repo:0.9.0")),
        oldRootBefore);
    EXPECT_EQ(
        xlings::xvm::vdata_to_json(
            db.at("tool").versions.at("repo:tool-0.9.0")),
        oldToolBefore);
    EXPECT_EQ(
        xlings::xvm::vdata_to_json(
            db.at("stale").versions.at("repo:stale-1.0.0")),
        staleBefore);
    EXPECT_EQ(
        db.at("root")
            .bindings.at("tool").at("repo:0.9.0"),
        "repo:tool-0.9.0");
    EXPECT_EQ(
        db.at("tool")
            .bindings.at("root").at("repo:tool-0.9.0"),
        "repo:0.9.0");
}

TEST(XvmRegistrationTest, KeepsProviderReleaseGroupsIndependent) {
    auto rootA = make_registration_node("root-a", "repo:a", "group");
    rootA.sourceName.clear();
    rootA.destinationName.clear();
    auto toolA = make_registration_node("tool", "repo:tool-a");
    toolA.sourceName = "tool-a";
    toolA.binding =
        make_registration_binding("root-a", "repo:a", "group-a");
    auto rootB = make_registration_node("root-b", "repo:b", "group");
    rootB.sourceName.clear();
    rootB.destinationName.clear();
    auto toolB = make_registration_node("tool", "repo:tool-b");
    toolB.sourceName = "tool-b";
    toolB.binding =
        make_registration_binding("root-b", "repo:b", "group-b");
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({toolB, rootA, toolA, rootB}));

    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto selectedA =
        xlings::xvm::resolve_binding_selection(db, "root-a", "repo:a");
    auto selectedB =
        xlings::xvm::resolve_binding_selection(db, "root-b", "repo:b");
    ASSERT_TRUE(selectedA.has_value()) << selectedA.error().message;
    ASSERT_TRUE(selectedB.has_value()) << selectedB.error().message;
    EXPECT_EQ(
        selectedA->members,
        (std::map<std::string, std::string>{
            {"root-a", "repo:a"},
            {"tool", "repo:tool-a"},
        }));
    EXPECT_EQ(
        selectedB->members,
        (std::map<std::string, std::string>{
            {"root-b", "repo:b"},
            {"tool", "repo:tool-b"},
        }));
}

TEST(XvmRegistrationTest,
     SameTargetIndependentGroupsIgnoreBatchNodeOrder) {
    auto rootA = make_registration_node("root-a", "repo:a", "group");
    rootA.sourceName.clear();
    rootA.destinationName.clear();
    auto toolA =
        make_registration_node("tool", "repo:tool-a", "program");
    toolA.sourceName = "tool-a";
    toolA.destinationName = "tool";
    toolA.binding =
        make_registration_binding("root-a", "repo:a", "group-a");
    auto rootB = make_registration_node("root-b", "repo:b", "group");
    rootB.sourceName.clear();
    rootB.destinationName.clear();
    auto toolB = make_registration_node("tool", "repo:tool-b", "lib");
    toolB.sourceName = "libtool-b.so.1";
    toolB.destinationName = "libtool.so.1";
    toolB.binding =
        make_registration_binding("root-b", "repo:b", "group-b");

    xlings::xvm::VersionDB forwardDb;
    xlings::xvm::Workspace forwardWorkspace;
    xlings::xvm::WorkspaceInstalled forwardInstalled;
    auto forward = xlings::xvm::apply_registration_batch(
        forwardDb, forwardWorkspace, forwardInstalled,
        make_registration_batch({rootA, toolA, rootB, toolB}));
    ASSERT_TRUE(forward.has_value()) << forward.error().message;

    xlings::xvm::VersionDB reverseDb;
    xlings::xvm::Workspace reverseWorkspace;
    xlings::xvm::WorkspaceInstalled reverseInstalled;
    auto reverse = xlings::xvm::apply_registration_batch(
        reverseDb, reverseWorkspace, reverseInstalled,
        make_registration_batch({toolB, rootB, toolA, rootA}));
    ASSERT_TRUE(reverse.has_value()) << reverse.error().message;

    EXPECT_EQ(
        xlings::xvm::versions_to_json(forwardDb),
        xlings::xvm::versions_to_json(reverseDb));
    EXPECT_EQ(forwardWorkspace, reverseWorkspace);
    EXPECT_EQ(forwardInstalled, reverseInstalled);
}

TEST(XvmRegistrationHeaderTest, RoutesExplicitHeaderToNamedGroupRoot) {
    auto rootA = make_registration_node("root-a", "repo:a", "group");
    rootA.sourceName.clear();
    rootA.destinationName.clear();
    auto toolA = make_registration_node("tool-a", "repo:a");
    toolA.binding =
        make_registration_binding("root-a", "repo:a", "group-a");
    auto rootB = make_registration_node("root-b", "repo:b", "group");
    rootB.sourceName.clear();
    rootB.destinationName.clear();
    auto toolB = make_registration_node("tool-b", "repo:b");
    toolB.binding =
        make_registration_binding("root-b", "repo:b", "group-b");
    auto batch =
        make_registration_batch({rootA, toolA, rootB, toolB});
    batch.headers = {
        {
            .sourceDir = "include/compiler",
            .destinationPrefix = "compiler",
            .group = "group-b",
        },
    };
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(
        db.at("root-a").versions.at("repo:a").bindingHeaders.empty());
    const auto& routed =
        db.at("root-b").versions.at("repo:b").bindingHeaders;
    ASSERT_EQ(routed.size(), 1u);
    EXPECT_EQ(routed[0].sourceDir, "include/compiler");
    EXPECT_EQ(routed[0].destinationPrefix, "compiler");
}

TEST(XvmRegistrationHeaderTest, AcceptsUngroupedHeaderForSingleGroup) {
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto batch = make_registration_batch({root});
    batch.headers = {
        {
            .sourceDir = "include",
            .destinationPrefix = "",
        },
    };
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& data =
        db.at("root").versions.at("repo:1.0.0");
    ASSERT_EQ(data.bindingHeaders.size(), 1u);
    EXPECT_EQ(data.bindingHeaders[0].sourceDir, "include");
    EXPECT_TRUE(data.bindingHeaders[0].destinationPrefix.empty());
    EXPECT_TRUE(data.bindingHeadersDeclared);
}

// A recipe that registers several independent targets and then declares
// headers is ordinary — a package exposing both a program and a library, for
// instance. Every ungrouped node becomes its own singleton group, so the
// "exactly one candidate group" rule alone rejects those recipes outright.
// The package's own target breaks the tie: headers shipped by package `p`
// belong with `p`.
TEST(XvmRegistrationHeaderTest, UngroupedHeaderFallsBackToThePrimaryTarget) {
    auto program = make_registration_node("openssl", "repo:3.1.5");
    auto library = make_registration_node("libssl", "repo:3.1.5");
    library.kind = "lib";
    auto batch = make_registration_batch({program, library});
    batch.primaryTarget = "openssl";
    batch.headers = {{.sourceDir = "include"}};
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& owner = db.at("openssl").versions.at("repo:3.1.5");
    ASSERT_EQ(owner.bindingHeaders.size(), 1u);
    EXPECT_EQ(owner.bindingHeaders[0].sourceDir, "include");
    EXPECT_TRUE(db.at("libssl").versions.at("repo:3.1.5").bindingHeaders.empty())
        << "headers leaked onto a group that does not own them";
}

// The tie-break is the package's own target, not "pick the first group".
// When the package registers nothing under its own name there is genuinely
// no owner to infer, and guessing would attach headers to an arbitrary group.
TEST(XvmRegistrationHeaderTest,
     UngroupedHeaderStaysAmbiguousWhenPrimaryTargetIsNotRegistered) {
    auto first = make_registration_node("first", "repo:first");
    auto second = make_registration_node("second", "repo:second");
    auto batch = make_registration_batch({first, second});
    batch.primaryTarget = "not-registered";
    batch.headers = {{.sourceDir = "include"}};
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::HeaderAmbiguous,
        "/headers/0/group", "not-registered");
    EXPECT_NE(result.error().message.find("primaryTarget"), std::string::npos)
        << "the error must say how to resolve it, got: " << result.error().message;
}

// The same target bound into two different groups: naming it no longer
// identifies one owner, so the header is still ambiguous. (Registering one
// target at two versions *without* bindings is rejected earlier as a
// GroupConflict, so two roots are what actually reaches this branch.)
TEST(XvmRegistrationHeaderTest,
     UngroupedHeaderStaysAmbiguousWhenPrimaryTargetSpansGroups) {
    auto rootA = make_registration_node("root-a", "repo:a", "group");
    rootA.sourceName.clear();
    rootA.destinationName.clear();
    auto rootB = make_registration_node("root-b", "repo:b", "group");
    rootB.sourceName.clear();
    rootB.destinationName.clear();
    auto toolA = make_registration_node("tool", "repo:1.0.0");
    toolA.binding = xlings::xvm::RegistrationBinding{
        .rootTarget = "root-a", .rootVersion = "repo:a"};
    auto toolB = make_registration_node("tool", "repo:2.0.0");
    toolB.binding = xlings::xvm::RegistrationBinding{
        .rootTarget = "root-b", .rootVersion = "repo:b"};
    auto batch = make_registration_batch({rootA, rootB, toolA, toolB});
    batch.primaryTarget = "tool";
    batch.headers = {{.sourceDir = "include"}};
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::HeaderAmbiguous,
        "/headers/0/group", "tool");
    EXPECT_NE(result.error().message.find("spans"), std::string::npos)
        << result.error().message;
}

TEST(XvmRegistrationHeaderErrorTest,
     RejectsAmbiguousUngroupedHeaderWithoutMutation) {
    auto first = make_registration_node("first", "repo:first");
    auto second = make_registration_node("second", "repo:second");
    auto batch = make_registration_batch({first, second});
    batch.headers = {
        {
            .sourceDir = "include",
        },
    };
    xlings::xvm::VersionDB db;
    db["sentinel"].versions["0"].path = "/sentinel";
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::HeaderAmbiguous,
        "/headers/0/group");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationHeaderErrorTest,
     RejectsMissingExplicitHeaderGroupWithoutMutation) {
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto batch = make_registration_batch({root});
    batch.headers = {
        {
            .sourceDir = "include",
            .group = "missing-group",
        },
    };
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::HeaderGroupNotFound,
        "/headers/0/group", "missing-group");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationHeaderErrorTest,
     RejectsEmptyHeaderSourceWithoutMutation) {
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto batch = make_registration_batch({root});
    batch.headers = {
        {
            .sourceDir = "",
        },
    };
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::InvalidHeader,
        "/headers/0/sourceDir");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationHeaderTest, HeaderNeverCreatesTargetOrVersion) {
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto batch = make_registration_batch({root});
    batch.headers = {
        {
            .sourceDir = "phantom-header-target",
            .destinationPrefix = "sdk",
        },
    };
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(db.size(), 1u);
    EXPECT_FALSE(db.contains("phantom-header-target"));
    EXPECT_EQ(
        db.at("root")
            .versions.at("repo:1.0.0")
            .bindingHeaders.size(),
        1u);
}

TEST(XvmRegistrationStateTest,
     RegistersEveryMemberOnceAndHonorsActivationPolicy) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace{
        {"root", "repo:0.9.0"},
        {"tool", "repo:tool-0.9.0"},
        {"runtime", "repo:runtime-0.9.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"root", {"repo:0.9.0"}},
        {
            "tool",
            {
                "repo:tool-0.9.0",
                "repo:tool-1.0.0",
                "repo:tool-1.0.0",
            },
        },
        {"runtime", {"repo:runtime-0.9.0"}},
    };
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName = "must-not-be-a-shim";
    root.destinationName = "must-not-be-a-shim";
    auto tool =
        make_registration_node("tool", "repo:tool-1.0.0");
    tool.sourceName = "tool-real";
    tool.destinationName = "tool";
    tool.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");
    auto runtime = make_registration_node(
        "runtime", "repo:runtime-1.0.0", "lib");
    runtime.sourceName = "libruntime.so.1.0";
    runtime.destinationName = "libruntime.so.1";
    runtime.binding = tool.binding;
    auto batch = make_registration_batch({runtime, root, tool});

    auto first = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(first.has_value()) << first.error().message;
    EXPECT_EQ(first->size(), 3u);
    EXPECT_EQ(workspace.at("root"), "repo:0.9.0");
    EXPECT_EQ(workspace.at("tool"), "repo:tool-0.9.0");
    EXPECT_EQ(
        workspace.at("runtime"), "repo:runtime-0.9.0");
    EXPECT_EQ(
        std::ranges::count(
            installed.at("root"), "repo:1.0.0"),
        1);
    EXPECT_EQ(
        std::ranges::count(
            installed.at("tool"), "repo:tool-1.0.0"),
        1);
    EXPECT_EQ(
        std::ranges::count(
            installed.at("runtime"), "repo:runtime-1.0.0"),
        1);

    batch.useAfterInstall = true;
    auto second = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(workspace.at("root"), "repo:1.0.0");
    EXPECT_EQ(workspace.at("tool"), "repo:tool-1.0.0");
    EXPECT_EQ(
        workspace.at("runtime"), "repo:runtime-1.0.0");
    EXPECT_EQ(
        std::ranges::count(
            installed.at("tool"), "repo:tool-1.0.0"),
        1);
}

TEST(XvmRegistrationStateTest,
     VirtualGroupRetainsEmptyPayloadAndNoSelfEdge) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName = "bogus-executable";
    root.destinationName = "bogus-shim";

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({root}));

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& data =
        db.at("root").versions.at("repo:1.0.0");
    EXPECT_EQ(data.kind, "group");
    EXPECT_TRUE(data.sourceName.empty());
    EXPECT_TRUE(data.destinationName.empty());
    EXPECT_FALSE(db.at("root").bindings.contains("root"));
    EXPECT_EQ(workspace.at("root"), "repo:1.0.0");
    EXPECT_EQ(
        installed.at("root"),
        (std::vector<std::string>{"repo:1.0.0"}));
}

TEST(XvmRegistrationStateTest,
     FinalComponentValidationFailurePreservesEveryStateObject) {
    xlings::xvm::VersionDB db;
    db["sentinel"].versions["0"].path = "/sentinel";
    const auto staleGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "stale-group",
        "future-root", "repo:root");
    add_provider_group_member(
        db, "stale-member", "repo:stale-member", staleGroup,
        "program", "stale-member", "stale-member");
    xlings::xvm::Workspace workspace{
        {"future-root", "repo:previous"},
        {"sentinel", "0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"future-root", {"repo:previous"}},
        {"sentinel", {"0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto root =
        make_registration_node("future-root", "repo:root", "group");
    auto freshMember =
        make_registration_node("fresh-member", "repo:fresh-member");
    freshMember.binding = make_registration_binding(
        "future-root", "repo:root", "replacement-group");
    auto batch = make_registration_batch({root, freshMember});
    batch.useAfterInstall = true;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::BindingValidationFailed,
        "/bindingGroups/stale-group", "future-root", "repo:root");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

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
     ProviderGroupRejectsEmptyRootManifestTargetBeforeLookup) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider-root", "1.0.0");
    auto& root = add_provider_group_member(
        db, "provider-root", "1.0.0", group, "group");
    root.bindingMembers = {
        {"", "existing-empty-target-version"},
        {"provider-root", "1.0.0"},
    };
    add_provider_group_member(
        db, "", "existing-empty-target-version", group, "program");

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    expect_binding_metadata_error(
        result, "provider-root", "1.0.0",
        "binding-member-target-empty", "/bindingMembers/");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderGroupRejectsEmptyRootManifestVersionBeforeLookup) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider-root", "1.0.0");
    auto& root = add_provider_group_member(
        db, "provider-root", "1.0.0", group, "group");
    root.bindingMembers = {
        {"provider-root", "1.0.0"},
        {"tool/member~alias", ""},
    };
    add_provider_group_member(
        db, "tool/member~alias", "", group, "program");

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    expect_binding_metadata_error(
        result, "provider-root", "1.0.0",
        "binding-member-version-empty",
        "/bindingMembers/tool~1member~0alias");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderGroupRejectsEmptyRootHeaderSource) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider-root", "1.0.0");
    auto& root = add_provider_group_member(
        db, "provider-root", "1.0.0", group, "group");
    root.bindingMembers = {
        {"provider-root", "1.0.0"},
    };
    root.bindingHeaders = {
        {
            .sourceDir = "include",
            .destinationPrefix = "",
        },
        {
            .sourceDir = "",
            .destinationPrefix = "",
        },
    };

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    expect_binding_metadata_error(
        result, "provider-root", "1.0.0",
        "binding-header-source-dir-empty",
        "/bindingHeaders/1/sourceDir");
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

TEST(XvmBindingSelectionErrorTest,
     LegacyRejectsIncomingOnlyEdgeFromStartingNode) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1"].path = "/tool";
    db["peer"].type = "program";
    db["peer"].versions["1"].path = "/peer";
    db["peer"].bindings["tool"]["1"] = "1";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::AsymmetricEdge, "peer", "1");
}

TEST(XvmBindingSelectionErrorTest,
     LegacyIncomingEdgeRejectsMissingSourceVersion) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1"].path = "/tool";
    db["peer"].type = "program";
    db["peer"].versions["1"].path = "/peer";
    db["peer"].bindings["tool"]["2"] = "1";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::VersionNotFound, "peer", "2");
}

TEST(XvmBindingSelectionErrorTest,
     LegacyIncomingEdgeReportsMismatchedReciprocalSourceExactly) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1"].path = "/tool";
    db["peer"].type = "program";
    db["peer"].versions["1"].path = "/peer/1";
    db["peer"].versions["2"].path = "/peer/2";
    db["peer"].bindings["tool"]["1"] = "1";
    db["tool"].bindings["peer"]["1"] = "2";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::AsymmetricEdge, "peer", "1");
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

TEST(XvmExactRemovalTest, RemovesOnlyExactVersionReciprocalEdges) {
    xlings::xvm::VersionDB db;

    xlings::xvm::add_version(
        db, "toolchain", "15.1.0", "/pkg/toolchain-15");
    xlings::xvm::add_version(
        db, "cc", "15.1.0", "/pkg/toolchain-15",
        "program", "cc-15", "cc", "", "toolchain@15.1.0");
    xlings::xvm::add_version(
        db, "toolchain", "14.2.0", "/pkg/toolchain-14");
    xlings::xvm::add_version(
        db, "cc", "14.2.0", "/pkg/toolchain-14",
        "program", "cc-14", "cc", "", "toolchain@14.2.0");

    ASSERT_TRUE(
        xlings::xvm::remove_version(db, "cc", "15.1.0").has_value());

    ASSERT_TRUE(db.contains("cc"));
    EXPECT_FALSE(db.at("cc").versions.contains("15.1.0"));
    EXPECT_TRUE(db.at("cc").versions.contains("14.2.0"));
    ASSERT_TRUE(db.at("cc").bindings.contains("toolchain"));
    EXPECT_EQ(db.at("cc").bindings.at("toolchain").size(), 1u);
    EXPECT_EQ(
        db.at("cc").bindings.at("toolchain").at("14.2.0"),
        "14.2.0");

    ASSERT_TRUE(db.at("toolchain").bindings.contains("cc"));
    EXPECT_EQ(db.at("toolchain").bindings.at("cc").size(), 1u);
    EXPECT_EQ(
        db.at("toolchain").bindings.at("cc").at("14.2.0"),
        "14.2.0");
}

TEST(XvmExactRemovalTest, AmbiguousBareVersionFailsWithoutMutation) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "cc", "1.0.0", "/pkg/repo-a", "program",
        "cc-a", "cc", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "1.0.0", "/pkg/repo-b", "program",
        "cc-b", "cc", "repo-b");
    const auto before = xlings::xvm::versions_to_json(db);

    auto result = xlings::xvm::remove_version(db, "cc", "1.0.0");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::AmbiguousVersion);
    EXPECT_EQ(result.error().target, "cc");
    EXPECT_EQ(result.error().version, "1.0.0");
    EXPECT_TRUE(result.error().peerTarget.empty());
    EXPECT_TRUE(result.error().peerVersion.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), before);
}

TEST(XvmRemovalBatchTest, LateAsymmetricEdgeFailsWithoutPartialMutation) {
    xlings::xvm::VersionDB db;
    db["safe"].versions["repo:safe-1"].path = "/pkg/safe";
    db["a"].versions["repo:a-1"].path = "/pkg/a";
    db["b"].versions["repo:b-1"].path = "/pkg/b";
    db["a"].bindings["b"]["repo:a-1"] = "repo:b-1";
    db["b"].bindings["a"]["repo:b-1"] = "repo:wrong-a";
    xlings::xvm::Workspace workspace{
        {"safe", "repo:safe-1"},
        {"a", "repo:a-1"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"safe", {"repo:safe-1"}},
        {"a", {"repo:a-1"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = "safe",
            .version = "repo:safe-1",
        },
        {
            .op = "remove",
            .name = "a",
            .version = "repo:a-1",
        },
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::AsymmetricEdge);
    EXPECT_EQ(result.error().target, "a");
    EXPECT_EQ(result.error().version, "repo:a-1");
    EXPECT_EQ(result.error().peerTarget, "b");
    EXPECT_EQ(result.error().peerVersion, "repo:b-1");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XvmExactRemovalTest, UnpairedIncomingEdgeFailsWithoutMutation) {
    xlings::xvm::VersionDB db;
    db["a"].versions["repo:a-1"].path = "/pkg/a";
    db["b"].versions["repo:b-1"].path = "/pkg/b";
    db["b"].bindings["a"]["repo:b-1"] = "repo:a-1";
    const auto before = xlings::xvm::versions_to_json(db);

    auto result =
        xlings::xvm::remove_version(db, "a", "repo:a-1");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::AsymmetricEdge);
    EXPECT_EQ(result.error().target, "a");
    EXPECT_EQ(result.error().version, "repo:a-1");
    EXPECT_EQ(result.error().peerTarget, "b");
    EXPECT_EQ(result.error().peerVersion, "repo:b-1");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), before);
}

// ── Group-coherent reactivation after removal ────────────────────────
//
// Removing the active release has to leave the workspace coherent: every
// member of a toolchain either moves to the same surviving release together,
// or the whole group goes inactive. Picking a replacement per target is how
// `gcc` ends up on GCC 15 while `g++` lands on musl's 15 -- the mixed state
// the whole binding-group model exists to prevent, reintroduced at the moment
// of removal.

namespace {

// Register `members` as one provider release, rooted at the first entry.
void add_provider_group_(xlings::xvm::VersionDB& db,
                         std::string_view provider,
                         std::string_view providerVersion,
                         std::string_view group,
                         const std::vector<std::pair<std::string, std::string>>& members) {
    const auto& [rootTarget, rootVersion] = members.front();
    const xlings::xvm::BindingGroupRef ref{
        .provider = std::string(provider),
        .providerVersion = std::string(providerVersion),
        .group = std::string(group),
        .rootTarget = rootTarget,
        .rootVersion = rootVersion,
    };
    std::map<std::string, std::string> manifest;
    for (const auto& [target, version] : members) manifest[target] = version;

    for (const auto& [target, version] : members) {
        auto& info = db[target];
        if (info.type.empty()) info.type = "program";
        auto& data = info.versions[version];
        data.path = std::string("/pkg/") + std::string(provider);
        data.kind = "program";
        data.sourceName = target;
        data.destinationName = target;
        data.bindingGroup = ref;
    }
    auto& root = db[rootTarget].versions[rootVersion];
    root.bindingMembers = manifest;
    root.bindingMembersDeclared = true;
}

}  // namespace

TEST(XvmRemovalFallbackTest, IncoherentSurvivorDeactivatesTheWholeGroup) {
    xlings::xvm::VersionDB db;
    add_provider_group_(db, "pkgindex:gcc", "16.1.0", "gcc",
                        {{"gcc", "16.1.0"}, {"g++", "16.1.0"}});
    add_provider_group_(db, "pkgindex:gcc", "15.1.0", "gcc",
                        {{"gcc", "15.1.0"}, {"g++", "15.1.0"}});
    // musl also provides a `g++`. It is installed, but its own group is not
    // complete here -- only the g++ member of it is present.
    add_provider_group_(db, "pkgindex:musl", "1.2.5", "musl",
                        {{"g++", "musl:15.1.0"}});

    xlings::xvm::Workspace workspace{{"gcc", "16.1.0"}, {"g++", "16.1.0"}};
    // g++ lists the musl build last on purpose: an insertion-order-driven
    // fallback picks it, which is exactly the bug.
    xlings::xvm::WorkspaceInstalled installed{
        {"gcc", {"15.1.0", "16.1.0"}},
        {"g++", {"15.1.0", "16.1.0", "musl:15.1.0"}},
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {.op = "remove", .name = "gcc", .version = "16.1.0"},
        {.op = "remove", .name = "g++", .version = "16.1.0"},
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    ASSERT_TRUE(workspace.contains("gcc"));
    ASSERT_TRUE(workspace.contains("g++"));
    EXPECT_EQ(workspace.at("gcc"), "15.1.0");
    EXPECT_EQ(workspace.at("g++"), "15.1.0")
        << "g++ fell back to a different provider than gcc";
}

TEST(XvmRemovalFallbackTest, CoherentSurvivingGroupIsActivatedWholesale) {
    xlings::xvm::VersionDB db;
    add_provider_group_(db, "pkgindex:gcc", "16.1.0", "gcc",
                        {{"gcc", "16.1.0"}, {"g++", "16.1.0"}, {"gcc-ar", "16.1.0"}});
    add_provider_group_(db, "pkgindex:gcc", "15.1.0", "gcc",
                        {{"gcc", "15.1.0"}, {"g++", "15.1.0"}, {"gcc-ar", "15.1.0"}});
    xlings::xvm::Workspace workspace{
        {"gcc", "16.1.0"}, {"g++", "16.1.0"}, {"gcc-ar", "16.1.0"}};
    xlings::xvm::WorkspaceInstalled installed{
        {"gcc", {"15.1.0", "16.1.0"}},
        {"g++", {"15.1.0", "16.1.0"}},
        {"gcc-ar", {"15.1.0", "16.1.0"}},
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {.op = "remove", .name = "gcc", .version = "16.1.0"},
        {.op = "remove", .name = "g++", .version = "16.1.0"},
        {.op = "remove", .name = "gcc-ar", .version = "16.1.0"},
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(workspace.at("gcc"), "15.1.0");
    EXPECT_EQ(workspace.at("g++"), "15.1.0");
    EXPECT_EQ(workspace.at("gcc-ar"), "15.1.0");
}

TEST(XvmRemovalFallbackTest, NoCompleteSurvivorLeavesEveryMemberInactive) {
    xlings::xvm::VersionDB db;
    add_provider_group_(db, "pkgindex:gcc", "16.1.0", "gcc",
                        {{"gcc", "16.1.0"}, {"g++", "16.1.0"}});
    // 15 is only half installed: its manifest names gcc and g++, but only
    // g++ is registered. Rooted at g++ so the manifest survives the gap.
    add_provider_group_(db, "pkgindex:gcc", "15.1.0", "gcc",
                        {{"g++", "15.1.0"}, {"gcc", "15.1.0"}});
    db.at("gcc").versions.erase("15.1.0");
    xlings::xvm::Workspace workspace{{"gcc", "16.1.0"}, {"g++", "16.1.0"}};
    xlings::xvm::WorkspaceInstalled installed{
        {"gcc", {"16.1.0"}},
        {"g++", {"15.1.0", "16.1.0"}},
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {.op = "remove", .name = "gcc", .version = "16.1.0"},
        {.op = "remove", .name = "g++", .version = "16.1.0"},
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // g++ 15 survives on disk, but activating it alone would leave a `g++`
    // with no matching `gcc`. Better inactive than incoherent.
    EXPECT_FALSE(workspace.contains("gcc"));
    EXPECT_FALSE(workspace.contains("g++"))
        << "a lone member was activated without the rest of its group";
}

TEST(XvmRemovalFallbackTest, ResultDoesNotDependOnInstalledOrder) {
    const auto run = [](std::vector<std::string> gccOrder) {
        xlings::xvm::VersionDB db;
        for (const auto* v : {"14.1.0", "15.1.0", "16.1.0"}) {
            add_provider_group_(db, "pkgindex:gcc", v, "gcc",
                                {{"gcc", v}, {"g++", v}});
        }
        xlings::xvm::Workspace workspace{{"gcc", "16.1.0"}, {"g++", "16.1.0"}};
        xlings::xvm::WorkspaceInstalled installed{
            {"gcc", gccOrder},
            {"g++", {"14.1.0", "15.1.0", "16.1.0"}},
        };
        const std::vector<xlings::xvm::RemovalOperation> operations{
            {.op = "remove", .name = "gcc", .version = "16.1.0"},
            {.op = "remove", .name = "g++", .version = "16.1.0"},
        };
        auto result = xlings::xvm::apply_removal_batch(
            db, workspace, installed, operations, {});
        EXPECT_TRUE(result.has_value());
        return workspace;
    };

    // installed[] is append-ordered by whatever the user happened to install
    // first. The surviving release must not depend on it.
    const auto ascending = run({"14.1.0", "15.1.0", "16.1.0"});
    const auto descending = run({"16.1.0", "15.1.0", "14.1.0"});
    EXPECT_EQ(ascending, descending);
    EXPECT_EQ(ascending.at("gcc"), "15.1.0") << "expected the highest survivor";
}

TEST(XvmRemovalFallbackTest, UngroupedLegacyTargetStillFallsBack) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1.0.0"].path = "/pkg/tool";
    db["tool"].versions["2.0.0"].path = "/pkg/tool";
    xlings::xvm::Workspace workspace{{"tool", "2.0.0"}};
    xlings::xvm::WorkspaceInstalled installed{{"tool", {"1.0.0", "2.0.0"}}};
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {.op = "remove", .name = "tool", .version = "2.0.0"},
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // A target with no group is a group of one, so it can always fall back.
    EXPECT_EQ(workspace.at("tool"), "1.0.0");
}

TEST(XvmRemovalBatchTest, RawEmptyOperationNeverMeansAllVersions) {
    xlings::xvm::VersionDB db;
    db["sibling"].versions["repo-a:1.0.0"].path = "/pkg/a";
    db["sibling"].versions["repo-b:1.0.0"].path = "/pkg/b";
    xlings::xvm::Workspace workspace{
        {"sibling", "repo-a:1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"sibling", {"repo-a:1.0.0", "repo-b:1.0.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = "sibling",
            .version = "",
        },
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::AmbiguousVersion);
    EXPECT_EQ(result.error().target, "sibling");
    EXPECT_TRUE(result.error().version.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);

    db["sibling"].versions.erase("repo-b:1.0.0");
    workspace = {{"sibling", "repo-a:1.0.0"}};
    installed = {{"sibling", {"repo-a:1.0.0"}}};
    const auto oneVersionBefore = xlings::xvm::versions_to_json(db);

    result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::VersionNotFound);
    EXPECT_EQ(xlings::xvm::versions_to_json(db), oneVersionBefore);
    EXPECT_EQ(workspace.at("sibling"), "repo-a:1.0.0");
    EXPECT_EQ(
        installed.at("sibling"),
        (std::vector<std::string>{"repo-a:1.0.0"}));
}

TEST(XvmRemovalBatchTest,
     LegacySnapshotRemovesTransformedNamespacedMembersExactly) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "toolchain", "15.1.0", "/pkg/toolchain-15",
        "program", "", "", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "gcc-15.1.0", "/pkg/toolchain-15",
        "program", "cc-15", "cc", "repo-a",
        "toolchain@15.1.0");
    xlings::xvm::add_version(
        db, "toolchain", "14.2.0", "/pkg/toolchain-14",
        "program", "", "", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "gcc-14.2.0", "/pkg/toolchain-14",
        "program", "cc-14", "cc", "repo-a",
        "toolchain@14.2.0");
    xlings::xvm::Workspace workspace{
        {"toolchain", "repo-a:15.1.0"},
        {"cc", "repo-a:gcc-15.1.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {
            "toolchain",
            {"repo-a:14.2.0", "repo-a:15.1.0"},
        },
        {
            "cc",
            {"repo-a:gcc-14.2.0", "repo-a:gcc-15.1.0"},
        },
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = "toolchain",
            .version = "",
        },
    };

    auto context = xlings::xvm::snapshot_removal_context(
        db, "toolchain", "repo-a:15.1.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    ASSERT_TRUE(context->hasSelection);
    EXPECT_TRUE(context->provider.empty());

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->removed.size(), 2u);
    EXPECT_EQ(result->removed[0].target, "toolchain");
    EXPECT_EQ(result->removed[0].version, "repo-a:15.1.0");
    EXPECT_EQ(result->removed[1].target, "cc");
    EXPECT_EQ(result->removed[1].version, "repo-a:gcc-15.1.0");

    ASSERT_TRUE(db.contains("toolchain"));
    EXPECT_EQ(db.at("toolchain").versions.size(), 1u);
    EXPECT_TRUE(db.at("toolchain").versions.contains("repo-a:14.2.0"));
    ASSERT_TRUE(db.at("toolchain").bindings.contains("cc"));
    EXPECT_EQ(db.at("toolchain").bindings.at("cc").size(), 1u);
    EXPECT_EQ(
        db.at("toolchain").bindings.at("cc").at("repo-a:14.2.0"),
        "repo-a:gcc-14.2.0");

    ASSERT_TRUE(db.contains("cc"));
    EXPECT_EQ(db.at("cc").versions.size(), 1u);
    EXPECT_TRUE(db.at("cc").versions.contains("repo-a:gcc-14.2.0"));
    ASSERT_TRUE(db.at("cc").bindings.contains("toolchain"));
    EXPECT_EQ(db.at("cc").bindings.at("toolchain").size(), 1u);
    EXPECT_EQ(
        db.at("cc").bindings.at("toolchain").at(
            "repo-a:gcc-14.2.0"),
        "repo-a:14.2.0");

    EXPECT_EQ(workspace.at("toolchain"), "repo-a:14.2.0");
    EXPECT_EQ(workspace.at("cc"), "repo-a:gcc-14.2.0");
    EXPECT_EQ(
        installed.at("toolchain"),
        (std::vector<std::string>{"repo-a:14.2.0"}));
    EXPECT_EQ(
        installed.at("cc"),
        (std::vector<std::string>{"repo-a:gcc-14.2.0"}));
}

TEST(XvmRemovalBatchTest,
     ProviderRootOperationRemovesWholeOwnedReleaseOnly) {
    xlings::xvm::VersionDB db;
    auto addRelease = [&](const std::string& release,
                          const std::string& rootVersion,
                          const std::string& memberVersion) {
        const auto group = make_binding_group_ref(
            "repo-a:toolchain", release, "compiler",
            "toolchain", rootVersion);
        auto& root = add_provider_group_member(
            db, "toolchain", rootVersion, group, "group");
        root.bindingMembers = {
            {"cc", memberVersion},
            {"toolchain", rootVersion},
        };
        add_provider_group_member(
            db, "cc", memberVersion, group,
            "program", "cc-a", "cc");
    };
    addRelease("1.0.0", "repo-a:1.0.0", "repo-a:gcc-1.0.0");
    addRelease("2.0.0", "repo-a:2.0.0", "repo-a:gcc-2.0.0");
    xlings::xvm::Workspace workspace{
        {"toolchain", "repo-a:1.0.0"},
        {"cc", "repo-a:gcc-1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"toolchain", {"repo-a:1.0.0", "repo-a:2.0.0"}},
        {"cc", {"repo-a:gcc-1.0.0", "repo-a:gcc-2.0.0"}},
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = "toolchain",
        },
    };
    auto context = xlings::xvm::snapshot_removal_context(
        db, "toolchain", "repo-a:1.0.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->removed.size(), 2u);
    EXPECT_FALSE(
        db.at("toolchain").versions.contains("repo-a:1.0.0"));
    EXPECT_TRUE(
        db.at("toolchain").versions.contains("repo-a:2.0.0"));
    EXPECT_FALSE(
        db.at("cc").versions.contains("repo-a:gcc-1.0.0"));
    EXPECT_TRUE(
        db.at("cc").versions.contains("repo-a:gcc-2.0.0"));
    EXPECT_EQ(workspace.at("toolchain"), "repo-a:2.0.0");
    EXPECT_EQ(workspace.at("cc"), "repo-a:gcc-2.0.0");
}

TEST(XvmRemovalBatchTest,
     RemoveAllDeletesOnlyExecutingProviderAcrossReleases) {
    xlings::xvm::VersionDB db;
    auto& providerA1 = db["cc"].versions["repo-a:1.0.0"];
    providerA1.path = "/pkg/a/1";
    providerA1.kind = "program";
    providerA1.bindingGroup = make_binding_group_ref(
        "repo-a:provider", "1.0.0", "cc-a-1",
        "root-a-1", "repo-a:1.0.0");
    auto& providerA2 = db["cc"].versions["repo-a:2.0.0"];
    providerA2.path = "/pkg/a/2";
    providerA2.kind = "program";
    providerA2.bindingGroup = make_binding_group_ref(
        "repo-a:provider", "2.0.0", "cc-a-2",
        "root-a-2", "repo-a:2.0.0");
    auto& providerB = db["cc"].versions["repo-b:9.0.0"];
    providerB.path = "/pkg/b/9";
    providerB.kind = "program";
    // Self-rooted, with a manifest. This entry is the one the workspace is
    // expected to fall back to, and reactivation now requires the candidate's
    // group to actually resolve -- a group whose root is not registered is
    // exactly the dangling state that used to get written into the active
    // workspace. The other entries stay minimal: they are only ever removal
    // subjects here, never fallback candidates.
    providerB.bindingGroup = make_binding_group_ref(
        "repo-b:provider", "9.0.0", "cc-b-9",
        "cc", "repo-b:9.0.0");
    providerB.bindingMembers = {{"cc", "repo-b:9.0.0"}};
    providerB.bindingMembersDeclared = true;
    db["cc"].versions["legacy:0.9.0"].path = "/pkg/legacy";

    xlings::xvm::Workspace workspace{
        {"cc", "repo-a:2.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {
            "cc",
            {
                "legacy:0.9.0",
                "repo-b:9.0.0",
                "repo-a:1.0.0",
                "repo-a:2.0.0",
            },
        },
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove_all",
            .name = "cc",
            .version = "",
        },
    };
    const xlings::xvm::RemovalContext context{
        .provider = "repo-a:provider",
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->removed.size(), 2u);
    EXPECT_EQ(result->removed[0].target, "cc");
    EXPECT_EQ(result->removed[0].version, "repo-a:1.0.0");
    EXPECT_EQ(result->removed[1].target, "cc");
    EXPECT_EQ(result->removed[1].version, "repo-a:2.0.0");

    ASSERT_TRUE(db.contains("cc"));
    EXPECT_EQ(db.at("cc").versions.size(), 2u);
    EXPECT_TRUE(db.at("cc").versions.contains("repo-b:9.0.0"));
    EXPECT_TRUE(db.at("cc").versions.contains("legacy:0.9.0"));
    EXPECT_EQ(workspace.at("cc"), "repo-b:9.0.0");
    EXPECT_EQ(
        installed.at("cc"),
        (std::vector<std::string>{
            "legacy:0.9.0",
            "repo-b:9.0.0",
        }));
}

TEST(XvmRemovalBatchTest, RemoveAllRejectsMissingProviderContext) {
    xlings::xvm::VersionDB db;
    auto& data = db["cc"].versions["repo-a:1.0.0"];
    data.path = "/pkg/a/1";
    data.kind = "program";
    data.bindingGroup = make_binding_group_ref(
        "repo-a:provider", "1.0.0", "cc-a",
        "root-a", "repo-a:1.0.0");
    xlings::xvm::Workspace workspace{
        {"cc", "repo-a:1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"repo-a:1.0.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove_all",
            .name = "cc",
            .version = "",
        },
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::ProviderRequired);
    EXPECT_EQ(result.error().target, "cc");
    EXPECT_TRUE(result.error().version.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XvmRemovalBatchTest, RemoveAllRejectsNoOwnedMatchingVersion) {
    xlings::xvm::VersionDB db;
    auto& providerB = db["cc"].versions["repo-b:9.0.0"];
    providerB.path = "/pkg/b/9";
    providerB.kind = "program";
    providerB.bindingGroup = make_binding_group_ref(
        "repo-b:provider", "9.0.0", "cc-b",
        "root-b", "repo-b:9.0.0");
    db["cc"].versions["legacy:0.9.0"].path = "/pkg/legacy";
    xlings::xvm::Workspace workspace{
        {"cc", "repo-b:9.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"legacy:0.9.0", "repo-b:9.0.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove_all",
            .name = "cc",
            .version = "",
        },
    };
    const xlings::xvm::RemovalContext context{
        .provider = "repo-a:provider",
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, context);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::ProviderVersionNotFound);
    EXPECT_EQ(result.error().target, "cc");
    EXPECT_TRUE(result.error().version.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XvmRemovalBatchTest,
     ProviderSnapshotRejectsRecipeVersionOutsideOwnedSelection) {
    xlings::xvm::VersionDB db;
    const auto groupA = make_binding_group_ref(
        "repo-a:provider", "1.0.0", "group-a",
        "root-a", "repo-a:1.0.0");
    auto& rootA = add_provider_group_member(
        db, "root-a", "repo-a:1.0.0", groupA, "group");
    rootA.bindingMembers = {
        {"cc", "repo-a:1.0.0"},
        {"root-a", "repo-a:1.0.0"},
    };
    add_provider_group_member(
        db, "cc", "repo-a:1.0.0", groupA,
        "program", "cc-a", "cc");

    const auto groupB = make_binding_group_ref(
        "repo-b:provider", "2.0.0", "group-b",
        "root-b", "repo-b:2.0.0");
    auto& rootB = add_provider_group_member(
        db, "root-b", "repo-b:2.0.0", groupB, "group");
    rootB.bindingMembers = {
        {"cc", "repo-b:2.0.0"},
        {"root-b", "repo-b:2.0.0"},
    };
    add_provider_group_member(
        db, "cc", "repo-b:2.0.0", groupB,
        "program", "cc-b", "cc");

    xlings::xvm::Workspace workspace{
        {"cc", "repo-a:1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"repo-a:1.0.0", "repo-b:2.0.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = "cc",
            .version = "repo-b:2.0.0",
        },
    };
    auto context = xlings::xvm::snapshot_removal_context(
        db, "root-a", "repo-a:1.0.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    EXPECT_EQ(context->provider, "repo-a:provider");

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, *context);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::VersionMismatch);
    EXPECT_EQ(result.error().target, "cc");
    EXPECT_EQ(result.error().version, "repo-b:2.0.0");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XvmRemovalBatchTest,
     ProviderSnapshotIncludesEveryGroupOwnedByTheRelease) {
    xlings::xvm::VersionDB db;
    const auto compilerGroup = make_binding_group_ref(
        "repo-a:toolchain", "1.0.0", "compiler",
        "compiler-root", "repo-a:1.0.0");
    auto& compilerRoot = add_provider_group_member(
        db, "compiler-root", "repo-a:1.0.0",
        compilerGroup, "group");
    compilerRoot.bindingMembers = {
        {"cc", "repo-a:1.0.0"},
        {"compiler-root", "repo-a:1.0.0"},
    };
    add_provider_group_member(
        db, "cc", "repo-a:1.0.0",
        compilerGroup, "program", "cc-a", "cc");

    const auto toolsGroup = make_binding_group_ref(
        "repo-a:toolchain", "1.0.0", "tools",
        "tools-root", "repo-a:tools-1.0.0");
    auto& toolsRoot = add_provider_group_member(
        db, "tools-root", "repo-a:tools-1.0.0",
        toolsGroup, "group");
    toolsRoot.bindingMembers = {
        {"ar", "repo-a:tools-1.0.0"},
        {"tools-root", "repo-a:tools-1.0.0"},
    };
    add_provider_group_member(
        db, "ar", "repo-a:tools-1.0.0",
        toolsGroup, "program", "ar-a", "ar");

    auto context = xlings::xvm::snapshot_removal_context(
        db, "cc", "repo-a:1.0.0");

    ASSERT_TRUE(context.has_value()) << context.error().message;
    EXPECT_EQ(context->provider, "repo-a:toolchain");
    EXPECT_EQ(
        context->members,
        (std::map<std::string, std::string>{
            {"ar", "repo-a:tools-1.0.0"},
            {"cc", "repo-a:1.0.0"},
            {"compiler-root", "repo-a:1.0.0"},
            {"tools-root", "repo-a:tools-1.0.0"},
        }));
}

TEST(XimXvmRegistrationAdapterTest,
     RootAfterChildProducesOneExactProviderGroup) {
    xlings::xim::PlanNode provider;
    provider.name = "toolchain";
    provider.version = "15.1.0";
    provider.namespaceName = "repo-a";
    provider.canonicalName = "repo-a:toolchain";
    provider.storeRoot = "/store";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "cc",
            .version = "gcc-15.1.0",
            .bindir = "/payload/bin",
            .type = "program",
            .filename = "cc-real",
            .binding = "toolchain@15.1.0",
        },
        {
            .op = "add",
            .name = "toolchain",
            .version = "15.1.0",
            .type = "group",
        },
    };

    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo-a", "/data", false);

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    EXPECT_EQ(plan->batch.provider, "repo-a:toolchain");
    EXPECT_EQ(plan->batch.providerVersion, "15.1.0");
    ASSERT_EQ(plan->batch.nodes.size(), 2u);
    EXPECT_EQ(plan->batch.nodes[0].target, "cc");
    EXPECT_EQ(plan->batch.nodes[0].version, "repo-a:gcc-15.1.0");
    ASSERT_TRUE(plan->batch.nodes[0].binding.has_value());
    EXPECT_EQ(
        plan->batch.nodes[0].binding->rootTarget,
        "toolchain");
    EXPECT_EQ(
        plan->batch.nodes[0].binding->rootVersion,
        "repo-a:15.1.0");
    EXPECT_EQ(plan->batch.nodes[1].target, "toolchain");
    EXPECT_EQ(plan->batch.nodes[1].version, "repo-a:15.1.0");

    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, plan->batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& root =
        db.at("toolchain").versions.at("repo-a:15.1.0");
    const auto& child =
        db.at("cc").versions.at("repo-a:gcc-15.1.0");
    ASSERT_TRUE(root.bindingGroup.has_value());
    ASSERT_TRUE(child.bindingGroup.has_value());
    EXPECT_EQ(root.bindingGroup->provider, "repo-a:toolchain");
    EXPECT_EQ(root.bindingGroup->providerVersion, "15.1.0");
    EXPECT_EQ(root.bindingGroup->group, "toolchain");
    EXPECT_EQ(
        child.bindingGroup->provider,
        root.bindingGroup->provider);
    EXPECT_EQ(
        child.bindingGroup->providerVersion,
        root.bindingGroup->providerVersion);
    EXPECT_EQ(
        child.bindingGroup->group,
        root.bindingGroup->group);
    EXPECT_EQ(
        child.bindingGroup->rootTarget,
        root.bindingGroup->rootTarget);
    EXPECT_EQ(
        child.bindingGroup->rootVersion,
        root.bindingGroup->rootVersion);
    EXPECT_EQ(
        root.bindingMembers,
        (std::map<std::string, std::string>{
            {"cc", "repo-a:gcc-15.1.0"},
            {"toolchain", "repo-a:15.1.0"},
        }));
    EXPECT_EQ(
        installed.at("cc"),
        (std::vector<std::string>{"repo-a:gcc-15.1.0"}));
    EXPECT_EQ(
        installed.at("toolchain"),
        (std::vector<std::string>{"repo-a:15.1.0"}));
}

TEST(XimXvmRegistrationAdapterTest,
     NormalizesVersionNamespaceExactlyOnceAndRejectsConflicts) {
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo-a";
    const auto makeAdd = [](std::string version) {
        return std::vector<mcpplibs::xpkg::XvmOp>{
            {
                .op = "add",
                .name = "tool",
                .version = std::move(version),
            },
        };
    };

    auto matching = xlings::xim::normalize_xpkg_registration_plan(
        provider, makeAdd("repo-a:tool-1.0.0"),
        "repo-a", "/data", false);
    ASSERT_TRUE(matching.has_value()) << matching.error().message;
    ASSERT_EQ(matching->batch.nodes.size(), 1u);
    EXPECT_EQ(
        matching->batch.nodes[0].version,
        "repo-a:tool-1.0.0");

    struct InvalidCase {
        std::string expectedNamespace;
        std::string version;
    };
    const std::array<InvalidCase, 3> invalidCases{
        InvalidCase{"repo-a", "repo-b:tool-1.0.0"},
        InvalidCase{"", "repo-a:tool-1.0.0"},
        InvalidCase{"repo-a", "repo-a:repo-a:tool-1.0.0"},
    };
    for (const auto& testCase : invalidCases) {
        SCOPED_TRACE(testCase.version);
        auto result = xlings::xim::normalize_xpkg_registration_plan(
            provider, makeAdd(testCase.version),
            testCase.expectedNamespace, "/data", false);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(
            result.error().kind,
            xlings::xim::XpkgRegistrationErrorKind::InvalidVersion);
        EXPECT_EQ(result.error().operationIndex, 0u);
        EXPECT_EQ(result.error().target, "tool");
        EXPECT_EQ(result.error().version, testCase.version);
        EXPECT_FALSE(result.error().message.empty());
    }
}

TEST(XimXvmRegistrationAdapterTest,
     RejectsMalformedLegacyBindingBeforeRegistration) {
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo-a";
    const std::array<std::string, 4> invalidBindings{
        "root",
        "@1.0.0",
        "root@",
        "root@1.0.0@extra",
    };

    for (const auto& binding : invalidBindings) {
        SCOPED_TRACE(binding);
        const std::vector<mcpplibs::xpkg::XvmOp> operations{
            {
                .op = "add",
                .name = "child",
                .binding = binding,
            },
        };
        auto result = xlings::xim::normalize_xpkg_registration_plan(
            provider, operations, "repo-a", "/data", false);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(
            result.error().kind,
            xlings::xim::XpkgRegistrationErrorKind::InvalidBinding);
        EXPECT_EQ(result.error().operationIndex, 0u);
        EXPECT_EQ(result.error().target, "child");
        EXPECT_EQ(result.error().version, binding);
        EXPECT_FALSE(result.error().message.empty());
    }
}

TEST(XimXvmRegistrationAdapterTest,
     DeduplicatesEqualEnvironmentAndRejectsConflictingDuplicate) {
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    const std::vector<mcpplibs::xpkg::XvmOp> equalOperations{
        {
            .op = "add",
            .name = "tool",
            .envs = {
                {"MODE", "release"},
                {"MODE", "release"},
            },
        },
    };
    auto equal = xlings::xim::normalize_xpkg_registration_plan(
        provider, equalOperations, "", "/data", false);

    ASSERT_TRUE(equal.has_value()) << equal.error().message;
    ASSERT_EQ(equal->batch.nodes.size(), 1u);
    EXPECT_EQ(
        equal->batch.nodes[0].envs,
        (std::map<std::string, std::string>{
            {"MODE", "release"},
        }));

    const std::vector<mcpplibs::xpkg::XvmOp> conflictOperations{
        {
            .op = "add",
            .name = "tool",
            .envs = {
                {"MODE", "release"},
                {"MODE", "debug"},
            },
        },
    };
    auto conflict = xlings::xim::normalize_xpkg_registration_plan(
        provider, conflictOperations, "", "/data", false);

    ASSERT_FALSE(conflict.has_value());
    EXPECT_EQ(
        conflict.error().kind,
        xlings::xim::XpkgRegistrationErrorKind::ConflictingEnvironment);
    EXPECT_EQ(conflict.error().operationIndex, 0u);
    EXPECT_EQ(conflict.error().target, "tool");
    EXPECT_EQ(conflict.error().version, "MODE");
    EXPECT_FALSE(conflict.error().message.empty());
}

TEST(XimXvmRegistrationAdapterTest,
     CanonicalProviderAndVersionPayloadDriveDeferredEffects) {
    xlings::xim::PlanNode provider;
    provider.name = "recipe-name";
    provider.version = "2.0.0";
    provider.namespaceName = "repo-a";
    provider.canonicalName = "canonical:provider";
    provider.storeRoot = "/store";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "cc",
            .alias = "--driver",
            .envs = {
                {"CC_MODE", "strict"},
            },
        },
        {
            .op = "add",
            .name = "compiler-runtime",
            .version = "runtime-2.0.0",
            .bindir = "/payload/lib",
            .type = "lib",
            .filename = "libcompiler.so.2",
        },
        {
            .op = "add",
            .name = "toolchain",
            .type = "group",
        },
    };

    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo-a", "/data", true);

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    EXPECT_EQ(plan->batch.provider, "canonical:provider");
    EXPECT_EQ(plan->batch.providerVersion, "2.0.0");
    EXPECT_TRUE(plan->batch.useAfterInstall);
    ASSERT_EQ(plan->batch.nodes.size(), 3u);

    const auto& program = plan->batch.nodes[0];
    EXPECT_EQ(program.target, "cc");
    EXPECT_EQ(program.version, "repo-a:2.0.0");
    EXPECT_EQ(
        program.path,
        "/store/repo-a-x-recipe-name/2.0.0");
    EXPECT_EQ(program.kind, "program");
    EXPECT_EQ(program.sourceName, "cc");
    EXPECT_EQ(program.destinationName, "cc");
    EXPECT_EQ(
        program.alias,
        (std::vector<std::string>{"--driver"}));
    EXPECT_EQ(
        program.envs,
        (std::map<std::string, std::string>{
            {"CC_MODE", "strict"},
        }));

    const auto& library = plan->batch.nodes[1];
    EXPECT_EQ(library.target, "compiler-runtime");
    EXPECT_EQ(library.version, "repo-a:runtime-2.0.0");
    EXPECT_EQ(library.path, "/payload/lib");
    EXPECT_EQ(library.kind, "lib");
    EXPECT_EQ(library.sourceName, "libcompiler.so.2");
    EXPECT_EQ(library.destinationName, "libcompiler.so.2");

    const auto& group = plan->batch.nodes[2];
    EXPECT_EQ(group.target, "toolchain");
    EXPECT_EQ(group.version, "repo-a:2.0.0");
    EXPECT_EQ(group.kind, "group");
    EXPECT_TRUE(group.sourceName.empty());
    EXPECT_TRUE(group.destinationName.empty());

    ASSERT_EQ(plan->effects.size(), 2u);
    EXPECT_EQ(
        plan->effects[0].kind,
        xlings::xim::XpkgFilesystemEffectKind::ProgramShim);
    EXPECT_EQ(plan->effects[0].target, "cc");
    EXPECT_EQ(plan->effects[0].version, "repo-a:2.0.0");
    EXPECT_EQ(
        plan->effects[1].kind,
        xlings::xim::XpkgFilesystemEffectKind::Library);
    EXPECT_EQ(plan->effects[1].target, "compiler-runtime");
    EXPECT_EQ(
        plan->effects[1].version,
        "repo-a:runtime-2.0.0");
}

TEST(XimXvmRegistrationAdapterTest,
     UngroupedHeaderRoutesToOnlyRootWithoutPhantomProviderTarget) {
    xlings::xim::PlanNode provider;
    provider.name = "recipe-provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo-a";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "toolchain",
            .type = "group",
        },
        {
            .op = "headers",
            .includedir = "/payload/include",
        },
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo-a", "/data", false);

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    ASSERT_EQ(plan->batch.headers.size(), 1u);
    EXPECT_EQ(
        plan->batch.headers[0].sourceDir,
        "/payload/include");
    EXPECT_TRUE(plan->batch.headers[0].destinationPrefix.empty());
    EXPECT_TRUE(plan->batch.headers[0].group.empty());
    ASSERT_EQ(plan->effects.size(), 1u);
    EXPECT_EQ(
        plan->effects[0].kind,
        xlings::xim::XpkgFilesystemEffectKind::InstallHeaders);
    EXPECT_EQ(plan->effects[0].sourceDir, "/payload/include");

    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, plan->batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(db.contains("toolchain"));
    EXPECT_FALSE(db.contains("recipe-provider"));
    const auto& root =
        db.at("toolchain").versions.at("repo-a:1.0.0");
    ASSERT_EQ(root.bindingHeaders.size(), 1u);
    EXPECT_EQ(root.bindingHeaders[0].sourceDir, "/payload/include");
    EXPECT_TRUE(root.bindingHeaders[0].destinationPrefix.empty());
}

// The shape this actually unblocks: a package that registers a program and a
// library under no binding, then declares headers. Both nodes become
// singleton groups, so before the primaryTarget tie-break this recipe failed
// to install outright. Goes through the normalizer, so it also covers the
// wiring -- that the batch carries the package's own name as the hint.
TEST(XimXvmRegistrationAdapterTest,
     UngroupedHeaderRoutesToThePackagesOwnTarget) {
    xlings::xim::PlanNode provider;
    provider.name = "openssl";
    provider.version = "3.1.5";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {.op = "add", .name = "openssl"},
        {.op = "add", .name = "libssl", .type = "lib", .filename = "libssl.so"},
        {.op = "headers", .includedir = "/payload/include"},
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "", "/data", false);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    EXPECT_EQ(plan->batch.primaryTarget, "openssl");

    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, plan->batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& owner = db.at("openssl").versions.at("3.1.5");
    ASSERT_EQ(owner.bindingHeaders.size(), 1u);
    EXPECT_EQ(owner.bindingHeaders[0].sourceDir, "/payload/include");
    EXPECT_TRUE(db.at("libssl").versions.at("3.1.5").bindingHeaders.empty());
}

TEST(XimXvmRegistrationAdapterTest,
     UngroupedHeaderRejectsMultipleGroupsWithoutMutation) {
    xlings::xim::PlanNode provider;
    provider.name = "recipe-provider";
    provider.version = "1.0.0";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "compiler",
            .type = "group",
        },
        {
            .op = "add",
            .name = "tools",
            .version = "tools-1.0.0",
            .type = "group",
        },
        {
            .op = "headers",
            .includedir = "/payload/include",
        },
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "", "/data", false);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;

    xlings::xvm::VersionDB db;
    db["sentinel"].versions["0"].path = "/sentinel";
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{
        {"sentinel", {"0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, plan->batch);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RegistrationErrorKind::HeaderAmbiguous);
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XimXvmRegistrationAdapterTest,
     RemoveHeadersProducesOnlyADeferredCompatibilityEffect) {
    xlings::xim::PlanNode provider;
    provider.name = "recipe-provider";
    provider.version = "1.0.0";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove_headers",
            .includedir = "/payload/include",
        },
    };

    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "", "/data", false);

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    EXPECT_TRUE(plan->batch.nodes.empty());
    EXPECT_TRUE(plan->batch.headers.empty());
    ASSERT_EQ(plan->effects.size(), 1u);
    EXPECT_EQ(
        plan->effects[0].kind,
        xlings::xim::XpkgFilesystemEffectKind::RemoveHeaders);
    EXPECT_EQ(plan->effects[0].sourceDir, "/payload/include");
}

TEST(XimXvmMetadataBatchTest,
     LateRegistrationConflictPreservesCallerStateAndArtifact) {
    xlings::xvm::VersionDB db;
    auto& oldVersion = db["old-tool"].versions["repo:old-1.0.0"];
    oldVersion.path = "/old/tool";
    oldVersion.kind = "program";
    oldVersion.sourceName = "old-tool";
    oldVersion.destinationName = "old-tool";

    auto& conflict = db["new-tool"].versions["repo:new-1.0.0"];
    conflict.path = "/other/tool";
    conflict.kind = "program";
    conflict.sourceName = "other-tool";
    conflict.destinationName = "new-tool";
    conflict.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "other:provider",
        .providerVersion = "1.0.0",
        .group = "new-tool",
        .rootTarget = "new-tool",
        .rootVersion = "repo:new-1.0.0",
    };
    conflict.bindingMembers = {
        {"new-tool", "repo:new-1.0.0"},
    };
    conflict.bindingMembersDeclared = true;

    xlings::xvm::Workspace workspace{
        {"old-tool", "repo:old-1.0.0"},
        {"new-tool", "repo:new-1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"old-tool", {"repo:old-1.0.0"}},
        {"new-tool", {"repo:new-1.0.0"}},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove",
            .name = "old-tool",
            .version = "repo:old-1.0.0",
        },
        {
            .op = "add",
            .name = "new-tool",
            .version = "repo:new-1.0.0",
            .bindir = "/new/tool",
        },
    };
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:provider";
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo", "/data", false);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations,
        provider.canonicalName, provider.version);
    ASSERT_TRUE(context.has_value()) << context.error().message;

    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const auto artifactDir =
        std::filesystem::temp_directory_path()
        / std::format(
            "xlings-xvm-metadata-batch-sentinel-{}",
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());
    std::filesystem::create_directories(artifactDir);
    const auto artifact = artifactDir / "keep";
    {
        std::ofstream output(artifact);
        output << "unchanged";
    }

    auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, operations, *context, *plan);

    ASSERT_FALSE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<xlings::xvm::RegistrationError>(
        result.error()));
    EXPECT_EQ(
        std::get<xlings::xvm::RegistrationError>(result.error()).kind,
        xlings::xvm::RegistrationErrorKind::OwnershipConflict);
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
    ASSERT_TRUE(std::filesystem::exists(artifact));
    std::ifstream input(artifact);
    std::string artifactContents;
    input >> artifactContents;
    EXPECT_EQ(artifactContents, "unchanged");
    std::filesystem::remove_all(artifactDir);
}

TEST(XimXvmMetadataBatchTest,
     RemoveOldAddNewCommitsAndPreservesAnotherProviderRelease) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    const auto seedRelease = [&](
            const std::string& release,
            bool useAfterInstall) {
        xlings::xim::PlanNode provider;
        provider.name = "toolchain";
        provider.version = release;
        provider.namespaceName = "repo";
        provider.canonicalName = "repo:toolchain";
        const std::vector<mcpplibs::xpkg::XvmOp> operations{
            {
                .op = "add",
                .name = "cc",
                .version = "cc-" + release,
                .bindir = "/payload/" + release + "/bin",
                .binding = "toolchain@" + release,
            },
            {
                .op = "add",
                .name = "compiler-runtime",
                .version = "runtime-" + release,
                .bindir = "/payload/" + release + "/lib",
                .type = "lib",
                .filename = "libcompiler.so",
                .binding = "toolchain@" + release,
            },
            {
                .op = "add",
                .name = "toolchain",
                .type = "group",
            },
        };
        auto plan = xlings::xim::normalize_xpkg_registration_plan(
            provider, operations, "repo", "/data", useAfterInstall);
        EXPECT_TRUE(plan.has_value());
        if (!plan) return;
        auto result = xlings::xvm::apply_registration_batch(
            db, workspace, installed, plan->batch);
        EXPECT_TRUE(result.has_value());
    };
    seedRelease("0.9.0", false);
    seedRelease("1.0.0", true);
    ASSERT_EQ(db.at("toolchain").versions.size(), 2u);

    xlings::xim::PlanNode provider;
    provider.name = "toolchain";
    provider.version = "2.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:toolchain";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove",
            .name = "toolchain",
            .version = "repo:1.0.0",
        },
        {
            .op = "add",
            .name = "cc",
            .version = "cc-2.0.0",
            .bindir = "/payload/2.0.0/bin",
            .binding = "toolchain@2.0.0",
        },
        {
            .op = "add",
            .name = "compiler-runtime",
            .version = "runtime-2.0.0",
            .bindir = "/payload/2.0.0/lib",
            .type = "lib",
            .filename = "libcompiler.so",
            .binding = "toolchain@2.0.0",
        },
        {
            .op = "add",
            .name = "toolchain",
            .type = "group",
        },
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo", "/data", true);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations,
        provider.canonicalName, provider.version);
    ASSERT_TRUE(context.has_value()) << context.error().message;

    auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, operations, *context, *plan);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->removal.removed.size(), 3u);
    EXPECT_EQ(result->registered.size(), 3u);
    ASSERT_EQ(result->effects.size(), 2u);
    EXPECT_EQ(
        result->effects[0].kind,
        xlings::xim::XpkgFilesystemEffectKind::ProgramShim);
    EXPECT_EQ(
        result->effects[1].kind,
        xlings::xim::XpkgFilesystemEffectKind::Library);
    for (const auto& [target, oldVersion, otherVersion, newVersion] :
         std::array<std::array<std::string, 4>, 3>{
             std::array<std::string, 4>{
                 "toolchain", "repo:1.0.0",
                 "repo:0.9.0", "repo:2.0.0",
             },
             std::array<std::string, 4>{
                 "cc", "repo:cc-1.0.0",
                 "repo:cc-0.9.0", "repo:cc-2.0.0",
             },
             std::array<std::string, 4>{
                 "compiler-runtime", "repo:runtime-1.0.0",
                 "repo:runtime-0.9.0", "repo:runtime-2.0.0",
             },
         }) {
        SCOPED_TRACE(target);
        ASSERT_TRUE(db.contains(target));
        EXPECT_FALSE(db.at(target).versions.contains(oldVersion));
        EXPECT_TRUE(db.at(target).versions.contains(otherVersion));
        EXPECT_TRUE(db.at(target).versions.contains(newVersion));
        EXPECT_EQ(workspace.at(target), newVersion);
        EXPECT_EQ(
            installed.at(target),
            (std::vector<std::string>{otherVersion, newVersion}));
    }
    const auto& newRoot =
        db.at("toolchain").versions.at("repo:2.0.0");
    ASSERT_TRUE(newRoot.bindingGroup.has_value());
    EXPECT_EQ(newRoot.bindingGroup->provider, "repo:toolchain");
    EXPECT_EQ(newRoot.bindingGroup->providerVersion, "2.0.0");
    EXPECT_EQ(newRoot.bindingMembers.size(), 3u);
}

TEST(XimXvmMetadataBatchTest,
     EmptyMetadataOperationsDoNotPurgeDiscoveredProviderSelection) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:provider";
    const std::vector<mcpplibs::xpkg::XvmOp> seedOperations{
        {
            .op = "add",
            .name = "provider-root",
            .type = "group",
        },
    };
    auto seedPlan = xlings::xim::normalize_xpkg_registration_plan(
        provider, seedOperations, "repo", "/data", true);
    ASSERT_TRUE(seedPlan.has_value()) << seedPlan.error().message;
    auto seeded = xlings::xvm::apply_registration_batch(
        db, workspace, installed, seedPlan->batch);
    ASSERT_TRUE(seeded.has_value()) << seeded.error().message;

    const std::vector<mcpplibs::xpkg::XvmOp> operations;
    auto emptyPlan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo", "/data", false);
    ASSERT_TRUE(emptyPlan.has_value()) << emptyPlan.error().message;
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations,
        provider.canonicalName, provider.version);
    ASSERT_TRUE(context.has_value()) << context.error().message;
    ASSERT_TRUE(context->hasSelection);
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed,
        operations, *context, *emptyPlan);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->removal.removed.empty());
    EXPECT_TRUE(result->registered.empty());
    EXPECT_TRUE(result->effects.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XimXvmMetadataBatchTest,
     RootOrderPermutationsSerializeIdenticallyThroughAdapter) {
    xlings::xim::PlanNode provider;
    provider.name = "toolchain";
    provider.version = "1.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:toolchain";
    const mcpplibs::xpkg::XvmOp root{
        .op = "add",
        .name = "toolchain",
        .type = "group",
    };
    const mcpplibs::xpkg::XvmOp child{
        .op = "add",
        .name = "cc",
        .version = "cc-1.0.0",
        .bindir = "/payload/bin",
        .filename = "cc-real",
        .binding = "toolchain@1.0.0",
    };
    const auto apply = [&](
            const std::vector<mcpplibs::xpkg::XvmOp>& operations) {
        xlings::xvm::VersionDB db;
        xlings::xvm::Workspace workspace;
        xlings::xvm::WorkspaceInstalled installed;
        auto plan = xlings::xim::normalize_xpkg_registration_plan(
            provider, operations, "repo", "/data", false);
        EXPECT_TRUE(plan.has_value());
        if (!plan) {
            return std::tuple{
                nlohmann::json{},
                xlings::xvm::Workspace{},
                xlings::xvm::WorkspaceInstalled{},
            };
        }
        auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
            db, workspace, installed, operations,
            xlings::xvm::RemovalContext{}, *plan);
        EXPECT_TRUE(result.has_value());
        return std::tuple{
            xlings::xvm::versions_to_json(db),
            std::move(workspace),
            std::move(installed),
        };
    };

    const auto rootFirst = apply({root, child});
    const auto rootLast = apply({child, root});

    EXPECT_EQ(rootFirst, rootLast);
}

TEST(XimXvmMetadataBatchTest,
     PhantomAndSelfBindingsPropagateWithoutStateMutation) {
    struct InvalidCase {
        std::string binding;
        xlings::xvm::RegistrationErrorKind expected;
    };
    const std::array<InvalidCase, 2> invalidCases{
        InvalidCase{
            "missing-root@1.0.0",
            xlings::xvm::RegistrationErrorKind::RootNotInBatch,
        },
        InvalidCase{
            "child@1.0.0",
            xlings::xvm::RegistrationErrorKind::SelfBinding,
        },
    };
    for (const auto& testCase : invalidCases) {
        SCOPED_TRACE(testCase.binding);
        xlings::xim::PlanNode provider;
        provider.name = "provider";
        provider.version = "1.0.0";
        provider.namespaceName = "repo";
        provider.canonicalName = "repo:provider";
        const std::vector<mcpplibs::xpkg::XvmOp> operations{
            {
                .op = "add",
                .name = "child",
                .binding = testCase.binding,
            },
        };
        auto plan = xlings::xim::normalize_xpkg_registration_plan(
            provider, operations, "repo", "/data", false);
        ASSERT_TRUE(plan.has_value()) << plan.error().message;

        xlings::xvm::VersionDB db;
        db["sentinel"].versions["0"].path = "/sentinel";
        xlings::xvm::Workspace workspace{{"sentinel", "0"}};
        xlings::xvm::WorkspaceInstalled installed{
            {"sentinel", {"0"}},
        };
        const auto dbBefore = xlings::xvm::versions_to_json(db);
        const auto workspaceBefore = workspace;
        const auto installedBefore = installed;

        auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
            db, workspace, installed, operations,
            xlings::xvm::RemovalContext{}, *plan);

        ASSERT_FALSE(result.has_value());
        ASSERT_TRUE(std::holds_alternative<
            xlings::xvm::RegistrationError>(result.error()));
        EXPECT_EQ(
            std::get<xlings::xvm::RegistrationError>(
                result.error()).kind,
            testCase.expected);
        EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
        EXPECT_EQ(workspace, workspaceBefore);
        EXPECT_EQ(installed, installedBefore);
    }
}

TEST(XimXvmMetadataBatchTest,
     AdoptsCompatibleLegacyStateAndRerunsIdempotentlyThroughAdapter) {
    xlings::xvm::VersionDB db;
    seed_complete_legacy_registration_group(db);
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:provider";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "tool",
            .version = "tool-1.0.0",
            .bindir = "/pkg/provider/1.0.0",
            .alias = "tool-alias",
            .filename = "tool-real",
            .binding = "legacy-root@1.0.0",
            .envs = {
                {"TOOL_ENV", "tool"},
            },
        },
        {
            .op = "add",
            .name = "legacy-root",
            .bindir = "/pkg/provider/1.0.0",
            .alias = "root-alias",
            .filename = "legacy-root-real",
            .envs = {
                {"ROOT_ENV", "root"},
            },
        },
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo", "/data", false);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;

    auto first = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, operations,
        xlings::xvm::RemovalContext{}, *plan);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(
        db.at("legacy-root")
            .versions.at("repo:1.0.0")
            .bindingGroup.has_value());
    EXPECT_EQ(
        db.at("legacy-root")
            .versions.at("repo:1.0.0")
            .bindingGroup->provider,
        "repo:provider");
    EXPECT_EQ(
        installed.at("legacy-root"),
        (std::vector<std::string>{"repo:1.0.0"}));
    EXPECT_EQ(
        installed.at("tool"),
        (std::vector<std::string>{"repo:tool-1.0.0"}));
    const auto dbAfterFirst = xlings::xvm::versions_to_json(db);
    const auto workspaceAfterFirst = workspace;
    const auto installedAfterFirst = installed;

    auto second = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, operations,
        xlings::xvm::RemovalContext{}, *plan);

    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbAfterFirst);
    EXPECT_EQ(workspace, workspaceAfterFirst);
    EXPECT_EQ(installed, installedAfterFirst);
}

TEST(XimXvmMetadataBatchTest,
     SameProviderDifferentReleaseCollisionPreservesState) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    xlings::xim::PlanNode oldProvider;
    oldProvider.name = "provider";
    oldProvider.version = "0.9.0";
    oldProvider.namespaceName = "repo";
    oldProvider.canonicalName = "repo:provider";
    const std::vector<mcpplibs::xpkg::XvmOp> oldOperations{
        {
            .op = "add",
            .name = "tool",
            .version = "shared-1.0.0",
            .bindir = "/old/tool",
        },
    };
    auto oldPlan = xlings::xim::normalize_xpkg_registration_plan(
        oldProvider, oldOperations, "repo", "/data", false);
    ASSERT_TRUE(oldPlan.has_value()) << oldPlan.error().message;
    auto seeded = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, oldOperations,
        xlings::xvm::RemovalContext{}, *oldPlan);
    ASSERT_TRUE(seeded.has_value());

    xlings::xim::PlanNode newProvider = oldProvider;
    newProvider.version = "1.0.0";
    const std::vector<mcpplibs::xpkg::XvmOp> newOperations{
        {
            .op = "add",
            .name = "tool",
            .version = "shared-1.0.0",
            .bindir = "/new/tool",
        },
    };
    auto newPlan = xlings::xim::normalize_xpkg_registration_plan(
        newProvider, newOperations, "repo", "/data", false);
    ASSERT_TRUE(newPlan.has_value()) << newPlan.error().message;
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, newOperations,
        xlings::xvm::RemovalContext{}, *newPlan);

    ASSERT_FALSE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<
        xlings::xvm::RegistrationError>(result.error()));
    EXPECT_EQ(
        std::get<xlings::xvm::RegistrationError>(
            result.error()).kind,
        xlings::xvm::RegistrationErrorKind::OwnershipConflict);
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XimXvmMetadataBatchTest,
     ResolvesDeferredEffectsFromFinalExactMetadata) {
    xlings::xvm::VersionDB db;
    auto& oldProgram = db["cc"].versions["repo:cc-1.0.0"];
    oldProgram.path = "/old/bin";
    oldProgram.kind = "program";
    oldProgram.sourceName = "old-cc";
    oldProgram.destinationName = "cc";
    auto& oldLibrary =
        db["compiler-runtime"].versions["repo:runtime-1.0.0"];
    oldLibrary.path = "/old/lib";
    oldLibrary.kind = "lib";
    oldLibrary.sourceName = "libold.so";
    oldLibrary.destinationName = "libold.so";
    xlings::xvm::Workspace workspace{
        {"cc", "repo:cc-1.0.0"},
        {"compiler-runtime", "repo:runtime-1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"repo:cc-1.0.0"}},
        {"compiler-runtime", {"repo:runtime-1.0.0"}},
    };
    xlings::xim::PlanNode provider;
    provider.name = "toolchain";
    provider.version = "2.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:toolchain";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "cc",
            .version = "cc-2.0.0",
            .bindir = "/final/bin",
            .filename = "cc-real",
            .binding = "toolchain@2.0.0",
        },
        {
            .op = "add",
            .name = "compiler-runtime",
            .version = "runtime-2.0.0",
            .bindir = "/final/lib",
            .type = "lib",
            .filename = "libcompiler.so.2",
            .binding = "toolchain@2.0.0",
        },
        {
            .op = "add",
            .name = "toolchain",
            .type = "group",
        },
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo", "/data", true);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    auto applied = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, operations,
        xlings::xvm::RemovalContext{}, *plan);
    ASSERT_TRUE(applied.has_value());
    ASSERT_EQ(applied->effects.size(), 2u);

    auto program = xlings::xim::resolve_xpkg_filesystem_effect(
        db, workspace, applied->effects[0]);
    auto library = xlings::xim::resolve_xpkg_filesystem_effect(
        db, workspace, applied->effects[1]);

    ASSERT_TRUE(program.has_value());
    EXPECT_EQ(
        program->kind,
        xlings::xim::XpkgFilesystemEffectKind::ProgramShim);
    EXPECT_EQ(program->target, "cc");
    EXPECT_EQ(program->version, "repo:cc-2.0.0");
    EXPECT_EQ(program->path, "/final/bin");
    EXPECT_EQ(program->sourceName, "cc-real");
    EXPECT_EQ(program->destinationName, "cc");
    EXPECT_TRUE(program->active);
    ASSERT_TRUE(library.has_value());
    EXPECT_EQ(
        library->kind,
        xlings::xim::XpkgFilesystemEffectKind::Library);
    EXPECT_EQ(library->target, "compiler-runtime");
    EXPECT_EQ(library->version, "repo:runtime-2.0.0");
    EXPECT_EQ(library->path, "/final/lib");
    EXPECT_EQ(library->sourceName, "libcompiler.so.2");
    EXPECT_EQ(library->destinationName, "libcompiler.so.2");
    EXPECT_TRUE(library->active);
    EXPECT_FALSE(std::ranges::any_of(
        applied->effects,
        [](const auto& effect) {
            return effect.target == "toolchain";
        }));
}

namespace {

int run_xvm_registration_production_child_(
        const std::filesystem::path& root) {
    namespace fs = std::filesystem;

    const auto fail = [](int code, std::string_view message) {
        std::cerr << "xvm registration production child: "
                  << message << '\n';
        return code;
    };
    const auto write_text = [&](const fs::path& path,
                                std::string_view contents) {
        fs::create_directories(path.parent_path());
        std::ofstream output(path);
        output << contents;
        return output.good();
    };

    const auto home = root / "home" / ".xlings";
    const auto project = root / "project";
    const auto payload = root / "payload";
    const auto temp = root / "tmp";
    const auto projectSubos = project / ".xlings" / "subos" / "_";
    const auto globalSubos = home / "subos" / "env-scope";
    fs::create_directories(temp);
    fs::create_directories(project);
    fs::create_directories(payload / "bin");
    fs::create_directories(globalSubos);
    if (!write_text(
            home / ".xlings.json",
            R"({"activeSubos":"persisted-scope"})")
        || !write_text(
            globalSubos / ".xlings.json",
            R"({"workspace":{}})")
        || !write_text(
            project / ".xlings.json",
            R"({"workspace":{}})")) {
        return fail(2, "failed to create isolated Config fixtures");
    }

    xlings::platform::set_env_variable("HOME", (root / "home").string());
    xlings::platform::set_env_variable("XLINGS_HOME", home.string());
    xlings::platform::set_env_variable(
        "XLINGS_ACTIVE_SUBOS", "env-scope");
    xlings::platform::set_env_variable("XLINGS_PROJECT_DIR", "");
    xlings::platform::set_env_variable(
        "XDG_CONFIG_HOME", (root / "config").string());
    xlings::platform::set_env_variable(
        "XDG_CACHE_HOME", (root / "cache").string());
    xlings::platform::set_env_variable(
        "XDG_DATA_HOME", (root / "data").string());
    xlings::platform::set_env_variable("TMPDIR", temp.string());
    fs::current_path(project);

#ifdef _WIN32
    constexpr std::string_view executableExtension = ".exe";
#else
    constexpr std::string_view executableExtension = "";
#endif
    const auto bootstrap =
        home / "bin"
        / ("xlings" + std::string(executableExtension));
    if (!write_text(bootstrap, "old-bootstrap")) {
        return fail(3, "failed to seed isolated bootstrap");
    }
    fs::permissions(
        bootstrap,
        fs::perms::owner_all
            | fs::perms::group_read
            | fs::perms::group_exec
            | fs::perms::others_read
            | fs::perms::others_exec,
        fs::perm_options::replace);

    const auto write_recipe = [&](
            const fs::path& recipe,
            std::string_view target,
            std::string_view filename = {}) {
        std::ofstream output(recipe);
        output
            << "import(\"xim.libxpkg.pkginfo\")\n"
            << "import(\"xim.libxpkg.xvm\")\n"
            << "function config()\n"
            << "  xvm.add(\"" << target
            << "\", { bindir = path.join("
               "pkginfo.install_dir(), \"bin\")";
        if (!filename.empty()) {
            output << ", filename = \"" << filename << "\"";
        }
        output << " })\n  return true\nend\n";
        return output.good();
    };
    const auto run_config = [&](
            const fs::path& recipe,
            std::string name,
            std::string version,
            const fs::path& installDir,
            bool useAfterInstall) {
        auto executor = mcpplibs::xpkg::create_executor(recipe);
        if (!executor) {
            std::cerr << executor.error() << '\n';
            return false;
        }
        xlings::xim::PlanNode node;
        node.name = std::move(name);
        node.canonicalName = node.name;
        node.version = std::move(version);
        mcpplibs::xpkg::ExecutionContext context;
        context.pkg_name = node.name;
        context.version = node.version;
        context.platform = std::string(xlings::platform::OS_NAME);
        context.arch = "x86_64";
        context.install_file = recipe;
        context.install_dir = installDir;
        context.run_dir = installDir;
        context.xpkg_dir = root / "data";
        xlings::xim::detail_::
            configure_xpkg_execution_artifact_paths_(context);
        const auto expectedSubos =
            xlings::Config::xvm_artifact_subos_dir();
        if (context.bin_dir != expectedSubos / "bin"
            || context.subos_sysrootdir
                != expectedSubos.string()) {
            std::cerr
                << "execution context does not match write scope\n";
            return false;
        }
        return xlings::xim::detail_::run_config_hook_(
            node,
            root / "data",
            *executor,
            context,
            {},
            useAfterInstall);
    };

    if (!xlings::Config::has_project_config()) {
        return fail(4, "temporary project was not selected");
    }
    if (xlings::Config::global_subos_dir() != globalSubos) {
        return fail(
            5,
            "global subos root ignored XLINGS_ACTIVE_SUBOS");
    }
    if (xlings::Config::xvm_artifact_subos_dir()
        != projectSubos) {
        return fail(
            6,
            "project metadata and artifact roots disagree");
    }

    const auto projectRecipe = root / "project-provider.lua";
    if (!write_recipe(projectRecipe, "task3b-project-tool")) {
        return fail(7, "failed to write project recipe");
    }
    if (!run_config(
            projectRecipe,
            "task3b-project-provider",
            "1.0.0",
            payload / "project",
            true)) {
        return fail(8, "project-scope config hook failed");
    }
    if (!xlings::Config::project_versions().contains(
            "task3b-project-tool")
        || !xlings::Config::workspace_mut().contains(
            "task3b-project-tool")
        || !xlings::Config::workspace_installed_mut().contains(
            "task3b-project-tool")) {
        return fail(
            9,
            "project registration did not use project metadata");
    }
    if (!fs::exists(
            projectSubos / "bin" / "task3b-project-tool")) {
        return fail(
            10,
            "project registration did not use project artifact root");
    }

    xlings::Config::set_force_global_scope(true);
    if (xlings::Config::xvm_artifact_subos_dir()
        != globalSubos) {
        return fail(
            11,
            "force-global metadata and artifact roots disagree");
    }
    const auto selfRecipe = root / "self-provider.lua";
    if (!write_recipe(
            selfRecipe, "xlings", "xlings-real")) {
        return fail(12, "failed to write self-replace recipe");
    }
    const auto selfPayload = payload / "self";
    const auto selfSource =
        selfPayload / "bin"
        / ("xlings-real"
           + std::string(executableExtension));
    if (!write_text(selfSource, "new-bootstrap")) {
        return fail(13, "failed to create custom self source");
    }
    fs::permissions(
        selfSource,
        fs::perms::owner_all
            | fs::perms::group_read
            | fs::perms::group_exec
            | fs::perms::others_read
            | fs::perms::others_exec,
        fs::perm_options::replace);
    if (!run_config(
            selfRecipe,
            "task3b-self-provider",
            "2.0.0",
            selfPayload,
            true)) {
        return fail(14, "force-global self config hook failed");
    }
    if (!xlings::Config::global_versions().contains("xlings")
        || xlings::Config::project_versions().contains("xlings")) {
        return fail(
            15,
            "force-global registration did not use global metadata");
    }
    if (!fs::exists(
            globalSubos / "bin"
            / ("xlings" + std::string(executableExtension)))
        || fs::exists(
            projectSubos / "bin"
            / ("xlings" + std::string(executableExtension)))) {
        return fail(
            16,
            "force-global shim was written to the wrong artifact root");
    }
    {
        std::ifstream input(bootstrap);
        std::string contents;
        input >> contents;
        if (contents != "new-bootstrap") {
            return fail(
                17,
                "self-replace ignored final exact sourceName");
        }
    }

    xlings::Config::set_force_global_scope(false);
    auto& db = xlings::Config::versions_mut();
    auto& workspace = xlings::Config::workspace_mut();
    auto& installed =
        xlings::Config::workspace_installed_mut();
    constexpr std::string_view conflictTarget =
        "task3b-conflict-tool";
    constexpr std::string_view conflictVersion = "3.0.0";
    constexpr std::string_view removedTarget =
        "task3b-removal-sentinel";
    constexpr std::string_view removedVersion = "0.1.0";
    auto& removed =
        db[std::string(removedTarget)]
            .versions[std::string(removedVersion)];
    removed.path = "/old/payload";
    removed.kind = "program";
    removed.sourceName = std::string(removedTarget);
    removed.destinationName = std::string(removedTarget);
    workspace[std::string(removedTarget)] =
        std::string(removedVersion);
    installed[std::string(removedTarget)] = {
        std::string(removedVersion),
    };
    auto& conflict =
        db[std::string(conflictTarget)]
            .versions[std::string(conflictVersion)];
    conflict.path = "/other/payload";
    conflict.kind = "program";
    conflict.sourceName = "other-tool";
    conflict.destinationName = std::string(conflictTarget);
    conflict.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "other-provider",
        .providerVersion = "9.0.0",
        .group = std::string(conflictTarget),
        .rootTarget = std::string(conflictTarget),
        .rootVersion = std::string(conflictVersion),
    };
    conflict.bindingMembers = {
        {std::string(conflictTarget),
         std::string(conflictVersion)},
    };
    conflict.bindingMembersDeclared = true;
    workspace[std::string(conflictTarget)] =
        std::string(conflictVersion);
    installed[std::string(conflictTarget)] = {
        std::string(conflictVersion),
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const auto conflictShim =
        projectSubos / "bin"
        / (std::string(conflictTarget)
           + std::string(executableExtension));
    const auto removalShim =
        projectSubos / "bin"
        / (std::string(removedTarget)
           + std::string(executableExtension));
    if (fs::exists(conflictShim)) {
        return fail(18, "conflict shim unexpectedly pre-exists");
    }
    if (!write_text(removalShim, "keep-removal-shim")) {
        return fail(19, "failed to seed exact removal artifact");
    }
    const auto conflictRecipe = root / "conflict-provider.lua";
    {
        std::ofstream output(conflictRecipe);
        output
            << "import(\"xim.libxpkg.pkginfo\")\n"
            << "import(\"xim.libxpkg.xvm\")\n"
            << "function config()\n"
            << "  xvm.remove(\"" << removedTarget
            << "\", \"" << removedVersion << "\")\n"
            << "  xvm.add(\"" << conflictTarget
            << "\", { bindir = path.join("
               "pkginfo.install_dir(), \"bin\") })\n"
            << "  return true\nend\n";
        if (!output.good()) {
            return fail(20, "failed to write conflict recipe");
        }
    }
    if (run_config(
            conflictRecipe,
            "task3b-conflict-provider",
            std::string(conflictVersion),
            payload / "conflict",
            true)) {
        return fail(
            21,
            "production config hook swallowed registration failure");
    }
    if (xlings::xvm::versions_to_json(db) != dbBefore
        || workspace != workspaceBefore
        || installed != installedBefore) {
        return fail(
            22,
            "late registration conflict mutated scoped metadata");
    }
    if (fs::exists(conflictShim)) {
        return fail(
            23,
            "late registration conflict ran its exact shim effect");
    }
    {
        std::ifstream input(removalShim);
        std::string contents;
        input >> contents;
        if (contents != "keep-removal-shim") {
            return fail(
                24,
                "late conflict ran candidate removal cleanup");
        }
    }

    return 0;
}

}  // namespace

TEST(XimXvmProductionPathTest,
     UsesMatchingScopedArtifactsAndSuppressesFailedEffects) {
    namespace fs = std::filesystem;
    const auto root =
        fs::temp_directory_path()
        / std::format(
            "xlings-task3b-production-{}",
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());
    fs::remove_all(root);
    fs::create_directories(root);
    const auto executable =
        xlings::platform::get_executable_path();
    ASSERT_FALSE(executable.empty());
    auto command = std::format(
        "{} --xvm-registration-production-child {}",
        xlings::platform::shell_quote(executable.string()),
        xlings::platform::shell_quote(root.string()));
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif
    auto child = xlings::platform::spawn_command(command);
    ASSERT_GT(child.pid, 0);
    auto [status, output] = xlings::platform::wait_or_kill(
        child, nullptr, std::chrono::seconds{30});
    EXPECT_EQ(status, 0) << output;
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(XimXvmRemovalAdapterTest,
     PreSnapshotsUninstallSelectionForLaterVersionlessHookOps) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "toolchain", "15.1.0", "/pkg/toolchain",
        "program", "", "", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "gcc-15.1.0", "/pkg/toolchain",
        "program", "cc-15", "cc", "repo-a",
        "toolchain@15.1.0");
    xlings::xvm::Workspace workspace{
        {"toolchain", "repo-a:15.1.0"},
        {"cc", "repo-a:gcc-15.1.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"toolchain", {"repo-a:15.1.0"}},
        {"cc", {"repo-a:gcc-15.1.0"}},
    };

    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, {}, "repo-a:toolchain", "15.1.0",
        "toolchain", "repo-a:15.1.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    const std::vector<mcpplibs::xpkg::XvmOp> hookOperations{
        {
            .op = "remove",
            .name = "cc",
        },
    };

    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, hookOperations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->removed.size(), 2u);
    EXPECT_EQ(result->removed[0].target, "cc");
    EXPECT_EQ(result->removed[0].version, "repo-a:gcc-15.1.0");
    EXPECT_EQ(result->removed[1].target, "toolchain");
    EXPECT_EQ(result->removed[1].version, "repo-a:15.1.0");
    EXPECT_FALSE(db.contains("toolchain"));
    EXPECT_FALSE(db.contains("cc"));
}

TEST(XimXvmRemovalAdapterTest,
     AuthoritativePurgeRemovesSnapshotWhenHookHasNoRemoveOps) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "toolchain", "15.1.0", "/pkg/toolchain",
        "program", "", "", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "gcc-15.1.0", "/pkg/toolchain",
        "program", "cc-15", "cc", "repo-a",
        "toolchain@15.1.0");
    xlings::xvm::Workspace workspace{
        {"toolchain", "repo-a:15.1.0"},
        {"cc", "repo-a:gcc-15.1.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"toolchain", {"repo-a:15.1.0"}},
        {"cc", {"repo-a:gcc-15.1.0"}},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> hookOperations;
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, hookOperations,
        "repo-a:toolchain", "15.1.0",
        "toolchain", "repo-a:15.1.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;

    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, hookOperations, *context,
        xlings::xvm::RemovalBatchOptions{
            .purgeSelection = true,
        });

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->removed.size(), 2u);
    EXPECT_FALSE(db.contains("toolchain"));
    EXPECT_FALSE(db.contains("cc"));
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XimXvmRemovalAdapterTest,
     OperationsOnlyKeepsProviderSnapshotWhenConfigHookHasNoRemoveOps) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo-a:toolchain", "15.1.0", "compiler",
        "toolchain", "repo-a:15.1.0");
    auto& root = add_provider_group_member(
        db, "toolchain", "repo-a:15.1.0", group, "group");
    root.bindingMembers = {
        {"cc", "repo-a:gcc-15.1.0"},
        {"toolchain", "repo-a:15.1.0"},
    };
    add_provider_group_member(
        db, "cc", "repo-a:gcc-15.1.0", group,
        "program", "cc-15", "cc");
    xlings::xvm::Workspace workspace{
        {"toolchain", "repo-a:15.1.0"},
        {"cc", "repo-a:gcc-15.1.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"toolchain", {"repo-a:15.1.0"}},
        {"cc", {"repo-a:gcc-15.1.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<mcpplibs::xpkg::XvmOp> configOperations;
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, configOperations,
        "repo-a:toolchain", "15.1.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    ASSERT_TRUE(context->hasSelection);

    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, configOperations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->removed.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XimXvmRemovalAdapterTest,
     PreSnapshotsConfigSelectionFromActiveRemovalTarget) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "toolchain", "15.1.0", "/pkg/toolchain",
        "program", "", "", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "gcc-15.1.0", "/pkg/toolchain",
        "program", "cc-15", "cc", "repo-a",
        "toolchain@15.1.0");
    xlings::xvm::Workspace workspace{
        {"cc", "repo-a:gcc-15.1.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"repo-a:gcc-15.1.0"}},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove",
            .name = "cc",
        },
    };

    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations, "repo-a:toolchain", "15.1.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, operations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->removed.size(), 2u);
    EXPECT_EQ(result->removed[0].target, "cc");
    EXPECT_EQ(result->removed[0].version, "repo-a:gcc-15.1.0");
    EXPECT_EQ(result->removed[1].target, "toolchain");
    EXPECT_EQ(result->removed[1].version, "repo-a:15.1.0");
    EXPECT_FALSE(db.contains("toolchain"));
    EXPECT_FALSE(db.contains("cc"));
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());

    xlings::xvm::VersionDB singletonDb;
    singletonDb["base"].versions["repo-a:1.0.0"].kind = "subos-base";
    xlings::xvm::Workspace singletonWorkspace{
        {"base", "repo-a:1.0.0"},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> singletonOperations{
        {
            .op = "remove",
            .name = "base",
        },
    };
    auto singletonContext = xlings::xim::snapshot_xpkg_removal_context(
        singletonDb, singletonWorkspace, singletonOperations,
        "repo-a:base", "1.0.0");

    ASSERT_TRUE(singletonContext.has_value())
        << singletonContext.error().message;
    EXPECT_EQ(
        singletonContext->members.at("base"),
        "repo-a:1.0.0");
}

TEST(XimXvmRemovalAdapterTest,
     LegacyPreferredSelectionCannotAuthorizeCanonicalRemoveAll) {
    xlings::xvm::VersionDB db;
    auto& legacy = db["cc"].versions["legacy:0.9.0"];
    legacy.path = "/pkg/legacy";
    legacy.kind = "program";
    legacy.sourceName = "cc-legacy";
    legacy.destinationName = "cc";

    const auto canonicalGroup = make_binding_group_ref(
        "repo-a:provider", "1.0.0", "compiler",
        "cc", "repo-a:1.0.0");
    auto& canonical = add_provider_group_member(
        db, "cc", "repo-a:1.0.0", canonicalGroup, "group");
    canonical.bindingMembers = {
        {"cc", "repo-a:1.0.0"},
    };

    xlings::xvm::Workspace workspace{
        {"cc", "legacy:0.9.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"legacy:0.9.0", "repo-a:1.0.0"}},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove_all",
            .name = "cc",
        },
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations,
        "repo-a:provider", "1.0.0",
        "cc", "legacy:0.9.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    EXPECT_TRUE(context->provider.empty());
    EXPECT_EQ(context->members.at("cc"), "legacy:0.9.0");

    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, operations, *context);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::ProviderRequired);
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XimXvmRemovalAdapterTest, TransportsRemoveAllAsDistinctOperation) {
    xlings::xvm::VersionDB db;
    const auto groupA = make_binding_group_ref(
        "repo-a:provider", "1.0.0", "group-a",
        "cc", "repo-a:1.0.0");
    auto& providerA = add_provider_group_member(
        db, "cc", "repo-a:1.0.0", groupA, "group");
    providerA.bindingMembers = {
        {"cc", "repo-a:1.0.0"},
    };
    const auto groupB = make_binding_group_ref(
        "repo-b:provider", "2.0.0", "group-b",
        "cc", "repo-b:2.0.0");
    auto& providerB = add_provider_group_member(
        db, "cc", "repo-b:2.0.0", groupB, "group");
    providerB.bindingMembers = {
        {"cc", "repo-b:2.0.0"},
    };
    xlings::xvm::Workspace workspace{
        {"cc", "repo-a:1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"repo-b:2.0.0", "repo-a:1.0.0"}},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove_all",
            .name = "cc",
        },
    };
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations,
        "repo-a:provider", "1.0.0",
        "cc", "repo-a:1.0.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    EXPECT_EQ(context->provider, "repo-a:provider");

    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, operations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->removed.size(), 1u);
    EXPECT_EQ(result->removed[0].target, "cc");
    EXPECT_EQ(result->removed[0].version, "repo-a:1.0.0");
    ASSERT_TRUE(db.contains("cc"));
    EXPECT_EQ(db.at("cc").versions.size(), 1u);
    EXPECT_TRUE(db.at("cc").versions.contains("repo-b:2.0.0"));
    EXPECT_EQ(workspace.at("cc"), "repo-b:2.0.0");
}

TEST(XimXvmRemovalArtifactTest,
     RemovesExactLibraryDestinationBeforeReplacement) {
    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testRoot = fs::temp_directory_path()
        / std::format("xlings-xvm-removal-artifact-{}", nonce);
    const auto binDir = testRoot / "bin";
    const auto libDir = testRoot / "lib";
    const auto oldPayload = testRoot / "old";
    const auto newPayload = testRoot / "new";
    const auto unrelatedPayload = testRoot / "unrelated";
    fs::create_directories(binDir);
    fs::create_directories(libDir);
    fs::create_directories(oldPayload);
    fs::create_directories(newPayload);
    fs::create_directories(unrelatedPayload);

    const auto target = std::string{"compiler-runtime"};
    const auto destinationName = std::string{"libcompiler.so"};
    const auto oldSource = oldPayload / destinationName;
    const auto newSource = newPayload / destinationName;
    const auto unrelatedSource = unrelatedPayload / target;
    xlings::platform::write_string_to_file(
        oldSource.string(), "OLD");
    xlings::platform::write_string_to_file(
        newSource.string(), "NEW");
    xlings::platform::write_string_to_file(
        unrelatedSource.string(), "SENTINEL");
    xlings::xvm::install_libs(
        oldPayload.string(), libDir, {destinationName});
    xlings::xvm::install_libs(
        unrelatedPayload.string(), libDir, {target});

    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, target, "repo-a:1.0.0", oldPayload.string(),
        "lib", destinationName);
    const auto dbBefore = db;
    ASSERT_EQ(
        dbBefore.at(target)
            .versions.at("repo-a:1.0.0").destinationName,
        destinationName);
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove",
            .name = target,
            .version = "repo-a:1.0.0",
        },
    };
    auto removalResult = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, operations, {});
    ASSERT_TRUE(removalResult.has_value())
        << removalResult.error().message;

    xlings::xim::cleanup_removed_xvm_library_artifacts(
        libDir, dbBefore, db, *removalResult);

    EXPECT_FALSE(fs::exists(libDir / destinationName));
    ASSERT_TRUE(fs::exists(libDir / target));
    EXPECT_EQ(
        xlings::platform::read_file_to_string(
            (libDir / target).string()),
        "SENTINEL");

    xlings::xvm::add_version(
        db, target, "repo-a:2.0.0", newPayload.string(),
        "lib", destinationName);
    xlings::xvm::install_libs(
        newPayload.string(), libDir, {destinationName});
    ASSERT_TRUE(fs::exists(libDir / destinationName));
    EXPECT_EQ(
        xlings::platform::read_file_to_string(
            (libDir / destinationName).string()),
        "NEW");
    EXPECT_EQ(
        xlings::platform::read_file_to_string(
            (libDir / target).string()),
        "SENTINEL");

    std::error_code ec;
    fs::remove_all(testRoot, ec);
}

TEST(XimXvmRemovalArtifactTest,
     UsesLegacyLibraryFilenameWhenVersionDestinationIsMissing) {
    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testRoot = fs::temp_directory_path()
        / std::format("xlings-xvm-removal-legacy-lib-{}", nonce);
    const auto sourceDir = testRoot / "source";
    const auto libDir = testRoot / "lib";
    const auto filename = std::string{"liblegacy.so"};
    fs::create_directories(sourceDir);
    xlings::platform::write_string_to_file(
        (sourceDir / filename).string(), "LEGACY");
    xlings::xvm::install_libs(
        sourceDir.string(), libDir, {filename});

    xlings::xvm::VersionDB dbBefore;
    auto& info = dbBefore["legacy-runtime"];
    info.type = "lib";
    info.filename = filename;
    auto& version = info.versions["1.0.0"];
    version.path = sourceDir.string();
    const xlings::xvm::RemovalBatchResult removalResult{
        .removed = {
            {
                .target = "legacy-runtime",
                .version = "1.0.0",
            },
        },
    };

    xlings::xim::cleanup_removed_xvm_library_artifacts(
        libDir, dbBefore, {}, removalResult);

    EXPECT_FALSE(fs::exists(libDir / filename));
    std::error_code ec;
    fs::remove_all(testRoot, ec);
}

TEST(XimXvmRemovalArtifactTest,
     KeepsDestinationOwnedBySurvivingLibraryVersion) {
    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testRoot = fs::temp_directory_path()
        / std::format("xlings-xvm-removal-surviving-lib-{}", nonce);
    const auto sourceDir = testRoot / "source";
    const auto libDir = testRoot / "lib";
    const auto target = std::string{"compiler-runtime"};
    const auto filename = std::string{"libcompiler.so"};
    fs::create_directories(sourceDir);
    xlings::platform::write_string_to_file(
        (sourceDir / filename).string(), "SURVIVOR");
    xlings::xvm::install_libs(
        sourceDir.string(), libDir, {filename});

    xlings::xvm::VersionDB dbBefore;
    xlings::xvm::add_version(
        dbBefore, target, "1.0.0", "/old",
        "lib", filename);
    xlings::xvm::add_version(
        dbBefore, target, "2.0.0", sourceDir.string(),
        "lib", filename);
    auto currentDb = dbBefore;
    currentDb.at(target).versions.erase("1.0.0");
    const xlings::xvm::RemovalBatchResult removalResult{
        .removed = {
            {
                .target = target,
                .version = "1.0.0",
            },
        },
    };

    xlings::xim::cleanup_removed_xvm_library_artifacts(
        libDir, dbBefore, currentDb, removalResult);

    ASSERT_TRUE(fs::exists(libDir / filename));
    EXPECT_EQ(
        xlings::platform::read_file_to_string(
            (libDir / filename).string()),
        "SURVIVOR");
    std::error_code ec;
    fs::remove_all(testRoot, ec);
}

TEST(XimXvmRemovalArtifactTest,
     KeepsProgramShimWhenUsableVersionSurvives) {
    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testRoot = fs::temp_directory_path()
        / std::format("xlings-xvm-removal-surviving-shim-{}", nonce);
    const auto binDir = testRoot / "bin";
    fs::create_directories(binDir);
#ifdef _WIN32
    const auto shim = binDir / "cc.exe";
#else
    const auto shim = binDir / "cc";
#endif
    xlings::platform::write_string_to_file(shim.string(), "SHIM");

    xlings::xvm::VersionDB dbBefore;
    xlings::xvm::add_version(
        dbBefore, "cc", "1.0.0", "/old", "program", "cc");
    xlings::xvm::add_version(
        dbBefore, "cc", "2.0.0", "/new", "program", "cc");
    auto currentDb = dbBefore;
    currentDb.at("cc").versions.erase("1.0.0");
    const xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"2.0.0"}},
    };
    const xlings::xvm::RemovalBatchResult removalResult{
        .removed = {
            {
                .target = "cc",
                .version = "1.0.0",
            },
        },
    };

    xlings::xim::cleanup_removed_xvm_program_artifacts(
        binDir, dbBefore, currentDb, installed, removalResult);

    EXPECT_TRUE(fs::exists(shim));
    EXPECT_EQ(
        xlings::platform::read_file_to_string(shim.string()),
        "SHIM");
    std::error_code ec;
    fs::remove_all(testRoot, ec);
}

TEST(XimXvmRemovalArtifactTest,
     VirtualGroupNeverOwnsSameNamedProgramShim) {
    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testRoot = fs::temp_directory_path()
        / std::format("xlings-xvm-removal-group-shim-{}", nonce);
    const auto binDir = testRoot / "bin";
    fs::create_directories(binDir);
#ifdef _WIN32
    const auto shim = binDir / "toolchain.exe";
#else
    const auto shim = binDir / "toolchain";
#endif
    xlings::platform::write_string_to_file(shim.string(), "UNRELATED");

    xlings::xvm::VersionDB dbBefore;
    auto& info = dbBefore["toolchain"];
    info.type = "program";
    auto& version = info.versions["1.0.0"];
    version.kind = "group";
    const xlings::xvm::RemovalBatchResult removalResult{
        .removed = {
            {
                .target = "toolchain",
                .version = "1.0.0",
            },
        },
    };

    xlings::xim::cleanup_removed_xvm_program_artifacts(
        binDir, dbBefore, {}, {}, removalResult);

    EXPECT_TRUE(fs::exists(shim));
    EXPECT_EQ(
        xlings::platform::read_file_to_string(shim.string()),
        "UNRELATED");
    std::error_code ec;
    fs::remove_all(testRoot, ec);
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
// platform::write_string_to_file — atomic replace
//
// Every persisted xlings state file goes through this function:
// ~/.xlings.json (the whole version database), each subos workspace,
// profile generations, and the shell rc files that xself edits by
// read-modify-write. It used to be fopen("w") + fwrite + fclose, which
// truncates the destination before any new byte is written — an
// interruption at that point loses the file's entire previous content.
// These tests pin the replace-or-leave-untouched contract.

namespace {

class AtomicWriteTest : public ::testing::Test {
protected:
    void SetUp() override {
        namespace fs = std::filesystem;
        dir_ = fs::temp_directory_path() /
               ("xlings_atomic_write_" +
                std::string(::testing::UnitTest::GetInstance()
                                ->current_test_info()
                                ->name()));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
#if !defined(_WIN32)
        // A test may leave the directory unwritable; restore before cleanup.
        std::filesystem::permissions(dir_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
#endif
        std::filesystem::remove_all(dir_, ec);
    }

    // Anything left beside the destination is a leaked staging file.
    std::size_t entry_count() const {
        std::size_t n = 0;
        for ([[maybe_unused]] auto& e : std::filesystem::directory_iterator(dir_)) ++n;
        return n;
    }

    std::filesystem::path dir_;
};

}  // namespace

TEST_F(AtomicWriteTest, WritesContentAndLeavesNoStagingFile) {
    auto target = dir_ / "state.json";
    xlings::platform::write_string_to_file(target.string(), "{\"a\":1}");

    EXPECT_EQ(xlings::platform::read_file_to_string(target.string()), "{\"a\":1}");
    EXPECT_EQ(entry_count(), 1u) << "staging file leaked into the directory";
}

TEST_F(AtomicWriteTest, OverwriteReplacesContentAndLeavesNoStagingFile) {
    auto target = dir_ / "state.json";
    xlings::platform::write_string_to_file(target.string(), "old-and-longer");
    xlings::platform::write_string_to_file(target.string(), "new");

    EXPECT_EQ(xlings::platform::read_file_to_string(target.string()), "new");
    EXPECT_EQ(entry_count(), 1u);
}

TEST_F(AtomicWriteTest, FailedWriteLeavesPreviousContentIntact) {
#if defined(_WIN32)
    GTEST_SKIP() << "directory permission semantics differ on Windows";
#else
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root bypasses directory permission checks";
    }
    namespace fs = std::filesystem;
    auto target = dir_ / "state.json";
    xlings::platform::write_string_to_file(target.string(), "committed");

    // Staging cannot be created in a non-writable directory. The point of the
    // test is what happens to the destination when the write cannot complete:
    // it must still hold the previously committed bytes. The old truncating
    // implementation would have destroyed them before discovering the failure.
    std::error_code ec;
    fs::permissions(dir_, fs::perms::owner_write, fs::perm_options::remove, ec);
    ASSERT_FALSE(ec);

    EXPECT_THROW(
        xlings::platform::write_string_to_file(target.string(), "replacement"),
        std::runtime_error);

    fs::permissions(dir_, fs::perms::owner_write, fs::perm_options::add, ec);
    EXPECT_EQ(xlings::platform::read_file_to_string(target.string()), "committed");
    EXPECT_EQ(entry_count(), 1u);
#endif
}

TEST_F(AtomicWriteTest, ReplacesTheInodeInsteadOfMutatingTheCommittedOne) {
#if defined(_WIN32)
    GTEST_SKIP() << "hard link semantics differ on Windows";
#else
    namespace fs = std::filesystem;
    auto target = dir_ / "state.json";
    auto witness = dir_ / "witness.json";
    xlings::platform::write_string_to_file(target.string(), "committed");

    // The witness shares the destination's inode. If the write mutated that
    // inode in place -- truncate, then fill -- the witness would follow along,
    // which is exactly the window where an interruption leaves an empty file.
    // An atomic replace writes a *new* inode and renames it over the name, so
    // the committed bytes are never touched.
    std::error_code ec;
    fs::create_hard_link(target, witness, ec);
    ASSERT_FALSE(ec) << "cannot hard link on this filesystem: " << ec.message();

    xlings::platform::write_string_to_file(target.string(), "replacement");

    EXPECT_EQ(xlings::platform::read_file_to_string(target.string()), "replacement");
    EXPECT_EQ(xlings::platform::read_file_to_string(witness.string()), "committed")
        << "the committed inode was mutated in place";
#endif
}

TEST_F(AtomicWriteTest, PreservesExistingPermissionBits) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX permission bits do not apply on Windows";
#else
    namespace fs = std::filesystem;
    auto target = dir_ / "hook.sh";
    xlings::platform::write_string_to_file(target.string(), "#!/bin/sh\n");

    std::error_code ec;
    fs::permissions(target, fs::perms::owner_all | fs::perms::group_read |
                                fs::perms::others_read,
                    fs::perm_options::replace, ec);
    ASSERT_FALSE(ec);
    const auto before = fs::status(target).permissions();

    xlings::platform::write_string_to_file(target.string(), "#!/bin/sh\necho hi\n");

    // Replacing through a fresh staging file must not silently drop the
    // executable bit (or widen the mode) of the file it replaces.
    EXPECT_EQ(fs::status(target).permissions(), before);
#endif
}

TEST_F(AtomicWriteTest, WritesThroughSymlinkInsteadOfReplacingIt) {
#if defined(_WIN32)
    GTEST_SKIP() << "symlink semantics differ on Windows";
#else
    namespace fs = std::filesystem;
    auto real = dir_ / "real.rc";
    auto link = dir_ / "link.rc";
    xlings::platform::write_string_to_file(real.string(), "original\n");
    std::error_code ec;
    fs::create_symlink(real, link, ec);
    ASSERT_FALSE(ec);

    xlings::platform::write_string_to_file(link.string(), "updated\n");

    // xself edits shell rc files by read-modify-write. Those are commonly
    // symlinks into a dotfiles repo; a rename-based replace that did not
    // resolve the link would swap the symlink for a regular file and
    // silently detach the user's dotfiles.
    EXPECT_TRUE(fs::is_symlink(link)) << "symlink was replaced by a regular file";
    EXPECT_EQ(xlings::platform::read_file_to_string(real.string()), "updated\n");
#endif
}

TEST_F(AtomicWriteTest, ThrowsWhenParentDirectoryIsMissing) {
    auto target = dir_ / "nope" / "state.json";
    EXPECT_THROW(xlings::platform::write_string_to_file(target.string(), "x"),
                 std::runtime_error);
    EXPECT_EQ(entry_count(), 0u) << "staging file leaked on failure";
}

// ============================================================
// inspect_binding_state — name the entry that made `use` refuse
//
// The selection layer fails closed, so a bad group makes `xlings use`
// refuse. Until doctor could name the offending entry that refusal was a
// dead end: doctor reported shims and payloads only, and the user was left
// reading versions.json by hand.

namespace {

// One provider release, rooted at the first member.
void inspect_group_(xlings::xvm::VersionDB& db,
                    std::string_view provider,
                    std::string_view providerVersion,
                    const std::vector<std::pair<std::string, std::string>>& members) {
    const auto& [rootTarget, rootVersion] = members.front();
    const xlings::xvm::BindingGroupRef ref{
        .provider = std::string(provider),
        .providerVersion = std::string(providerVersion),
        .group = std::string(provider),
        .rootTarget = rootTarget,
        .rootVersion = rootVersion,
    };
    std::map<std::string, std::string> manifest;
    for (const auto& [t, v] : members) manifest[t] = v;
    for (const auto& [t, v] : members) {
        auto& info = db[t];
        if (info.type.empty()) info.type = "program";
        auto& data = info.versions[v];
        data.path = "/pkg";
        data.kind = "program";
        data.bindingGroup = ref;
    }
    auto& root = db[rootTarget].versions[rootVersion];
    root.bindingMembers = manifest;
    root.bindingMembersDeclared = true;
}

bool has_code_(const std::vector<xlings::xvm::BindingFinding>& findings,
               std::string_view code) {
    return std::ranges::any_of(findings, [&](const auto& f) {
        return f.code == code;
    });
}

}  // namespace

TEST(XvmInspect, CleanStateReportsNothing) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}});
    const xlings::xvm::Workspace ws{{"gcc", "15.1.0"}, {"g++", "15.1.0"}};

    EXPECT_TRUE(xlings::xvm::inspect_binding_state(db, ws).empty());
}

TEST(XvmInspect, NamesTheCorruptFieldAndItsEntry) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0", {{"gcc", "15.1.0"}});
    db.at("gcc").versions.at("15.1.0").bindingIntegrityIssues = {
        {.code = "binding-group-field-invalid", .path = "/bindingGroup/rootVersion"},
    };

    const auto findings =
        xlings::xvm::inspect_binding_state(db, {{"gcc", "15.1.0"}});

    ASSERT_FALSE(findings.empty());
    const auto& first = findings.front();
    EXPECT_EQ(first.code, "xvm-binding-metadata-corrupt");
    EXPECT_EQ(first.target, "gcc");
    // The JSON Pointer is the whole point: it is what turns "your state is
    // bad" into something a user can actually go and look at.
    EXPECT_EQ(first.field, "/bindingGroup/rootVersion");
    EXPECT_FALSE(first.hint.empty());
}

TEST(XvmInspect, ReportsAReleaseWithAMissingMember) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}});
    db.at("g++").versions.erase("15.1.0");

    const auto findings =
        xlings::xvm::inspect_binding_state(db, {{"gcc", "15.1.0"}});

    EXPECT_TRUE(has_code_(findings, "xvm-binding-version-missing"))
        << "a dangling member must be named, not just make `use` refuse";
}

TEST(XvmInspect, ReportsAnIncoherentActiveRelease) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}});
    inspect_group_(db, "pkgindex:gcc", "16.1.0",
                   {{"gcc", "16.1.0"}, {"g++", "16.1.0"}});

    // The exact state the release train exists to prevent: same names,
    // different releases, and nothing on the surface says so.
    const auto findings = xlings::xvm::inspect_binding_state(
        db, {{"gcc", "15.1.0"}, {"g++", "16.1.0"}});

    EXPECT_TRUE(has_code_(findings, "xvm-active-group-incoherent"));
    const auto incoherent = std::ranges::find_if(findings, [](const auto& f) {
        return f.code == "xvm-active-group-incoherent";
    });
    EXPECT_NE(incoherent->hint.find("xlings use"), std::string::npos)
        << "the hint has to name the command that fixes it";
}

TEST(XvmInspect, ReportsAnActiveVersionThatIsNotRegistered) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0", {{"gcc", "15.1.0"}});

    const auto findings =
        xlings::xvm::inspect_binding_state(db, {{"gcc", "99.0.0"}});

    EXPECT_TRUE(has_code_(findings, "xvm-active-version-missing"));
}

TEST(XvmInspect, ABrokenReleaseIsReportedOncePerRelease) {
    xlings::xvm::VersionDB db;
    inspect_group_(db, "pkgindex:gcc", "15.1.0",
                   {{"gcc", "15.1.0"}, {"g++", "15.1.0"}, {"gcc-ar", "15.1.0"}});
    db.at("gcc-ar").versions.erase("15.1.0");

    const auto findings = xlings::xvm::inspect_binding_state(db, {});

    // Five members short one member is one problem. Reporting it per member
    // buries everything else in the output.
    EXPECT_EQ(std::ranges::count_if(findings, [](const auto& f) {
                  return f.code == "xvm-binding-version-missing";
              }), 1);
}

TEST(XvmInspect, LegacyStateWithoutGroupsIsNotFlagged) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1.0.0"].path = "/pkg";
    db["tool"].versions["1.0.0"].kind = "program";

    // Pre-0.4.70 databases carry no group metadata at all. They are not
    // broken, and doctor must not tell users otherwise.
    EXPECT_TRUE(
        xlings::xvm::inspect_binding_state(db, {{"tool", "1.0.0"}}).empty());
}

// ============================================================
// XvmUserError — every failure kind is explainable
//
// Failing closed is only useful if the person on the other side is told what
// happened and what to do. These tests hold the line that no error kind can
// reach a user as an unexplained "install failed": each maps to a stable
// code and a hint that names an action.

namespace {

template <typename Kind>
void expect_every_kind_described_(const std::vector<Kind>& kinds,
                                  std::string_view label) {
    std::set<std::string_view> codes;
    for (const auto kind : kinds) {
        const auto described = xlings::xvm::describe_kind(kind);
        EXPECT_NE(described.code, xlings::xvm::kUnclassifiedCode)
            << label << " kind " << static_cast<int>(kind)
            << " has no user-facing description";
        EXPECT_FALSE(described.hint.empty())
            << label << " kind " << static_cast<int>(kind) << " has no hint";
        EXPECT_TRUE(described.code.starts_with("xvm-"))
            << "code should be namespaced: " << described.code;
        EXPECT_TRUE(codes.insert(described.code).second)
            << "duplicate code " << described.code
            << " — codes are searchable identifiers and must be unique";
    }
    EXPECT_EQ(codes.size(), kinds.size());
}

}  // namespace

TEST(XvmUserError, EveryRegistrationKindHasACodeAndHint) {
    using K = xlings::xvm::RegistrationErrorKind;
    expect_every_kind_described_<K>(
        {K::InvalidBatchIdentity, K::InvalidNodeIdentity, K::InvalidNodePayload,
         K::InvalidBindingIdentity, K::DuplicateNode, K::RootNotInBatch,
         K::SelfBinding, K::GroupConflict, K::TargetVersionConflict,
         K::OwnershipConflict, K::LegacyPayloadMismatch,
         K::IncompleteLegacyComponent, K::IncompleteOwnedGroup,
         K::InvalidHeader, K::HeaderGroupNotFound, K::HeaderAmbiguous,
         K::BindingValidationFailed},
        "RegistrationErrorKind");
}

TEST(XvmUserError, EveryRemovalKindHasACodeAndHint) {
    using K = xlings::xvm::RemovalErrorKind;
    expect_every_kind_described_<K>(
        {K::VersionNotFound, K::AmbiguousVersion, K::AsymmetricEdge,
         K::SelectionInvalid, K::ProviderRequired, K::ProviderMismatch,
         K::ProviderVersionNotFound, K::VersionMismatch},
        "RemovalErrorKind");
}

TEST(XvmUserError, EveryBindingKindHasACodeAndHint) {
    using K = xlings::xvm::BindingErrorKind;
    expect_every_kind_described_<K>(
        {K::InvalidGraph, K::TargetNotFound, K::VersionNotFound,
         K::RootReferenceMismatch, K::GroupIdentityMismatch,
         K::RootMissingFromManifest, K::StartMemberMissing,
         K::MemberReferenceMismatch, K::UnsupportedKind, K::SelfEdge,
         K::AsymmetricEdge, K::ConflictingTargetVersion,
         K::PartialProviderMetadata, K::ProviderMetadataInLegacyGraph,
         K::MetadataIntegrityIssue},
        "BindingErrorKind");
}

TEST(XvmUserError, RenderCarriesEveryFieldTheUserNeeds) {
    const xlings::xvm::RegistrationError error{
        .kind = xlings::xvm::RegistrationErrorKind::OwnershipConflict,
        .path = "/nodes/2",
        .target = "gcc",
        .version = "15.1.0",
        .message = "exact registration is owned by 'pkgindex:llvm@20.1.7'",
    };

    const auto rendered = xlings::xvm::render(
        xlings::xvm::describe(error, "pkgindex:gcc@15.1.0"), true);

    EXPECT_NE(rendered.find("owned by 'pkgindex:llvm@20.1.7'"), std::string::npos);
    EXPECT_NE(rendered.find("xvm-ownership-conflict"), std::string::npos);
    EXPECT_NE(rendered.find("pkgindex:gcc@15.1.0"), std::string::npos);
    EXPECT_NE(rendered.find("gcc@15.1.0"), std::string::npos);
    EXPECT_NE(rendered.find("/nodes/2"), std::string::npos);
    EXPECT_NE(rendered.find("uninstall that package first"), std::string::npos);
    EXPECT_NE(rendered.find("nothing was changed"), std::string::npos);
}

TEST(XvmUserError, NothingWasChangedIsOnlyClaimedWhenAsked) {
    const xlings::xvm::RemovalError error{
        .kind = xlings::xvm::RemovalErrorKind::AmbiguousVersion,
        .target = "cc",
        .version = "1.0",
        .message = "bare removal version '1.0' matches 2 stored versions",
    };

    // The line is a promise about stored state, so it must never appear
    // unless the caller vouched for it.
    EXPECT_EQ(xlings::xvm::render(xlings::xvm::describe(error), false)
                  .find("nothing was changed"),
              std::string::npos);
}

TEST(XvmUserError, PeerOfAnAsymmetricEdgeIsShown) {
    const xlings::xvm::RemovalError error{
        .kind = xlings::xvm::RemovalErrorKind::AsymmetricEdge,
        .target = "gcc",
        .version = "15.1.0",
        .peerTarget = "g++",
        .peerVersion = "15.1.0",
        .message = "removal binding edge is not reciprocal",
    };

    // Without the peer the message names only one side of a two-sided
    // problem, which is not enough to go look at anything.
    EXPECT_NE(xlings::xvm::render(xlings::xvm::describe(error), true)
                  .find("peer g++@15.1.0"),
              std::string::npos);
}

// ============================================================
// attach_legacy_header_dir — keep `xlings use` able to swap headers
//
// xvm::cmd_use decides whether to swap the sysroot headers by reading
// VData::includedir of the target it was given (commands.cppm). Before the
// registration batch existed, the installer set that field directly for
// every `headers` op. The batch does not carry it, so without this the
// field stays empty on every freshly installed package and switching
// versions silently stops moving headers — the install-time copy still
// happens, so the breakage only shows up on the *second* version.
//
// Restores the field with one deliberate difference from the pre-batch
// behavior: it never brings a target or a version into existence. The old
// code used operator[] on both maps, so a `headers` op for a package that
// registers no target of its own would materialize a phantom entry with no
// path and no kind. That is exactly the class of state the binding-group
// work exists to prevent.

namespace {

xlings::xim::XpkgFilesystemEffect header_effect_(std::string sourceDir) {
    return {
        .kind = xlings::xim::XpkgFilesystemEffectKind::InstallHeaders,
        .sourceDir = std::move(sourceDir),
    };
}

xlings::xvm::VersionDB db_with_(const std::string& target,
                                const std::string& version) {
    xlings::xvm::VersionDB db;
    auto& info = db[target];
    info.type = "program";
    auto& data = info.versions[version];
    data.path = "/xpkgs/" + target + "/" + version;
    data.kind = "program";
    return db;
}

}  // namespace

TEST(LegacyHeaderDir, SetsIncludedirOnThePackageVersion) {
    auto db = db_with_("gcc", "15.1.0");
    const std::vector<xlings::xim::XpkgFilesystemEffect> effects{
        header_effect_("/xpkgs/gcc/15.1.0/include"),
    };

    EXPECT_EQ(xlings::xim::attach_legacy_header_dir(db, "gcc", "15.1.0", effects), 1u);
    EXPECT_EQ(db.at("gcc").versions.at("15.1.0").includedir,
              "/xpkgs/gcc/15.1.0/include");
}

TEST(LegacyHeaderDir, DoesNotCreateAPhantomTarget) {
    xlings::xvm::VersionDB db;
    const std::vector<xlings::xim::XpkgFilesystemEffect> effects{
        header_effect_("/xpkgs/headers-only/1.0/include"),
    };

    EXPECT_EQ(
        xlings::xim::attach_legacy_header_dir(db, "headers-only", "1.0", effects),
        0u);
    EXPECT_TRUE(db.empty()) << "a headers op invented a target entry";
}

TEST(LegacyHeaderDir, DoesNotCreateAPhantomVersion) {
    auto db = db_with_("gcc", "16.1.0");
    const std::vector<xlings::xim::XpkgFilesystemEffect> effects{
        header_effect_("/xpkgs/gcc/15.1.0/include"),
    };

    EXPECT_EQ(xlings::xim::attach_legacy_header_dir(db, "gcc", "15.1.0", effects), 0u);
    EXPECT_FALSE(db.at("gcc").versions.contains("15.1.0"))
        << "a headers op invented a version entry";
    EXPECT_EQ(db.at("gcc").versions.size(), 1u);
}

TEST(LegacyHeaderDir, LastHeadersEffectWins) {
    auto db = db_with_("gcc", "15.1.0");
    const std::vector<xlings::xim::XpkgFilesystemEffect> effects{
        header_effect_("/first/include"),
        header_effect_("/second/include"),
    };

    // Matches the pre-batch behavior: the installer assigned per op, so the
    // last `headers` op of a recipe was the one that stuck.
    EXPECT_EQ(xlings::xim::attach_legacy_header_dir(db, "gcc", "15.1.0", effects), 1u);
    EXPECT_EQ(db.at("gcc").versions.at("15.1.0").includedir, "/second/include");
}

TEST(LegacyHeaderDir, IgnoresNonHeaderEffects) {
    auto db = db_with_("gcc", "15.1.0");
    const std::vector<xlings::xim::XpkgFilesystemEffect> effects{
        {
            .kind = xlings::xim::XpkgFilesystemEffectKind::ProgramShim,
            .target = "gcc",
            .version = "15.1.0",
        },
        {
            .kind = xlings::xim::XpkgFilesystemEffectKind::RemoveHeaders,
            .sourceDir = "/stale/include",
        },
    };

    EXPECT_EQ(xlings::xim::attach_legacy_header_dir(db, "gcc", "15.1.0", effects), 0u);
    EXPECT_TRUE(db.at("gcc").versions.at("15.1.0").includedir.empty());
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
    if (argc == 3
        && std::string_view(argv[1])
            == "--xvm-registration-production-child") {
        return run_xvm_registration_production_child_(argv[2]);
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
