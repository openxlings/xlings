export module xlings.xdev.selection;

import std;
import xlings.libs.json;

export namespace xlings::xdev::selection {

namespace fs = std::filesystem;

struct Case {
    std::string name;
    std::string area;
    std::string cost { "fast" };
    std::vector<std::string> requires_;
    std::vector<std::string> resources;
};

struct Test {
    std::string id;                  // mcpp's member-qualified binary or script:suite:index
    fs::path source;                // checkout-relative, from mcpp's discovery
    std::string member;
    std::vector<Case> cases;        // compiled XTEST registry, not source guesses
    bool metadata_known { false };
    bool script { false };
    std::string suite;
    std::size_t command_index { 0 };
    std::vector<std::string> platforms;
    long long duration_ms { 1000 };
};

struct Source {
    fs::path path;
    std::string module;
    std::vector<std::string> imports;
    std::vector<std::string> includes;
};

Source scan_source(fs::path path, std::string_view contents);
std::expected<std::vector<Source>, std::string> sources(const fs::path& root);
// Interfaces, implementation units and local headers all participate. An
// unsupported/global build input selects conservatively, never silently less.
std::set<std::string> affected(const std::vector<Test>& tests, const std::vector<Source>& graph,
                              std::span<const fs::path> changed);

struct Options {
    std::string lane { "main" };     // pr | main | nightly
    std::string platform;             // linux | macos | windows; empty = all
    std::string pattern;
    std::string category;             // unit | e2e | perf
    std::string area;                 // requires compiled metadata for every candidate
    std::size_t shards { 1 };
    std::size_t shard { 0 };          // 0 = all; CLI indices are 1-based
    bool changed { false };
    bool require_metadata { false };
};

struct Selected {
    Test test;
    std::string filter;               // gtest case filter; empty = all cases
    std::vector<std::string> requires_;
    std::vector<std::string> resources;
    std::size_t shard { 1 };
};

std::expected<std::vector<Selected>, std::string>
select(std::span<const Test> tests, const Options& options,
       const std::set<std::string>& impacted = {});
std::expected<std::pair<std::size_t, std::size_t>, std::string> parse_shard(std::string_view text);
// Input: mcpp test --list --message-format json, optionally augmented by
// xdev's exported compiled registries. Invalid records are errors, not skips.
std::expected<std::vector<Test>, std::string> discovery(std::string_view ndjson, const fs::path& root);
std::expected<void, std::string> apply_timings(std::vector<Test>& tests, const nlohmann::json& timings, std::string_view platform = {});
nlohmann::json matrix(std::span<const Test> tests, const Options& options,
                      const std::set<std::string>& impacted = {});

}  // namespace xlings::xdev::selection
