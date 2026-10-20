// luban.vm.native.vz: a Mac's Virtualization.framework, as the Luban host
// machine uses it. On a Mac the framework judges a machine's whole
// configuration (kernel, initramfs, command line, shares, the agent's
// console port, the console, NAT) without starting it; a binary without the
// virtualization entitlement (this test) is told exactly that, and nothing
// else may be wrong. Elsewhere it says it is a Mac's.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import luban.vm.native.vz;

namespace tk = xlings::testkit;
namespace platform = xlings::platform;
namespace fs = std::filesystem;
namespace vz = luban::vm::vz;

XTEST(LubanVz, TheFrameworkJudgesTheHostMachinesWholeConfiguration, .area = "luban", .covers = {"CARRIER-VZ-HELPER"}) {
    auto home = tk::Home::isolated("luban-vz");
    const auto dir = home.root();
    tk::write_file(dir / "vmlinuz", std::string(4096, '\0'));
    tk::write_file(dir / "initramfs.cpio", std::string(512, '\0'));
    fs::create_directories(dir / "tree");
    // The agent's port and the console: a stream pair where there is one (not Windows).
    auto agent = platform::unix_stream_pair();
    if constexpr (platform::is_macos) ASSERT_TRUE(agent);
    vz::Plan plan{.kernel = dir / "vmlinuz", .initrd = dir / "initramfs.cpio",
                  .cmdline = "console=hvc0 quiet luban.carrier=1 luban.fs=virtiofs",
                  .shares = {{"root", dir / "tree", false}}, .grants = true,
                  .agent_fd = agent ? (*agent)[1] : -1, .console_fd = agent ? (*agent)[0] : -1};
    const auto checked = vz::validate(plan);
    if constexpr (!platform::is_macos) {
        EXPECT_FALSE(checked);
        EXPECT_NE(vz::unsupported().find("macOS"), std::string::npos) << vz::unsupported();
    } else {
        std::println("this Mac: {}", vz::unsupported().empty() ? std::string("can run a machine") : vz::unsupported());
        if (!checked) EXPECT_NE(checked.error().find("entitlement"), std::string::npos)
            << "the configuration itself was refused: " << checked.error();
    }
    if (agent) {
        std::vector<int> fds{(*agent)[0], (*agent)[1]};
        platform::close_fds(fds);
    }
}
