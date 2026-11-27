// The specification of the xpkg format xlings publishes (docs/specs/xpkg.md)
// is the one of the libxpkg it builds with, byte for byte (design 2026-11-27
// §9.9): the text is changed in libxpkg and brought here with the version.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import mcpplibs.xpkg.conformance;

namespace fs = std::filesystem;

XTEST(XpkgFormatDoc, TheSitesCopyIsTheSpecificationOfTheLibxpkgBuiltWith, .area = "xim", .covers = {"XPKG-API-MANIFEST"}) {
    const auto doc = fs::path(__FILE__).parent_path().parent_path().parent_path() / "docs" / "specs" / "xpkg.md";
    std::ifstream in(doc, std::ios::binary);
    ASSERT_TRUE(in) << doc;
    std::ostringstream text;
    text << in.rdbuf();
    EXPECT_TRUE(text.str() == mcpplibs::xpkg::conformance::specification())
        << "docs/specs/xpkg.md differs from libxpkg's docs/spec/xpkg.md of the version in mcpp.toml: copy it from there";
}
