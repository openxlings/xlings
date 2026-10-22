// openbus's model and wire (luban/specs/openbus/SPEC.md): paths and
// patterns, frames, the fields codec, envelopes, region references, the
// histogram a router reports with.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import openbus;

namespace ob = openbus;

XTEST(Openbus, PathsAndPatterns, .area = "luban", .covers = {"OPENBUS-MODEL"}) {
    EXPECT_TRUE(ob::valid_path("host/h1/system/desk/service/xdesk/state"));
    EXPECT_FALSE(ob::valid_path("host/H1"));
    EXPECT_FALSE(ob::valid_path("host//x"));
    EXPECT_FALSE(ob::valid_path("host/*/x")) << "a wildcard is a pattern's, never an offer's";
    EXPECT_TRUE(ob::valid_pattern("host/*/system/**"));
    EXPECT_TRUE(ob::matches("host/*/topic/x", "host/h1/topic/x"));
    EXPECT_FALSE(ob::matches("host/*/topic/x", "host/h1/system/a/topic/x"));
    EXPECT_TRUE(ob::matches("host/**/topic/x", "host/h1/system/a/topic/x"));
    EXPECT_TRUE(ob::matches("host/**/topic/x", "host/topic/x")) << "** takes zero segments too";
    EXPECT_TRUE(ob::matches("**", "a/b/c"));
    EXPECT_FALSE(ob::matches("a/*", "a"));
    EXPECT_EQ(ob::endpoint_kind("device"), ob::EndpointKind::device);
}

XTEST(Openbus, FramesRoundTripAndAStreamIsCutAnywhere, .area = "luban", .covers = {"OPENBUS-MODEL"}) {
    ob::Frame f{.kind = static_cast<std::uint8_t>(ob::Kind::reply), .flags = ob::with_priority(0, ob::Priority::bulk),
                .codec = 1, .channel = 77, .seq = 5, .trace = 0xdeadbeef, .payload = "{\"ok\":true}"};
    const auto bytes = ob::encode(f) + ob::encode(ob::Frame{.kind = static_cast<std::uint8_t>(ob::Kind::reply_end), .channel = 77});
    EXPECT_EQ(bytes.substr(0, 2), "OB");
    ob::Decoder d;
    std::vector<ob::Frame> got;
    for (char c : bytes) {   // a byte at a time: frames are whole however the stream is cut
        d.feed(std::string_view(&c, 1));
        while (auto n = d.next()) {
            ASSERT_TRUE(n.has_value());
            if (!*n) break;
            got.push_back(std::move(**n));
        }
    }
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].payload, f.payload);
    EXPECT_EQ(got[0].channel, 77u);
    EXPECT_EQ(got[0].trace, 0xdeadbeefu);
    EXPECT_EQ(ob::priority_of(got[0].flags), ob::Priority::bulk);
    EXPECT_TRUE(got[1].is(ob::Kind::reply_end));
    ob::Decoder bad;
    bad.feed("XX" + std::string(30, '\0'));
    EXPECT_EQ(bad.next().error(), ob::WireError::bad_magic);
    auto huge = ob::encode_header(f, static_cast<std::uint32_t>(ob::kMaxPayload + 1));
    ob::Decoder big;
    big.feed(std::string_view(huge.data(), huge.size()));
    EXPECT_EQ(big.next().error(), ob::WireError::too_large);
    EXPECT_EQ(ob::name_of(static_cast<ob::Kind>(200)), "?");
}

XTEST(Openbus, FieldsAndEnvelopes, .area = "luban", .covers = {"OPENBUS-MODEL"}) {
    ob::Fields f{{"path", "service/x"}, {"feature", "regions"}, {"feature", "direct"}};
    f.add("n", 42);
    const auto back = ob::Fields::decode(f.encode());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->value("path"), "service/x");
    EXPECT_EQ(back->all("feature").size(), 2u);
    EXPECT_EQ(back->number("n"), 42);
    EXPECT_FALSE(ob::Fields::decode(std::string("\x05\x00" "ab", 4)).has_value()) << "a cut field is malformed";
    const auto payload = ob::address(ob::Fields{{"path", "service/x"}}, std::string("\0body", 5));
    const auto env = ob::open_envelope(payload);
    ASSERT_TRUE(env.has_value());
    EXPECT_EQ(env->fields.value("path"), "service/x");
    EXPECT_EQ(env->body, std::string_view("\0body", 5));
    EXPECT_TRUE(ob::addressed(static_cast<std::uint8_t>(ob::Kind::publish)));
    EXPECT_FALSE(ob::addressed(static_cast<std::uint8_t>(ob::Kind::reply)));
}

XTEST(Openbus, RegionsTasksAndTheHistogram, .area = "luban", .covers = {"OPENBUS-MODEL"}) {
    const ob::RegionRef r{.id = 3, .offset = 1ull << 40, .length = 8294400, .version = 9};
    const auto back = ob::decode_region_ref(ob::encode(r));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->offset, r.offset);
    EXPECT_EQ(back->length, r.length);
    EXPECT_EQ(ob::region_kind("cuda-ipc"), ob::RegionKind::cuda_ipc);
    EXPECT_EQ(ob::coherence("owned"), ob::Coherence::owned);
    EXPECT_EQ(ob::task_state("input-required"), ob::TaskState::input_required);
    EXPECT_TRUE(ob::finished(ob::TaskState::canceled));
    EXPECT_TRUE(ob::valid_effect("irreversible"));
    EXPECT_FALSE(ob::valid_effect("dangerous"));
    ob::Histogram h;
    for (int k = 0; k < 99; ++k) h.add(1000);    // ~1 µs
    h.add(1'000'000);                             // ~1 ms
    EXPECT_EQ(h.count(), 100u);
    EXPECT_EQ(h.percentile(0.5), 1024u);
    EXPECT_GE(h.percentile(1.0), 1'000'000u);
    const auto again = ob::Histogram::decode(h.encode());
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->count(), 100u);
}
