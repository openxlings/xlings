#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;
import xlings.testkit.index_fixture;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

XTEST(HttpFixture, DefaultHomeInitializesAndInstallsThroughItsOwnedLoopbackMirror,
      .area = "testkit", .covers = {"TEST-HTTP-FIXTURE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("default-http");
    const auto config = nlohmann::json::parse(tk::read_file(home.dir() / ".xlings.json"));
    const auto base = xlings::testkit::index_fixture::url();
    ASSERT_TRUE(base.starts_with("http://127.0.0.1:"));
    EXPECT_EQ(config["XLINGS_RES"]["GLOBAL"][0], base);
    EXPECT_EQ(config["index_repos"][0]["source"], "git");
    const auto entry = home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings");
    fs::create_directories(entry.parent_path());
    fs::copy_file(tk::xlings_binary(), entry);
    const auto initialized = home.xlings({"self", "init"});
    ASSERT_EQ(initialized.exit_code, 0) << initialized.transcript();
    const auto installed = home.xlings({"install", "xim:fixture-data@1.0.0", "-y"});
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    const auto payload = home.dir() / "data/xpkgs/xim-x-fixture-data/1.0.0";
    bool found = false;
    for (const auto& file : fs::recursive_directory_iterator(payload)) {
        if (file.path().filename() == "fixture.txt") {
            EXPECT_EQ(tk::read_file(file.path()), "xlings HTTP fixture\n");
            found = true;
        }
    }
    EXPECT_TRUE(found) << installed.transcript();
    const auto missing = home.xlings({"install", "xim:no-public-fixture-package@1.0.0", "-y"});
    EXPECT_NE(missing.exit_code, 0) << missing.transcript();
    EXPECT_LT(missing.elapsed, std::chrono::seconds(10));
}
