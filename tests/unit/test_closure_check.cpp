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


// ── rule E: which tag the search path is in ────────────────────────────────
//
// DT_RUNPATH is consulted only for the object carrying it; DT_RPATH is
// consulted for every dlopen anywhere in the process. A GL program reaches its
// driver through three levels of dlopen, so the tag -- not the path -- decides
// whether it renders on the GPU. Measured on a real home: 1 of 73 installed
// executables carried DT_RPATH while 55 of the other 68 already carried the
// correct PATH.
//
// Synthetic ELFs rather than real ones: the parser is the thing under test,
// and a real binary would make these cases depend on whichever toolchain built
// the fixture.
namespace {

// A minimal 64-bit LE ELF with one PT_DYNAMIC segment holding the given
// (tag, value) pairs. Only the fields path_tag_of reads are filled in.
std::string synth_elf(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& dyn,
                      bool is64 = true) {
    const std::size_t ehsize   = is64 ? 64u : 52u;
    const std::size_t phentsz  = is64 ? 56u : 32u;
    const std::size_t phoff    = ehsize;
    const std::size_t dynoff   = phoff + phentsz;
    const std::size_t w        = is64 ? 8u : 4u;
    std::string b(dynoff + (dyn.size() + 1) * 2 * w, '\0');

    auto put = [&](std::size_t off, std::uint64_t v, int width) {
        for (int i = 0; i < width; ++i)
            b[off + static_cast<std::size_t>(i)] =
                static_cast<char>((v >> (8 * i)) & 0xff);
    };

    b[0] = '\x7f'; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = is64 ? 2 : 1;   // class
    b[5] = 1;              // little-endian
    if (is64) {
        put(32, phoff, 8); put(54, phentsz, 2); put(56, 1, 2);
        put(phoff, 2, 4);            // p_type = PT_DYNAMIC
        put(phoff + 8, dynoff, 8);   // p_offset
    } else {
        put(28, phoff, 4); put(42, phentsz, 2); put(44, 1, 2);
        put(phoff, 2, 4);
        put(phoff + 4, dynoff, 4);
    }
    std::size_t at = dynoff;
    for (auto& [tag, val] : dyn) {
        put(at, tag, static_cast<int>(w));
        put(at + w, val, static_cast<int>(w));
        at += 2 * w;
    }
    return b;   // trailing zeros are the DT_NULL terminator
}

fs::path write_synth(const fs::path& dir, const std::string& name,
                     const std::string& bytes) {
    fs::create_directories(dir);
    auto p = dir / name;
    std::ofstream(p, std::ios::binary).write(bytes.data(),
                                             static_cast<std::streamsize>(bytes.size()));
    return p;
}

constexpr std::uint64_t DT_NEEDED = 1, DT_RPATH = 15, DT_RUNPATH = 29;

} // namespace

TEST(ClosureCheckTag, RunpathIsReportedAsNonTransitive) {
    const auto dir = fs::temp_directory_path() / "xlings-tagtest-runpath";
    fs::remove_all(dir);
    auto f = write_synth(dir, "a.elf", synth_elf({{DT_NEEDED, 1}, {DT_RUNPATH, 2}}));
    EXPECT_EQ(cc::path_tag_of(f), cc::PathTag::Runpath);
    fs::remove_all(dir);
}

TEST(ClosureCheckTag, RpathIsTheTransitiveOne) {
    const auto dir = fs::temp_directory_path() / "xlings-tagtest-rpath";
    fs::remove_all(dir);
    auto f = write_synth(dir, "a.elf", synth_elf({{DT_RPATH, 2}, {DT_NEEDED, 1}}));
    EXPECT_EQ(cc::path_tag_of(f), cc::PathTag::Rpath);
    fs::remove_all(dir);
}

// When both tags are present the loader IGNORES DT_RPATH and uses DT_RUNPATH,
// so the ELF behaves as Runpath. Reading whichever came first in the table
// would report the opposite on half of them -- and DT_RPATH first is the
// common layout, so the wrong reading would look right in casual testing.
TEST(ClosureCheckTag, BothTagsBehaveAsRunpathWhicheverComesFirst) {
    const auto dir = fs::temp_directory_path() / "xlings-tagtest-both";
    fs::remove_all(dir);
    auto a = write_synth(dir, "rpath-first.elf",
                         synth_elf({{DT_RPATH, 2}, {DT_RUNPATH, 3}}));
    auto b = write_synth(dir, "runpath-first.elf",
                         synth_elf({{DT_RUNPATH, 3}, {DT_RPATH, 2}}));
    EXPECT_EQ(cc::path_tag_of(a), cc::PathTag::Runpath);
    EXPECT_EQ(cc::path_tag_of(b), cc::PathTag::Runpath);
    fs::remove_all(dir);
}

TEST(ClosureCheckTag, NoSearchPathIsNeitherTag) {
    const auto dir = fs::temp_directory_path() / "xlings-tagtest-none";
    fs::remove_all(dir);
    auto f = write_synth(dir, "a.elf", synth_elf({{DT_NEEDED, 1}}));
    EXPECT_EQ(cc::path_tag_of(f), cc::PathTag::None);
    fs::remove_all(dir);
}

TEST(ClosureCheckTag, ThirtyTwoBitElfIsParsedToo) {
    const auto dir = fs::temp_directory_path() / "xlings-tagtest-32";
    fs::remove_all(dir);
    auto f = write_synth(dir, "a.elf", synth_elf({{DT_RUNPATH, 2}}, /*is64=*/false));
    EXPECT_EQ(cc::path_tag_of(f), cc::PathTag::Runpath);
    fs::remove_all(dir);
}

// Anything that is not a little-endian ELF declines rather than guesses. A
// wrong guess here would MISREPORT -- a payload called out for a tag it does
// not have -- which is worse than saying nothing.
TEST(ClosureCheckTag, NonElfAndBigEndianDecline) {
    const auto dir = fs::temp_directory_path() / "xlings-tagtest-junk";
    fs::remove_all(dir);
    auto junk = write_synth(dir, "junk", std::string(200, 'x'));
    EXPECT_EQ(cc::path_tag_of(junk), cc::PathTag::None);

    auto be = synth_elf({{DT_RUNPATH, 2}});
    be[5] = 2;   // big-endian
    auto bef = write_synth(dir, "be.elf", be);
    EXPECT_EQ(cc::path_tag_of(bef), cc::PathTag::None);
    fs::remove_all(dir);
}
