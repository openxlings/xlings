module xlings.xdev.selection;

import std;
import xlings.xdev.resources;
import xlings.libs.json;

namespace xlings::xdev::selection {

namespace {

struct Token { std::string text; bool quoted { false }; };

// Only declarations and quoted includes are needed. Comments and strings,
// including raw fixture strings, must not become fictitious dependencies.
std::vector<Token> tokens(std::string_view code) {
    std::vector<Token> out;
    for (std::size_t at = 0; at < code.size();) {
        if (std::isspace(static_cast<unsigned char>(code[at]))) { ++at; continue; }
        if (code.substr(at, 2) == "//") {
            const auto end = code.find('\n', at + 2);
            at = end == code.npos ? code.size() : end + 1;
            continue;
        }
        if (code.substr(at, 2) == "/*") {
            const auto end = code.find("*/", at + 2);
            at = end == code.npos ? code.size() : end + 2;
            continue;
        }
        if (code.substr(at, 2) == "R\"") {
            const auto begin = code.find('(', at + 2);
            if (begin != code.npos && begin - at <= 18) {
                const auto closing = ")" + std::string(code.substr(at + 2, begin - at - 2)) + "\"";
                const auto end = code.find(closing, begin + 1);
                at = end == code.npos ? code.size() : end + closing.size();
                continue;
            }
        }
        if (code[at] == '"' || code[at] == '\'') {
            const char quote = code[at++];
            std::string value;
            while (at < code.size() && code[at] != quote) {
                if (code[at] == '\\' && at + 1 < code.size()) ++at;
                value += code[at++];
            }
            if (at < code.size()) ++at;
            out.push_back({value, true});
            continue;
        }
        const auto begin = at++;
        if (std::isalnum(static_cast<unsigned char>(code[begin])) || code[begin] == '_')
            while (at < code.size()
                   && (std::isalnum(static_cast<unsigned char>(code[at])) || code[at] == '_')) ++at;
        out.push_back({std::string(code.substr(begin, at - begin)), false});
    }
    return out;
}

bool under(const fs::path& path, std::string_view prefix) {
    const auto value = path.generic_string();
    return value == prefix || value.starts_with(std::string(prefix) + "/");
}

bool platform_ok(std::span<const std::string> required, std::string_view platform) {
    for (const auto& cap : required)
        if ((cap == "linux" || cap == "macos" || cap == "windows") && cap != platform) return false;
    return true;
}

bool expensive(const Case& test) {
    if (test.cost == "slow") return true;
    for (const auto& cap : test.requires_)
        if (cap == "sandbox" || cap == "bwrap" || cap == "sudo" || cap == "docker"
            || cap == "qemu" || cap == "kvm" || cap == "network" || cap == "landlock") return true;
    return false;
}

bool has(std::span<const std::string> values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

}  // namespace

Source scan_source(fs::path path, std::string_view contents) {
    Source source{.path = std::move(path)};
    const auto ts = tokens(contents);
    for (std::size_t at = 0; at < ts.size(); ++at) {
        if (ts[at].quoted) continue;
        if (ts[at].text == "#" && at + 2 < ts.size() && ts[at + 1].text == "include" && ts[at + 2].quoted) {
            source.includes.push_back(ts[at + 2].text);
            continue;
        }
        if (ts[at].text != "module" && ts[at].text != "import") continue;
        const auto keyword = ts[at].text;
        std::string name;
        auto end = at + 1;
        for (; end < ts.size() && ts[end].text != ";"; ++end) {
            if (ts[end].quoted || ts[end].text == "{" || ts[end].text == "}") { name.clear(); break; }
            if (ts[end].text != "." && ts[end].text != ":"
                && !std::ranges::all_of(ts[end].text, [](unsigned char c) { return std::isalnum(c) || c == '_'; })) {
                name.clear(); break;
            }
            name += ts[end].text;
        }
        if (name.empty() || end == ts.size()) continue;
        if (keyword == "module" && !name.starts_with(':')) source.module = name;
        else if (keyword == "import") source.imports.push_back(name);
        at = end;
    }
    for (auto& name : source.imports)
        if (name.starts_with(':')) name = source.module.substr(0, source.module.find(':')) + name;
    return source;
}

std::expected<std::vector<Source>, std::string> sources(const fs::path& root) {
    std::vector<Source> graph;
    std::error_code ec;
    for (auto top : {"src", "modules", "apps", "tests"}) {
        const auto dir = root / top;
        if (!fs::exists(dir, ec)) { if (ec) return std::unexpected(ec.message()); continue; }
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) { if (ec) break; continue; }
            const auto extension = it->path().extension();
            if (extension != ".cpp" && extension != ".cppm" && extension != ".h" && extension != ".hpp") continue;
            std::ifstream in(it->path(), std::ios::binary);
            if (!in) return std::unexpected("cannot read dependency source " + it->path().string());
            const auto text = std::string{std::istreambuf_iterator<char>(in), {}};
            if (in.bad()) return std::unexpected("cannot read dependency source " + it->path().string());
            graph.push_back(scan_source(it->path().lexically_relative(root), text));
        }
        if (ec) return std::unexpected("cannot inspect dependency sources: " + ec.message());
    }
    return graph;
}

std::set<std::string> affected(const std::vector<Test>& tests, const std::vector<Source>& graph,
                              std::span<const fs::path> changed) {
    std::set<fs::path> dirty;
    std::set<std::string> modules;
    std::map<std::string, std::vector<fs::path>> headers;
    for (const auto& source : graph) headers[source.path.filename().string()].push_back(source.path);
    bool global { false }, product { false };
    for (const auto& path : changed) {
        dirty.insert(path.lexically_normal());
        product = product || under(path, "src") || under(path, "modules") || under(path, "res");
        const auto extension = path.extension();
        if (path.filename() == "mcpp.toml" || path.filename() == "mcpp.lock"
            || path.filename() == ".xlings.json" || under(path, ".github") || under(path, "res")) global = true;
        if ((extension == ".cpp" || extension == ".cppm" || extension == ".h" || extension == ".hpp")
            && std::ranges::find(graph, path.lexically_normal(), &Source::path) == graph.end()) global = true;
    }
    bool progress { true };
    while (progress) {
        progress = false;
        for (const auto& source : graph) {
            bool depends = dirty.contains(source.path) || (!source.module.empty() && modules.contains(source.module));
            for (const auto& name : source.imports) depends = depends || modules.contains(name);
            for (const auto& include : source.includes) {
                depends = depends || dirty.contains((source.path.parent_path() / include).lexically_normal())
                    || dirty.contains(fs::path(include).lexically_normal());
                // Include search paths vary by package; duplicate matching names
                // deliberately overselect rather than guess which compiler chose.
                for (const auto& candidate : headers[fs::path(include).filename().string()])
                    depends = depends || dirty.contains(candidate);
            }
            if (!depends) continue;
            progress = dirty.insert(source.path).second || progress;
            if (!source.module.empty()) progress = modules.insert(source.module).second || progress;
        }
    }
    std::set<std::string> impacted;
    for (const auto& test : tests)
        if (global || dirty.contains(test.source) || (product && (under(test.source, "tests/e2e") || test.script)))
            impacted.insert(test.id);
    return impacted;
}

std::expected<std::pair<std::size_t, std::size_t>, std::string> parse_shard(std::string_view text) {
    const auto slash = text.find('/');
    std::size_t index { 0 }, count { 0 };
    if (slash == text.npos) return std::unexpected("--shard expects i/n with 1 <= i <= n");
    const auto [left, le] = std::from_chars(text.data(), text.data() + slash, index);
    const auto [right, re] = std::from_chars(text.data() + slash + 1, text.data() + text.size(), count);
    if (le != std::errc{} || re != std::errc{} || left != text.data() + slash || right != text.data() + text.size()
        || index == 0 || count == 0 || index > count || count > 1024)
        return std::unexpected("--shard expects i/n with 1 <= i <= n <= 1024");
    return std::pair{index, count};
}

std::expected<std::vector<Selected>, std::string>
select(std::span<const Test> tests, const Options& options, const std::set<std::string>& impacted) {
    if (options.lane != "pr" && options.lane != "main" && options.lane != "nightly")
        return std::unexpected("--lane must be pr, main or nightly");
    if (options.shards == 0 || options.shards > 1024 || options.shard > options.shards)
        return std::unexpected("invalid shard count or index");
    if (!options.category.empty() && options.category != "unit" && options.category != "e2e" && options.category != "perf")
        return std::unexpected("unknown test category " + options.category);
    if (!options.platform.empty() && options.platform != "linux" && options.platform != "macos" && options.platform != "windows")
        return std::unexpected("unknown platform " + options.platform);
    std::vector<Selected> selected;
    for (const auto& test : tests) {
        if (!options.pattern.empty() && test.id.find(options.pattern) == std::string::npos) continue;
        const bool perf = test.source.stem().string().ends_with("_perf");
        if (options.category.empty() && options.pattern.empty() && options.lane != "nightly" && perf) continue;
        if (!options.category.empty()) {
            if ((options.category == "unit" && !under(test.source, "tests/unit"))
                || (options.category == "e2e" && (!under(test.source, "tests/e2e") || perf))
                || (options.category == "perf" && !perf)) continue;
        }
        if (!options.platform.empty() && !test.platforms.empty() && !has(test.platforms, options.platform)) continue;
        if (options.require_metadata && !test.metadata_known && !test.script)
            return std::unexpected("compiled metadata is missing for " + test.id + "; build tests or provide --discovery");
        if (!options.area.empty() && !test.metadata_known)
            return std::unexpected("--area needs compiled metadata for " + test.id + "; build tests or provide --discovery");
        Selected entry{.test = test};
        if (test.metadata_known && !test.cases.empty()) {
            std::size_t kept { 0 };
            for (const auto& testCase : test.cases) {
                if (!options.platform.empty() && !platform_ok(testCase.requires_, options.platform)) continue;
                if (!options.area.empty() && testCase.area != options.area) continue;
                if (options.lane == "pr") {
                    if (has(testCase.requires_, "network")) continue;
                    if (options.changed && expensive(testCase) && !impacted.contains(test.id)) continue;
                }
                if (!entry.filter.empty()) entry.filter += ':';
                entry.filter += testCase.name;
                ++kept;
                for (const auto& resource : testCase.resources) {
                    auto name = resources::normalize(resource);
                    if (!name) return std::unexpected(test.id + ": " + name.error());
                    if (!has(entry.resources, *name)) entry.resources.push_back(*name);
                }
                for (const auto& cap : testCase.requires_)
                    if (!has(entry.requires_, cap)) entry.requires_.push_back(cap);
            }
            if (kept == 0) continue;
            if (kept == test.cases.size()) entry.filter.clear();
        } else if (options.lane == "pr" && test.script
                   && test.suite != "lint" && test.suite != "contract-scripts") {
            // The legacy aggregate e2e adapter has no per-command network
            // declaration. It belongs to main/nightly until migrated.
            continue;
        }
        selected.push_back(std::move(entry));
    }
    std::ranges::sort(selected, [](const Selected& a, const Selected& b) {
        if (a.test.duration_ms != b.test.duration_ms) return a.test.duration_ms > b.test.duration_ms;
        return a.test.id < b.test.id;
    });
    std::vector<long long> load(options.shards, 0);
    for (auto& test : selected) {
        const auto least = std::ranges::min_element(load);
        test.shard = static_cast<std::size_t>(least - load.begin()) + 1;
        *least += test.test.duration_ms;
    }
    if (options.shard != 0) std::erase_if(selected, [&](const Selected& test) { return test.shard != options.shard; });
    std::ranges::sort(selected, {}, [](const Selected& test) { return test.test.id; });
    return selected;
}

std::expected<std::vector<Test>, std::string> discovery(std::string_view ndjson, const fs::path& root) {
    std::vector<Test> tests;
    std::set<std::string> ids;
    std::istringstream stream{std::string(ndjson)};
    for (std::string line; std::getline(stream, line);) {
        if (line.empty()) continue;
        const auto row = nlohmann::json::parse(line, nullptr, false);
        if (!row.is_object()) return std::unexpected("invalid JSON test discovery record");
        if (row.contains("summary")) continue;
        try {
            Test test;
            const auto name = row.at("test").get<std::string>();
            test.member = row.value("member", "");
            test.id = test.member.empty() ? name : test.member + ":" + name;
            const fs::path source = row.at("main").get<std::string>();
            test.source = (source.is_absolute() ? source.lexically_relative(root) : source).lexically_normal();
            if (test.id.empty() || test.source.empty() || test.source.is_absolute() || *test.source.begin() == ".."
                || !fs::is_regular_file(root / test.source) || !ids.insert(test.id).second)
                return std::unexpected("discovery has an invalid, absent or duplicate test source: " + name);
            test.duration_ms = under(test.source, "tests/unit") ? 1000 : 5000;
            if (auto cases = row.find("cases"); cases != row.end()) {
                if (!cases->is_array()) return std::unexpected("discovery cases must be an array");
                test.metadata_known = true;
                std::set<std::string> names;
                for (const auto& value : *cases) {
                    Case testCase{.name = value.at("test").get<std::string>(),
                        .area = value.value("area", ""), .cost = value.value("cost", "fast"),
                        .requires_ = value.value("requires", std::vector<std::string>{}),
                        .resources = value.value("resources", std::vector<std::string>{})};
                    if (testCase.name.empty() || testCase.name.find_first_of(":*?\n\r") != testCase.name.npos
                        || !names.insert(testCase.name).second)
                        return std::unexpected("invalid or duplicate discovered case name");
                    if (testCase.cost != "fast" && testCase.cost != "medium" && testCase.cost != "slow")
                        return std::unexpected("invalid discovered test cost");
                    test.cases.push_back(std::move(testCase));
                }
            }
            tests.push_back(std::move(test));
        } catch (const nlohmann::json::exception& error) {
            return std::unexpected("invalid test discovery: " + std::string(error.what()));
        }
    }
    return tests;
}

std::expected<void, std::string> apply_timings(std::vector<Test>& tests, const nlohmann::json& timings, std::string_view platform) {
    if (!timings.is_object()) return std::unexpected("timings must map test binary IDs to positive milliseconds");
    for (auto& test : tests) {
        auto value = platform.empty() ? timings.end() : timings.find(std::string(platform) + ":" + test.id);
        if (value == timings.end()) value = timings.find(test.id);
        if (value == timings.end()) continue;
        if (!value->is_number_integer() || *value <= 0 || *value > 86400000)
            return std::unexpected("invalid timing for " + test.id);
        test.duration_ms = value->get<long long>();
    }
    return {};
}

nlohmann::json matrix(std::span<const Test> tests, const Options& options, const std::set<std::string>& impacted) {
    nlohmann::json include = nlohmann::json::array();
    for (auto platform : {"linux", "macos", "windows"}) {
        if (!options.platform.empty() && options.platform != platform) continue;
        auto perPlatform = options;
        perPlatform.platform = platform;
        perPlatform.shard = 0;
        const auto selected = select(tests, perPlatform, impacted);
        if (!selected) throw std::invalid_argument(selected.error());
        for (std::size_t shard = 1; shard <= options.shards; ++shard) {
            nlohmann::json ids = nlohmann::json::array(), filters = nlohmann::json::object();
            std::set<std::string> capabilities, resourceNames;
            long long duration { 0 };
            for (const auto& test : *selected) {
                if (test.shard != shard) continue;
                ids.push_back(test.test.id);
                filters[test.test.id] = test.filter;
                capabilities.insert(test.requires_.begin(), test.requires_.end());
                resourceNames.insert(test.resources.begin(), test.resources.end());
                duration += test.test.duration_ms;
            }
            if (ids.empty()) continue;
            include.push_back({{"platform", platform}, {"lane", options.lane},
                {"shard", std::format("{}/{}", shard, options.shards)}, {"tests", ids},
                {"filters", filters}, {"requires", std::vector<std::string>(capabilities.begin(), capabilities.end())},
                {"resources", std::vector<std::string>(resourceNames.begin(), resourceNames.end())},
                {"estimated_ms", duration}});
        }
    }
    return {{"include", include}};
}

}  // namespace xlings::xdev::selection
