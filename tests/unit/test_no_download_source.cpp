// Telling "this package has no payload on purpose" apart from "this recipe is
// broken".
//
// The installer used to warn on both, because both arrive as a failed
// `resolve_resource` and that function returns a bare string for every reason
// it can fail. Packages whose whole design is to have no payload --
// `nvidia-gl-host-link`, `wsl-gl-host-link` -- therefore printed
//
//     [warn] skipping nvidia-gl-host-link: resource has neither url nor source
//
// on every single install, describing correct behaviour as a problem.
//
// The predicate under test asks the RECIPE instead of reading libxpkg's error
// text. These tests exist to keep it narrow: the moment it starts answering
// "true" for a recipe that merely failed to resolve, a genuinely broken
// package goes quiet, and that is a strictly worse bug than the noise it
// replaced.
#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import xlings.core.xim.compatibility;

namespace xp = mcpplibs::xpkg;

namespace {

// The shape of `["0.1.1"] = { }` -- an entry that declares nothing.
xp::Package payload_free_package() {
    xp::Package pkg;
    pkg.name = "nvidia-gl-host-link";
    pkg.xpm.entries["linux"]["0.1.1"] = xp::PlatformResource{};
    return pkg;
}

}  // namespace

TEST(NoDownloadSource, AnEmptyEntryWithNoPackageSourceIsDeliberate) {
    auto pkg = payload_free_package();
    const auto* entry = xlings::xim::find_entry(pkg, "linux", "0.1.1");
    ASSERT_NE(entry, nullptr);

    EXPECT_TRUE(xlings::xim::declares_no_download_source(pkg, entry, "linux"));
}

TEST(NoDownloadSource, AnEntryWithAUrlIsNotDeliberate) {
    auto pkg = payload_free_package();
    pkg.xpm.entries["linux"]["0.1.1"].url = "https://example.com/x.tar.gz";
    const auto* entry = xlings::xim::find_entry(pkg, "linux", "0.1.1");
    ASSERT_NE(entry, nullptr);

    EXPECT_FALSE(xlings::xim::declares_no_download_source(pkg, entry, "linux"));
}

// A package-level `source` (a URL template, or "xlings-res") makes an empty
// version entry perfectly downloadable -- that is the whole point of the
// field. Treating the empty entry as "declares nothing" here would silence a
// real resolution failure for the majority of recipes in the index.
TEST(NoDownloadSource, APackageLevelSourceCountsAsDeclared) {
    auto pkg = payload_free_package();
    pkg.xpm.source = "xlings-res";
    const auto* entry = xlings::xim::find_entry(pkg, "linux", "0.1.1");
    ASSERT_NE(entry, nullptr);

    EXPECT_FALSE(xlings::xim::declares_no_download_source(pkg, entry, "linux"));
}

TEST(NoDownloadSource, APlatformSourceOverridesThePackageSource) {
    auto pkg = payload_free_package();
    pkg.xpm.source = "xlings-res";
    // An explicitly EMPTY platform source is a deliberate override saying this
    // platform has nothing to fetch, even though other platforms do.
    pkg.xpm.platform_sources["linux"] = "";
    const auto* entry = xlings::xim::find_entry(pkg, "linux", "0.1.1");
    ASSERT_NE(entry, nullptr);

    EXPECT_TRUE(xlings::xim::declares_no_download_source(pkg, entry, "linux"));
}

// Per-arch urls are still urls.
TEST(NoDownloadSource, APerArchUrlCountsAsDeclared) {
    auto pkg = payload_free_package();
    xp::ArchResource arch;
    arch.url = "https://example.com/x86_64.tar.gz";
    pkg.xpm.entries["linux"]["0.1.1"].archs["x86_64"] = arch;
    const auto* entry = xlings::xim::find_entry(pkg, "linux", "0.1.1");
    ASSERT_NE(entry, nullptr);

    EXPECT_FALSE(xlings::xim::declares_no_download_source(pkg, entry, "linux"));
}

TEST(NoDownloadSource, AnXlingsResEntryCountsAsDeclared) {
    auto pkg = payload_free_package();
    pkg.xpm.entries["linux"]["0.1.1"].is_res = true;
    const auto* entry = xlings::xim::find_entry(pkg, "linux", "0.1.1");
    ASSERT_NE(entry, nullptr);

    EXPECT_FALSE(xlings::xim::declares_no_download_source(pkg, entry, "linux"));
}

// "No entry for this version" is a different answer with a different remedy,
// and conflating the two would silence every unknown-version failure.
TEST(NoDownloadSource, NoEntryIsNotTheSameAsNoSource) {
    auto pkg = payload_free_package();
    const auto* missing = xlings::xim::find_entry(pkg, "linux", "9.9.9");
    ASSERT_EQ(missing, nullptr);

    EXPECT_FALSE(xlings::xim::declares_no_download_source(pkg, missing, "linux"));
}
