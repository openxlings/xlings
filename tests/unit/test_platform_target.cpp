#include <gtest/gtest.h>

import std;
import xlings.platform.target;

using namespace xlings::platform;

// The build target is what packages must match: this process's ABI. Reading it
// from the machine would install aarch64 artifacts into an x86_64 process's
// world the moment anyone runs the emulated build.
TEST(PlatformTarget, BuildDescribesThisProcess) {
    EXPECT_FALSE(build().os.empty());
    EXPECT_NE(build().arch, "unknown")
        << "this build has no arch token, so no package can be matched to it";
    EXPECT_EQ(build().str(), build().os + "-" + build().arch);

    // Per-OS spelling, matching release asset names.
    if (build().os == "macosx") {
        EXPECT_NE(build().arch, "aarch64") << "macOS spells it arm64";
    } else {
        EXPECT_NE(build().arch, "arm64") << "Linux and Windows spell it aarch64";
    }
}

// The host is what the kernel reports, and it only ever differs under
// emulation. Falling back to the build target means a caller never has to
// handle "unknown machine" -- the only interesting case is disagreement.
TEST(PlatformTarget, HostIsReadableAndAgreesUnlessEmulated) {
    EXPECT_FALSE(host().os.empty());
    EXPECT_FALSE(host().arch.empty());
    EXPECT_EQ(host().os, build().os) << "the OS cannot be emulated away here";
    EXPECT_EQ(is_emulated(), build().arch != host().arch);

    // Memoised: two reads of the same question give the same answer.
    EXPECT_EQ(host(), host());
    EXPECT_EQ(build(), build());
}
