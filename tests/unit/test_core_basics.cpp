// tests/unit/test_core_basics.cpp — i18n, log, utils, cmdline and UI — leaf helpers with no xim/xvm state.
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
import xlings.i18n;
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
// i18n tests
// ============================================================

TEST(I18nTest, SetAndGetLanguageEn) {
    xlings::i18n::set_language("en");
    EXPECT_EQ(xlings::i18n::language(), "en");
}

TEST(I18nTest, SetAndGetLanguageZh) {
    xlings::i18n::set_language("zh");
    EXPECT_EQ(xlings::i18n::language(), "zh");
}

TEST(I18nTest, AutoMeansFollowTheSystem) {
    // "" and "auto" both un-pin. Spelled as two cases because `--lang auto` is
    // how a user goes back to following the system after pinning, and an
    // implementation that only cleared on "" would silently keep the pin.
    for (auto* v : { "", "auto" }) {
        xlings::i18n::set_language(v);
        const auto resolved = xlings::i18n::language();
        EXPECT_FALSE(resolved.empty()) << v;
        // Resolves to a language that HAS a catalogue -- never to a tag we
        // cannot serve, which would make every lookup fall through to English
        // while claiming to be something else.
        EXPECT_TRUE(resolved == "en" || resolved == "zh") << resolved;
    }
}

TEST(I18nTest, ChineseOverridesEnglish) {
    xlings::i18n::set_language("en");
    const auto en = xlings::i18n::tr("help.usage");
    xlings::i18n::set_language("zh");
    const auto zh = xlings::i18n::tr("help.usage");
    EXPECT_EQ(en, "USAGE");
    EXPECT_NE(zh, en) << "zh did not override a key it defines";
}

TEST(I18nTest, MissingChineseKeyFallsBackToEnglishPerKey) {
    // The overlay is per KEY, not per file: a language that translates some of
    // the catalogue must still render the rest, or landing a translation
    // incrementally would blank out everything not yet done.
    xlings::i18n::set_language("zh");
    // `common.builtin` is in en; whether zh has it or not, the result must be
    // non-empty and must not be the raw key.
    const auto v = xlings::i18n::tr("common.builtin");
    EXPECT_FALSE(v.empty());
    EXPECT_NE(v, "common.builtin");
}

TEST(I18nTest, EnglishIsComplete) {
    // English is the base and the last stop before a key renders as itself, so
    // an empty entry here would put a blank where a label belongs.
    xlings::i18n::set_language("en");
    for (const auto& e : xlings::i18n::english_catalogue()) {
        EXPECT_FALSE(e.key.empty());
        EXPECT_FALSE(e.text.empty()) << e.key;
        EXPECT_EQ(xlings::i18n::tr(e.key), e.text) << e.key;
    }
}

TEST(I18nTest, UnknownKeyRendersAsItselfNotAsBlank) {
    // A key nobody translated has to be visible and greppable. Returning ""
    // would silently delete a label and leave an empty column, which is the
    // failure mode this whole release is about.
    xlings::i18n::set_language("en");
    EXPECT_EQ(xlings::i18n::tr("nobody.defined.this"), "nobody.defined.this");
}

TEST(I18nTest, TranslateFormat) {
    xlings::i18n::set_language("en");
    const auto msg = xlings::i18n::trf("{} installed", std::string("gcc@15.1.0"));
    EXPECT_NE(msg.find("gcc@15.1.0"), std::string::npos);
    EXPECT_EQ(msg.find("{}"), std::string::npos);
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
// display_path — `@xlings` in place of the home prefix
//
// Output is full of these: doctor lists payload and shim paths, config
// prints the layout, install and error hints quote destinations. An
// absolute home path is noise, and in a pasted log it is the user's
// account name.
//
// Display only. `${XLINGS_HOME}` stays the storage placeholder in the
// version database; this one is never read back.
// ============================================================

TEST(DisplayPath, LeavesAPathOutsideTheHomeAlone) {
    // Substituting a prefix that does not apply would misstate where the
    // file is -- worse than the verbosity it saves. "Unchanged" includes the
    // separators: an earlier version normalized them and turned /usr/lib
    // into \usr\lib on Windows.
    const auto outside = std::filesystem::path("/usr/lib/libc.so.6");
    EXPECT_EQ(xlings::Config::display_path(outside), outside.string());
}

TEST(DisplayPath, DoesNotMatchOnAPartialComponent) {
    // A sibling directory whose name merely starts with the home's is not
    // inside it: ".xlings-backup" must not become "@xlings-backup".
    const auto& home = xlings::Config::paths().homeDir;
    if (home.empty()) GTEST_SKIP() << "no home resolved in this environment";
    const auto sibling =
        std::filesystem::path(home.string() + "-backup") / "data";
    EXPECT_EQ(xlings::Config::display_path(sibling), sibling.string());
}

TEST(DisplayPath, RewritesTheHomeAndItsChildren) {
    const auto& home = xlings::Config::paths().homeDir;
    if (home.empty()) GTEST_SKIP() << "no home resolved in this environment";
    EXPECT_EQ(xlings::Config::display_path(home), "@xlings");
    // Build the expectation as a path: a literal "@xlings/data/xpkgs" is a
    // Linux-only assertion, which is how the first version of these tests
    // failed on Windows.
    EXPECT_EQ(xlings::Config::display_path(home / "data" / "xpkgs"),
              (std::filesystem::path("@xlings") / "data" / "xpkgs").string());
}

TEST(DisplayPath, NormalizesBeforeComparing) {
    const auto& home = xlings::Config::paths().homeDir;
    if (home.empty()) GTEST_SKIP() << "no home resolved in this environment";
    // A path that reaches the same place by a longer route still belongs to
    // the home; saying otherwise would be inconsistent output for one place.
    EXPECT_EQ(xlings::Config::display_path(home / "data" / ".." / "data"),
              (std::filesystem::path("@xlings") / "data").string());
}
