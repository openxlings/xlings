#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.store;
import xlings.subos.rootfs;

// What keeps a payload alive besides a workspace: a retained root generation
// that links into it (SubOS design part 3 §7.1).

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace rf = xlings::subos::rootfs;

namespace {

const std::map<std::string, std::string> kBox{{"XLINGS_ACTIVE_SUBOS", "box"}};

fs::path tool_payload(const tk::Home& home) {
    return home.dir() / "data/xpkgs/xim-x-fixture-tool/1.0.0";
}

}  // namespace

XTEST(RootRetention, RemoveKeepsWhatAnOlderGenerationLinksIntoAndRollbackFindsIt,
      .area = "subos", .covers = {"ROOT-GC-ROOT"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("root-retention");
    auto created = home.xlings({"subos", "new", "box", "--rootfs"});
    ASSERT_EQ(created.exit_code, 0) << created.transcript();
    const auto box = home.dir() / "subos/box";

    auto installed = home.xlings({"install", "fixture-tool", "-y"}, kBox);
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    const auto with_tool = rf::current(box);
    ASSERT_TRUE(with_tool);
    ASSERT_TRUE(fs::exists(box / "root/usr/bin/fixture-tool")) << installed.transcript();

    auto removed = home.xlings({"remove", "fixture-tool", "-y"}, kBox);
    ASSERT_EQ(removed.exit_code, 0) << removed.transcript();
    EXPECT_NE(rf::current(box), with_tool);
    EXPECT_FALSE(fs::exists(box / "root/usr/bin/fixture-tool"));
    // Removed from every workspace, and still on disk: generation `with_tool`
    // links into it. The ledger says so.
    EXPECT_TRUE(fs::exists(tool_payload(home) / "bin/fixture-tool")) << removed.transcript();
    EXPECT_NE(removed.transcript().find("stay for rollback"), std::string::npos) << removed.transcript();
    auto ledger = xlings::store::read_ledger(xlings::store::ledger_path(home.dir() / "data"));
    ASSERT_TRUE(ledger) << ledger.error();
    ASSERT_EQ(ledger->size(), 1u);
    EXPECT_EQ(ledger->front().target, "fixture-tool");

    // The defect this replaces: rollback succeeded and /usr/bin pointed at a
    // payload `remove` had deleted.
    auto rolled = home.xlings({"subos", "rollback", "box", "--to", std::to_string(*with_tool)});
    ASSERT_EQ(rolled.exit_code, 0) << rolled.transcript();
    EXPECT_TRUE(fs::exists(box / "root/usr/bin/fixture-tool")) << rolled.transcript();
}

XTEST(RootRetention, PruningTheLastHolderReleasesTheRetainedPayload,
      .area = "subos", .covers = {"ROOT-GC-ROOT", "ROOT-GEN-RETAIN"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("root-release");
    ASSERT_EQ(home.xlings({"subos", "new", "box", "--rootfs"}).exit_code, 0);
    const auto box = home.dir() / "subos/box";
    ASSERT_EQ(home.xlings({"install", "fixture-tool", "-y"}, kBox).exit_code, 0);
    const auto holder = rf::current(box);
    ASSERT_TRUE(holder);
    ASSERT_EQ(home.xlings({"remove", "fixture-tool", "-y"}, kBox).exit_code, 0);
    ASSERT_TRUE(fs::exists(tool_payload(home)));

    // Push the holder out of the kept window with generations of other
    // content, then let one more workspace change prune it.
    for (std::size_t i = 0; i <= rf::kKeepGenerations; ++i) {
        rf::Plan plan;
        plan.links.push_back({"usr/share/marker-" + std::to_string(i), "/nonexistent", "test"});
        ASSERT_TRUE(rf::commit(box, plan, "filler"));
    }
    auto installed = home.xlings({"install", "fixture-data", "-y"}, kBox);
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    EXPECT_FALSE(fs::exists(box / rf::kGenerations / std::to_string(*holder)));
    EXPECT_FALSE(fs::exists(tool_payload(home))) << installed.transcript();
    auto ledger = xlings::store::read_ledger(xlings::store::ledger_path(home.dir() / "data"));
    ASSERT_TRUE(ledger) << ledger.error();
    EXPECT_TRUE(ledger->empty());
}

XTEST(RootRetention, ARootPointerThatNamesNoGenerationPrunesNothing,
      .area = "subos", .covers = {"ROOT-GEN-RETAIN"}, .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("root-prune-unknown");
    const auto box = home.dir() / "subos/box";
    for (int i = 0; i != 8; ++i) {
        rf::Plan plan;
        plan.links.push_back({"usr/share/m" + std::to_string(i), "/nonexistent", "test"});
        ASSERT_TRUE(rf::commit(box, plan, "filler"));
    }
    const auto before = rf::generations(box);
    fs::remove(box / rf::kPointer);
    fs::create_directory_symlink("somewhere-else", box / rf::kPointer);
    EXPECT_TRUE(rf::prune(box, 1).empty());
    EXPECT_EQ(rf::generations(box), before);
}
