// A neutral identity's persona (Luban design §C4): made once, then the same;
// never remade from a file it cannot read.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.subos.home_view;
import xlings.subos.persona;
import xlings.subos.policy;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace persona = xlings::subos::persona;
namespace pol = xlings::subos::policy;
using xlings::subos::HomeView;

XTEST(Persona, IsMadeOnceAndThenTheSame, .area = "subos", .covers = {"PRIVACY-PERSONA-STABLE"}) {
    auto home = tk::Home::isolated("persona");
    const HomeView view{home.dir()};
    auto first = persona::read_or_make(view, "agent");
    ASSERT_TRUE(first) << first.error();
    EXPECT_EQ(first->hostname.size(), 12u);
    EXPECT_EQ(first->machine_id.size(), 32u);
    auto again = persona::read_or_make(view, "agent");
    ASSERT_TRUE(again) << again.error();
    EXPECT_EQ(again->hostname, first->hostname);
    EXPECT_EQ(again->machine_id, first->machine_id);
    auto other = persona::read_or_make(view, "other");
    ASSERT_TRUE(other);
    EXPECT_NE(other->hostname, first->hostname) << "two instances, two personas";

    tk::write_file(view.persona_file("agent"), "{not json");
    EXPECT_FALSE(persona::read_or_make(view, "agent")) << "could not read is not a reason to become someone else";
    EXPECT_EQ(tk::read_file(view.persona_file("agent")), "{not json");
}

XTEST(Persona, AZoneIsUtcAnIanaNameOrNothing, .area = "subos", .covers = {"PRIVACY-TZ-PROXY"}) {
    EXPECT_EQ(persona::normalize_zone("utc"), "UTC");
    EXPECT_EQ(persona::normalize_zone("Asia/Tokyo"), "Asia/Tokyo");
    EXPECT_EQ(persona::normalize_zone("America/Argentina/Buenos_Aires"), "America/Argentina/Buenos_Aires");
    EXPECT_EQ(persona::normalize_zone("Etc/GMT+3"), "Etc/GMT+3");
    EXPECT_EQ(persona::normalize_zone("../../etc/passwd"), "");
    EXPECT_EQ(persona::normalize_zone("Asia/Tokyo; rm -rf"), "");
    EXPECT_EQ(persona::normalize_zone("<html>"), "");
}

XTEST(Persona, AnUnchosenZoneIsNotWrittenAsUtc, .area = "subos", .covers = {"PRIVACY-TZ-PROXY"}) {
    auto p = pol::preset(pol::Preset::Private);
    auto j = pol::to_json(p);
    EXPECT_FALSE(j["isolation"]["identity"].contains("tz"))
        << "an unchosen zone follows the network; written as UTC it becomes a choice nobody made\n" << j.dump();
    p.tz = "proxy";
    p.geo_lookup = "https://example.invalid/tz";
    j = pol::to_json(p);
    EXPECT_EQ(j["isolation"]["identity"]["tz"], "proxy");
    EXPECT_EQ(j["isolation"]["identity"]["geo_lookup"], "https://example.invalid/tz");
}
