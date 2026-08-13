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
    // weakly_canonical, not temp_directory_path() raw: on Windows CI the latter
    // is an 8.3 short path (C:\Users\RUNNER~1\...) while the code under test
    // canonicalises and gets the long form. The two never compare equal, and no
    // amount of case/separator folding fixes RUNNER~1 vs runneradmin.
    std::filesystem::path root =
        std::filesystem::weakly_canonical(std::filesystem::temp_directory_path())
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

// These assertions ask "does the message NAME this path", not "does it spell it
// the way this test literal does". std::filesystem normalises separators on
// Windows, and the fixtures use POSIX-shaped literals, so a raw substring match
// compares two spellings of one path and fails only there. Windows paths are
// also case-insensitive, and canonicalisation can change the case it hands
// back. Compare in one spelling and one case; the paths themselves are still
// compared whole, so this loosens the rendering, not the claim.
std::string gen(std::string s) {
    for (auto& c : s) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}
bool names(const std::string& haystack, const std::string& needle) {
    return gen(haystack).find(gen(needle)) != std::string::npos;
}

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

// Separators are normalized to FIND the split point; the result is sliced out
// of the caller's own spelling. Both helpers must make that choice the same
// way, because their results are compared against each other -- one normalized
// slice and one original slice make a single store two different strings, and
// that comparison would fail silently and only on Windows.
TEST(PayloadOf, AcceptsWindowsSeparators) {
    const std::string store = R"(C:\Users\ci\.xlings\data\xpkgs)";
    const std::string payload = store + R"(\xim-x-glibc\2.44)";
    const std::string core = payload + R"(\lib64\libc.so.6)";

    EXPECT_EQ(ec::payload_of(core), payload);
    EXPECT_EQ(ec::provider_of(payload), "xim-x-glibc");
    EXPECT_EQ(ec::store_root_of(core), store);

    // The invariant itself, so a future divergence fails HERE rather than
    // inside whatever compares the two: a core file's payload always starts
    // with the store root that same core file is in, in either spelling.
    for (const auto& spelling : {core, std::string(
             "C:/Users/ci/.xlings/data/xpkgs"
             "/xim-x-glibc/2.44/lib64/libc.so.6")}) {
        EXPECT_TRUE(ec::payload_of(spelling)
                        .starts_with(ec::store_root_of(spelling)))
            << "payload_of and store_root_of disagree on: " << spelling;
    }
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
    EXPECT_EQ(gen(f.interpPayload), gen(glibc("2.39")));
    EXPECT_EQ(gen(f.rpathPayload), gen(glibc("2.44")));
    // Both paths must appear: the reader should not need LD_DEBUG.
    auto msg = ec::describe(f);
    EXPECT_TRUE(names(msg, glibc("2.39"))) << msg;
    EXPECT_TRUE(names(msg, glibc("2.44"))) << msg;
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
    EXPECT_TRUE(names(ec::describe(finding), (hostLib / "libc.so.6").string()));
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
    EXPECT_TRUE(names(message, "/lib64/ld-linux-x86-64.so.2")) << message;
    EXPECT_TRUE(names(message, payloadLib.string())) << message;
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
    EXPECT_TRUE(names(ec::describe(finding), subosLib.string()));
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
    EXPECT_TRUE(names(ec::describe(finding), subosLib.string()));
    EXPECT_TRUE(names(ec::describe(finding), libcDir.string()));
}

// A PT_INTERP inside a SubOS lib dir is our own loader reached through a view.
// Measured on a real home: classifying it as the HOST's loader produced 143 of
// 145 "loader/libc split" findings, every one of them a payload loader paired
// with a libc from the same payload. The RUNPATH side already resolved through
// symlinks; resolving only one side is what made them disagree.
TEST(SameSource, InterpThroughASubosViewIsNotTheHostLoader) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("xlings-interp-view-{}", std::chrono::steady_clock::now()
            .time_since_epoch().count());
    const auto payload = root / "data" / "xpkgs" / "xim-x-glibc" / "2.39";
    const auto payloadLib = payload / "lib";
    const auto subosLib = root / "subos" / "default" / "lib";
    fs::create_directories(payloadLib);
    fs::create_directories(subosLib);
    std::ofstream(payloadLib / "ld-linux-x86-64.so.2") << "loader";
    std::ofstream(payloadLib / "libc.so.6") << "libc";
    // The SubOS lib dir is a symlink farm into the payload -- the shape the
    // loader migration produces.
    fs::create_symlink(payloadLib / "ld-linux-x86-64.so.2",
                       subosLib / "ld-linux-x86-64.so.2");
    fs::create_symlink(payloadLib / "libc.so.6", subosLib / "libc.so.6");

    const auto interp = (subosLib / "ld-linux-x86-64.so.2").string();
    std::vector<std::string> rpath{subosLib.string()};
    const auto f = ec::check(root.string() + "/bin/tool", interp, rpath);

    EXPECT_FALSE(f.violated)
        << "a payload loader and its own payload libc, both reached through "
           "the SubOS view, are one build: " << ec::describe(f);
    EXPECT_EQ(f.reason, ec::Finding::Reason::None);

    fs::remove_all(root);
}

// The reverse rule still has to fire for a genuine host interpreter.
TEST(SameSource, ARealHostInterpWithAPayloadCoreStillViolates) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("xlings-host-interp-{}", std::chrono::steady_clock::now()
            .time_since_epoch().count());
    const auto payloadLib = root / "data" / "xpkgs" / "xim-x-glibc" / "2.39" / "lib";
    fs::create_directories(payloadLib);
    std::ofstream(payloadLib / "libc.so.6") << "libc";

    std::vector<std::string> rpath{payloadLib.string()};
    const auto f = ec::check(root.string() + "/bin/tool",
                             "/lib64/ld-linux-x86-64.so.2", rpath);

    EXPECT_TRUE(f.violated);
    EXPECT_EQ(f.reason, ec::Finding::Reason::HostLoaderPayloadCore);

    fs::remove_all(root);
}

// glibc and musl are not each other's core runtime. A musl cross-compiler bakes
// its own libdir into RUNPATH, so glibc binaries it produces carry a musl loader
// on a path they never resolve libc through -- `libc.so.6` is not in there under
// any name. Comparing across families reported that as a loader/libc split.
TEST(SameSource, AMuslLoaderIsNotAGlibcBinarysLibc) {
    namespace fs = std::filesystem;
    TempTree tree;
    const auto glibcLib = tree.payload("2.39") / "lib64";
    const auto muslLib = tree.root / "data" / "xpkgs" / "xim-x-musl-gcc"
                       / "16.1.0" / "x86_64-linux-musl" / "lib";
    TempTree::touch(glibcLib / "ld-linux-x86-64.so.2");
    TempTree::touch(glibcLib / "libc.so.6");
    TempTree::touch(muslLib / "ld-musl-x86_64.so.1");

    std::vector<std::string> rpath{muslLib.string(), glibcLib.string()};
    const auto f = ec::check((tree.root / "bin" / "rigged").string(),
                             (glibcLib / "ld-linux-x86-64.so.2").string(),
                             rpath);
    EXPECT_FALSE(f.violated) << ec::describe(f);
}

// ...and the same across the other direction, so this is a family rule rather
// than a musl exemption.
TEST(SameSource, AGlibcLoaderIsNotAMuslBinarysLibc) {
    namespace fs = std::filesystem;
    TempTree tree;
    const auto muslLib = tree.root / "data" / "xpkgs" / "xim-x-musl" / "1.2.5" / "lib";
    const auto glibcLib = tree.payload("2.39") / "lib64";
    TempTree::touch(muslLib / "ld-musl-x86_64.so.1");
    TempTree::touch(glibcLib / "ld-linux-x86-64.so.2");
    TempTree::touch(glibcLib / "libc.so.6");

    std::vector<std::string> rpath{glibcLib.string()};
    const auto f = ec::check((tree.root / "bin" / "tool").string(),
                             (muslLib / "ld-musl-x86_64.so.1").string(),
                             rpath);
    EXPECT_FALSE(f.violated) << ec::describe(f);
}

// Within one family the rule is unchanged: a payload glibc loader with the
// host's glibc libc is still the crash-before-main case (measured on godot).
TEST(SameSource, WithinAFamilyAHostLibcStillViolates) {
    namespace fs = std::filesystem;
    TempTree tree;
    const auto payloadLib = tree.payload("2.39") / "lib64";
    const auto hostLib = tree.root / "usr" / "lib" / "x86_64-linux-gnu";
    TempTree::touch(payloadLib / "ld-linux-x86-64.so.2");
    TempTree::touch(hostLib / "ld-linux-x86-64.so.2");

    std::vector<std::string> rpath{hostLib.string()};
    const auto f = ec::check((tree.root / "bin" / "godot").string(),
                             (payloadLib / "ld-linux-x86-64.so.2").string(),
                             rpath);
    EXPECT_TRUE(f.violated);
    EXPECT_EQ(f.reason, ec::Finding::Reason::PayloadMismatch);
}

// ── the per-command scan cache ───────────────────────────────────────
//
// `self doctor --fix` re-detects after every repair phase -- up to eight
// passes -- and each one used to re-walk every payload's bytes. The cache
// makes the repeats cheap. Its ONE correctness requirement is that a payload
// the repair ladder actually reinstalled is re-read, because that is the
// payload whose verdict was supposed to change.
//
// A cache that never invalidated would make `--fix` report the damage it just
// repaired, forever, while looking faster. That is this repository's signature
// failure -- an answer indistinguishable from the right one -- so it gets a
// test that fails when the invalidation stops working, not one that merely
// shows the cache returns something.

namespace {

struct CacheTree {
    std::filesystem::path root =
        std::filesystem::weakly_canonical(std::filesystem::temp_directory_path())
        / std::format("xlings-scan-cache-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    CacheTree() { std::filesystem::create_directories(payload()); }
    ~CacheTree() { std::error_code ec; std::filesystem::remove_all(root, ec); }

    std::filesystem::path payload() const {
        return root / "data" / "xpkgs" / "xim-x-demo" / "1.0.0";
    }
    std::filesystem::path stamp() const {
        return payload() / ".xpkg-install.json";
    }
    void write_stamp(std::string_view text) const {
        std::ofstream(stamp(), std::ios::trunc) << text;
    }
};

}  // namespace

TEST(PayloadScanCache, RepeatedScanOfAnUnchangedPayloadIsAHit) {
    CacheTree tree;
    tree.write_stamp("{\"version\":\"1.0.0\"}\n");

    ec::PayloadScanCache cache{".xpkg-install.json"};
    cache.scan(tree.payload());
    cache.scan(tree.payload());
    cache.scan(tree.payload());

    EXPECT_EQ(cache.misses(), 1u);
    EXPECT_EQ(cache.hits(), 2u);
}

// The one that matters: a reinstall must be seen.
//
// The stamp is rewritten by every install (installer.cpp's
// write_payload_stamp), so a changed stamp is exactly the signal that the
// payload under it is not the one that was scanned.
TEST(PayloadScanCache, RewritingTheInstallStampInvalidates) {
    CacheTree tree;
    tree.write_stamp("{\"version\":\"1.0.0\"}\n");

    ec::PayloadScanCache cache{".xpkg-install.json"};
    cache.scan(tree.payload());
    ASSERT_EQ(cache.misses(), 1u);

    // A different size is enough; a same-size rewrite is covered by the write
    // time, which the filesystem updates. Asserting on size alone would pass
    // for the wrong reason.
    tree.write_stamp("{\"version\":\"1.0.0\",\"registered\":3}\n");

    cache.scan(tree.payload());
    EXPECT_EQ(cache.misses(), 2u) << "a reinstalled payload was served from cache";
    EXPECT_EQ(cache.hits(), 0u);
}

// A payload with no stamp at all -- installed before stamps existed -- must
// still be cacheable, and must still notice a change. The directory's own
// write time carries it: adding or removing an entry updates it, and an
// install writes its stamp through a rename, which does too.
TEST(PayloadScanCache, UnstampedPayloadFallsBackToTheDirectory) {
    CacheTree tree;

    ec::PayloadScanCache cache{".xpkg-install.json"};
    cache.scan(tree.payload());
    cache.scan(tree.payload());
    EXPECT_EQ(cache.misses(), 1u);
    EXPECT_EQ(cache.hits(), 1u);

    // Sleep past the filesystem's timestamp granularity before touching the
    // directory. Without this the test is a coin flip on filesystems with
    // coarse mtime, and a flaky invalidation test is worse than none.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    std::ofstream(tree.payload() / "new-file") << "x";

    cache.scan(tree.payload());
    EXPECT_EQ(cache.misses(), 2u) << "a changed payload directory was served from cache";
}

// Two different payloads are two entries, not one shared verdict.
TEST(PayloadScanCache, KeyedPerPayloadNotGlobally) {
    CacheTree tree;
    const auto other = tree.root / "data" / "xpkgs" / "xim-x-other" / "2.0.0";
    std::filesystem::create_directories(other);

    ec::PayloadScanCache cache{".xpkg-install.json"};
    cache.scan(tree.payload());
    cache.scan(other);
    EXPECT_EQ(cache.misses(), 2u);
    EXPECT_EQ(cache.hits(), 0u);

    cache.scan(tree.payload());
    cache.scan(other);
    EXPECT_EQ(cache.hits(), 2u);
}
