// opencaps.conformance: what a part says it provides must hold (SPEC.md §6):
// registered names at registered levels.
export module opencaps.conformance;

import std;
import opencaps;

export namespace opencaps::conformance {

struct Report {
    int held { 0 };
    std::vector<std::string> failed;
    bool ok() const { return failed.empty(); }
};

Report check(const Provides& provides);

}  // namespace opencaps::conformance
