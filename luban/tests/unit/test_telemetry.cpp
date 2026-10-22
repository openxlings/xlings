// The telemetry kit (luban/kits/telemetry; design 2026-10-22 §8.1): metrics
// by name are exact under threads and their quantiles bounded; the flight
// recorder keeps the last records in order; the log writes one object a
// line, filters by level, rotates, and the recorder still holds what the
// file's level left out; spans feed histograms and, while profiling, a
// trace; a dump gathers everything and names a provider that failed;
// `control` is the whole interface.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import telemetry;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = telemetry::Json;

namespace {

std::vector<Json> lines_of(const fs::path& p) {
    std::ifstream in(p);
    std::vector<Json> out;
    for (std::string line; std::getline(in, line);) out.push_back(Json::parse(line));
    return out;
}

}  // namespace

XTEST(Telemetry, MetricsAreExactUnderThreadsAndQuantilesBounded, .area = "luban", .covers = {"TELEMETRY-METRICS"}) {
    telemetry::Registry r;
    auto& c = r.counter("t.calls");
    auto& h = r.histogram("t.latency");
    auto& g = r.gauge("t.queue");
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < 8; ++t)
            ts.emplace_back([&] {
                for (std::uint64_t k = 1; k <= 10'000; ++k) {
                    c.add();
                    h.record(k);
                    g.add(1);
                    g.add(-1);
                }
            });
    }
    EXPECT_EQ(c.value(), 80'000u);
    EXPECT_EQ(h.count(), 80'000u);
    EXPECT_EQ(h.sum(), 8u * 10'000u * 10'001u / 2u);
    EXPECT_EQ(h.max(), 10'000u);
    EXPECT_EQ(g.value(), 0);
    EXPECT_GE(g.high(), 1);
    // The median of 1..10000 is 5000, in [4096, 8192): reported as that bucket's upper bound.
    EXPECT_EQ(h.quantile(0.5), 8191u);
    EXPECT_EQ(h.quantile(1.0), 10'000u);   // never above the largest seen
    EXPECT_EQ(&r.counter("t.calls"), &c);  // one instrument a name

    const auto s = r.snapshot();
    EXPECT_EQ(s["counters"]["t.calls"], 80'000u);
    EXPECT_EQ(s["histograms"]["t.latency"]["unit"], "ns");
    EXPECT_EQ(s["histograms"]["t.latency"]["count"], 80'000u);
    EXPECT_TRUE(s["histograms"]["t.latency"]["buckets"].is_array());
    EXPECT_EQ(s["gauges"]["t.queue"]["value"], 0);
    r.reset();
    EXPECT_EQ(c.value(), 0u);
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.quantile(0.5), 0u);
}

XTEST(Telemetry, TheRecorderKeepsTheLastInOrder, .area = "luban", .covers = {"TELEMETRY-RECORDER"}) {
    telemetry::Recorder rec(4);
    for (int k = 0; k < 10; ++k) rec.add(telemetry::Record{.name = std::to_string(k)});
    EXPECT_EQ(rec.total(), 10u);
    const auto all = rec.last();
    ASSERT_EQ(all.size(), 4u);
    EXPECT_EQ(all.front().name, "6");
    EXPECT_EQ(all.back().name, "9");
    const auto two = rec.last(2);
    ASSERT_EQ(two.size(), 2u);
    EXPECT_EQ(two[0].name, "8");
    EXPECT_EQ(two[1].name, "9");
    telemetry::Recorder few(8);
    few.add(telemetry::Record{.name = "a"});
    few.add(telemetry::Record{.name = "b"});
    ASSERT_EQ(few.last().size(), 2u);
    EXPECT_EQ(few.last()[0].name, "a");
}

XTEST(Telemetry, TheLogIsOneObjectALineFilteredRotatedAndRecorded, .area = "luban", .covers = {"TELEMETRY-LOG"}) {
    auto home = tk::Home::isolated("telemetry-log");
    const auto file = home.root() / "logs" / "t.ndjson";
    telemetry::Log log({.file = file, .level = telemetry::Level::info, .component = "test", .rotate_bytes = 400, .keep = 2});
    ASSERT_TRUE(log.is_open());
    log.debug("only-recorded", {{"k", 1}});
    log.info("connect", {{"node", "n1"}});
    log.warn("slow", {{"queued", 42}});
    auto lines = lines_of(file);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0]["event"], "connect");
    EXPECT_EQ(lines[0]["component"], "test");
    EXPECT_EQ(lines[0]["level"], "info");
    EXPECT_EQ(lines[0]["node"], "n1");
    EXPECT_TRUE(lines[0].contains("ts"));
    EXPECT_EQ(lines[1]["queued"], 42);
    // The debug line is not in the file, but the flight recorder has it.
    bool recorded = false;
    for (const auto& r : telemetry::recorder().last(64))
        recorded = recorded || (r.name == "only-recorded" && r.category == "test" && r.level == telemetry::Level::debug);
    EXPECT_TRUE(recorded);
    // Past 400 bytes the file moves to .1, then .2; never more than `keep`.
    for (int k = 0; k < 40; ++k) log.info("fill", {{"k", k}, {"pad", std::string(40, 'x')}});
    EXPECT_TRUE(fs::exists(file.string() + ".1"));
    EXPECT_TRUE(fs::exists(file.string() + ".2"));
    EXPECT_FALSE(fs::exists(file.string() + ".3"));
    EXPECT_LT(fs::file_size(file), 400u);
    for (const auto& l : lines_of(file.string() + ".1")) EXPECT_EQ(l["event"], "fill");
    // A level asked at run time.
    log.set_level(telemetry::Level::warn);
    const auto before = lines_of(file).size();
    log.info("quiet");
    EXPECT_EQ(lines_of(file).size(), before);
    // A file it cannot write is said on the recorder, not thrown.
    fs::create_directories(home.root() / "a-dir.ndjson");
    telemetry::Log bad({.file = home.root() / "a-dir.ndjson", .component = "bad"});
    EXPECT_FALSE(bad.is_open());
    bad.error("lost");
    bool said = false;
    for (const auto& r : telemetry::recorder().last(64)) said = said || (r.name == "log-unwritable" && r.category == "bad");
    EXPECT_TRUE(said);
}

XTEST(Telemetry, SpansFeedHistogramsAndWhileProfilingATrace, .area = "luban", .covers = {"TELEMETRY-PROFILE"}) {
    telemetry::name_thread("test-main");
    auto& h = telemetry::metrics().histogram("t.span.work");
    h.reset();
    telemetry::set_profiling(false);
    const auto before = telemetry::recorder().total();
    { telemetry::Span s("work", &h, "test"); }
    EXPECT_EQ(h.count(), 1u);
    EXPECT_EQ(telemetry::recorder().total(), before);   // off: no record
    { telemetry::Span s("nothing", &h, "test"); s.discard(); }
    EXPECT_EQ(h.count(), 1u);

    EXPECT_EQ(telemetry::control("profile on")["profiling"], true);
    {
        telemetry::Span outer("frame", &h, "test");
        telemetry::Span inner("compose", nullptr, "test");
        telemetry::mark("sent", "test", {{"bytes", 10}});
    }
    EXPECT_EQ(telemetry::control("profile off")["profiling"], false);
    EXPECT_EQ(h.count(), 2u);

    const auto trace = telemetry::control("trace");
    ASSERT_TRUE(trace["traceEvents"].is_array());
    bool frame = false, compose = false, sent = false, thread_named = false;
    for (const auto& e : trace["traceEvents"]) {
        if (e["ph"] == "X" && e["name"] == "frame") frame = e["dur"].get<double>() >= 0.0 && e["cat"] == "test";
        if (e["ph"] == "X" && e["name"] == "compose") compose = true;
        if (e["ph"] == "i" && e["name"] == "sent") sent = e["args"]["bytes"] == 10;
        if (e["ph"] == "M" && e["name"] == "thread_name" && e["args"]["name"] == "test-main") thread_named = true;
    }
    EXPECT_TRUE(frame);
    EXPECT_TRUE(compose);
    EXPECT_TRUE(sent);
    EXPECT_TRUE(thread_named);
}

XTEST(Telemetry, ADumpGathersEverythingAndNamesAFailedProvider, .area = "luban", .covers = {"TELEMETRY-DUMP"}) {
    auto home = tk::Home::isolated("telemetry-dump");
    telemetry::identify("telemetry-test");
    telemetry::metrics().counter("t.dump.seen").add(3);
    telemetry::provide("test.windows", [] { return Json{{"open", 2}}; });
    telemetry::provide("test.broken", []() -> Json { throw std::runtime_error("the list is locked"); });
    telemetry::log().info("before-the-dump");

    const auto d = telemetry::dump();
    EXPECT_EQ(d["telemetry"], "0.1");
    EXPECT_EQ(d["program"], "telemetry-test");
    EXPECT_EQ(d["metrics"]["counters"]["t.dump.seen"], 3u);
    EXPECT_EQ(d["state"]["test.windows"]["open"], 2);
    EXPECT_NE(d["state"]["test.broken"]["error"].get<std::string>().find("the list is locked"), std::string::npos);
    bool before = false;
    for (const auto& r : d["recent"]) before = before || r["name"] == "before-the-dump";
    EXPECT_TRUE(before);
    EXPECT_TRUE(d.contains("uptime_ns"));

    telemetry::withdraw("test.broken");
    EXPECT_FALSE(telemetry::dump()["state"].contains("test.broken"));

    const auto written = telemetry::dump_to(home.root() / "dumps");
    ASSERT_TRUE(written.has_value()) << written.error();
    EXPECT_TRUE(written->filename().string().starts_with("telemetry-test-"));
    std::ifstream in(*written);
    const auto back = Json::parse(in);
    EXPECT_EQ(back["state"]["test.windows"]["open"], 2);

    // The interface, one word or two.
    EXPECT_TRUE(telemetry::control("metrics")["metrics"]["counters"].contains("t.dump.seen"));
    EXPECT_LE(telemetry::control("recent 3")["recent"].size(), 3u);
    EXPECT_EQ(telemetry::control("level debug")["level"], "debug");
    EXPECT_EQ(telemetry::log().level(), telemetry::Level::debug);
    telemetry::log().set_level(telemetry::Level::info);
    const auto unknown = telemetry::control("make coffee");
    EXPECT_TRUE(unknown.contains("error"));
    EXPECT_TRUE(unknown["requests"].is_array());
    EXPECT_EQ(telemetry::control("reset")["reset"], true);
    EXPECT_EQ(telemetry::metrics().counter("t.dump.seen").value(), 0u);
    telemetry::withdraw("test.windows");
}
