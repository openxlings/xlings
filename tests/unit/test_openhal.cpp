// openhal: the device source of the machine a test runs on passes the
// conformance check, and the check refuses what breaks the specification
// (each clause has a source that breaks it, so a check that holds nothing
// cannot pass).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import openhal;
import openhal.conformance;
import openhal.host;

namespace {

using openhal::Device;
using openhal::Kind;
using openhal::Listing;

// A source with a fixed answer per kind; `flip` changes the ids between calls.
struct Fixed {
    std::map<Kind, Listing> answers;
    mutable int calls { 0 };
    bool flip { false };
    std::string label { "fixed" };
    std::string_view name() const { return label; }
    Listing list(Kind k) const {
        ++calls;
        auto it = answers.find(k);
        if (it == answers.end()) return std::vector<Device>{};
        if (flip && it->second)
            if (auto copy = *it->second; !copy.empty()) {
                copy.front().id += std::to_string(calls);
                return copy;
            }
        return it->second;
    }
};

bool failed_(const openhal::conformance::Report& r, std::string_view clause) {
    return std::ranges::any_of(r.failed, [&](const std::string& f) { return f.starts_with(clause); });
}

}  // namespace

static_assert(openhal::Source<openhal::host::Host>);
static_assert(openhal::Source<Fixed>);
static_assert(!openhal::Source<int>);

XTEST(OpenHal, ThisMachinesSourceConforms, .area = "luban", .covers = {"HAL-DEVICES"}) {
    const openhal::host::Host here;
    const auto report = openhal::conformance::check(here);
    for (const auto& f : report.failed) ADD_FAILURE() << f;
    EXPECT_GT(report.held, 0);
    EXPECT_FALSE(here.name().empty());
    // Every kind answers: devices, none, or a reason.
    for (const auto kind : openhal::kKinds) {
        const auto l = here.list(kind);
        if (!l) EXPECT_FALSE(l.error().reason.empty()) << openhal::name_of(kind);
    }
}

XTEST(OpenHal, TheCheckRefusesEachBrokenClause, .area = "luban", .covers = {"HAL-DEVICES"}) {
    namespace c = openhal::conformance;
    const Device gpu{Kind::Gpu, "pci:0000:01:00.0", "A GPU", "nvidia", {}};
    {
        Fixed good;
        good.answers[Kind::Gpu] = std::vector<Device>{gpu};
        good.answers[Kind::Input] = std::unexpected(openhal::Unreadable{"not here"});
        EXPECT_TRUE(c::check(good).ok());
    }
    {
        Fixed nameless;
        nameless.label.clear();
        EXPECT_TRUE(failed_(c::check(nameless), "4.1"));
    }
    {
        Fixed silent;
        silent.answers[Kind::Display] = std::unexpected(openhal::Unreadable{""});
        EXPECT_TRUE(failed_(c::check(silent), "4.2"));
    }
    {
        Fixed wrongKind;
        wrongKind.answers[Kind::Display] = std::vector<Device>{gpu};
        EXPECT_TRUE(failed_(c::check(wrongKind), "4.3"));
    }
    {
        Fixed anonymous;
        anonymous.answers[Kind::Gpu] = std::vector<Device>{Device{Kind::Gpu, "", "", {}, {}}};
        EXPECT_TRUE(failed_(c::check(anonymous), "4.4"));
    }
    {
        Fixed twice;
        twice.answers[Kind::Gpu] = std::vector<Device>{gpu, gpu};
        EXPECT_TRUE(failed_(c::check(twice), "4.5"));
    }
    {
        Fixed unstable;
        unstable.answers[Kind::Gpu] = std::vector<Device>{gpu};
        unstable.flip = true;
        EXPECT_TRUE(failed_(c::check(unstable), "4.6"));
    }
}
