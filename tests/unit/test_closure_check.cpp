// The decision core of the install-time closure check (rule D at execution
// point 2). The scan plumbing needs patchelf and real ELFs and is exercised
// end-to-end by tests/e2e/closure_guard_differential_test.sh -- against the
// index CI's dep-closure-check.sh on the same tree, so the two cannot drift
// silently. What is pinned HERE is the part that decides.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

import std;
import xlings.core.closure_check;

namespace cc = xlings::closurecheck;
namespace fs = std::filesystem;

// ── unprovided(): the rule-D verdict ─────────────────────────────────

TEST(ClosureCheckUnprovided, ReportsOnlySonamesNobodyProvides) {
    std::vector<std::string> needed = {
        "libc.so.6",        // store provides
        "libself.so.1",     // payload provides
        "libnothere.so.9",  // nobody does
    };
    auto missing = cc::unprovided(needed,
                                  /*self=*/ {"libself.so.1"},
                                  /*store=*/{"libc.so.6"});
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0], "libnothere.so.9");
}

TEST(ClosureCheckUnprovided, AFullyServedClosureIsEmpty) {
    std::vector<std::string> needed = {"libc.so.6", "libm.so.6"};
    EXPECT_TRUE(cc::unprovided(needed, {}, {"libc.so.6", "libm.so.6"}).empty());
    EXPECT_TRUE(cc::unprovided({}, {}, {}).empty());
}

// ── store_sonames(): what counts as a provider ───────────────────────
//
// Two properties carried over from the CI script, each learned the hard way:
// symlinks count (the soname usually IS the symlink), and the payload under
// test cannot vouch for itself.

class ClosureCheckStore : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        root = fs::temp_directory_path()
             / ("xlings-closure-test-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    void TearDown() override { fs::remove_all(root); }

    static void touch(const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream(p).put('x');
    }
};

TEST_F(ClosureCheckStore, SymlinksCountAndThePayloadUnderTestDoesNot) {
    const auto xpkgs = root / "xpkgs";
    // A provider: real file plus the soname symlink binaries actually NEED.
    touch(xpkgs / "xim-x-gcc-runtime" / "15.1.0" / "lib64" / "libstdc++.so.6.0.33");
    fs::create_symlink("libstdc++.so.6.0.33",
                       xpkgs / "xim-x-gcc-runtime" / "15.1.0" / "lib64"
                             / "libstdc++.so.6");
    // The payload being scanned: its own .so must not appear as a provider.
    const auto payload = xpkgs / "xim-x-under-test" / "1.0";
    touch(payload / "lib" / "libself.so.1");

    auto store = cc::store_sonames(xpkgs, payload);
    EXPECT_TRUE(store.contains("libstdc++.so.6"));        // the symlink
    EXPECT_TRUE(store.contains("libstdc++.so.6.0.33"));   // the file
    EXPECT_FALSE(store.contains("libself.so.1"));         // excluded
}

TEST_F(ClosureCheckStore, HostLinkStyleSymlinkPayloadsAreProviders) {
    // *-host-link packages install symlinks pointing at host libraries. The
    // provider map must accept them without a special case -- that property
    // is what lets the hardware exception pass rule D as written.
    const auto xpkgs = root / "xpkgs";
    const auto hostLib = root / "fake-host" / "libcuda.so.1";
    touch(hostLib);
    fs::create_directories(xpkgs / "xim-x-libcuda-host-link" / "1.0" / "lib");
    fs::create_symlink(hostLib, xpkgs / "xim-x-libcuda-host-link" / "1.0"
                                      / "lib" / "libcuda.so.1");

    auto store = cc::store_sonames(xpkgs, xpkgs / "xim-x-other" / "1.0");
    EXPECT_TRUE(store.contains("libcuda.so.1"));
}

TEST_F(ClosureCheckStore, PayloadSonamesSeesItsOwnOffering) {
    const auto payload = root / "payload";
    touch(payload / "lib" / "libfoo.so.2");
    fs::create_symlink("libfoo.so.2", payload / "lib" / "libfoo.so");
    touch(payload / "bin" / "not-a-library");

    auto self = cc::payload_sonames(payload);
    EXPECT_TRUE(self.contains("libfoo.so.2"));
    EXPECT_TRUE(self.contains("libfoo.so"));
    EXPECT_FALSE(self.contains("not-a-library"));
}
