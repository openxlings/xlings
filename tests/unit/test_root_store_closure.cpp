#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.home.evidence;
import xlings.core.subos.store_closure;
import xlings.core.subos.root_view;
import xlings.subos.rootfs;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace closure = xlings::subos_root::store_closure;
namespace view = xlings::subos_root::root_view;
namespace evidence = xlings::home::evidence;
namespace rf = xlings::subos::rootfs;
using Json = nlohmann::json;

XTEST(RootStoreClosure, PrivateSkeletonExposesOnlyOptedInPayloadsAndMetadata, .area = "subos",
      .covers = {"ROOT-STORE-CLOSURE", "LUBAN-TOOL-SHIPPED"}) {
    if constexpr (!tk::is_posix)
        GTEST_SKIP() << "root projection uses directory symlinks";
    auto home = tk::Home::isolated("root-store-closure");
    const auto visible = fs::canonical(home.dir()) / "data/xpkgs/fixture-x-visible/1.0.0";
    const auto hidden = fs::canonical(home.dir()) / "data/xpkgs/fixture-x-hidden/2.0.0";
    const auto scope = fs::canonical(home.dir()) / "subos/box";
    tk::write_file(visible / "bin/visible", "visible program");
    tk::write_file(hidden / "private.txt", "another scope's payload");
    tk::write_file(visible / ".xlings-resolution.json",
                   R"({"package":"fixture:visible@1.0.0","deps":[]})");
    Json versions = {
        {"visible",
         {{"type", "program"},
          {"versions", {{"fixture:1.0.0", {{"path", visible.string() + "/bin"}}}}}}},
        {"hidden",
         {{"type", "program"}, {"versions", {{"fixture:2.0.0", {{"path", hidden.string()}}}}}}}};
    tk::write_file(fs::canonical(home.dir()) / ".xlings.json", Json{{"versions", versions}}.dump());
    // The entry and luban beside it: a root's /usr/bin/luban links there.
    tk::write_file(fs::canonical(home.dir()) / "bin/xlings", "entry");
    tk::write_file(fs::canonical(home.dir()) / "bin/luban", "luban");
    tk::write_file(
        scope / ".xlings.json",
        Json{{"workspace",
              {{"visible", {{"active", "fixture:1.0.0"}, {"installed", {"fixture:1.0.0"}}}}}}}
            .dump());
    rf::Inputs generation;
    generation.payloads = {visible};
    generation.programs = {{"visible", visible / "bin/visible"}};
    ASSERT_TRUE(rf::commit(scope, rf::plan(generation), "fixture"));
    auto inputs = closure::read_scope(fs::canonical(home.dir()), "box", scope / "rootfs");
    ASSERT_TRUE(inputs) << inputs.error();
    auto prepared = view::prepare(*inputs);
    ASSERT_TRUE(prepared) << prepared.error();
    const auto& bindings = (*prepared)->bindings();
    EXPECT_TRUE(std::ranges::any_of(bindings, [&](const auto& binding) {
        return binding.source == visible && binding.destination == visible;
    }));
    EXPECT_FALSE(std::ranges::any_of(bindings, [&](const auto& binding) {
        return binding.source == hidden || binding.source == fs::canonical(home.dir()) ||
               binding.source == fs::canonical(home.dir()) / "data/xpkgs";
    }));
    EXPECT_FALSE(fs::exists((*prepared)->private_instance().parent_path().parent_path() /
                            "data/xpkgs/fixture-x-hidden/2.0.0"));
    for (const auto* program : {"xlings", "luban"}) {
        const auto at = fs::canonical(home.dir()) / "bin" / program;
        EXPECT_TRUE(std::ranges::any_of(bindings, [&](const auto& binding) {
            return binding.source == at && binding.destination == at;
        })) << program << " is not in the root's view";
    }
    const auto primary = std::ranges::find_if(bindings, [&](const auto& binding) {
        return binding.destination == fs::canonical(home.dir()) / ".xlings.json";
    });
    ASSERT_NE(primary, bindings.end());
    const auto filtered = Json::parse(tk::read_file(primary->source));
    EXPECT_TRUE(filtered["versions"].contains("visible"));
    EXPECT_FALSE(filtered["versions"].contains("hidden"));
    (*prepared)->close();
    fs::remove(visible / ".xlings-resolution.json");
    EXPECT_FALSE(view::prepare(*inputs));
    EXPECT_EQ(tk::read_file(hidden / "private.txt"), "another scope's payload");
}

XTEST(RootStoreClosure, CheckedEvidenceTranslatesOnlyTheProvedRecordedHome, .area = "subos",
      .covers = {"ROOT-STORE-CLOSURE"}) {
    auto home = tk::Home::isolated("root-recorded-home");
    const auto recordedHome = fs::canonical(home.root()) / "recorded";
    const auto recordedSecond = recordedHome / "data/xpkgs/fixture-x-second/2.0.0";
    const auto first = fs::canonical(home.dir()) / "data/xpkgs/fixture-x-first/1.0.0";
    const auto second = fs::canonical(home.dir()) / "data/xpkgs/fixture-x-second/2.0.0";
    tk::write_file(second / "lib/libsecond.a", "library");
    tk::write_file(
        first / ".xlings-resolution.json",
        Json{{"package", "fixture:first@1.0.0"},
             {"deps",
              Json::array({{{"spec", "fixture:second@2.0.0"},
                            {"name", "fixture:second"},
                            {"version", "2.0.0"},
                            {"source", "fixture"},
                            {"install_dir", recordedSecond.string()},
                            {"libdirs", {(recordedSecond / "lib").string()}}}})}}
            .dump());
    auto owner = evidence::physical_store_root(fs::canonical(home.dir()), first);
    auto dependency = evidence::physical_store_root(fs::canonical(home.dir()), second);
    ASSERT_TRUE(owner);
    ASSERT_TRUE(dependency);
    owner->recordedHome = recordedHome;
    dependency->recordedHome = recordedHome;
    const auto resolve =
        [&](const fs::path& recorded) -> std::expected<evidence::OwnedPayload, std::string> {
        if (recorded != recordedSecond)
            return std::unexpected("path has no proved mapping");
        return *dependency;
    };
    auto result = evidence::read_checked_resolution(*owner, resolve);
    ASSERT_TRUE(result) << result.error();
    ASSERT_EQ(result->dependencies.size(), 1U);
    EXPECT_EQ(result->dependencies[0].installDir, fs::canonical(second));
    EXPECT_EQ(result->dependencies[0].libdirs[0], fs::canonical(second / "lib"));
    dependency->recordedHome.clear();
    EXPECT_FALSE(evidence::read_checked_resolution(*owner, resolve));
}

XTEST(RootStoreClosure, BorrowedHomeMetadataIsLimitedToTheCheckedScope, .area = "subos",
      .covers = {"ROOT-STORE-CLOSURE", "HOME-LAYER-RESOLVE"}) {
    if constexpr (!tk::is_posix)
        GTEST_SKIP() << "root projection uses directory symlinks";
    auto home = tk::Home::isolated("root-borrowed-closure");
    const auto system = fs::canonical(home.root()) / "system";
    const auto payload = system / "data/xpkgs/fixture-x-visible/1.0.0";
    const auto hidden = system / "data/xpkgs/fixture-x-hidden/2.0.0";
    const auto scope = fs::canonical(home.dir()) / "subos/box";
    tk::write_file(payload / "bin/visible", "borrowed tool");
    tk::write_file(hidden / "private.txt", "other source scope");
    tk::write_file(payload / ".xlings-resolution.json",
                   R"({"package":"fixture:visible@1.0.0","deps":[]})");
    tk::write_file(system / ".xlings-home", R"({"mode":"root","layout":"multi"})");
    Json versions = {
        {"visible",
         {{"type", "program"},
          {"versions", {{"fixture:1.0.0", {{"path", (payload / "bin").string()}}}}}}},
        {"hidden",
         {{"type", "program"}, {"versions", {{"fixture:2.0.0", {{"path", hidden.string()}}}}}}}};
    tk::write_file(system / ".xlings.json", Json{{"versions", versions}}.dump());
    tk::write_file(
        system / "subos/default/.xlings.json",
        Json{{"workspace",
              {{"visible", {{"active", "fixture:1.0.0"}, {"installed", {"fixture:1.0.0"}}}}}}}
            .dump());
    versions.erase("hidden");
    versions["visible"]["versions"]["fixture:1.0.0"]["layer"] = {{"home", system.string()},
                                                                 {"scope", "default"}};
    tk::write_file(fs::canonical(home.dir()) / ".xlings.json", Json{{"versions", versions}}.dump());
    tk::write_file(
        scope / ".xlings.json",
        Json{{"workspace",
              {{"visible", {{"active", "fixture:1.0.0"}, {"installed", {"fixture:1.0.0"}}}}}}}
            .dump());
    rf::Inputs generation;
    generation.payloads = {payload};
    generation.programs = {{"visible", payload / "bin/visible"}};
    ASSERT_TRUE(rf::commit(scope, rf::plan(generation), "borrowed fixture"));
    auto inputs = closure::read_scope(fs::canonical(home.dir()), "box", scope / "rootfs");
    ASSERT_TRUE(inputs) << inputs.error();
    auto prepared = view::prepare(*inputs);
    ASSERT_TRUE(prepared) << prepared.error();
    const auto& bindings = (*prepared)->bindings();
    EXPECT_TRUE(std::ranges::any_of(bindings, [&](const auto& binding) {
        return binding.source == payload && binding.destination == payload;
    }));
    EXPECT_FALSE(std::ranges::any_of(bindings, [&](const auto& binding) {
        return binding.source == system || binding.source == system / "data/xpkgs" ||
               binding.source == hidden;
    }));
    const auto primary = std::ranges::find_if(bindings, [&](const auto& binding) {
        return binding.destination == system / ".xlings.json";
    });
    ASSERT_NE(primary, bindings.end());
    const auto filtered = Json::parse(tk::read_file(primary->source));
    EXPECT_TRUE(filtered["versions"].contains("visible"));
    EXPECT_FALSE(filtered["versions"].contains("hidden"));
    (*prepared)->close();
    EXPECT_EQ(tk::read_file(hidden / "private.txt"), "other source scope");
}

// A link into another payload is that payload's file (libglvnd's
// lib/glx-vendor, which the graphics stack fills per host with Mesa's and
// NVIDIA's vendors): not a reason to refuse the root it is not in -- a second
// desktop system came out without its desktop so -- while the payload's own
// ELF naming a search path outside the closure still is.
XTEST(RootStoreClosure, ALinkIntoAnotherPayloadIsThatPayloadsNotThisOnes, .area = "subos",
      .covers = {"ROOT-STORE-CLOSURE"}) {
    if constexpr (!tk::is_posix)
        GTEST_SKIP() << "root projection uses directory symlinks";
    auto home = tk::Home::isolated("root-store-closure-links");
    const auto visible = fs::canonical(home.dir()) / "data/xpkgs/fixture-x-visible/1.0.0";
    const auto hidden = fs::canonical(home.dir()) / "data/xpkgs/fixture-x-hidden/2.0.0";
    const auto scope = fs::canonical(home.dir()) / "subos/box";
    tk::write_file(visible / "bin/visible", "visible program");
    tk::write_elf(hidden / "lib/libvendor.so.0", {.rpath = (hidden / "lib").string()});
    fs::create_directories(visible / "lib/glx-vendor");
    fs::create_symlink(hidden / "lib/libvendor.so.0", visible / "lib/glx-vendor/libvendor.so.0");
    tk::write_file(visible / ".xlings-resolution.json",
                   R"({"package":"fixture:visible@1.0.0","deps":[]})");
    Json versions = {
        {"visible",
         {{"type", "program"},
          {"versions", {{"fixture:1.0.0", {{"path", visible.string() + "/bin"}}}}}}}};
    tk::write_file(fs::canonical(home.dir()) / ".xlings.json", Json{{"versions", versions}}.dump());
    tk::write_file(fs::canonical(home.dir()) / "bin/xlings", "entry");
    tk::write_file(fs::canonical(home.dir()) / "bin/luban", "luban");
    tk::write_file(
        scope / ".xlings.json",
        Json{{"workspace",
              {{"visible", {{"active", "fixture:1.0.0"}, {"installed", {"fixture:1.0.0"}}}}}}}
            .dump());
    rf::Inputs generation;
    generation.payloads = {visible};
    generation.programs = {{"visible", visible / "bin/visible"}};
    ASSERT_TRUE(rf::commit(scope, rf::plan(generation), "fixture"));
    auto inputs = closure::read_scope(fs::canonical(home.dir()), "box", scope / "rootfs");
    ASSERT_TRUE(inputs) << inputs.error();
    {
        auto prepared = view::prepare(*inputs);
        ASSERT_TRUE(prepared) << prepared.error();
        (*prepared)->close();
    }
    // Its own library with the same search path is still refused.
    tk::write_elf(visible / "lib/libown.so.0", {.rpath = (hidden / "lib").string()});
    auto refused = view::prepare(*inputs);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find("outside its recorded root closure"), std::string::npos) << refused.error();
}

XTEST(RootStoreClosure, AMemberTheScopeNeverTookIsNotProjectedNorABlock, .area = "subos",
      .covers = {"ROOT-STORE-CLOSURE"}) {
    // Systems sharing a home's store: another one installed a later revision
    // of the same release, which has a member this scope never took (J38: a
    // desktop's root could no longer be updated at all). The scope's root is
    // what it opted into -- the member is not projected, and not a refusal.
    if constexpr (!tk::is_posix)
        GTEST_SKIP() << "root projection uses directory symlinks";
    auto home = tk::Home::isolated("root-store-closure-member");
    const auto payload = fs::canonical(home.dir()) / "data/xpkgs/fixture-x-visible/1.0.0";
    const auto scope = fs::canonical(home.dir()) / "subos/box";
    tk::write_file(payload / "bin/visible", "visible program");
    tk::write_file(payload / "share/applications/visible.desktop", "[Desktop Entry]");
    tk::write_file(payload / ".xlings-resolution.json", R"({"package":"fixture:visible@1.0.0","deps":[]})");
    const Json group{{"group", "visible"}, {"provider", "fixture:visible"}, {"rootTarget", "visible"},
                     {"rootVersion", "1.0.0"}, {"version", "1.0.0"}};
    Json versions = {
        {"visible",
         {{"type", "program"}, {"filename", "visible"},
          {"bindings", {{"visible.files.1", {{"1.0.0", "1.0.0"}}}}},
          {"versions", {{"1.0.0", {{"bindingGroup", group},
                                   {"bindingMembers", {{"visible", "1.0.0"}, {"visible.files.1", "1.0.0"}}},
                                   {"kind", "program"}, {"path", payload.string() + "/bin"},
                                   {"sourceName", "visible"}, {"destinationName", "visible"}}}}}}},
        {"visible.files.1",
         {{"type", "files"},
          {"bindings", {{"visible", {{"1.0.0", "1.0.0"}}}}},
          {"versions", {{"1.0.0", {{"bindingGroup", group}, {"kind", "files"}, {"path", payload.string()},
                                   {"fileSrc", "share/applications"}, {"fileDst", "share/applications"}}}}}}}};
    tk::write_file(fs::canonical(home.dir()) / ".xlings.json", Json{{"versions", versions}}.dump());
    tk::write_file(fs::canonical(home.dir()) / "bin/xlings", "entry");
    tk::write_file(scope / ".xlings.json",
                   Json{{"workspace", {{"visible", {{"active", "1.0.0"}, {"installed", {"1.0.0"}}}}}}}.dump());
    rf::Inputs generation;
    generation.payloads = {payload};
    generation.programs = {{"visible", payload / "bin/visible"}};
    ASSERT_TRUE(rf::commit(scope, rf::plan(generation), "fixture"));
    auto inputs = closure::read_scope(fs::canonical(home.dir()), "box", scope / "rootfs");
    ASSERT_TRUE(inputs) << inputs.error();
    auto collected = closure::collect(*inputs);
    ASSERT_TRUE(collected) << collected.error();
    auto prepared = view::prepare(*inputs);
    ASSERT_TRUE(prepared) << prepared.error();
    const auto primary = std::ranges::find_if((*prepared)->bindings(), [&](const auto& binding) {
        return binding.destination == fs::canonical(home.dir()) / ".xlings.json";
    });
    ASSERT_NE(primary, (*prepared)->bindings().end());
    const auto filtered = Json::parse(tk::read_file(primary->source));
    EXPECT_TRUE(filtered["versions"].contains("visible"));
    EXPECT_FALSE(filtered["versions"].contains("visible.files.1")) << "not this scope's: " << filtered.dump();
    (*prepared)->close();
}
