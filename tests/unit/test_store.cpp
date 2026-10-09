#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.store;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace st = xlings::store;

XTEST(Store, APayloadRootIsTheVersionDirectoryUnderXpkgs,
      .area = "store", .covers = {"ROOT-GC-ROOT"}) {
    EXPECT_EQ(st::payload_root("/h/data/xpkgs/xim-x-gcc/16.1.0/bin/gcc"),
              fs::path("/h/data/xpkgs/xim-x-gcc/16.1.0"));
    EXPECT_EQ(st::payload_root("/h/data/xpkgs/xim-x-gcc/16.1.0"), fs::path("/h/data/xpkgs/xim-x-gcc/16.1.0"));
    EXPECT_FALSE(st::payload_root("/h/data/xpkgs/xim-x-gcc"));
    EXPECT_FALSE(st::payload_root("/usr/bin/sh"));
}

XTEST(Store, PinsNameEveryGenerationThatLinksIntoThePayloadAndEveryUnreadableOne,
      .area = "store", .covers = {"ROOT-GC-ROOT"}) {
    const fs::path payload = "/h/data/xpkgs/xim-x-tool/1";
    const std::vector<st::RootSet> roots{
        {"dev", 3, {"/h/data/xpkgs/xim-x-tool/1/bin/tool", "/h/data/xpkgs/xim-x-other/2/bin/o"}},
        {"dev", 4, {"/h/data/xpkgs/xim-x-other/2/bin/o"}},
        // A sibling version whose name starts with the same text is not inside.
        {"ci", 1, {"/h/data/xpkgs/xim-x-tool/10/bin/tool"}},
    };
    auto none = st::pins(payload, roots, {});
    ASSERT_EQ(none.holders.size(), 1u);
    EXPECT_EQ(none.holders.front(), (st::Holder{"dev", 3}));
    EXPECT_TRUE(none.held());

    const std::vector<std::string> unreadable{"generation 7 of 'ci'"};
    auto unknown = st::pins("/h/data/xpkgs/xim-x-absent/1", roots, unreadable);
    EXPECT_TRUE(unknown.holders.empty());
    EXPECT_TRUE(unknown.held()) << "an unreadable generation might link into it";
    EXPECT_FALSE(st::pins("/h/data/xpkgs/xim-x-absent/1", roots, {}).held());
}

XTEST(Store, TheLedgerIsAbsentEmptyAndAnUnparseableOneReleasesNothing,
      .area = "store", .covers = {"ROOT-GC-ROOT"}) {
    auto home = tk::Home::isolated("store-ledger");
    const auto ledger = st::ledger_path(home.dir() / "data");
    auto empty = st::read_ledger(ledger);
    ASSERT_TRUE(empty) << empty.error();
    EXPECT_TRUE(empty->empty());

    ASSERT_TRUE(st::retain(ledger, {"/p/a", "a", "1", {}}));
    ASSERT_TRUE(st::retain(ledger, {"/p/b", "b", "2", {}}));
    ASSERT_TRUE(st::retain(ledger, {"/p/a", "a", "1", "2026-10-09T00:00:00Z"}));  // replaces
    auto two = st::read_ledger(ledger);
    ASSERT_TRUE(two) << two.error();
    ASSERT_EQ(two->size(), 2u);
    ASSERT_TRUE(st::forget(ledger, "/p/a"));
    ASSERT_TRUE(st::forget(ledger, "/p/never"));
    auto one = st::read_ledger(ledger);
    ASSERT_TRUE(one) << one.error();
    ASSERT_EQ(one->size(), 1u);
    EXPECT_EQ(one->front().target, "b");
    EXPECT_FALSE(one->front().since.empty());

    tk::write_file(ledger, "{ not json");
    EXPECT_FALSE(st::read_ledger(ledger));
    EXPECT_FALSE(st::retain(ledger, {"/p/c", "c", "3", {}})) << "a ledger that cannot be read is not rewritten";
}
