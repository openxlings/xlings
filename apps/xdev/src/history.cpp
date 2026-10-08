module xlings.xdev.history;

import std;
import xlings.libs.json;

namespace xlings::xdev::history {
using nlohmann::json;
namespace {
using Key = std::tuple<std::string, std::string, std::string, std::string>;
Key key(const Observation& o) { return {o.platform, o.lane, o.kind, o.name}; }
long long median(std::vector<long long> values) {
    std::ranges::sort(values);
    const auto mid = values.size() / 2;
    return values.size() % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2;
}
std::expected<void, std::string> validate(const Observation& o) {
    if ((o.kind != "binary" && o.kind != "script" && o.kind != "case") || o.name.empty()
        || (o.platform != "linux" && o.platform != "macos" && o.platform != "windows")
        || o.lane.empty() || o.run.empty()
        || (o.status != "pass" && o.status != "fail" && o.status != "skip")
        || o.ms < 0 || o.ms > 86400000)
        return std::unexpected("invalid trend observation: " + o.name);
    return {};
}
}

std::expected<Result, std::string> append(const json& previous, std::span<const Observation> current) {
    std::map<Key, std::vector<Observation>> grouped;
    try {
        if (!previous.is_null()) {
            if (!previous.is_object() || previous.at("format") != 1 || !previous.at("observations").is_array())
                return std::unexpected("unsupported trend history format");
            for (const auto& row : previous.at("observations")) {
                Observation o{row.at("kind").get<std::string>(), row.at("name").get<std::string>(),
                    row.at("platform").get<std::string>(), row.at("lane").get<std::string>(),
                    row.at("run").get<std::string>(), row.at("status").get<std::string>(), row.at("ms").get<long long>()};
                if (const auto valid = validate(o); !valid) return std::unexpected(valid.error());
                auto& group = grouped[key(o)];
                if (group.size() >= 64) return std::unexpected("trend history exceeds retention for " + o.name);
                if (std::ranges::any_of(group, [&](const Observation& p) { return p.run == o.run; }))
                    return std::unexpected("duplicate trend execution: " + o.name);
                group.push_back(std::move(o));
            }
        }
    } catch (const std::exception& e) { return std::unexpected(std::string("invalid trend history: ") + e.what()); }

    std::string md = "\n### Timing trends\n\n| test | platform / lane | current ms | prior median ms | change |\n|---|---|---:|---:|---:|\n";
    bool compared = false;
    for (const auto& o : current) {
        if (const auto valid = validate(o); !valid) return std::unexpected(valid.error());
        auto& group = grouped[key(o)];
        const auto same = std::ranges::find(group, o.run, &Observation::run);
        if (same != group.end()) {
            if (same->status != o.status || same->ms != o.ms)
                return std::unexpected("conflicting results for the same execution: " + o.name);
            continue;
        }
        std::vector<long long> prior;
        for (const auto& p : group) if (p.status == "pass" && p.ms > 0) prior.push_back(p.ms);
        if (o.kind != "case" && o.status == "pass" && o.ms > 0 && !prior.empty()) {
            const auto baseline = median(prior);
            md += std::format("| `{}` | {} / {} | {} | {} | {:.1f}% |\n", o.name, o.platform, o.lane,
                o.ms, baseline, 100.0 * (double(o.ms) / double(baseline) - 1.0));
            compared = true;
        }
        if (group.size() == 64) group.erase(group.begin());
        group.push_back(o);
    }
    if (!compared) md += "\nNo prior successful execution with a duration for this platform and lane.\n";
    Result result{.history = {{"format", 1}, {"observations", json::array()}}, .timings = json::object()};
    std::map<std::string, std::vector<long long>> weights;
    std::string flaky;
    for (const auto& [k, group] : grouped) {
        bool passed = false, failed = false;
        for (const auto& o : group) {
            result.history["observations"].push_back({{"kind", o.kind}, {"name", o.name}, {"platform", o.platform},
                {"lane", o.lane}, {"run", o.run}, {"status", o.status}, {"ms", o.ms}});
            passed |= o.status == "pass";
            failed |= o.status == "fail";
            if (o.kind != "case" && o.status == "pass" && o.ms > 0)
                weights[o.platform + ":" + o.name].push_back(o.ms);
        }
        if (passed && failed) flaky += std::format("- `{}` ({}, {}) has both passing and failing executions.\n",
            std::get<3>(k), std::get<0>(k), std::get<1>(k));
    }
    for (auto& [id, values] : weights) result.timings[id] = std::max(1LL, median(std::move(values)));
    if (!flaky.empty()) md += "\n### Intermittent results\n\n" + flaky;
    result.markdown = std::move(md);
    return result;
}
}
