// openas.conformance: what a system's description of its verbs must hold
// (SPEC.md §6).
export module openas.conformance;

import std;
import openas;

export namespace openas::conformance {

struct Report {
    int held { 0 };
    std::vector<std::string> failed;
    bool ok() const { return failed.empty(); }
};

Report check(std::span<const Verb> verbs);

}  // namespace openas::conformance
