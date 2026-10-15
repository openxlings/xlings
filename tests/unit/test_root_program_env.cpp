// A root's /usr/bin links a program straight to its payload file -- unless
// something has to be added when it runs. Arguments were; the environment its
// recipe registered was not, and vim in a root could not find its runtime
// files (`E1187: Failed to source defaults.vim`: $VIM is how it finds them).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.subos.root;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace xvm = xlings::xvm;

XTEST(RootProgramEnv, AProgramWithARegisteredEnvironmentKeepsItsShimInARoot,
      .area = "subos", .covers = {"ROOT-PROGRAM-ENV"}) {
    auto home = tk::Home::isolated("root-program-env");
    const auto vim = home.dir() / "data/xpkgs/xim-x-vim/9.2/bin";
    const auto jq = home.dir() / "data/xpkgs/xim-x-jq/1.7/bin";
    tk::write_file(vim / "vim", "vim");
    tk::write_file(jq / "jq", "jq");
    xvm::VersionDB db;
    xvm::add_version(db, "vim", "9.2", vim.string());
    xvm::add_version(db, "jq", "1.7", jq.string());
    db["vim"].versions["9.2"].envs["VIM"] = (vim.parent_path() / "share/vim").string();

    const auto in = xlings::subos_root::inputs(home.dir(), home.dir() / "subos/box",
                                               xvm::Workspace{{"vim", "9.2"}, {"jq", "1.7"}}, db);
    const auto target_of = [&](std::string_view name) {
        for (const auto& p : in.programs)
            if (p.name == name) return p.target;
        return fs::path{};
    };
    EXPECT_EQ(target_of("vim"), home.dir() / "bin/xlings") << "its environment is added by the shim";
    EXPECT_EQ(target_of("jq"), jq / "jq") << "nothing to add: linked straight to the payload";
}

// A root's default runtime is the one its generation carries (part 3 §4):
// usr/lib/xlings-abi/<family>, what the home's runtime indirection is inside it.
XTEST(RootProgramEnv, ARootCarriesItsOwnRuntimeInItsGeneration,
      .area = "subos", .covers = {"RUNTIME-SCOPE"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "the runtime indirection is Linux's";
    auto home = tk::Home::isolated("root-runtime");
    const auto glibc = home.dir() / "data/xpkgs/xim-x-glibc/2.44.3";
    tk::write_file(glibc / "lib64/ld-linux-x86-64.so.2", "loader");
    tk::write_file(glibc / "lib64/libc.so.6", "libc");
    tk::write_file(glibc / "bin/ldd", "ldd");
    xvm::VersionDB db;
    xvm::add_version(db, "ldd", "2.44.3", (glibc / "bin").string());
    const auto in = xlings::subos_root::inputs(home.dir(), home.dir() / "subos/box", xvm::Workspace{{"ldd", "2.44.3"}}, db);
    std::map<std::string, fs::path> runtime(in.runtime.begin(), in.runtime.end());
    EXPECT_EQ(runtime["linux-x86_64-glibc/libc.so.6"], glibc / "lib64/libc.so.6");
    EXPECT_EQ(runtime["linux-x86_64-glibc/ld-linux-x86-64.so.2"], glibc / "lib64/ld-linux-x86-64.so.2");
}
