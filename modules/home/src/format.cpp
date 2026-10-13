module xlings.home.format;

import std;
import xlings.libs.json;
import xlings.platform;

namespace xlings::home::format {

namespace {

std::vector<long> parts_(std::string_view v) {
    std::vector<long> out;
    if (!v.empty() && (v.front() == 'v' || v.front() == 'V')) v.remove_prefix(1);
    while (!v.empty()) {
        const auto dot = v.find('.');
        const auto part = v.substr(0, dot);
        long n = 0;
        std::from_chars(part.data(), part.data() + part.size(), n);
        out.push_back(n);
        if (dot == std::string_view::npos) break;
        v.remove_prefix(dot + 1);
    }
    return out;
}

}  // namespace

std::span<const Epoch> epochs() {
    static const std::array<Epoch, 2> value{{
        {1, "0.4.20", "packages, versions and SubOS workspaces"},
        {2, "2026.10.9.1", "SubOS policies, sessions, roots and Luban environments"},
    }};
    return value;
}

int compare_versions(std::string_view a, std::string_view b) {
    const auto x = parts_(a), y = parts_(b);
    for (std::size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

ClientFormat client_format(std::string_view version) {
    // The layout gate arrived with layout 2.
    if (compare_versions(version, "2026.10.9.1") < 0) return {.gates = false, .knows = 1};
    int knows = 1;
    for (const auto& e : epochs())
        if (compare_versions(version, e.since) >= 0) knows = std::max(knows, e.layout);
    return {.gates = true, .knows = knows};
}

std::optional<SwitchNote> switch_note(int layout, std::string_view version) {
    const auto client = client_format(version);
    if (client.knows >= layout) return std::nullopt;
    std::string unseen;
    for (const auto& e : epochs())
        if (e.layout > client.knows && e.layout <= layout) {
            if (!unseen.empty()) unseen += "; ";
            unseen += e.brings;
        }
    SwitchNote note;
    note.read_only = client.gates;
    note.text = client.gates
        ? std::format("this home is layout {}; xlings {} knows layout {} and only reads it -- "
                      "nothing it does is saved, and it does not see: {}", layout, version, client.knows, unseen)
        : std::format("this home is layout {}; xlings {} knows layout {}: it does not see {} "
                      "(they stay as they are for a newer client)", layout, version, client.knows, unseen);
    return note;
}

Report run(const fs::path& home, std::span<const Step> steps) {
    Report report;
    for (const auto& step : steps) {
        try {
            if (step.needed && !step.needed(home)) continue;
            auto done = step.apply(home);
            if (done) report.done.emplace_back(std::string(step.name), *done);
            else report.failed.emplace_back(std::string(step.name), done.error());
        } catch (const std::exception& e) {
            report.failed.emplace_back(std::string(step.name), e.what());
        } catch (...) {
            report.failed.emplace_back(std::string(step.name), "failed");
        }
    }
    return report;
}

bool told(Problem p) { return p == Problem::Reinstallable || p == Problem::Decision; }

std::string_view to_string(Problem p) {
    switch (p) {
    case Problem::Format: return "format";
    case Problem::Derived: return "derived";
    case Problem::Dangling: return "dangling";
    case Problem::Reinstallable: return "reinstallable";
    case Problem::Recipe: return "recipe";
    case Problem::Decision: return "decision";
    }
    return "format";
}

}  // namespace xlings::home::format
