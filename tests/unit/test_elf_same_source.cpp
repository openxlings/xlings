// The invariant that a binary's loader and its libc come from one payload.
//
// Each case here is one row of the table in
// .agents/docs/2026-08-05-dependency-resolution-single-source.md §6.4.
#include <gtest/gtest.h>

import std;
import xlings.core.elf_same_source;

namespace ec = xlings::elfcheck;

namespace {
const std::string STORE = "/home/u/.xlings/data/xpkgs";
std::string glibc(const std::string& v, const std::string& sub = "") {
    return STORE + "/xim-x-glibc/" + v + (sub.empty() ? "" : "/" + sub);
}
}  // namespace

TEST(PayloadOf, TakesProviderAndVersionAndStopsThere) {
    EXPECT_EQ(ec::payload_of(glibc("2.39", "lib64/ld-linux-x86-64.so.2")),
              glibc("2.39"));
    EXPECT_EQ(ec::payload_of(glibc("2.44", "lib64")), glibc("2.44"));
    // Already a payload directory: idempotent.
    EXPECT_EQ(ec::payload_of(glibc("2.39")), glibc("2.39"));
}

TEST(PayloadOf, IsEmptyOutsideAStore) {
    EXPECT_EQ(ec::payload_of("$ORIGIN"), "");
    EXPECT_EQ(ec::payload_of("/home/u/.xlings/subos/default/lib"), "");
    EXPECT_EQ(ec::payload_of("/usr/lib/x86_64-linux-gnu"), "");
    EXPECT_EQ(ec::payload_of(""), "");
}

TEST(ProviderOf, IsTheStoreDirectoryName) {
    EXPECT_EQ(ec::provider_of(glibc("2.39")), "xim-x-glibc");
    EXPECT_EQ(ec::provider_of(STORE + "/local-x-gcc/16.1.0"), "local-x-gcc");
    EXPECT_EQ(ec::provider_of(""), "");
}

// The case that motivated all of this, measured on a real binary.
TEST(SameSource, LoaderFromOnePayloadAndLibcFromAnotherIsAViolation) {
    std::vector<std::string> rpath{
        STORE + "/local-x-gcc/16.1.0/lib64",
        glibc("2.44", "lib64"),
        STORE + "/xim-x-binutils/2.42/lib",
    };
    auto f = ec::check("gcc", glibc("2.39", "lib64/ld-linux-x86-64.so.2"), rpath);
    EXPECT_TRUE(f.violated);
    EXPECT_EQ(f.provider, "xim-x-glibc");
    EXPECT_EQ(f.interpPayload, glibc("2.39"));
    EXPECT_EQ(f.rpathPayload, glibc("2.44"));
    // Both paths must appear: the reader should not need LD_DEBUG.
    auto msg = ec::describe(f);
    EXPECT_NE(msg.find(glibc("2.39")), std::string::npos);
    EXPECT_NE(msg.find(glibc("2.44")), std::string::npos);
}

TEST(SameSource, AgreementPasses) {
    std::vector<std::string> rpath{
        STORE + "/local-x-gcc/16.1.0/lib64",
        glibc("2.44", "lib64"),
    };
    EXPECT_FALSE(ec::check("gcc", glibc("2.44", "lib64/ld-linux-x86-64.so.2"),
                           rpath).violated);
}

// A shared library does not choose a loader; the program that loads it does.
TEST(SameSource, NoInterpreterSkips) {
    std::vector<std::string> rpath{glibc("2.44", "lib64")};
    EXPECT_FALSE(ec::check("libfoo.so.1", "", rpath).violated);
}

TEST(SameSource, ProviderAbsentFromRunpathPasses) {
    std::vector<std::string> rpath{STORE + "/xim-x-binutils/2.42/lib"};
    EXPECT_FALSE(ec::check("tool", glibc("2.39", "lib64/ld-linux-x86-64.so.2"),
                           rpath).violated);
}

// The loader takes the first match, so same-source being present is enough.
TEST(SameSource, AnySameSourceEntryPasses) {
    std::vector<std::string> rpath{
        glibc("2.39", "lib64"),
        glibc("2.44", "lib64"),
    };
    EXPECT_FALSE(ec::check("gcc", glibc("2.39", "lib64/ld-linux-x86-64.so.2"),
                           rpath).violated);
}

TEST(SameSource, RelativeAndNonStoreEntriesAreIgnored) {
    std::vector<std::string> rpath{
        "$ORIGIN", "$ORIGIN/../lib",
        "/home/u/.xlings/subos/default/lib",
        "/usr/lib/x86_64-linux-gnu",
    };
    EXPECT_FALSE(ec::check("gcc", glibc("2.39", "lib64/ld-linux-x86-64.so.2"),
                           rpath).violated);
}

// An interpreter outside any store (a payload that was never elfpatched) is
// not this check's business — the loader-audit rule covers that separately.
TEST(SameSource, InterpreterOutsideAStoreSkips) {
    std::vector<std::string> rpath{glibc("2.44", "lib64")};
    EXPECT_FALSE(ec::check("python", "/lib64/ld-linux-x86-64.so.2",
                           rpath).violated);
}
