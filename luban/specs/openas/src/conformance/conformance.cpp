module openas.conformance;

import std;
import openas;

namespace openas::conformance {

namespace {

bool verb_name_(std::string_view n) {
    if (n.empty() || !std::islower(static_cast<unsigned char>(n.front()))) return false;
    return std::ranges::all_of(n, [](char c) {
        return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '.';
    });
}

}  // namespace

Report check(std::span<const Verb> verbs) {
    Report r;
    const auto hold = [&](bool b, std::string what) {
        if (b) ++r.held;
        else r.failed.push_back(std::move(what));
    };
    std::set<std::string> names;
    for (const auto& v : verbs) {
        hold(verb_name_(v.name), std::format("6.1 {}: a verb's name is lower case words", v.name));
        hold(names.insert(v.name).second, std::format("6.2 {}: names are unique", v.name));
        hold(!v.summary.empty(), std::format("6.3 {}: says what it does", v.name));
        std::set<std::string> params;
        for (const auto& p : v.arguments) hold(params.insert(p.name).second, std::format("6.4 {}: {} is named once", v.name, p.name));
        for (const auto& p : v.options) hold(params.insert(p.name).second, std::format("6.4 {}: {} is named once", v.name, p.name));
        for (const auto& a : v.answers)
            hold(std::ranges::any_of(v.options, [&](const Param& p) { return p.name == a; }),
                 std::format("6.5 {}: its answer {} is one of its options", v.name, a));
        if (asks_first(v.effect))
            hold(!v.answers.empty(), std::format("6.6 {}: a verb that asks first ({}) names the option that answers",
                                                 v.name, name_of(v.effect)));
    }
    return r;
}

}  // namespace openas::conformance
