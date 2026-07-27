// tests/unit/test_xpkg_spec.cpp — the xpackage spec gate.
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
// xpackage spec gate
//
// The defect these cover: `spec` used to be compared only against the
// literal "2", so every other value -- including a future revision -- fell
// through to V1 semantics silently. For spec "2" that meant skipping the
// fail-closed arch gate, i.e. installing the wrong architecture without a
// word, which is worse than refusing.

TEST(XpkgSpecGate, AbsentSpecIsV1AndSupported) {
    const auto support = xlings::xim::xpkg_spec_support("");
    EXPECT_TRUE(support.supported);
    EXPECT_EQ(support.declared, 1);
}

TEST(XpkgSpecGate, ImplementedRevisionsAreSupported) {
    for (const auto* spec : {"1", "2"}) {
        const auto support = xlings::xim::xpkg_spec_support(spec);
        EXPECT_TRUE(support.supported) << "spec " << spec;
    }
    EXPECT_EQ(xlings::xim::xpkg_spec_support("2").declared, 2);
}

TEST(XpkgSpecGate, NewerRevisionIsRefusedNotDowngraded) {
    const auto support = xlings::xim::xpkg_spec_support("3");
    EXPECT_FALSE(support.supported);
    // Reported, not zero: the caller distinguishes "too new, go update" from
    // "we could not read this at all".
    EXPECT_EQ(support.declared, 3);
}

TEST(XpkgSpecGate, UnreadableSpecIsRefusedRatherThanAssumed) {
    // A value we cannot parse is exactly the case where guessing V1 is
    // unsafe -- "2.1" plainly asks for something, we just cannot tell what.
    for (const auto* spec : {"2.1", "v2", "abc", "0", "-1", " 2", "2 "}) {
        const auto support = xlings::xim::xpkg_spec_support(spec);
        EXPECT_FALSE(support.supported) << "spec '" << spec << "'";
        EXPECT_EQ(support.declared, 0) << "spec '" << spec << "'";
    }
}

TEST(XpkgSpecGate, TheCapMatchesWhatIsActuallyImplemented) {
    // A guard on the constant itself: bumping it without implementing the
    // revision is the failure mode this whole gate exists to prevent, and
    // the only mechanical trace of "implemented" is the arch enforcement
    // that spec 2 introduced.
    EXPECT_EQ(xlings::xim::max_supported_xpkg_spec, 2);
    EXPECT_TRUE(xlings::xim::xpkg_spec_support("2").supported);
    EXPECT_FALSE(
        xlings::xim::xpkg_spec_support(
            std::to_string(xlings::xim::max_supported_xpkg_spec + 1)).supported);
}
