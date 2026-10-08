#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.core.home.prefix_domain;
import xlings.core.home;
import xlings.core.home.domain_producer;
import xlings.libs.json;
import xlings.subos.rootfs;
import xlings.subos.home_view;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace d = xlings::home::prefix_domain;
namespace producer = xlings::home::domain_producer;

XTEST(PrefixDomain, SameOwnerStillRejectsInvalidHomeAuthority, .area = "home",
      .covers = {"DOM-LAYOUTS"}) {
    auto home = tk::Home::isolated("domain-owner-marker");
    const auto owner = fs::canonical(home.dir());
    tk::write_file(owner / ".xlings-home", R"({"mode":"root","layout":"invalid"})");
    EXPECT_FALSE(d::resolve(owner, owner));
    tk::write_file(owner / ".xlings-home", R"({"mode":"unknown","layout":2})");
    EXPECT_FALSE(d::resolve(owner, owner));
    tk::write_file(owner / ".xlings-home", R"({"mode":"root","layout":"single"})");
    auto valid = d::resolve(owner, owner);
    ASSERT_TRUE(valid) << valid.error();
    EXPECT_FALSE(valid->privateHome);
    EXPECT_EQ(valid->physicalHome, owner);
    EXPECT_EQ(valid->layout, "single");
}

XTEST(PrefixDomain, DistinctOwnersReserveIndependentDomainsWithoutHostWrites, .area = "home",
      .covers = {"DOM-LAYOUTS", "DOM-BUILD-INSIDE"}, .requires_ = {"posix"}) {
    auto first = tk::Home::isolated("domain-first");
    auto second = tk::Home::isolated("domain-second");
    const auto missing = first.root() / "absent-system";
    auto one = d::resolve(first.dir(), "/xlings", missing);
    auto two = d::resolve(second.dir(), "/xlings", missing);
    ASSERT_TRUE(one) << one.error(); ASSERT_TRUE(two) << two.error();
    ASSERT_TRUE(d::prepare_private(*one)); ASSERT_TRUE(d::prepare_private(*two));
    EXPECT_NE(one->physicalHome, two->physicalHome);
    EXPECT_EQ(one->logicalHome, two->logicalHome);
    EXPECT_FALSE(fs::exists(missing));
    EXPECT_TRUE(d::parse(d::serialize(*one), first.dir()));
    EXPECT_FALSE(d::parse(d::serialize(*one), second.dir()));
    EXPECT_EQ(nlohmann::json::parse(tk::read_file(one->physicalHome / ".xlings-domain.json"))["owner_home"],
              fs::canonical(first.dir()).generic_string());
}

XTEST(PrefixDomain, UnknownPrivateTreesAndRedirectedParentsArePreserved, .area = "home",
      .covers = {"DOM-LAYOUTS"}, .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("domain-unknown");
    const auto missing = home.root() / "absent-system";
    const auto privateHome = home.dir() / "domains/xlings/private";
    tk::write_file(privateHome / "sentinel", "user-owned");
    EXPECT_FALSE(d::resolve(home.dir(), "/xlings", missing));
    EXPECT_EQ(tk::read_file(privateHome / "sentinel"), "user-owned");
    if constexpr (!tk::is_windows) {
        auto other = tk::Home::isolated("domain-linked-parent");
        fs::create_directory_symlink(home.dir() / "domains", other.dir() / "domains");
        EXPECT_FALSE(d::resolve(other.dir(), "/xlings", missing));
        EXPECT_EQ(tk::read_file(privateHome / "sentinel"), "user-owned");
    }
}

XTEST(PrefixDomain, SystemSourceIsCheckedReadOnlyAndPinnedAgainstDrift, .area = "home",
      .covers = {"DOM-LAYOUTS", "HOME-LAYER-RESOLVE"}, .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("domain-system-proof");
    const auto system = home.root() / "system";
    tk::write_file(system / ".xlings-home", R"({"mode":"multi","layout":2})");
    tk::write_file(system / "sentinel", "system payload");
    auto domain = d::resolve(home.dir(), "/xlings", system);
    ASSERT_TRUE(domain) << domain.error();
    ASSERT_TRUE(domain->systemSource);
    EXPECT_EQ(*domain->systemSource, fs::canonical(system));
    ASSERT_TRUE(d::prepare_private(*domain));
    EXPECT_EQ(tk::read_file(system / "sentinel"), "system payload");
    auto marker = nlohmann::json::parse(tk::read_file(domain->physicalHome / ".xlings-domain.json"));
    marker["system_source"] = (home.root() / "unrelated").generic_string();
    tk::write_file(domain->physicalHome / ".xlings-domain.json", marker.dump());
    EXPECT_FALSE(d::resolve(home.dir(), "/xlings", system));
    EXPECT_EQ(tk::read_file(system / "sentinel"), "system payload");
    tk::write_file(system / ".xlings-home", R"({"mode":"root","layout":42})");
    EXPECT_FALSE(d::resolve(home.dir(), "/xlings", system));
}

XTEST(PrefixDomain, TypedMappingsRejectTraversalAndKeepPhysicalAuthority, .area = "home",
      .covers = {"DOM-PREFIX", "DOM-LAYOUTS"}, .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("domain-maps");
    auto domain = d::resolve(home.dir(), "/xlings", home.root() / "absent-system");
    ASSERT_TRUE(domain) << domain.error();
    EXPECT_EQ(*d::map_host(*domain, "/xlings/data/xpkgs/tool/1"), domain->physicalHome / "data/xpkgs/tool/1");
    EXPECT_EQ(*d::map_guest(*domain, domain->physicalHome / "data/xpkgs/tool/1"), fs::path("/xlings/data/xpkgs/tool/1"));
    EXPECT_FALSE(d::map_host(*domain, "/xlings/../etc"));
    EXPECT_FALSE(d::map_host(*domain, "/xlings-foreign/data"));
    EXPECT_FALSE(d::map_host(*domain, "relative"));
    EXPECT_FALSE(d::map_guest(*domain, home.dir() / "data/xpkgs/tool/1"));
}

XTEST(PrefixDomain, ProducerCannotPublishAnUncommittedOrForeignControlScope, .area = "home",
      .covers = {"DOM-BUILD-INSIDE"}, .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("domain-control-proof");
    auto domain = d::resolve(home.dir(), "/xlings", home.root() / "absent-system");
    ASSERT_TRUE(domain); ASSERT_TRUE(d::prepare_private(*domain));
    const auto actual = domain->physicalHome / "subos/demo";
    tk::write_file(actual / ".xlings.json", R"({"workspace":{}})");
    tk::write_file(xlings::subos::HomeView{domain->physicalHome}.instance_file("demo"), R"({"kind":"rootfs"})");
    EXPECT_FALSE(producer::publish_scope(*domain, "demo"));
    EXPECT_FALSE(fs::exists(home.dir() / "subos/demo"));
    auto committed = xlings::subos::rootfs::commit(actual, {}, "domain unit fixture");
    ASSERT_TRUE(committed) << committed.error();
    ASSERT_TRUE(producer::publish_scope(*domain, "demo"));
    auto scope = producer::read_scope(home.dir(), "demo");
    ASSERT_TRUE(scope) << scope.error(); ASSERT_TRUE(*scope);
    EXPECT_EQ((**scope).producerInstance, actual);
    EXPECT_EQ((**scope).logicalInstance, fs::path("/xlings/subos/demo"));
    EXPECT_FALSE(producer::publish_scope(*domain, "demo"));
    tk::write_file(actual / ".xlings.json", "{corrupt");
    EXPECT_FALSE(producer::read_scope(home.dir(), "demo"));
}

XTEST(PrefixDomain, KnownLegacyRootLayoutMigratesWithoutAcceptingUnknownNumbers, .area = "home",
      .covers = {"DOM-LAYOUTS"}) {
    auto home = tk::Home::isolated("domain-legacy-layout");
    for (const auto* layout : {"multi", "single"}) {
        tk::write_file(home.dir() / ".xlings-home", nlohmann::json{{"mode", "root"}, {"layout", 2},
            {"root_layout", layout}, {"future", 42}}.dump());
        auto shared = xlings::home::shares_store(home.dir());
        ASSERT_TRUE(shared) << shared.error();
        EXPECT_EQ(*shared, std::string_view(layout) == "multi");
        auto sameOwner = d::resolve(home.dir(), fs::canonical(home.dir()));
        ASSERT_TRUE(sameOwner) << sameOwner.error();
        ASSERT_TRUE(xlings::home::declare(home.dir(), std::nullopt, 2));
        const auto migrated = nlohmann::json::parse(tk::read_file(home.dir() / ".xlings-home"));
        EXPECT_EQ(migrated["layout"], layout);
        EXPECT_EQ(migrated["future"], 42);
    }
    for (auto marker : {nlohmann::json{{"mode", "root"}, {"layout", 3}, {"root_layout", "multi"}},
                        nlohmann::json{{"mode", "root"}, {"layout", 2}},
                        nlohmann::json{{"mode", "root"}, {"layout", "single"}, {"root_layout", "multi"}}}) {
        tk::write_file(home.dir() / ".xlings-home", marker.dump());
        EXPECT_FALSE(xlings::home::shares_store(home.dir()));
        EXPECT_FALSE(d::resolve(home.dir(), fs::canonical(home.dir())));
    }
}
