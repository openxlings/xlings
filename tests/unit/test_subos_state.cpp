#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.subos.root;
import xlings.subos.boot;
import xlings.subos.stage0;
import xlings.subos.home_view;
import xlings.subos.rootfs;
import xlings.subos.roles;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace sr = xlings::subos_root;
namespace bt = xlings::subos::boot;
namespace st = xlings::subos::stage0;
namespace rf = xlings::subos::rootfs;
namespace rl = xlings::subos::roles;
using xlings::subos::HomeView;

XTEST(SubosState, OnlyMissingLegacyKindDefaultsToViewAndUpdatesKeepUnknownKeys,
      .area = "subos", .covers = {"STATE-STRICT", "STATE-KEEP-UNKNOWN"}) {
    auto home = tk::Home::isolated("kind-state");
    const auto file = HomeView{home.dir()}.instance_file("box");
    EXPECT_EQ(sr::read_kind(home.dir(), "box").value(), rl::Kind::View);
    tk::write_file(file, R"({"future":{"feature":true}})");
    EXPECT_EQ(sr::read_kind(home.dir(), "box").value(), rl::Kind::View);
    ASSERT_TRUE(sr::declare_kind(home.dir(), "box", rl::Kind::Rootfs).has_value());
    const auto document = nlohmann::json::parse(tk::read_file(file));
    EXPECT_EQ(document["future"]["feature"], true);
    EXPECT_EQ(sr::read_kind(home.dir(), "box").value(), rl::Kind::Rootfs);
    for (auto bad : {"{broken", "[]", R"({"kind":7})", R"({"kind":null})", R"({"kind":"future-root"})"}) {
        tk::write_file(file, bad);
        const auto read = sr::read_kind(home.dir(), "box");
        EXPECT_FALSE(read.has_value()) << bad;
        EXPECT_FALSE(sr::declare_kind(home.dir(), "box", rl::Kind::View).has_value()) << bad;
        EXPECT_EQ(tk::read_file(file), bad);
    }
}

XTEST(SubosState, PresentUnreadableAndDanglingMetadataCannotBecomeMissingDefaults,
      .area = "subos", .covers = {"STATE-STRICT"}) {
    auto home = tk::Home::isolated("unreadable-state");
    const auto file = HomeView{home.dir()}.instance_file("box");
    fs::create_directories(file);
    EXPECT_FALSE(sr::read_kind(home.dir(), "box").has_value());
    EXPECT_FALSE(sr::declare_kind(home.dir(), "box", rl::Kind::View).has_value());
    EXPECT_TRUE(fs::is_directory(file));
    fs::remove(file);
    if constexpr (tk::is_posix) {
        fs::create_symlink("missing-instance", file);
        EXPECT_FALSE(sr::read_kind(home.dir(), "box").has_value());
        EXPECT_FALSE(sr::declare_kind(home.dir(), "box", rl::Kind::View).has_value());
        EXPECT_EQ(fs::read_symlink(file), fs::path("missing-instance"));
    }
    const auto boot = home.dir() / "boot.json";
    fs::create_directory(boot);
    EXPECT_FALSE(bt::load(boot).has_value());
    EXPECT_FALSE(bt::save(boot, bt::Config{}).has_value());
    EXPECT_TRUE(fs::is_directory(boot));
    fs::remove(boot);
    if constexpr (tk::is_posix) {
        fs::create_symlink("missing-boot", boot);
        EXPECT_FALSE(bt::load(boot).has_value());
        EXPECT_FALSE(bt::save(boot, bt::Config{}).has_value());
        EXPECT_EQ(fs::read_symlink(boot), fs::path("missing-boot"));
    }
}

XTEST(SubosState, BootUpdatesPreserveUnknownTopLevelAndLastFieldsFromTheCurrentFile,
      .area = "subos", .covers = {"STATE-KEEP-UNKNOWN", "BOOT-ONCE-FALLBACK"}) {
    auto home = tk::Home::isolated("boot-unknown");
    const auto file = home.dir() / "boot.json";
    tk::write_file(file, R"({"default":"core","fallback":"tiny","once":"trial", "future":{"v":2},"last":{"subos":"core","via":"default","good":true,"future-last":17}})");
    const auto loaded = bt::load(file);
    ASSERT_TRUE(loaded.has_value());
    auto document = nlohmann::json::parse(tk::read_file(file));
    document["added-after-load"] = "keep";
    tk::write_file(file, document.dump());
    auto consumed = bt::record_boot(*loaded, {"trial", "once"});
    ASSERT_TRUE(bt::save(file, consumed).has_value());
    document = nlohmann::json::parse(tk::read_file(file));
    EXPECT_EQ(document["future"]["v"], 2);
    EXPECT_EQ(document["added-after-load"], "keep");
    EXPECT_EQ(document["last"]["future-last"], 17);
    EXPECT_EQ(document["last"]["subos"], "trial");
    EXPECT_TRUE(document["once"].is_null());
    ASSERT_TRUE(bt::save(file, bt::mark_good(consumed)).has_value());
    document = nlohmann::json::parse(tk::read_file(file));
    EXPECT_EQ(document["last"]["future-last"], 17);
    EXPECT_EQ(document["last"]["good"], true);
}

XTEST(SubosState, InvalidBootTypesAreRejectedAndSaveLeavesTheDocumentUntouched,
      .area = "subos", .covers = {"STATE-STRICT"}) {
    auto home = tk::Home::isolated("boot-invalid");
    const auto file = home.dir() / "boot.json";
    EXPECT_EQ(bt::load(file)->default_entry, "default");
    for (auto bad : {"{broken", "[]", R"({"default":7})", R"({"fallback":null})",
                    R"({"once":false})", R"({"default":"../outside"})", R"({"tries":[]})",
                    R"({"tries":{"box":-1}})", R"({"tries":{"box":1.5}})",
                    R"({"tries":{"box":2147483648}})", R"({"last":null})",
                    R"({"last":{"subos":7}})", R"({"last":{"good":"true"}})",
                    R"({"last":{"via":"unknown"}})"}) {
        tk::write_file(file, bad);
        EXPECT_FALSE(bt::load(file).has_value()) << bad;
        EXPECT_FALSE(bt::save(file, bt::Config{}).has_value()) << bad;
        EXPECT_EQ(tk::read_file(file), bad);
    }
}

XTEST(SubosState, StageZeroRefusesInvalidAnchorsAndDeclaredInitInsteadOfGuessing,
      .area = "subos", .covers = {"STATE-STRICT", "BOOT-STAGE0"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "stage-0 is a Linux boot boundary";
    auto home = tk::Home::isolated("stage0-state");
    const auto anchor = home.root() / "anchor.json";
    EXPECT_EQ(st::read_home_anchor(anchor).value(), fs::path("/xlings"));
    for (auto bad : {"{broken", "[]", "{}", R"({"home":7})", R"({"home":""})",
                    R"({"home":"relative"})", R"({"home":"/xlings/../other"})"}) {
        tk::write_file(anchor, bad);
        EXPECT_FALSE(st::read_home_anchor(anchor).has_value()) << bad;
    }
    tk::write_file(anchor, R"({"home":"/xlings","future":true})");
    EXPECT_EQ(st::read_home_anchor(anchor).value(), fs::path("/xlings"));
    fs::remove(anchor);
    fs::create_symlink("missing-anchor", anchor);
    EXPECT_FALSE(st::read_home_anchor(anchor).has_value());

    const HomeView view{home.dir()};
    const auto init = home.root() / "init";
    tk::write_file(init, "#!/bin/sh\n");
    fs::permissions(init, fs::perms::owner_all);
    const rf::Plan plan{{{"usr/bin/init", init, "test"}}, {}};
    ASSERT_TRUE(rf::commit(view.instance("box"), plan, "test init").has_value());
    EXPECT_EQ(st::init_of(view, "box").value(), std::optional<fs::path>{"/sbin/init"});
    for (auto bad : {"{broken", "[]", R"({"init":7})", R"({"init":""})",
                    R"({"init":"relative"})", R"({"init":"/usr/../bin/init"})"}) {
        tk::write_file(view.instance_file("box"), bad);
        EXPECT_FALSE(st::init_of(view, "box").has_value()) << bad;
    }
    tk::write_file(view.instance_file("box"), R"({"init":"/usr/bin/init"})");
    EXPECT_EQ(st::init_of(view, "box").value(), std::optional<fs::path>{"/usr/bin/init"});
    fs::remove(init);
    EXPECT_FALSE(st::init_of(view, "box").has_value()) << "a dangling declared init is unreadable, not absent";
    fs::remove(view.instance_file("box"));
    fs::create_symlink("missing-instance", view.instance_file("box"));
    EXPECT_FALSE(st::init_of(view, "box").has_value());
}
