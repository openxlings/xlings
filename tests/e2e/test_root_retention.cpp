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

XTEST(RootGeneration, ACommitFlushesItsRecordsAndDirectoriesBeforePublishingThem,
      .area = "subos", .covers = {"ROOT-GEN-DURABLE"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("root-durable");
    ASSERT_EQ(home.xlings({"subos", "new", "box", "--rootfs"}).exit_code, 0);
    auto env = kBox;
    env["XLINGS_TRACE"] = "durability";
    auto installed = home.xlings({"install", "fixture-tool", "-y"}, env);
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    const auto box = home.dir() / "subos/box";
    const auto generation = rf::current(box);
    ASSERT_TRUE(generation);

    // The order of the flushes is the guarantee: everything a rename
    // publishes is on disk before the directory that holds the rename is.
    std::vector<std::string> trace;
    std::istringstream lines(installed.transcript());
    for (std::string line; std::getline(lines, line);)
        if (line.starts_with("[trace:durability] ")) trace.push_back(line.substr(19));
    auto at = [&](std::string_view needle) {
        const auto it = std::ranges::find_if(trace, [&](const std::string& t) { return t.find(needle) != std::string::npos; });
        return it == trace.end() ? std::ptrdiff_t{-1} : it - trace.begin();
    };
    const auto records = at("links.tsv");
    const auto bin = at("/usr/bin");
    const auto placedLine = std::format("dir {}", (box / rf::kGenerations).string());
    const auto placedIt = std::ranges::find(trace, placedLine);   // exact: staging lives under it
    const auto placed = placedIt == trace.end() ? std::ptrdiff_t{-1} : placedIt - trace.begin();
    const auto pointer = static_cast<std::ptrdiff_t>(trace.size()) - 1;
    ASSERT_GE(records, 0) << installed.transcript();
    ASSERT_GE(bin, 0) << installed.transcript();
    ASSERT_GE(placed, 0) << installed.transcript();
    EXPECT_LT(records, placed);
    EXPECT_LT(bin, placed);
    EXPECT_EQ(trace.back(), std::format("dir {}", box.string())) << "the pointer's directory is flushed last";
    EXPECT_LT(placed, pointer);
    EXPECT_TRUE(fs::exists(box / rf::kGenerations / std::format(".{}.inventory", *generation)));
}

XTEST(RootGeneration, ACommitPublishesLinksToLogicalPathsThatExistOnlyWhereTheRootIsUsed,
      .area = "subos", .covers = {"ROOT-GC-ROOT", "ROOT-GEN-ATOMIC"}, .requires_ = {"posix"}) {
    // An exported image's generation names payloads at the image home's
    // path; at export time they live in the staging tree, not there.
    auto home = tk::Home::isolated("root-logical");
    const auto image = home.dir() / "stage/subos/default";
    const fs::path logical = "/nonexistent-logical-home/data/xpkgs/fixture-x-tool/1/bin/tool";
    const auto placed = rf::commit(image, {{{"usr/bin/tool", logical, "fixture"}}, {}}, "export");
    ASSERT_TRUE(placed) << placed.error();
    EXPECT_EQ(rf::current(image), placed);
    // Choosing it again later, here, is a rollback: that does check.
    ASSERT_TRUE(rf::commit(image, {{{"usr/bin/other", logical, "fixture"}}, {}}, "second"));
    EXPECT_FALSE(rf::switch_to(image, *placed));
    EXPECT_TRUE(rf::switch_to(image, *placed, rf::Flush::Durable, rf::Verify::Tree));
}

XTEST(RootGeneration, ASwitchRefusesAGenerationWhosePayloadIsGone,
      .area = "subos", .covers = {"ROOT-GC-ROOT", "ROOT-SWITCH-SCALE"}, .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("root-switch-gone");
    const auto box = home.dir() / "subos/box";
    const auto payload = home.dir() / "data/xpkgs/fixture-x-tool/1";
    tk::write_file(payload / "bin/tool", "fixture");
    const auto first = rf::commit(box, {{{"usr/bin/tool", payload / "bin/tool", "fixture"}}, {}}, "first");
    ASSERT_TRUE(first) << first.error();
    const auto second = rf::commit(box, {{{"usr/bin/other", payload / "bin/tool", "fixture"}}, {}}, "second");
    ASSERT_TRUE(second) << second.error();
    fs::remove_all(payload);
    const auto refused = rf::switch_to(box, *first);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find("which is gone"), std::string::npos) << refused.error();
    EXPECT_EQ(rf::current(box), second);
    // Without the inventory the full walk decides, and says the same.
    fs::remove(box / rf::kGenerations / std::format(".{}.inventory", *first));
    const auto again = rf::switch_to(box, *first);
    ASSERT_FALSE(again);
    EXPECT_NE(again.error().find("which is gone"), std::string::npos) << again.error();
}
