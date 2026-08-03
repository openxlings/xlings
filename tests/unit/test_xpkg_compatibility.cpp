#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import xlings.core.xim.compatibility;

using namespace xlings::xim;

namespace {

mcpplibs::xpkg::Package with_entry(
    std::string name,
    std::vector<std::string> archs,
    std::string os,
    std::string version,
    mcpplibs::xpkg::PlatformResource entry) {
    mcpplibs::xpkg::Package package;
    package.name = std::move(name);
    package.spec = "1";
    package.archs = std::move(archs);
    package.xpm.entries[std::move(os)][std::move(version)] = std::move(entry);
    return package;
}

}  // namespace

// A resource that enumerates its architectures is describing artifacts, so a
// missing one is a real answer: refuse, before any request is made.
TEST(XpkgCompatibility, StrongEvidenceRefusesAMissingArch) {
    mcpplibs::xpkg::PlatformResource entry;
    entry.archs["x86_64"] = {.url = "https://example.invalid/x86_64.tar.gz"};
    auto package = with_entry("d2x", {"x86_64"}, "linux", "1.0.0", entry);

    const auto* resolved = find_entry(package, "linux", "1.0.0");
    ASSERT_NE(resolved, nullptr);
    auto result = check_target_compatibility(package, resolved, "linux", "aarch64");
    EXPECT_EQ(result.evidence, ArchEvidence::Strong);
    EXPECT_FALSE(result.supported);
    EXPECT_EQ(result.target, "linux-aarch64");
    EXPECT_EQ(result.supportedTargets,
              (std::vector<std::string>{"linux-x86_64"}));
    EXPECT_EQ(compatibility_error("xim:d2x@1", result),
              "E_UNSUPPORTED_TARGET: xim:d2x@1 has no linux-aarch64 artifact; "
              "supported targets: linux-x86_64");

    EXPECT_TRUE(check_target_compatibility(package, resolved, "linux", "x86_64")
                    .supported);
}

// Per-arch checksums are the same kind of claim, and so are per-arch upstream
// aliases -- all three are what a V2 recipe writes.
TEST(XpkgCompatibility, PerArchChecksumsAndAliasesAreStrongToo) {
    mcpplibs::xpkg::PlatformResource checksums;
    checksums.url = "https://example.invalid/tool-${arch}.tar.gz";
    checksums.sha256_by_arch["aarch64"] = "aa";
    auto byChecksum = with_entry("tool", {}, "linux", "1.0.0", checksums);
    const auto* resolvedChecksum = find_entry(byChecksum, "linux", "1.0.0");
    ASSERT_NE(resolvedChecksum, nullptr);
    EXPECT_EQ(classify_arch_evidence(byChecksum, resolvedChecksum),
              ArchEvidence::Strong);
    EXPECT_TRUE(check_target_compatibility(byChecksum, resolvedChecksum,
                                           "linux", "aarch64").supported);
    EXPECT_FALSE(check_target_compatibility(byChecksum, resolvedChecksum,
                                            "linux", "x86_64").supported);

    mcpplibs::xpkg::PlatformResource aliases;
    aliases.url = "https://example.invalid/tool-${arch_alias}.zip";
    aliases.arch_alias["x86_64"] = "amd64";
    auto byAlias = with_entry("tool", {}, "windows", "1.0.0", aliases);
    const auto* resolvedAlias = find_entry(byAlias, "windows", "1.0.0");
    ASSERT_NE(resolvedAlias, nullptr);
    EXPECT_EQ(classify_arch_evidence(byAlias, resolvedAlias),
              ArchEvidence::Strong);
    EXPECT_FALSE(check_target_compatibility(byAlias, resolvedAlias,
                                            "windows", "aarch64").supported);
}

// THE REGRESSION THIS GRADING EXISTS FOR.
//
// `go.lua` declares `archs = {"x86_64"}` -- a package-level union -- and its
// macOS entry is a `darwin-arm64` tarball, because arm64 is the only macOS
// artifact upstream ships. Refusing on the union rejects the very archive the
// recipe was about to download. Same shape for `git`, `make`, `gcc` and ~60
// other spec-V1 recipes on Linux aarch64.
TEST(XpkgCompatibility, WeakEvidenceInstallsAndOnlyAdvises) {
    mcpplibs::xpkg::PlatformResource entry;
    entry.url = "https://go.dev/dl/go1.26.2.darwin-arm64.tar.gz";
    auto go = with_entry("go", {"x86_64"}, "macosx", "1.26.2", entry);

    const auto* resolved = find_entry(go, "macosx", "1.26.2");
    ASSERT_NE(resolved, nullptr);
    auto result = check_target_compatibility(go, resolved, "macosx", "aarch64");
    EXPECT_EQ(result.evidence, ArchEvidence::Weak);
    EXPECT_TRUE(result.supported) << "a package-level union must never refuse";
    EXPECT_FALSE(result.advisory.empty())
        << "the mismatch is still worth saying out loud";
    EXPECT_NE(result.advisory.find("macosx-aarch64"), std::string::npos);

    // Matching the union is silent.
    auto matching = check_target_compatibility(go, resolved, "macosx", "x86_64");
    EXPECT_TRUE(matching.supported);
    EXPECT_TRUE(matching.advisory.empty());
}

// An ${arch} template or an XLINGS_RES resource is arch-parametric by
// construction. Nothing local can bound the set, so nothing local refuses.
TEST(XpkgCompatibility, OpenEvidenceNeverRefuses) {
    mcpplibs::xpkg::PlatformResource templated;
    templated.url = "https://example.invalid/tool-linux-${arch}.tar.gz";
    auto byTemplate = with_entry("tool", {"x86_64"}, "linux", "1.0.0", templated);
    const auto* resolvedTemplate = find_entry(byTemplate, "linux", "1.0.0");
    ASSERT_NE(resolvedTemplate, nullptr);
    auto result = check_target_compatibility(byTemplate, resolvedTemplate,
                                             "linux", "aarch64");
    EXPECT_EQ(result.evidence, ArchEvidence::Open);
    EXPECT_TRUE(result.supported);
    EXPECT_TRUE(result.advisory.empty());

    mcpplibs::xpkg::PlatformResource res;
    res.url = "XLINGS_RES";
    res.is_res = true;
    auto byRes = with_entry("tool", {"x86_64"}, "linux", "1.0.0", res);
    const auto* resolvedRes = find_entry(byRes, "linux", "1.0.0");
    ASSERT_NE(resolvedRes, nullptr);
    EXPECT_TRUE(check_target_compatibility(byRes, resolvedRes,
                                           "linux", "aarch64").supported);
}

TEST(XpkgCompatibility, ArmAliasesMatchAndEmptyMeansPortable) {
    mcpplibs::xpkg::PlatformResource entry;
    entry.url = "https://example.invalid/tool.tar.gz";

    auto arm = with_entry("tool", {"arm64"}, "macosx", "1.0.0", entry);
    auto armResult = check_target_compatibility(
        arm, find_entry(arm, "macosx", "1.0.0"), "macosx", "aarch64");
    EXPECT_TRUE(armResult.supported);
    EXPECT_TRUE(armResult.advisory.empty()) << "arm64 and aarch64 are the same";

    auto portable = with_entry("tool", {}, "windows", "1.0.0", entry);
    auto portableResult = check_target_compatibility(
        portable, find_entry(portable, "windows", "1.0.0"), "windows", "x86_64");
    EXPECT_EQ(portableResult.evidence, ArchEvidence::None);
    EXPECT_TRUE(portableResult.supported);
}

// No entry at all (unknown OS, unknown version) can only ever fall back to the
// package claim, which is never enough to refuse.
TEST(XpkgCompatibility, AMissingEntryCannotRefuse) {
    mcpplibs::xpkg::PlatformResource entry;
    entry.url = "https://example.invalid/tool.tar.gz";
    auto package = with_entry("tool", {"x86_64"}, "linux", "1.0.0", entry);

    EXPECT_EQ(find_entry(package, "macosx", "1.0.0"), nullptr);
    EXPECT_EQ(find_entry(package, "linux", "9.9.9"), nullptr);
    EXPECT_TRUE(check_target_compatibility(package, nullptr, "macosx", "aarch64")
                    .supported);
}

// `latest -> 1.0.0` must reach the real resource, and a recipe whose aliases
// form a cycle must report "no entry" rather than spin.
TEST(XpkgCompatibility, VersionAliasesAreFollowedAndBounded) {
    mcpplibs::xpkg::PlatformResource concrete;
    concrete.archs["aarch64"] = {.url = "https://example.invalid/arm.tar.gz"};
    mcpplibs::xpkg::PlatformResource alias;
    alias.ref = "1.0.0";

    mcpplibs::xpkg::Package package;
    package.name = "tool";
    package.xpm.entries["linux"]["1.0.0"] = concrete;
    package.xpm.entries["linux"]["latest"] = alias;
    const auto* resolved = find_entry(package, "linux", "latest");
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(entry_architectures(*resolved),
              (std::set<std::string>{"aarch64"}));

    mcpplibs::xpkg::Package cyclic;
    cyclic.xpm.entries["linux"]["a"].ref = "b";
    cyclic.xpm.entries["linux"]["b"].ref = "a";
    EXPECT_EQ(find_entry(cyclic, "linux", "a"), nullptr);
}
