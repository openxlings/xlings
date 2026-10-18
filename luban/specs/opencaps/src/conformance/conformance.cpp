module opencaps.conformance;

import std;
import opencaps;

namespace opencaps::conformance {

Report check(const Provides& provides) {
    Report r;
    for (const auto& [name, level] : provides) {
        const auto top = known_level(name);
        if (top == 0) r.failed.push_back(std::format("{}: not a registered capability (SPEC.md §5)", name));
        else if (level < 1 || level > top)
            r.failed.push_back(std::format("{}={}: registered levels are 1 to {}", name, level, top));
        else ++r.held;
    }
    return r;
}

}  // namespace opencaps::conformance
