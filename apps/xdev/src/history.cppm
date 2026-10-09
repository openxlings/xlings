export module xlings.xdev.history;

import std;
import xlings.libs.json;

export namespace xlings::xdev::history {

struct Observation {
    std::string kind;
    std::string name;
    std::string platform;
    std::string lane;
    std::string run;
    std::string status;
    long long ms { 0 };
};

struct Result {
    nlohmann::json history;
    nlohmann::json timings;
    std::string markdown;
};

// Keep 64 executions for each platform/lane/test, deduplicate re-reports,
// compare against prior successful medians, and export usable shard weights.
std::expected<Result, std::string> append(const nlohmann::json& previous,
                                        std::span<const Observation> current);

}
