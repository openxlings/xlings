module;

module xlings.core.semver;

import std;


// ── out-of-line class members ──────────────────────────────────

namespace xlings::semver {

Segment Segment::number(unsigned long long v) { return {true, v, {}}; }

Segment Segment::alpha(std::string t) { return {false, 0, std::move(t)}; }

} // namespace xlings::semver
