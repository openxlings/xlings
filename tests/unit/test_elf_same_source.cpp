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

struct TempTree {
    std::filesystem::path root =
        std::filesystem::temp_directory_path()
        / std::format("xlings-elf-same-source-{}",
                      std::chrono::steady_clock::now()
                          .time_since_epoch().count());

    TempTree() { std::filesystem::create_directories(root); }
    ~TempTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path payload(std::string_view version) const {
        return root / "data" / "xpkgs" / "xim-x-glibc" / version;
    }

    static void touch(const std::filesystem::path& path) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << "fixture\n";
    }
};

const ec::CoreDirProbe GLIBC_CORE_DIR = [](const std::filesystem::path& path) {
    const auto payload = ec::payload_of(path.string());
    return ec::provider_of(payload) == "xim-x-glibc";
};
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

TEST(PayloadOf, AcceptsWindowsSeparators) {
    const std::string store = R"(C:\Users\ci\.xlings\data\xpkgs)";
    const std::string payload = store + R"(\xim-x-glibc\2.44)";
    const std::string core = payload + R"(\lib64\libc.so.6)";

    EXPECT_EQ(ec::payload_of(core), payload);
    EXPECT_EQ(ec::provider_of(payload), "xim-x-glibc");
    EXPECT_EQ(ec::store_root_of(core), store);
}

// The case that motivated all of this, measured on a real binary.
TEST(SameSource, LoaderFromOnePayloadAndLibcFromAnotherIsAViolation) {
    std::vector<std::string> rpath{
        STORE + "/local-x-gcc/16.1.0/lib64",
        glibc("2.44", "lib64"),
        STORE + "/xim-x-binutils/2.42/lib",
    };
    auto f = ec::check("gcc", glibc("2.39", "lib64/ld-linux-x86-64.so.2"),
                       rpath, GLIBC_CORE_DIR);
    EXPECT_TRUE(f.violated);
    EXPECT_EQ(f.reason, ec::Finding::Reason::PayloadMismatch);
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
                           rpath, GLIBC_CORE_DIR).violated);
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

// The loader uses the first directory that actually contains the SONAME.
// A later same-source entry cannot wash out an earlier mismatching libc.
TEST(SameSource, FirstCoreRuntimeEntryDeterminesResult) {
    TempTree tree;
    const auto loaderDir = tree.payload("2.44") / "lib64";
    const auto mismatchDir = tree.payload("2.39") / "lib64";
    TempTree::touch(loaderDir / "ld-linux-x86-64.so.2");
    TempTree::touch(loaderDir / "libc.so.6");
    TempTree::touch(mismatchDir / "libc.so.6");

    std::vector<std::string> rpath{
        mismatchDir.string(),
        loaderDir.string(),
    };
    EXPECT_TRUE(ec::check((tree.payload("2.44") / "bin" / "gcc").string(),
                          (loaderDir / "ld-linux-x86-64.so.2").string(),
                          rpath).violated);
}

TEST(SameSource, AnyDifferentCoreRuntimeEntryViolates) {
    TempTree tree;
    const auto loaderDir = tree.payload("2.44") / "lib64";
    const auto laterDir = tree.payload("2.39") / "lib64";
    TempTree::touch(loaderDir / "ld-linux-x86-64.so.2");
    TempTree::touch(loaderDir / "libc.so.6");
    TempTree::touch(laterDir / "libc.so.6");

    std::vector<std::string> rpath{
        loaderDir.string(),
        laterDir.string(),
    };
    EXPECT_TRUE(ec::check((tree.payload("2.44") / "bin" / "gcc").string(),
                          (loaderDir / "ld-linux-x86-64.so.2").string(),
                          rpath).violated);
}

TEST(SameSource, RelativeAndNonStoreEntriesAreIgnored) {
    std::vector<std::string> rpath{
        "$ORIGIN", "$ORIGIN/../lib",
        "/home/u/.xlings/subos/default/lib",
        "/opt/vendor/lib",
    };
    EXPECT_FALSE(ec::check("gcc", glibc("2.39", "lib64/ld-linux-x86-64.so.2"),
                           rpath).violated);
}

TEST(SameSource, PayloadLoaderWithHostCoreRuntimeViolates) {
    TempTree tree;
    const auto loaderDir = tree.payload("2.44") / "lib64";
    const auto hostLib = tree.root / "host" / "lib";
    TempTree::touch(loaderDir / "ld-linux-x86-64.so.2");
    TempTree::touch(hostLib / "libc.so.6");

    std::vector<std::string> rpath{hostLib.string()};
    auto finding = ec::check((tree.payload("2.44") / "bin" / "tool").string(),
                             (loaderDir / "ld-linux-x86-64.so.2").string(),
                             rpath);
    EXPECT_TRUE(finding.violated);
    EXPECT_EQ(finding.reason, ec::Finding::Reason::PayloadMismatch);
    EXPECT_NE(ec::describe(finding).find((hostLib / "libc.so.6").string()),
              std::string::npos);
}

TEST(SameSource, HostLoaderWithPayloadLibcIsAViolation) {
    TempTree tree;
    const auto payloadLib = tree.payload("2.44") / "lib64";
    TempTree::touch(payloadLib / "libc.so.6");

    std::vector<std::string> rpath{payloadLib.string()};
    auto finding = ec::check((tree.root / "bin" / "python").string(),
                             "/lib64/ld-linux-x86-64.so.2", rpath);

    EXPECT_TRUE(finding.violated);
    EXPECT_EQ(finding.reason, ec::Finding::Reason::HostLoaderPayloadCore);
    auto message = ec::describe(finding);
    EXPECT_NE(message.find("/lib64/ld-linux-x86-64.so.2"), std::string::npos);
    EXPECT_NE(message.find(payloadLib.string()), std::string::npos);
}

TEST(SameSource, HostLoaderWithOrdinaryPayloadLibraryPasses) {
    TempTree tree;
    const auto payloadLib = tree.payload("2.44") / "lib64";
    TempTree::touch(payloadLib / "libX11.so.6");

    std::vector<std::string> rpath{payloadLib.string()};
    EXPECT_FALSE(ec::check((tree.root / "bin" / "viewer").string(),
                           "/lib64/ld-linux-x86-64.so.2", rpath).violated);
}

TEST(SameSource, PayloadLoaderAndMatchingPayloadLibcPass) {
    TempTree tree;
    const auto payloadLib = tree.payload("2.44") / "lib64";
    TempTree::touch(payloadLib / "libc.so.6");

    std::vector<std::string> rpath{payloadLib.string()};
    EXPECT_FALSE(ec::check((tree.payload("2.44") / "bin" / "gcc").string(),
                           (payloadLib / "ld-linux-x86-64.so.2").string(),
                           rpath).violated);
}

TEST(SameSource, FilesystemAliasesOfOnePayloadStillPass) {
    TempTree tree;
    const auto payloadLib = tree.payload("2.44") / "lib64";
    TempTree::touch(payloadLib / "ld-linux-x86-64.so.2");
    TempTree::touch(payloadLib / "libc.so.6");

    const auto aliasData = tree.root / "data-alias";
    std::filesystem::create_directory_symlink(tree.root / "data", aliasData);
    const auto aliasLib = aliasData / "xpkgs" / "xim-x-glibc" / "2.44"
                        / "lib64";

    std::vector<std::string> rpath{payloadLib.string()};
    EXPECT_FALSE(ec::check((tree.payload("2.44") / "bin" / "gcc").string(),
                           (aliasLib / "ld-linux-x86-64.so.2").string(),
                           rpath).violated);
}

TEST(SameSource, PayloadLoaderAndDifferentPayloadLibcViolate) {
    TempTree tree;
    const auto loaderDir = tree.payload("2.44") / "lib64";
    const auto libcDir = tree.payload("2.39") / "lib64";
    TempTree::touch(loaderDir / "ld-linux-x86-64.so.2");
    TempTree::touch(libcDir / "libc.so.6");

    std::vector<std::string> rpath{libcDir.string()};
    auto finding = ec::check((tree.payload("2.44") / "bin" / "gcc").string(),
                             (loaderDir / "ld-linux-x86-64.so.2").string(),
                             rpath);
    EXPECT_TRUE(finding.violated);
    EXPECT_EQ(finding.reason, ec::Finding::Reason::PayloadMismatch);
}

TEST(SameSource, DifferentProvidersStillCannotSplitLoaderAndLibc) {
    TempTree tree;
    const auto loaderDir = tree.root / "data" / "xpkgs"
                         / "vendor-a-x-glibc" / "2.44" / "lib64";
    const auto libcDir = tree.root / "data" / "xpkgs"
                       / "vendor-b-x-glibc" / "2.44" / "lib64";
    TempTree::touch(loaderDir / "ld-linux-x86-64.so.2");
    TempTree::touch(libcDir / "libc.so.6");

    std::vector<std::string> rpath{libcDir.string()};
    auto finding = ec::check((loaderDir.parent_path() / "bin" / "tool").string(),
                             (loaderDir / "ld-linux-x86-64.so.2").string(),
                             rpath);
    EXPECT_TRUE(finding.violated);
    EXPECT_EQ(finding.reason, ec::Finding::Reason::PayloadMismatch);
}

TEST(SameSource, OriginIsResolvedRelativeToTestedElf) {
    TempTree tree;
    const auto loaderDir = tree.payload("2.44") / "lib64";
    const auto libcDir = tree.payload("2.39") / "lib64";
    TempTree::touch(loaderDir / "ld-linux-x86-64.so.2");
    TempTree::touch(libcDir / "libc.so.6");

    const auto binary = tree.payload("2.44") / "bin" / "gcc";
    std::vector<std::string> rpath{"$ORIGIN/../../2.39/lib64"};
    auto finding = ec::check(binary.string(),
                             (loaderDir / "ld-linux-x86-64.so.2").string(),
                             rpath);
    EXPECT_TRUE(finding.violated);
    EXPECT_EQ(finding.reason, ec::Finding::Reason::PayloadMismatch);
}

TEST(SameSource, SubosSymlinkResolvingToPayloadLibcViolates) {
    TempTree tree;
    const auto loaderDir = tree.payload("2.44") / "lib64";
    const auto libcDir = tree.payload("2.39") / "lib64";
    const auto subosLib = tree.root / "subos" / "default" / "lib";
    TempTree::touch(loaderDir / "ld-linux-x86-64.so.2");
    TempTree::touch(libcDir / "libc.so.6");
    std::filesystem::create_directories(subosLib);
    std::filesystem::create_symlink(libcDir / "libc.so.6",
                                    subosLib / "libc.so.6");

    std::vector<std::string> rpath{subosLib.string()};
    auto finding = ec::check((tree.payload("2.44") / "bin" / "gcc").string(),
                             (loaderDir / "ld-linux-x86-64.so.2").string(),
                             rpath);
    EXPECT_TRUE(finding.violated);
    EXPECT_EQ(finding.reason, ec::Finding::Reason::PayloadMismatch);
    EXPECT_NE(ec::describe(finding).find(subosLib.string()), std::string::npos);
}

TEST(SameSource, SubosDirectoryCannotMixLoaderAndLibcSources) {
    TempTree tree;
    const auto loaderDir = tree.payload("2.44") / "lib64";
    const auto libcDir = tree.payload("2.39") / "lib64";
    const auto subosLib = tree.root / "subos" / "default" / "lib";
    TempTree::touch(loaderDir / "ld-linux-x86-64.so.2");
    TempTree::touch(libcDir / "libc.so.6");
    std::filesystem::create_directories(subosLib);
    // Create the agreeing loader first: an implementation that inspects one
    // arbitrary directory entry may see it and miss the split libc.
    std::filesystem::create_symlink(loaderDir / "ld-linux-x86-64.so.2",
                                    subosLib / "ld-linux-x86-64.so.2");
    std::filesystem::create_symlink(libcDir / "libc.so.6",
                                    subosLib / "libc.so.6");

    std::vector<std::string> rpath{subosLib.string()};
    auto finding = ec::check((tree.payload("2.44") / "bin" / "gcc").string(),
                             (loaderDir / "ld-linux-x86-64.so.2").string(),
                             rpath);
    EXPECT_TRUE(finding.violated);
    EXPECT_EQ(finding.reason, ec::Finding::Reason::PayloadMismatch);
    EXPECT_NE(ec::describe(finding).find(subosLib.string()), std::string::npos);
    EXPECT_NE(ec::describe(finding).find(libcDir.string()), std::string::npos);
}
