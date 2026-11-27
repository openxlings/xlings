// Rule M (design 2026-11-27 §5.4): what a payload gives the host to run or
// load -- its registered programs and libraries, the libraries directly in its
// exported runtime directories, its loader -- is ELF of the host's machine;
// the rest of the payload is the recipe's.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform.target;
import xlings.core.xvm.registration;
import xlings.core.xim.machine_rule;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace rule = xlings::xim::machine_rule;

namespace {

constexpr std::uint16_t kX86_64 = 0x3e;
constexpr std::uint16_t kAarch64 = 0xb7;

bool host_is_x86_64() { return std::string_view(xlings::platform::build_arch()) == "x86_64"; }
std::uint16_t host_machine() { return host_is_x86_64() ? kX86_64 : kAarch64; }
std::uint16_t other_machine() { return host_is_x86_64() ? kAarch64 : kX86_64; }
std::string other_name() { return host_is_x86_64() ? "aarch64" : "x86_64"; }

xlings::xvm::RegistrationNode program(const fs::path& bin, std::string name) {
    return { .target = name, .version = "1.0.0", .path = bin.string(), .kind = "program", .sourceName = name };
}

}  // namespace

XTEST(MachineRule, TheSurfaceIsWhatTheHostRunsOrLoads, .area = "xim", .covers = {"ARCH-MACHINE-RULE"},
      .requires_ = {"linux"}) {
    auto home = tk::Home::isolated("machine-rule-surface");
    const auto payload = home.root() / "payload";
    tk::write_elf(payload / "bin" / "good", { .machine = host_machine() });
    tk::write_elf(payload / "bin" / "bad", { .machine = other_machine() });
    tk::write_file(payload / "bin" / "script", "#!/bin/sh\necho hi\n");
    tk::write_elf(payload / "lib" / "libok.so", { .machine = host_machine() });
    tk::write_elf(payload / "lib" / "libforeign.so.1", { .machine = other_machine() });
    // Outside the surface: a directory below lib, a sysroot, firmware.
    tk::write_elf(payload / "lib" / "gcc" / "target" / "libgcc.so", { .machine = other_machine() });
    tk::write_elf(payload / "sysroot" / "lib" / "libc.so.6", { .machine = other_machine() });
    tk::write_elf(payload / "share" / "firmware.elf", { .machine = other_machine() });

    xlings::xvm::RegistrationBatch batch;
    batch.nodes = { program(payload / "bin", "good"), program(payload / "bin", "bad"),
                    program(payload / "bin", "script") };
    const auto foreign = rule::foreign(rule::of_registration(payload, batch, {}, {}, home.dir().string()));
    ASSERT_EQ(foreign.size(), 2u);
    EXPECT_EQ(fs::path(foreign[0].file).filename(), "bad");
    EXPECT_EQ(foreign[0].machine, other_name());
    EXPECT_EQ(fs::path(foreign[1].file).filename(), "libforeign.so.1");

    // Exported directories replace lib64 and lib: an exported `sysroot/lib` is
    // the host's to load, so it is judged; `lib` then is not.
    const auto exported = rule::foreign(rule::of_registration(
        payload, {}, {}, {"sysroot/lib"}, home.dir().string()));
    ASSERT_EQ(exported.size(), 1u);
    EXPECT_EQ(fs::path(exported[0].file).filename(), "libc.so.6");

    // The loader the package exports.
    tk::write_elf(payload / "lib64" / "ld-probe.so.2", { .machine = other_machine() });
    const auto loader = rule::foreign(rule::of_registration(
        payload, {}, "lib64/ld-probe.so.2", {"sysroot/none"}, home.dir().string()));
    ASSERT_EQ(loader.size(), 1u);
    EXPECT_EQ(fs::path(loader[0].file).filename(), "ld-probe.so.2");
}

XTEST(MachineRule, ThePayloadOfTheHostsMachinePasses, .area = "xim", .covers = {"ARCH-MACHINE-RULE"},
      .requires_ = {"linux"}) {
    auto home = tk::Home::isolated("machine-rule-pass");
    const auto payload = home.root() / "payload";
    tk::write_elf(payload / "bin" / "tool", { .machine = host_machine() });
    tk::write_elf(payload / "lib64" / "libtool.so", { .machine = host_machine() });
    // A link to a library of the same payload is judged once, as its target.
    fs::create_symlink("libtool.so", payload / "lib64" / "libtool.so.1");
    xlings::xvm::RegistrationBatch batch;
    batch.nodes = { program(payload / "bin", "tool") };
    EXPECT_TRUE(rule::foreign(rule::of_registration(payload, batch, {}, {}, home.dir().string())).empty());

    const auto line = rule::describe("xim:probe@1.0.0", {{ .file = "/p/bin/x", .machine = other_name() }});
    EXPECT_NE(line.find("xim:probe@1.0.0: /p/bin/x is for " + other_name() + ", and this machine is "
                        + rule::host_machine()), std::string::npos) << line;
    EXPECT_NE(rule::hint().find("xim-pkgindex/issues"), std::string::npos);
}
