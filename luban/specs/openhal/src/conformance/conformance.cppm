// openhal.conformance: what any device source must hold (SPEC.md §4).
//
// Compiling `check(source)` is half the check: a source that does not
// satisfy openhal::Source is refused there, with the requirement it misses.
// Running it is the other half: what a compile cannot see.
export module openhal.conformance;

import std;
import openhal;

export namespace openhal::conformance {

struct Report {
    int held { 0 };
    std::vector<std::string> failed;    // one line per clause that did not hold
    bool ok() const { return failed.empty(); }
};

template <Source S>
Report check(const S& source) {
    Report r;
    const auto hold = [&](bool b, std::string what) {
        if (b) ++r.held;
        else r.failed.push_back(std::move(what));
    };
    hold(!std::string_view(source.name()).empty(), "4.1 a source names itself");
    for (const auto kind : kKinds) {
        const auto first = source.list(kind);
        const auto second = source.list(kind);
        if (!first) {
            hold(!first.error().reason.empty(), std::format("4.2 {}: could not read, and says why", name_of(kind)));
            continue;
        }
        std::set<std::string> ids;
        for (const auto& d : *first) {
            hold(d.kind == kind, std::format("4.3 {}: lists only devices of the kind asked ({})", name_of(kind), d.id));
            hold(!d.id.empty(), std::format("4.4 {}: every device has an id", name_of(kind)));
            hold(!d.name.empty(), std::format("4.4 {}: every device has a name ({})", name_of(kind), d.id));
            hold(ids.insert(d.id).second, std::format("4.5 {}: ids are unique ({})", name_of(kind), d.id));
        }
        if (second) {
            std::set<std::string> again;
            for (const auto& d : *second) again.insert(d.id);
            hold(again == ids, std::format("4.6 {}: the same devices listed twice have the same ids", name_of(kind)));
        }
    }
    return r;
}

}  // namespace openhal::conformance
