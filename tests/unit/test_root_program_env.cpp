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
